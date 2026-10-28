import { resolve } from "node:path";
import { fileURLToPath } from "node:url";

import type { OutlineOptions, RgbaColor } from "./types.ts";

const REQUEST_MAGIC = "STK1";
const RESPONSE_MAGIC = "STR1";
const PROTOCOL_VERSION = 1;
const COMMAND_TRANSCODE = 1;
const TRIM_FLAG = 1 << 0;
const REQUEST_HEADER_BYTES = 18;
const RESPONSE_HEADER_BYTES = 26;
const FRAME_LENGTH_BYTES = 4;
const MAXIMUM_FRAME_BYTES = 17 * 1024 * 1024;
const MAXIMUM_SOURCE_BYTES = 16 * 1024 * 1024;
const MAXIMUM_STATIC_STICKER_BYTES = 100 * 1024;
const MAXIMUM_ANIMATED_STICKER_BYTES = 500 * 1024;
const MAXIMUM_ANIMATION_DURATION_MS = 10_000;
const DEFAULT_TIMEOUT_MS = 30_000;
const textDecoder = new TextDecoder("utf-8", { fatal: true });

type WorkerProcess = Bun.Subprocess<"pipe", "pipe", "ignore">;

interface WorkerSession {
  readonly process: WorkerProcess;
  readonly stdin: WorkerProcess["stdin"];
  readonly reader: BufferedReader;
}

/**
 * Bytes accepted as an input asset. The bytes are copied before a request is
 * queued, so callers may safely reuse or mutate their original buffer.
 */
export type NativeStickerSource = ArrayBuffer | ArrayBufferView;

export interface NativeStickerResult {
  /** WhatsApp-compatible WebP bytes. */
  readonly data: Uint8Array;
  readonly width: number;
  readonly height: number;
  readonly frameCount: number;
  readonly durationMs: number;
  readonly isAnimated: boolean;
}

export interface NativeStickerTranscodeOptions {
  /**
   * Remove fully transparent outer rows and columns before sizing. Defaults
   * to true.
   */
  readonly trim?: boolean;
  /** Alpha at or below this byte value is treated as transparent while trimming. */
  readonly alphaThreshold?: number;
  /** Optional outline rendered behind the source image. */
  readonly outline?: OutlineOptions;
  /**
   * Per-request deadline, including writes and reading the complete response.
   * A timed-out worker is killed and a fresh worker is used for a later call.
   */
  readonly timeoutMs?: number;
}

export interface NativeStickerWorkerOptions {
  /**
   * Path to the `sticker_worker` executable. It defaults to the installed
   * platform worker package, or the local CMake output for a source checkout.
   */
  readonly workerPath?: string;
  /**
   * Optional arguments after `workerPath`. This is mainly useful for wrapping
   * or testing the worker process.
   */
  readonly workerArgs?: readonly string[];
  /** Default per-request deadline in milliseconds. Defaults to 30 seconds. */
  readonly timeoutMs?: number;
}

/**
 * A response produced by the worker for a valid request that it could not
 * process, for example because the source format is unsupported.
 */
export class NativeStickerWorkerError extends Error {
  readonly status: number;
  readonly code: number;

  constructor(message: string, status: number, code: number) {
    super(message);
    this.name = "NativeStickerWorkerError";
    this.status = status;
    this.code = code;
  }
}

/** The native worker did not finish one request before its configured deadline. */
export class NativeStickerWorkerTimeoutError extends Error {
  readonly timeoutMs: number;

  constructor(timeoutMs: number) {
    super(`Sticker worker exceeded its ${timeoutMs} ms deadline.`);
    this.name = "NativeStickerWorkerTimeoutError";
    this.timeoutMs = timeoutMs;
  }
}

/** The worker exited, returned malformed protocol data, or could not be started. */
export class NativeStickerWorkerTransportError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "NativeStickerWorkerTransportError";
  }
}

/** A caller attempted to enqueue work after the worker was closed. */
export class NativeStickerWorkerClosedError extends Error {
  constructor() {
    super("Sticker worker is closed.");
    this.name = "NativeStickerWorkerClosedError";
  }
}

class WorkerProtocolError extends NativeStickerWorkerTransportError {
  constructor(message: string) {
    super(message);
    this.name = "WorkerProtocolError";
  }
}

class BufferedReader {
  readonly #reader: ReadableStreamDefaultReader<Uint8Array>;
  #current: Uint8Array | undefined;
  #offset = 0;

  constructor(stream: ReadableStream<Uint8Array>) {
    this.#reader = stream.getReader();
  }

