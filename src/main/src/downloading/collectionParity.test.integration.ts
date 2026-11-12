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

        // Core executor: the same archive and the same copy-all mapping,
        // driven by the prebuilt task through collection.run.
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
              task: {
                modId: "ParityMod",
                extract: { archive, destination: coreExtract, sevenZip },
                install: { temp: coreExtract, destination: coreStaging, instructions: [] },
                copyAll: true,
              },
            },
          ],
        });
        await client.collectionRun(started.jobId);
        const deadline = Date.now() + 10000;
        let executed = await client.collectionStatus(started.jobId);
        while (!executed.complete && Date.now() < deadline) {
          await new Promise((resolve) => setTimeout(resolve, 50));
          executed = await client.collectionStatus(started.jobId);
        }

        expect(executed.members.at(0)?.status).toBe("installed");
        expect(executed.members.at(0)?.result?.modId).toBe("ParityMod");
        expect(executed.members.at(0)?.result?.installationPath).toBe(coreStaging);
        expect(executed.complete).toBe(true);
        expect(treeHashes(coreStaging)).toEqual(treeHashes(tsStaging));
      } finally {
        await client.shutdown();
        rmSync(root, { recursive: true, force: true });
      }
    });

    it("runs core and TypeScript members from one mixed job", async () => {
      const root = mkdtempSync(path.join(tmpdir(), "collection-mixed-"));
      const coreSource = path.join(root, "core-source");
      const tsSource = path.join(root, "ts-source");
      const coreArchive = path.join(root, "core.zip");
      const tsArchive = path.join(root, "ts.zip");
      const coreExtract = path.join(root, "core-extract");
      const coreStaging = path.join(root, "core-staging");
      const tsExtract = path.join(root, "ts-extract");
      const tsStaging = path.join(root, "ts-staging");
      mkdirSync(coreSource, { recursive: true });
      mkdirSync(tsSource, { recursive: true });
      writeFileSync(path.join(coreSource, "core.txt"), "core-member");
      writeFileSync(path.join(tsSource, "ts.txt"), "typescript-member");
      execFileSync(sevenZip, ["a", "-tzip", coreArchive, "core.txt"], {
        cwd: coreSource,
        stdio: "ignore",
      });
      execFileSync(sevenZip, ["a", "-tzip", tsArchive, "ts.txt"], {
        cwd: tsSource,
        stdio: "ignore",
      });

      const client = new NativeCoreClient({ resolveCorePath: () => corePath });
      try {
        const started = await client.collectionStart({
          collectionId: "mixed-parity",
          gameId: "skyrimse",
          concurrency: 1,
          maxRetries: 0,
          members: [
            {
              ruleIndex: 0,
              ruleId: "requires_core",
              type: "requires",
              phase: 1,
              ignored: false,
              status: "pending",
              task: {
                modId: "CoreMod",
                extract: { archive: coreArchive, destination: coreExtract, sevenZip },
                install: { temp: coreExtract, destination: coreStaging, instructions: [] },
                copyAll: true,
              },
            },
            {
              ruleIndex: 1,
              ruleId: "requires_typescript",
              type: "requires",
              phase: 1,
              ignored: false,
              status: "pending",
            },
          ],
        });

        await client.collectionRun(started.jobId, true);
        const deadline = Date.now() + 10000;
        let status = await client.collectionStatus(started.jobId);
        while (!status.runComplete && Date.now() < deadline) {
          await new Promise((resolve) => setTimeout(resolve, 50));
          status = await client.collectionStatus(started.jobId);
        }

        expect(status.runComplete).toBe(true);
        expect(status.members[0]?.status).toBe("installed");
        expect(status.members[0]?.executor).toBe("core");
        expect(status.members[1]?.status).toBe("pending");
        expect(status.members[1]?.executor).toBe("typescript");

        const next = await client.collectionNext(started.jobId);
        expect(next.members.map((member) => member.ruleId)).toEqual(["requires_typescript"]);

        // TypeScript executor reference for the taskless member.
        execFileSync(sevenZip, ["x", "-y", `-o${tsExtract}`, tsArchive], { stdio: "ignore" });
        for (const rel of listRelativeFiles(tsExtract)) {
          const target = path.join(tsStaging, rel);
          mkdirSync(path.dirname(target), { recursive: true });
          copyFileSync(path.join(tsExtract, rel), target);
        }

        status = await client.collectionMember({
          jobId: started.jobId,
          ruleId: "requires_typescript",
          status: "installed",
          modId: "TsMod",
        });

        expect(status.complete).toBe(true);
        expect(treeHashes(coreStaging)).toEqual(treeHashes(coreSource));
        expect(treeHashes(tsStaging)).toEqual(treeHashes(tsSource));
      } finally {
        await client.shutdown();
        rmSync(root, { recursive: true, force: true });
      }
    });
  },
);
