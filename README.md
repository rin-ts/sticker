# @rin-ts/sticker

[![npm](https://img.shields.io/npm/v/%40rin-ts%2Fsticker?logo=npm)](https://www.npmjs.com/package/@rin-ts/sticker)
[![CI](https://img.shields.io/github/actions/workflow/status/rin-ts/sticker/ci.yml?branch=main&label=CI)](https://github.com/rin-ts/sticker/actions/workflows/ci.yml)
[![License](https://img.shields.io/npm/l/%40rin-ts%2Fsticker)](LICENSE)

Native WhatsApp WebP sticker conversion for Bun and TypeScript. Media decoding,
pixel processing, and WebP encoding run in a persistent C++ worker; the
TypeScript API sends bytes and returns validated sticker bytes.

## Highlights

- PNG, JPEG, static WebP, animated WebP, and FFmpeg-decoded video input
- 512 × 512 static or animated WebP output validated against conservative
  WhatsApp sticker limits
- Alpha trimming, proportional sizing, and alpha-aware outlines
- Fast high-quality lossy output by default, with an opt-in lossless static attempt
- Persistent workers, per-request deadlines, bounded input, and automatic
  worker replacement after a transport failure or timeout

## Install

```bash
bun add @rin-ts/sticker
```

Prebuilt workers are selected automatically on Linux x64 and Linux ARM64.
Pass `workerPath` only when using a custom native worker executable.
Keep a custom worker on the same package version as its TypeScript wrapper. A
newer wrapper can send ordinary requests to an older worker, but that worker
keeps its older encoding default; `preferLossless: true` is rejected by an
older worker.

## Quick start

Create a worker once during application startup and reuse it for requests.

```ts
import { createNativeStickerWorker } from "@rin-ts/sticker";

const worker = createNativeStickerWorker({ timeoutMs: 15_000 });
const source = await Bun.file("./photo.png").arrayBuffer();

const sticker = await worker.transcode(source, {
  trim: true,
  alphaThreshold: 4,
  outline: {
    width: 8,
    color: { red: 255, green: 255, blue: 255, alpha: 255 },
  },
});

await context.reply({ type: "sticker", media: sticker.data });
await worker.close();
```

`transcode()` resolves to:

| Field | Description |
| --- | --- |
| `data` | Validated WebP sticker bytes |
| `width`, `height` | Always `512` on success |
| `frameCount`, `durationMs` | Animation metadata; static stickers have one frame and zero duration |
| `isAnimated` | Whether the output contains multiple frames |

## Input support

| Input | Result |
| --- | --- |
| PNG, JPEG, static WebP | Static WebP sticker |
| Animated WebP | Animated WebP sticker |
| FFmpeg-supported video such as MP4, MOV, WebM, MKV, and AVI | Animated WebP sticker; audio is ignored |
| GIF, APNG, Lottie/Telegram `.tgs` | Rejected |

The worker receives bytes only; it is not a file-path CLI or an HTTP service.

## API

```ts
const worker = createNativeStickerWorker({
  workerPath: "/opt/rin/sticker_worker", // optional
  workerArgs: [],                        // optional
  timeoutMs: 30_000,                     // optional
});
```

`worker.transcode(source, options)` accepts an `ArrayBuffer` or any typed-array
view. `options` are:

| Option | Default | Description |
| --- | --- | --- |
| `trim` | `true` | Remove fully transparent outer rows and columns |
| `alphaThreshold` | `0` | Alpha value treated as transparent while trimming |
| `outline.width` | `0` | Outline radius, from 0 through 64 pixels |
| `outline.color` | opaque white | `{ red, green, blue, alpha? }`, each channel 0 through 255 |
| `preferLossless` | `false` | Attempt lossless static WebP before lossy fallback; it still falls back when output exceeds 100 KiB |
| `timeoutMs` | worker default (30,000 ms) | Deadline for writing, conversion, and reading the response |

Calls to one worker are intentionally serialized. Use a finite pool of workers
when parallel conversion is required. `close()` stops new work, drains already
accepted work, and terminates the child process.

## Limits and production behavior

- Source assets are limited to 16 MiB.
- Output is always 512 × 512 WebP.
- Static output is limited to 100 KiB.
- Animated output is limited to 500 KiB, 10 seconds, and requires multiple
  frames with delays of at least 8 ms.

The worker is discarded after a timeout, malformed protocol response, or
transport failure; the next queued call starts a clean process. Still enforce
download limits before calling this package, set an appropriate worker-pool
size, and apply OS-level CPU and memory limits in production.

## Benchmarking

Run the repeatable end-to-end suite:

```bash
bun run benchmark
```

It builds the native worker, generates deterministic full-size media fixtures
locally, then reports cold latency (worker spawn plus first conversion) and
warm latency (a reused worker). File I/O is excluded from warm measurements.
The default fixtures cover a 1280 × 854 image as PNG, JPEG, and static WebP; a
1024 × 1024 transparent graphic; a three-frame animated WebP; and a
three-frame 640 × 360 MP4. They are real encoded media files, not 1 × 1 test
pixels, and no fixture is downloaded.

Use your own media to measure the inputs your application handles:

```bash
bun run build:native
# Compare the fast default and lossless preference using the same source:
bun run benchmarks/benchmark.ts --iterations 10 ./artwork.webp
bun run benchmarks/benchmark.ts --iterations 10 --prefer-lossless ./artwork.webp
```

The report includes source and output size, cold latency, warm median and p95,
and warm throughput. Performance varies with codec library versions, CPU,
source dimensions, frame count, transparency, and how often an output must be
re-encoded to meet its byte limit; compare results on the deployment hardware.
Static output uses the high-quality lossy path by default for predictable
latency. Set `preferLossless: true` when lower latency is not important and a
lossless attempt is worthwhile; output still falls back to lossy above 100 KiB.

## Development

### Build from source

The native build requires Bun, CMake 3.20+, a C++20 compiler, `pkg-config`,
and development packages for libpng, libjpeg, libwebp, libwebpmux,
libwebpdemux, libavformat, libavcodec, libavutil, and libswscale. On Void,
install FFmpeg headers with:

```bash
sudo xbps-install -S ffmpeg6-devel
```

Then build:

```bash
bun install
bun run build:native
```

The worker is written to `build/native/bin/sticker_worker`.

### Test

```bash
bun run test
```

## Publishing

Maintain matching package versions and a `v<version>` tag, then run the
repository’s **Publish packages** GitHub Actions workflow with the intended npm
distribution tag. It publishes the platform packages before the main package
and skips package versions already present on npm. Configure npm trusted
publishers for the main and Linux platform packages after the initial release;
the workflow file is [`release.yml`](.github/workflows/release.yml).

## License

GPL-3.0-only. See [LICENSE](LICENSE).