  async readExactly(byteLength: number): Promise<Uint8Array> {
    const result = new Uint8Array(byteLength);
    let written = 0;

    while (written < byteLength) {
      let current = this.#current;
      if (current === undefined || this.#offset === current.byteLength) {
        const next = await this.#reader.read();
        if (next.done) {
          throw new NativeStickerWorkerTransportError(
            "Sticker worker closed stdout before its response was complete.",
          );
        }
        if (next.value.byteLength === 0) {
          continue;
        }
        current = next.value;
        this.#current = current;
        this.#offset = 0;
      }

      const available = current.byteLength - this.#offset;
      const needed = byteLength - written;
      const copied = Math.min(available, needed);
      result.set(current.subarray(this.#offset, this.#offset + copied), written);
      this.#offset += copied;
      written += copied;
    }

    return result;
  }

  cancel(): void {
    void this.#reader.cancel().catch(() => undefined);
  }
}

function platformWorkerPackage(): string | undefined {
  if (process.platform !== "linux") {
    return undefined;
  }

  if (process.arch === "x64") {
    return "@rin-ts/sticker-linux-x64-gnu";
  }
  if (process.arch === "arm64") {
    return "@rin-ts/sticker-linux-arm64-gnu";
  }
  return undefined;
}

function installedWorkerPath(executable: string): string | undefined {
  const packageName = platformWorkerPackage();
  if (packageName === undefined) {
    return undefined;
  }

  try {
    const manifestUrl = import.meta.resolve(`${packageName}/package.json`);
    return resolve(fileURLToPath(manifestUrl), "..", "bin", executable);
  } catch {
    return undefined;
  }
}

function defaultWorkerPath(): string {
  const executable = process.platform === "win32" ? "sticker_worker.exe" : "sticker_worker";
  return installedWorkerPath(executable)
    ?? resolve(import.meta.dir, "../build/native/bin", executable);
}

function requireByte(value: number, name: string): number {
  if (!Number.isInteger(value) || value < 0 || value > 255) {
    throw new RangeError(`${name} must be an integer from 0 through 255.`);
  }
  return value;
}

function requireTimeout(value: number, name: string): number {
  if (!Number.isSafeInteger(value) || value <= 0) {
    throw new RangeError(`${name} must be a positive integer.`);
  }
  return value;
}

function readUint32(bytes: Uint8Array, offset: number): number {
  return new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength).getUint32(offset, true);
}

function sourceBytes(source: NativeStickerSource): Uint8Array {
  if (source instanceof ArrayBuffer) {
    return new Uint8Array(source);
  }
  if (ArrayBuffer.isView(source)) {
    return new Uint8Array(source.buffer, source.byteOffset, source.byteLength);
  }
  throw new TypeError("source must be an ArrayBuffer or an ArrayBuffer view.");
}

function encodeColor(color: RgbaColor): readonly [number, number, number, number] {
  return [
    requireByte(color.red, "outline.color.red"),
    requireByte(color.green, "outline.color.green"),
    requireByte(color.blue, "outline.color.blue"),
    requireByte(color.alpha ?? 255, "outline.color.alpha"),
  ];
}

function encodeRequest(
  source: NativeStickerSource,
  options: NativeStickerTranscodeOptions,
): Uint8Array {
  if (options.trim !== undefined && typeof options.trim !== "boolean") {
    throw new TypeError("trim must be a boolean.");
  }

  const input = sourceBytes(source);
  if (input.byteLength > MAXIMUM_SOURCE_BYTES) {
    throw new RangeError(
      `Source exceeds the ${MAXIMUM_SOURCE_BYTES}-byte native worker input limit.`,
    );
  }

  const alphaThreshold = requireByte(options.alphaThreshold ?? 0, "alphaThreshold");
  const outlineWidth = options.outline?.width ?? 0;
  if (!Number.isInteger(outlineWidth) || outlineWidth < 0 || outlineWidth > 64) {
    throw new RangeError("outline.width must be an integer from 0 through 64.");
  }
  const color = encodeColor(
    options.outline?.color ?? { red: 255, green: 255, blue: 255, alpha: 255 },
  );

  const frame = new Uint8Array(FRAME_LENGTH_BYTES + REQUEST_HEADER_BYTES + input.byteLength);
  const view = new DataView(frame.buffer, frame.byteOffset, frame.byteLength);
  view.setUint32(0, REQUEST_HEADER_BYTES + input.byteLength, true);

  const header = FRAME_LENGTH_BYTES;
  frame[header] = REQUEST_MAGIC.charCodeAt(0);
  frame[header + 1] = REQUEST_MAGIC.charCodeAt(1);
  frame[header + 2] = REQUEST_MAGIC.charCodeAt(2);
  frame[header + 3] = REQUEST_MAGIC.charCodeAt(3);
  frame[header + 4] = PROTOCOL_VERSION;
  frame[header + 5] = COMMAND_TRANSCODE;
  frame[header + 6] = (options.trim ?? true) ? TRIM_FLAG : 0;
  frame[header + 7] = 0;
  frame[header + 8] = alphaThreshold;
  frame[header + 9] = outlineWidth;
  frame[header + 10] = color[0];
  frame[header + 11] = color[1];
  frame[header + 12] = color[2];
  frame[header + 13] = color[3];
  view.setUint32(header + 14, input.byteLength, true);
  frame.set(input, header + REQUEST_HEADER_BYTES);
  return frame;
}

function decodeResponse(frame: Uint8Array): NativeStickerResult {
  if (frame.byteLength < RESPONSE_HEADER_BYTES) {
    throw new WorkerProtocolError("Sticker worker response is shorter than its header.");
  }

  const view = new DataView(frame.buffer, frame.byteOffset, frame.byteLength);
  for (let index = 0; index < RESPONSE_MAGIC.length; index += 1) {
    if (view.getUint8(index) !== RESPONSE_MAGIC.charCodeAt(index)) {
      throw new WorkerProtocolError("Sticker worker response has an invalid magic value.");
    }
  }
  if (view.getUint8(4) !== PROTOCOL_VERSION) {
    throw new WorkerProtocolError("Sticker worker response uses an unsupported protocol version.");
  }

  const status = view.getUint8(5);
  if (status !== 0 && status !== 1 && status !== 2) {
    throw new WorkerProtocolError("Sticker worker response has an unknown status.");
  }
  const code = view.getUint8(6);
  if ((status === 0 && code !== 0) || (status !== 0 && code === 0)) {
    throw new WorkerProtocolError("Sticker worker response has an invalid error code.");
  }
  const animatedFlag = view.getUint8(7);
  if (animatedFlag !== 0 && animatedFlag !== 1) {
    throw new WorkerProtocolError("Sticker worker response has an invalid animation flag.");
  }

  const width = view.getUint16(8, true);
  const height = view.getUint16(10, true);
  const frameCount = view.getUint32(12, true);
  const durationMs = view.getUint32(16, true);
  const outputBytes = view.getUint32(20, true);
  const messageBytes = view.getUint16(24, true);
  const expectedLength = RESPONSE_HEADER_BYTES + outputBytes + messageBytes;
  if (expectedLength !== frame.byteLength) {
    throw new WorkerProtocolError("Sticker worker response length does not match its header.");
  }

  const outputStart = RESPONSE_HEADER_BYTES;
  const messageStart = outputStart + outputBytes;
  let message = "";
  try {
    message = textDecoder.decode(frame.subarray(messageStart));
  } catch {
    throw new WorkerProtocolError("Sticker worker response has a malformed UTF-8 message.");
  }

  if (status !== 0) {
    if (outputBytes !== 0) {
      throw new WorkerProtocolError("Sticker worker returned output bytes with an error status.");
    }
    throw new NativeStickerWorkerError(
      message || "Sticker worker could not process the source.",
      status,
      code,
    );
  }

  if (outputBytes === 0 || messageBytes !== 0) {
    throw new WorkerProtocolError("Sticker worker returned an invalid successful response.");
  }
  if (width !== 512 || height !== 512 || frameCount === 0) {
    throw new WorkerProtocolError("Sticker worker returned invalid sticker metadata.");
  }
  const isAnimated = animatedFlag === 1;
  if (!isAnimated && frameCount !== 1) {
    throw new WorkerProtocolError("Sticker worker marked a multi-frame result as static.");
  }
  if (isAnimated && frameCount < 2) {
    throw new WorkerProtocolError("Sticker worker marked a single-frame result as animated.");
  }
  if (isAnimated && durationMs > MAXIMUM_ANIMATION_DURATION_MS) {
    throw new WorkerProtocolError("Sticker worker returned an overlong animation.");
  }
  const outputLimit = isAnimated
    ? MAXIMUM_ANIMATED_STICKER_BYTES
    : MAXIMUM_STATIC_STICKER_BYTES;
  if (outputBytes > outputLimit) {
    throw new WorkerProtocolError("Sticker worker returned an oversized WebP.");
  }

  return {
    data: frame.slice(outputStart, messageStart),
    width,
    height,
    frameCount,
    durationMs,
    isAnimated,
  };
}

function withTimeout<T>(operation: Promise<T>, timeoutMs: number): Promise<T> {
  let timer: ReturnType<typeof setTimeout> | undefined;
  const deadline = new Promise<never>((_resolve, reject) => {
    timer = setTimeout(() => reject(new NativeStickerWorkerTimeoutError(timeoutMs)), timeoutMs);
  });
  return Promise.race([operation, deadline]).finally(() => {
    if (timer !== undefined) {
      clearTimeout(timer);
    }
  });
}

/**
 * A persistent, serial client for the native C++ sticker worker.
 *
 * Calls are deliberately serialized: one worker owns one stdin/stdout stream,
 * which keeps protocol framing deterministic and bounds the worker's peak
 * image memory. If a process dies, sends malformed data, or times out, it is
 * discarded; the next queued call starts a clean worker.
 */
export class NativeStickerWorker {
  readonly #workerPath: string;
  readonly #workerArgs: readonly string[];
  readonly #defaultTimeoutMs: number;
  #session: WorkerSession | undefined;
  #tail: Promise<void> = Promise.resolve();
  #closed = false;
  #closePromise: Promise<void> | undefined;

