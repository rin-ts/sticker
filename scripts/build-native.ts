import { mkdir } from "node:fs/promises";
import { resolve } from "node:path";

const root = resolve(import.meta.dir, "..");
const buildDirectory = resolve(root, "build/native");

async function run(command: string[]): Promise<void> {
  const process = Bun.spawn(command, {
    cwd: root,
    stdout: "inherit",
    stderr: "inherit",
  });
  const exitCode = await process.exited;
  if (exitCode !== 0) {
    throw new Error(`Command failed (${exitCode}): ${command.join(" ")}`);
  }
}

await mkdir(buildDirectory, { recursive: true });
await run([
  "cmake",
  "-S",
  root,
  "-B",
  buildDirectory,
  "-DCMAKE_BUILD_TYPE=Release",
  "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
]);
await run(["cmake", "--build", buildDirectory, "--config", "Release"]);
