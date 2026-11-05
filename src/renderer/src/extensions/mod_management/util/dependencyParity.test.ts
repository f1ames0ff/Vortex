import { existsSync } from "node:fs";
import path from "node:path";
import { pathToFileURL } from "node:url";

import { describe, expect, it } from "vitest";

import { reconstructModStatus, modRuleId } from "@/util/collectionInstallSession";

import type { IMod, IModRule } from "../types/IMod";
import { findDownloadByRef } from "./dependencies";
import { findModByRef } from "./findModByRef";
import { rulePhase } from "./rulePhase";
import { ruleInstallSpec } from "./testModReference";

const corePath =
  process.env.VORTEX_CORE_PATH ??
  path.resolve(process.cwd(), "../../native/build/release/vortex-core");

const gameId = "skyrimse";

const mods: Record<string, IMod> = {
  "mod-tag": {
    id: "mod-tag",
    state: "installed",
    type: "",
    attributes: { referenceTag: "tag-1", game: [gameId] },
  },
  "mod-md5": {
    id: "mod-md5",
    state: "installed",
    type: "",
    attributes: { fileMD5: "md5-1", game: [gameId] },
  },
  "mod-logical": {
    id: "mod-logical",
    state: "installed",
    type: "",
    attributes: { logicalFileName: "Cool Mod", version: "1.2.3", game: [gameId] },
  },
  "mod-expression": {
    id: "mod-expression",
    state: "installed",
    type: "",
    attributes: { fileName: "Cool Mod-123-1-2-3.7z", version: "3.0.0", game: [gameId] },
  },
  "mod-repo": {
    id: "mod-repo",
    state: "installed",
    type: "",
    attributes: { source: "nexus", modId: 123, fileId: 456, game: [gameId] },
  },
  "mod-spec-a": {
    id: "mod-spec-a",
    state: "installed",
    type: "",
    attributes: {
      logicalFileName: "Variant Mod",
      installerChoices: { a: "1" },
      game: [gameId],
    },
  },
  "mod-spec-b": {
    id: "mod-spec-b",
    state: "installed",
    type: "",
    attributes: {
      logicalFileName: "Variant Mod",
      installerChoices: { a: "2" },
      game: [gameId],
    },
  },
} as unknown as Record<string, IMod>;

const downloads = {
  "dl-finished": {
    id: "dl-finished",
    state: "finished",
    fileMD5: "md5-download",
    localPath: "Downloaded Mod-1-0.7z",
    game: [gameId],
    fileTime: 100,
    modInfo: {
      name: "Downloaded Mod",
      version: "1.0.0",
      referenceTag: "tag-dl",
      nexus: { ids: { modId: 999, fileId: 888 } },
    },
  },
  "dl-failed": {
    id: "dl-failed",
    state: "failed",
    fileMD5: "md5-failed",
    localPath: "Failed Mod-1-0.7z",
    game: [gameId],
    fileTime: 50,
    modInfo: { name: "Failed Mod", version: "1.0.0", referenceTag: "tag-failed" },
  },
} as never;

const rules: IModRule[] = [
  { type: "requires", reference: { tag: "tag-1", gameId }, phase: 1 },
  { type: "requires", reference: { fileMD5: "md5-1", gameId }, phase: 1 },
  { type: "requires", reference: { logicalFileName: "Cool Mod", gameId }, phase: 2 },
  { type: "requires", reference: { fileExpression: "Cool Mod*", gameId }, phase: 2 },
  {
    type: "requires",
    reference: { repo: { repository: "nexus", modId: "123", fileId: "456" }, gameId },
    phase: 3,
  },
  {
    type: "requires",
    reference: { logicalFileName: "Variant Mod", gameId },
    installerChoices: { a: "2" },
    phase: 3,
  },
  { type: "requires", reference: { tag: "tag-dl", gameId }, phase: 3 },
  { type: "requires", reference: { tag: "missing", gameId }, phase: 4 },
  { type: "recommends", reference: { tag: "tag-1", gameId } },
  { type: "requires", reference: { tag: "tag-1", gameId }, ignored: true },
] as unknown as IModRule[];

function expectedMembers() {
  return rules.map((rule, ruleIndex) => {
    const mod = findModByRef(rule.reference, mods, undefined, ruleInstallSpec(rule));
    const downloadId = findDownloadByRef(rule.reference, downloads);
    const download = downloadId !== undefined ? downloads[downloadId] : undefined;
    const entry: Record<string, unknown> = {
      ruleIndex,
      ruleId: modRuleId(rule),
      type: rule.type,
      phase: rulePhase(rule),
      ignored: rule.ignored === true,
      status: reconstructModStatus(rule, mod, download),
    };
    if (mod !== undefined) {
      entry.modId = mod.id;
    }
    if (downloadId !== undefined) {
      entry.downloadId = downloadId;
    }
    return entry;
  });
}

describe("dependency resolution parity", () => {
  it.skipIf(!existsSync(corePath))(
    "matches the TypeScript resolver and collection plan",
    async () => {
      const clientModule = await import(
        pathToFileURL(path.resolve(process.cwd(), "../../native/clients/vortex-core-client.mjs"))
          .href
      );
      const client = new clientModule.VortexCoreClient(corePath);
      try {
        const resolved = await client.request("deps.resolve", {
          rules,
          mods: Object.values(mods),
          downloads: Object.values(downloads),
        });
        const plan = await client.request("collection.plan", {
          rules,
          mods: Object.values(mods),
          downloads: Object.values(downloads),
        });

        const members = expectedMembers();
        expect(resolved.dependencies).toEqual(members);
        expect(plan.members).toEqual(members);
        expect(plan.phases).toEqual([0, 1, 2, 3, 4, 666]);
        expect(plan.totalRequired).toBe(9);
        expect(plan.totalOptional).toBe(1);
      } finally {
        await client.close();
      }
    },
  );
});