  constructor(options: NativeStickerWorkerOptions = {}) {
    const workerPath = options.workerPath ?? defaultWorkerPath();
    if (workerPath.length === 0) {
      throw new RangeError("workerPath must not be empty.");
    }
    if (options.workerArgs?.some((argument) => typeof argument !== "string")) {
      throw new TypeError("workerArgs must contain only strings.");
    }

    this.#workerPath = workerPath;
    this.#workerArgs = [...(options.workerArgs ?? [])];
    this.#defaultTimeoutMs = requireTimeout(
      options.timeoutMs ?? DEFAULT_TIMEOUT_MS,
      "timeoutMs",
    );
  }

  /**
   * Enqueues one input asset for conversion. Its source bytes are copied when
   * this method is called, before waiting behind any earlier request.
   */
  transcode(
    source: NativeStickerSource,
    options: NativeStickerTranscodeOptions = {},
  ): Promise<NativeStickerResult> {
    if (this.#closed) {
      return Promise.reject(new NativeStickerWorkerClosedError());
    }

    const request = encodeRequest(source, options);
    const timeoutMs = requireTimeout(options.timeoutMs ?? this.#defaultTimeoutMs, "timeoutMs");
    const work = this.#tail.then(() => this.#transcode(request, timeoutMs));
    this.#tail = work.then(
      () => undefined,
      () => undefined,
    );
    return work;
  }

