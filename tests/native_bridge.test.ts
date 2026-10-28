import { expect, test } from "bun:test";
import { resolve } from "node:path";

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

test("native worker frames sequential WebP transcodes with fast and lossless preferences", async () => {
  const worker = createNativeStickerWorker();
  try {
    const [plain, outlined, lossless] = await Promise.all([
      worker.transcode(onePixelPng),
      worker.transcode(onePixelPng, {
        outline: {
          width: 2,
          color: { red: 255, green: 0, blue: 0 },
        },
      }),
      worker.transcode(onePixelPng, { preferLossless: true }),
    ]);

    for (const result of [plain, outlined, lossless]) {
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
