import { expect, test } from "bun:test";
import { resolve } from "node:path";
import { deflateSync } from "node:zlib";

import {
  createNativeStickerWorker,
  NativeStickerWorkerError,
} from "../src/index.ts";

const workerPath = resolve(import.meta.dir, "../build/native/bin/sticker_worker");
const onePixelPng = new Uint8Array(
  Buffer.from(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=",
    "base64",
  ),
);

function writeUint32BigEndian(
  target: Uint8Array,
  offset: number,
  value: number,
): void {
  target[offset] = value >>> 24;
  target[offset + 1] = (value >>> 16) & 0xff;
  target[offset + 2] = (value >>> 8) & 0xff;
  target[offset + 3] = value & 0xff;
}

function crc32(bytes: Uint8Array): number {
  let value = 0xffff_ffff;
  for (const byte of bytes) {
    value ^= byte;
    for (let bit = 0; bit < 8; bit += 1) {
      value = (value >>> 1) ^ ((value & 1) === 0 ? 0 : 0xedb8_8320);
    }
  }
  return (value ^ 0xffff_ffff) >>> 0;
}

function concatBytes(parts: readonly Uint8Array[]): Uint8Array {
  const length = parts.reduce((total, part) => total + part.byteLength, 0);
  const result = new Uint8Array(length);
  let offset = 0;
  for (const part of parts) {
    result.set(part, offset);
    offset += part.byteLength;
  }
  return result;
}

function pngChunk(name: string, data: Uint8Array): Uint8Array {
  const encodedName = new TextEncoder().encode(name);
  const checksum = crc32(concatBytes([encodedName, data]));
  const chunk = new Uint8Array(12 + data.byteLength);
  writeUint32BigEndian(chunk, 0, data.byteLength);
  chunk.set(encodedName, 4);
  chunk.set(data, 8);
  writeUint32BigEndian(chunk, 8 + data.byteLength, checksum);
  return chunk;
}

function exifBoundaryPng(): Uint8Array {
  const width = 512;
  const height = 512;
  const detail = 32;
  const scanlines = new Uint8Array(height * (1 + width * 4));
  let state = 0x1234_5678;
  let offset = 0;
  for (let y = 0; y < height; y += 1) {
    scanlines[offset] = 0;
    offset += 1;
    for (let x = 0; x < width; x += 1) {
      state = (Math.imul(state, 1_664_525) + 1_013_904_223) >>> 0;
      const base = (x * 3 + y * 5) & 0xff;
      scanlines[offset] = (base + (((state >>> 24) * detail) >>> 8)) & 0xff;
      scanlines[offset + 1] =
        (base * 3 + ((((state >>> 16) & 0xff) * detail) >>> 8)) & 0xff;
      scanlines[offset + 2] =
        (base * 7 + ((((state >>> 8) & 0xff) * detail) >>> 8)) & 0xff;
      scanlines[offset + 3] = 0xff;
      offset += 4;
    }
  }

  const header = new Uint8Array(13);
  writeUint32BigEndian(header, 0, width);
  writeUint32BigEndian(header, 4, height);
  header.set([8, 6, 0, 0, 0], 8);
  return concatBytes([
    Uint8Array.from([137, 80, 78, 71, 13, 10, 26, 10]),
    pngChunk("IHDR", header),
    pngChunk("IDAT", new Uint8Array(deflateSync(scanlines))),
    pngChunk("IEND", new Uint8Array()),
  ]);
}

function findRiffChunk(webp: Uint8Array, fourCc: string): Uint8Array | undefined {
  for (let offset = 12; offset + 8 <= webp.byteLength;) {
    const chunkName = new TextDecoder().decode(webp.subarray(offset, offset + 4));
    const chunkSize = new DataView(
      webp.buffer,
      webp.byteOffset + offset + 4,
      4,
    ).getUint32(0, true);
    const dataStart = offset + 8;
    const dataEnd = dataStart + chunkSize;
    if (dataEnd > webp.byteLength) {
      return undefined;
    }
    if (chunkName === fourCc) {
      return webp.slice(dataStart, dataEnd);
    }
    offset = dataEnd + (chunkSize & 1);
  }
  return undefined;
}

test("native worker frames sequential WebP transcodes with fast and lossless preferences", async () => {
  const worker = createNativeStickerWorker();
  try {
    const [plain, outlined, lossless, withExif] = await Promise.all([
      worker.transcode(onePixelPng),
      worker.transcode(onePixelPng, {
        outline: {
          width: 2,
          color: { red: 255, green: 0, blue: 0 },
        },
      }),
      worker.transcode(onePixelPng, { preferLossless: true }),
      worker.transcode(onePixelPng, {
        exif: {
          packId: "bridge-pack",
          packName: "Bridge Pack",
          emojis: ["🤖"],
        },
      }),
    ]);

    for (const result of [plain, outlined, lossless, withExif]) {
      expect(result.width).toBe(512);
      expect(result.height).toBe(512);
      expect(result.frameCount).toBe(1);
      expect(result.durationMs).toBe(0);
      expect(result.isAnimated).toBeFalse();
      expect(result.data.byteLength).toBeGreaterThan(0);
      expect(result.data.byteLength).toBeLessThanOrEqual(100 * 1024);
      expect(new TextDecoder().decode(result.data.subarray(0, 4))).toBe("RIFF");
      expect(new TextDecoder().decode(result.data.subarray(8, 12))).toBe("WEBP");
    }
    const exif = findRiffChunk(withExif.data, "EXIF");
    expect(exif).toBeDefined();
    expect(new TextDecoder().decode(exif!.subarray(22))).toBe(
      '{"sticker-pack-id":"bridge-pack","sticker-pack-name":"Bridge Pack","emojis":["🤖"]}',
    );
  } finally {
    await worker.close();
  }
});

test("native worker remains usable after a processing error", async () => {
  const worker = createNativeStickerWorker({ workerPath });
  try {
    try {
      await worker.transcode(new Uint8Array([0, 1, 2]));
      throw new Error("Unsupported media unexpectedly transcoded.");
    } catch (error) {
      expect(error).toBeInstanceOf(NativeStickerWorkerError);
    }

    const result = await worker.transcode(onePixelPng);
    expect(result.data.byteLength).toBeGreaterThan(0);
    expect(result.isAnimated).toBeFalse();
  } finally {
    await worker.close();
  }
});

test("native worker accepts bounded EXIF beyond the static image budget", async () => {
  const worker = createNativeStickerWorker({ workerPath });
  try {
    const result = await worker.transcode(exifBoundaryPng(), {
      trim: false,
      exif: {
        packId: "bridge-pack",
        packName: "x".repeat(3900),
      },
    });
    expect(result.data.byteLength).toBeGreaterThan(100 * 1024);
    expect(result.data.byteLength).toBeLessThanOrEqual((100 * 1024) + 4096 + 128);
    expect(findRiffChunk(result.data, "EXIF")).toBeDefined();
  } finally {
    await worker.close();
  }
});
