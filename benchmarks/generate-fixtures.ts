import { mkdir } from "node:fs/promises";
import { resolve } from "node:path";

const root = resolve(import.meta.dir, "..");
const outputDirectory = resolve(import.meta.dir, "fixtures");
const fixtureBuilder = resolve(
  root,
  "build",
  "native",
  "bin",
  process.platform === "win32"
    ? "sticker_benchmark_fixtures.exe"
    : "sticker_benchmark_fixtures",
);

await mkdir(outputDirectory, { recursive: true });
const child = Bun.spawn([fixtureBuilder, outputDirectory], {
  cwd: root,
  stdout: "inherit",
  stderr: "inherit",
});
const exitCode = await child.exited;
if (exitCode !== 0) {
  throw new Error(`Benchmark fixture generation failed with exit code ${exitCode}.`);
}
