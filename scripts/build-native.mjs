#!/usr/bin/env node
import { spawnSync } from "node:child_process";
import { resolve } from "node:path";

const nativeDir = resolve(import.meta.dirname, "..", "native");

function run(args) {
  const result = spawnSync("cmake", args, {
    cwd: nativeDir,
    stdio: "inherit",
    shell: process.platform === "win32",
  });
  if (result.error) {
    console.error(`failed to run cmake: ${result.error.message}`);
    process.exit(1);
  }
  if (result.status !== 0) {
    process.exit(result.status ?? 1);
  }
}

run(["--preset", "release"]);
run(["--build", "--preset", "release", "--parallel"]);
