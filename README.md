# @rin-ts/sticker

`@rin-ts/sticker` converts media into WhatsApp-compatible WebP stickers for
Rin. It keeps codec and pixel work in a persistent native C++ worker, while
the Bun/TypeScript API manages requests and returns the finished WebP bytes.

The design is intended for fast, predictable server-side processing: requests
are serialized per worker, source and output sizes are bounded, and a worker
that times out or returns an invalid response is replaced before later work is
accepted.

## Supported media

- PNG, JPEG, static WebP, animated WebP, and videos decoded by FFmpeg
  (including MP4, MOV, WebM, MKV, and AVI)
- Transparency, alpha trimming, proportional 512 × 512 sizing, and optional
  alpha-aware outlines
- Static and animated WhatsApp-compatible WebP output

GIF, APNG, and Lottie/Telegram `.tgs` remain rejected. Videos are decoded
directly from the supplied bytes; audio streams are ignored.

## Install

Published packages include a prebuilt worker for Linux x64 and Linux ARM64.
They are available from the public npm registry, so no registry configuration
or authentication is needed.

```bash
bun add @rin-ts/sticker
```

The optional platform worker is selected automatically. Set `workerPath` only
when you supply a custom worker binary.

## Publishing

Maintain a matching `v<version>` Git tag and package versions, then a
maintainer runs the **Publish packages** workflow from GitHub Actions with that
tag and the intended npm distribution tag. The workflow publishes the Linux
packages before the main package and safely skips versions already on npm.
GitHub requires repository write access to start this manual workflow.

After the initial release, configure an npm trusted publisher for each package:
`@rin-ts/sticker`, `@rin-ts/sticker-linux-x64-gnu`, and
`@rin-ts/sticker-linux-arm64-gnu`. Use GitHub organization `rin-ts`,
repository `sticker`, workflow filename `release.yml`, and allow direct
`npm publish`. This uses GitHub OIDC credentials rather than a stored npm
token. npm requires a package to exist before it can be configured as a
trusted publisher, so the initial release needs a temporary granular npm token
in the `NPM_TOKEN` GitHub Actions secret. Revoke that token after the first
release, then configure the trusted publishers shortly before the next release.

## Build from source

The native build requires Bun, CMake 3.20+, a C++20 compiler, `pkg-config`,
and development packages for libpng, libjpeg, libwebp, libwebpmux,
libwebpdemux, libavformat, libavcodec, libavutil, and libswscale. On Void,
install these FFmpeg headers with `sudo xbps-install -S ffmpeg6-devel`.

```bash
bun install
bun run build:native
```

This produces `build/native/bin/sticker_worker`.

## Usage

Create one worker when the application starts, then reuse it for every sticker
request. Supply already-bounded media bytes; the worker accepts at most 16 MiB
per source asset.

```ts
import { createNativeStickerWorker } from "@rin-ts/sticker";

const stickerWorker = createNativeStickerWorker({
  // workerPath: "/opt/rin/sticker_worker",
  timeoutMs: 15_000,
});

const source = await Bun.file("./photo.png").arrayBuffer();

const sticker = await stickerWorker.transcode(source, {
  trim: true,
  alphaThreshold: 4,
  outline: {
    width: 8,
    color: { red: 255, green: 255, blue: 255, alpha: 255 },
  },
});

// For a Zapo/Rin handler:
await context.reply({ type: "sticker", media: sticker.data });

// During application shutdown:
await stickerWorker.close();
```

`transcode()` returns `data` (the WebP bytes), `width`, `height`,
`frameCount`, `durationMs`, and `isAnimated`.

## Output and operational limits

Successful output is validated as 512 × 512 WebP. Static stickers are capped
at 100 KiB; animated stickers are capped at 500 KiB, must contain more than
one frame, use frame delays of at least 8 ms, and last no more than 10 seconds.
These are conservative WhatsApp sticker-pack compatibility targets.

The worker is an isolated stdin/stdout subprocess, not a file-path CLI or an
HTTP server. It accepts bytes and rendering options only. For production,
continue to enforce download limits in the application, use finite worker
pools, and run workers with appropriate OS-level CPU and memory limits.

For development:

```bash
bun run test:native
```

## License

GPL-3.0-only. See [LICENSE](LICENSE).
