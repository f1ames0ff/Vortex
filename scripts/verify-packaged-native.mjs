#!/usr/bin/env node
import { existsSync, statSync } from "node:fs";
import { resolve } from "node:path";

const root = resolve(import.meta.dirname, "..");
const executable = process.platform === "win32" ? "vortex-core.exe" : "vortex-core";
const candidates = [
  resolve(root, "dist", "linux-unpacked", "resources", "native", executable),
  resolve(root, "dist", "win-unpacked", "resources", "native", executable),
];
const found = candidates.find((candidate) => existsSync(candidate));

if (found === undefined) {
  console.error(
    `packaged native core not found. Expected one of:\n${candidates.map((c) => `  ${c}`).join("\n")}`,
  );
  process.exit(1);
}

if (process.platform !== "win32") {
  const mode = statSync(found).mode;
  if ((mode & 0o111) === 0) {
    console.error(`packaged native core is not executable: ${found}`);
    process.exit(1);
  }
}

console.log(`packaged native core: ${found}`);