  /**
   * Stops accepting new work, drains requests already accepted, then terminates
   * the native child process.
   */
  close(): Promise<void> {
    if (this.#closePromise !== undefined) {
      return this.#closePromise;
    }

    this.#closed = true;
    this.#closePromise = this.#tail.then(() => {
      const session = this.#session;
      if (session !== undefined) {
        this.#discardSession(session);
      }
    });
    return this.#closePromise;
  }

  #startSession(): WorkerSession {
    let process: WorkerProcess;
    try {
      process = Bun.spawn([this.#workerPath, ...this.#workerArgs], {
        stdin: "pipe",
        stdout: "pipe",
        stderr: "ignore",
      });
    } catch (error) {
      const reason = error instanceof Error ? `: ${error.message}` : "";
      throw new NativeStickerWorkerTransportError(
        `Unable to start sticker worker "${this.#workerPath}"${reason}`,
      );
    }

    const session: WorkerSession = {
      process,
      stdin: process.stdin,
      reader: new BufferedReader(process.stdout),
    };
    this.#session = session;
    return session;
  }

  #discardSession(session: WorkerSession): void {
    if (this.#session === session) {
      this.#session = undefined;
    }
    session.reader.cancel();
    try {
      if (!session.process.killed && session.process.exitCode === null) {
        session.process.kill();
      }
    } catch {
      // The process may already have exited between the checks and kill().
    }
    void session.process.exited.catch(() => undefined);
  }

  async #transcode(request: Uint8Array, timeoutMs: number): Promise<NativeStickerResult> {
    const session = this.#session ?? this.#startSession();
    try {
      return await withTimeout(this.#exchange(session, request), timeoutMs);
    } catch (error) {
      if (!(error instanceof NativeStickerWorkerError)) {
        this.#discardSession(session);
      }
      throw error;
    }
  }

  async #exchange(session: WorkerSession, request: Uint8Array): Promise<NativeStickerResult> {
    if (session.process.exitCode !== null) {
      throw new NativeStickerWorkerTransportError("Sticker worker exited before it accepted a request.");
    }

    await session.stdin.write(request);
    await session.stdin.flush();

    const lengthBytes = await session.reader.readExactly(FRAME_LENGTH_BYTES);
    const responseLength = readUint32(lengthBytes, 0);
    if (responseLength < RESPONSE_HEADER_BYTES || responseLength > MAXIMUM_FRAME_BYTES) {
      throw new WorkerProtocolError("Sticker worker response frame exceeds protocol limits.");
    }
    const response = await session.reader.readExactly(responseLength);
    return decodeResponse(response);
  }
}

/** Creates a persistent native C++ worker client. */
export function createNativeStickerWorker(
  options?: NativeStickerWorkerOptions,
): NativeStickerWorker {
  return new NativeStickerWorker(options);
}
