import { stat } from "node:fs/promises";
import { basename, resolve } from "node:path";

import {
  createNativeStickerWorker,
  type NativeStickerResult,
} from "../src/index.ts";

interface BenchmarkOptions {
  readonly iterations: number;
  readonly warmups: number;
  readonly json: boolean;
  readonly preferLossless: boolean;
  readonly inputs: readonly string[];
}

interface TimingSummary {
  readonly minimumMs: number;
  readonly medianMs: number;
  readonly p95Ms: number;
  readonly maximumMs: number;
  readonly meanMs: number;
}

interface BenchmarkResult {
  readonly fixture: string;
  readonly sourceBytes: number;
  readonly coldMs: number;
  readonly warm: TimingSummary;
  readonly outputBytes: number;
  readonly frameCount: number;
  readonly durationMs: number;
  readonly animated: boolean;
}

const defaultInputs = [
  "photo.png",
  "photo.jpg",
  "photo.webp",
  "graphic.webp",
  "animated.webp",
  "clip.mp4",
] as const;
const fixtureDirectory = resolve(import.meta.dir, "fixtures");

function usage(): string {
  return [
    "Usage: bun run benchmarks/benchmark.ts [options] [media-file ...]",
    "",
    "Options:",
    "  --iterations, -n <count>  Timed calls per input (default: 3).",
    "  --warmups <count>          Untimed persistent-worker calls (default: 1).",
    "  --prefer-lossless          Use the lossless static-output preference.",
    "  --json                     Write the report as JSON.",
    "  --help, -h                 Show this help.",
    "",
    "Without media files, the generated PNG, JPEG, WebP, animated WebP, and MP4",
    "fixtures are benchmarked. Run `bun run benchmark:fixtures` first.",
  ].join("\n");
}

function parsePositiveInteger(value: string, name: string): number {
  const parsed = Number(value);
  if (!Number.isSafeInteger(parsed) || parsed <= 0) {
    throw new Error(`${name} must be a positive integer.`);
  }
  return parsed;
}

function parseArguments(args: readonly string[]): BenchmarkOptions {
  let iterations = 3;
  let warmups = 1;
  let json = false;
  let preferLossless = false;
  const inputs: string[] = [];
  let readingOptions = true;

  for (let index = 0; index < args.length; index += 1) {
    const argument = args[index];
    if (argument === undefined) {
      continue;
    }
    if (readingOptions && argument === "--") {
      readingOptions = false;
      continue;
    }
    if (readingOptions && (argument === "--help" || argument === "-h")) {
      console.log(usage());
      process.exit(0);
    }
    if (readingOptions && (argument === "--iterations" || argument === "-n")) {
      const value = args[index + 1];
      if (value === undefined) {
        throw new Error(`${argument} requires a value.`);
      }
      iterations = parsePositiveInteger(value, "iterations");
      index += 1;
      continue;
    }
    if (readingOptions && argument === "--warmups") {
      const value = args[index + 1];
      if (value === undefined) {
        throw new Error("--warmups requires a value.");
      }
      warmups = parsePositiveInteger(value, "warmups");
      index += 1;
      continue;
    }
    if (readingOptions && argument === "--json") {
      json = true;
      continue;
    }
    if (readingOptions && argument === "--prefer-lossless") {
      preferLossless = true;
      continue;
    }
    if (readingOptions && argument.startsWith("-")) {
      throw new Error(`Unknown option: ${argument}`);
    }
    inputs.push(resolve(argument));
  }

  return { iterations, warmups, json, preferLossless, inputs };
}

function summarize(samples: readonly number[]): TimingSummary {
  if (samples.length === 0) {
    throw new Error("No benchmark samples were collected.");
  }
  const sorted = [...samples].sort((left, right) => left - right);
  const total = sorted.reduce((sum, sample) => sum + sample, 0);
  const percentileIndex = Math.min(
    sorted.length - 1,
    Math.max(0, Math.ceil(sorted.length * 0.95) - 1),
  );

  return {
    minimumMs: sorted[0]!,
    medianMs: sorted[Math.floor(sorted.length / 2)]!,
    p95Ms: sorted[percentileIndex]!,
    maximumMs: sorted[sorted.length - 1]!,
    meanMs: total / sorted.length,
  };
}

