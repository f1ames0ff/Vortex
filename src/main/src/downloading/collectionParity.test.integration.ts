import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import {
  copyFileSync,
  existsSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  readdirSync,
  rmSync,
  statSync,
  writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import * as path from "node:path";

import { describe, expect, it } from "vitest";

import { NativeCoreClient } from "../nativeDeploy/service";

const corePath =
  process.env.VORTEX_CORE_PATH ??
  path.resolve(process.cwd(), "../../native/build/release/vortex-core");
const sevenZip = process.env.VORTEX_7Z_PATH ?? "/usr/bin/7z";

function listRelativeFiles(root: string, prefix = ""): string[] {
  const result: string[] = [];
  for (const name of readdirSync(path.join(root, prefix))) {
    const rel = prefix === "" ? name : path.join(prefix, name);
    if (statSync(path.join(root, rel)).isDirectory()) {
      result.push(...listRelativeFiles(root, rel));
    } else {
      result.push(rel.replace(/\\/g, "/"));
    }
  }
  return result.sort();
}

function treeHashes(root: string): Record<string, string> {
  const result: Record<string, string> = {};
  for (const rel of listRelativeFiles(root)) {
    result[rel] = createHash("sha256")
      .update(readFileSync(path.join(root, rel)))
      .digest("hex");
  }
  return result;
}

describe.skipIf(!existsSync(corePath) || !existsSync(sevenZip))(
  "collection executor parity",
  () => {
    it("installs the same simple archive as the TypeScript basic installer", async () => {
      const root = mkdtempSync(path.join(tmpdir(), "collection-parity-"));
      const source = path.join(root, "source");
      const archive = path.join(root, "fixture.zip");
      const tsExtract = path.join(root, "ts-extract");
      const tsStaging = path.join(root, "ts-staging");
      const coreExtract = path.join(root, "core-extract");
      const coreStaging = path.join(root, "core-staging");
      mkdirSync(path.join(source, "textures"), { recursive: true });
      mkdirSync(path.join(source, "meshes"), { recursive: true });
      writeFileSync(path.join(source, "textures", "a.dds"), "texture-a");
      writeFileSync(path.join(source, "meshes", "b.nif"), "mesh-b");
      writeFileSync(path.join(source, "readme.txt"), "readme");
      execFileSync(sevenZip, ["a", "-tzip", archive, "textures", "meshes", "readme.txt"], {
        cwd: source,
        stdio: "ignore",
      });

      const client = new NativeCoreClient({ resolveCorePath: () => corePath });
      try {
        // TypeScript reference: the generic installer maps every regular file
        // to a copy instruction with the same relative path.
        execFileSync(sevenZip, ["x", "-y", `-o${tsExtract}`, archive], { stdio: "ignore" });
        const files = listRelativeFiles(tsExtract);
        // Mirrors `basicInstaller.install`: every regular file becomes a copy
        // instruction with the same relative path, which is the TS executor used
        // for plain archives.
        const tsInstructions = files.map((file) => ({
          type: "copy",
          source: file,
          destination: file,
        }));
        for (const instruction of tsInstructions) {
          const target = path.join(tsStaging, instruction.destination);
          mkdirSync(path.dirname(target), { recursive: true });
          copyFileSync(path.join(tsExtract, instruction.source), target);
        }

        // Core executor: same archive and the same copy-all mapping.
        const started = await client.collectionStart({
          collectionId: "parity",
          gameId: "skyrimse",
          concurrency: 1,
          maxRetries: 0,
          members: [
            {
              ruleIndex: 0,
              ruleId: "requires_parity",
              type: "requires",
              phase: 1,
              ignored: false,
              status: "pending",
            },
          ],
        });
        await client.collectionTask({
          jobId: started.jobId,
          ruleId: "requires_parity",
          task: {
            modId: "ParityMod",
            extract: { archive, destination: coreExtract, sevenZip },
            install: {
              temp: coreExtract,
              destination: coreStaging,
              instructions: files.map((file) => ({
                type: "copy",
                source: file,
                destination: file,
              })),
            },
          },
        });
        const executed = await client.collectionExecute({
          jobId: started.jobId,
          ruleId: "requires_parity",
        });

        expect(executed.members.at(0)?.status).toBe("installed");
        expect(executed.complete).toBe(true);
        expect(treeHashes(coreStaging)).toEqual(treeHashes(tsStaging));
      } finally {
        await client.shutdown();
        rmSync(root, { recursive: true, force: true });
      }
    });
  },
);