function assertMatchingMetadata(
  expected: NativeStickerResult,
  actual: NativeStickerResult,
): void {
  if (
    expected.width !== actual.width
    || expected.height !== actual.height
    || expected.frameCount !== actual.frameCount
    || expected.durationMs !== actual.durationMs
    || expected.isAnimated !== actual.isAnimated
  ) {
    throw new Error("The worker returned inconsistent metadata across benchmark calls.");
  }
}

async function timeCold(
  source: Uint8Array,
  preferLossless: boolean,
): Promise<{
  readonly elapsedMs: number;
  readonly result: NativeStickerResult;
}> {
  const startedAt = performance.now();
  const worker = createNativeStickerWorker();
  try {
    const result = await worker.transcode(source, { preferLossless });
    return { elapsedMs: performance.now() - startedAt, result };
  } finally {
    await worker.close();
  }
}

async function benchmarkInput(
  inputPath: string,
  options: BenchmarkOptions,
): Promise<BenchmarkResult> {
  const [sourceBuffer, sourceMetadata] = await Promise.all([
    Bun.file(inputPath).arrayBuffer(),
    stat(inputPath),
  ]);
  const source = new Uint8Array(sourceBuffer);
  const cold = await timeCold(source, options.preferLossless);
  const worker = createNativeStickerWorker();

  try {
    for (let warmup = 0; warmup < options.warmups; warmup += 1) {
      const result = await worker.transcode(source, {
        preferLossless: options.preferLossless,
      });
      assertMatchingMetadata(cold.result, result);
    }

    const samples: number[] = [];
    let result = cold.result;
    for (let iteration = 0; iteration < options.iterations; iteration += 1) {
      const startedAt = performance.now();
      const current = await worker.transcode(source, {
        preferLossless: options.preferLossless,
      });
      samples.push(performance.now() - startedAt);
      assertMatchingMetadata(cold.result, current);
      result = current;
    }

    return {
      fixture: basename(inputPath),
      sourceBytes: sourceMetadata.size,
      coldMs: cold.elapsedMs,
      warm: summarize(samples),
      outputBytes: result.data.byteLength,
      frameCount: result.frameCount,
      durationMs: result.durationMs,
      animated: result.isAnimated,
    };
  } finally {
    await worker.close();
  }
}

function formatBytes(bytes: number): string {
  if (bytes < 1024) {
    return `${bytes} B`;
  }
  return `${(bytes / 1024).toFixed(1)} KiB`;
}

function formatMilliseconds(value: number): string {
  return `${value.toFixed(1)} ms`;
}

function printHumanReport(
  results: readonly BenchmarkResult[],
  options: BenchmarkOptions,
): void {
  console.log(
    `Sticker benchmark: ${options.iterations} timed call(s), `
    + `${options.warmups} persistent-worker warmup(s) per input, `
    + `${options.preferLossless ? "lossless preference" : "fast default"}.`,
  );
  console.log(
    "Cold includes worker spawn and the first conversion. Warm measurements "
    + "reuse one worker and exclude fixture file I/O.",
  );
  console.log("");

  for (const result of results) {
    const kind = result.animated
      ? `${result.frameCount} frames / ${result.durationMs} ms`
      : "static";
    const throughput = 1_000 / result.warm.medianMs;
    console.log(`${result.fixture} (${formatBytes(result.sourceBytes)} → ${formatBytes(result.outputBytes)}, ${kind})`);
    console.log(
      `  cold ${formatMilliseconds(result.coldMs)} | warm median `
      + `${formatMilliseconds(result.warm.medianMs)} | p95 `
      + `${formatMilliseconds(result.warm.p95Ms)} | range `
      + `${formatMilliseconds(result.warm.minimumMs)}–`
      + `${formatMilliseconds(result.warm.maximumMs)} | `
      + `${throughput.toFixed(2)} stickers/s`,
    );
  }
}

async function main(): Promise<void> {
  const options = parseArguments(process.argv.slice(2));
  const inputs = options.inputs.length === 0
    ? defaultInputs.map((input) => resolve(fixtureDirectory, input))
    : options.inputs;

  const results: BenchmarkResult[] = [];
  for (const inputPath of inputs) {
    try {
      results.push(await benchmarkInput(inputPath, options));
    } catch (error) {
      const reason = error instanceof Error ? error.message : String(error);
      throw new Error(`Benchmark failed for "${inputPath}": ${reason}`);
    }
  }

  if (options.json) {
    console.log(JSON.stringify({ options, results }, null, 2));
    return;
  }
  printHumanReport(results, options);
}

await main();
