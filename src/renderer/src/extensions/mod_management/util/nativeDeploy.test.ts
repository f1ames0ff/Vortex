import path from "node:path";

import type { NativeDeployRequest } from "@vortex/shared/ipc";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

vi.mock("../../../logging", () => ({ log: vi.fn() }));

vi.mock("../actions/session", () => ({
  addDeploymentFailures: vi.fn((gameId: string, failures: unknown[]) => ({
    type: "ADD_DEPLOYMENT_FAILURES",
    payload: { gameId, failures },
  })),
}));

vi.mock("../modMerging", () => ({
  mergedSourceName: (typeId: string) => `__merged.${typeId}`,
}));

vi.mock("./activationStore", () => ({
  manifestPaths: (deployPath: string, stagingPath: string, modType: string) => {
    const typeTag = modType !== "" ? `${modType}.` : "";
    return {
      manifestPath: path.join(deployPath, `vortex.deployment.${typeTag}json`),
      backupPath: path.join(stagingPath, `vortex.deployment.${typeTag}json`),
      binaryBackupPath: path.join(stagingPath, `vortex.deployment.${typeTag}msgpack`),
    };
  },
}));

vi.mock("../../../util/fs", () => ({
  statAsync: vi.fn(() => Promise.resolve({})),
}));

import {
  applyInstallNatively,
  cancelCollectionJobNatively,
  canDeployNatively,
  collectionStatusNatively,
  deployWithNativeCore,
  downloadFileNatively,
  extractArchiveNatively,
  hashFilesNatively,
  inspectStagingNatively,
  nextCollectionMembersNatively,
  pauseCollectionJobNatively,
  planCollectionNatively,
  purgeWithNativeCore,
  readPluginOrderNatively,
  resetNativeDeployStatus,
  resolveDependenciesNatively,
  scanPluginsNatively,
  startCollectionNatively,
  updateCollectionMemberNatively,
  writePluginOrderNatively,
} from "./nativeDeploy";

interface FakeNativeDeploy {
  status: ReturnType<typeof vi.fn>;
  plan: ReturnType<typeof vi.fn>;
  deploy: ReturnType<typeof vi.fn>;
  purge: ReturnType<typeof vi.fn>;
  stagingStatus: ReturnType<typeof vi.fn>;
  installApply: ReturnType<typeof vi.fn>;
  archiveList: ReturnType<typeof vi.fn>;
  archiveExtract: ReturnType<typeof vi.fn>;
  hashFiles: ReturnType<typeof vi.fn>;
  depsResolve: ReturnType<typeof vi.fn>;
  collectionPlan: ReturnType<typeof vi.fn>;
  pluginsScan: ReturnType<typeof vi.fn>;
  pluginsOrderRead: ReturnType<typeof vi.fn>;
  pluginsOrderWrite: ReturnType<typeof vi.fn>;
  collectionStart: ReturnType<typeof vi.fn>;
  collectionStatus: ReturnType<typeof vi.fn>;
  collectionNext: ReturnType<typeof vi.fn>;
  collectionMember: ReturnType<typeof vi.fn>;
  collectionPause: ReturnType<typeof vi.fn>;
  collectionCancel: ReturnType<typeof vi.fn>;
  downloadAdd: ReturnType<typeof vi.fn>;
  downloadStatus: ReturnType<typeof vi.fn>;
  downloadList: ReturnType<typeof vi.fn>;
  downloadPause: ReturnType<typeof vi.fn>;
  downloadResume: ReturnType<typeof vi.fn>;
  downloadCancel: ReturnType<typeof vi.fn>;
}

let nativeDeploy: FakeNativeDeploy;

function installApi(native: FakeNativeDeploy): void {
  (globalThis as unknown as { window: { api: unknown } }).window.api = { nativeDeploy: native };
}

function makeApi() {
  const dispatch = vi.fn();
  return {
    dispatch,
    api: {
      store: {
        getState: () => ({ app: { instanceId: "instance-1" } }),
        dispatch,
      },
    },
  };
}

beforeEach(() => {
  nativeDeploy = {
    status: vi.fn(() => Promise.resolve({ enabled: true, available: true, version: "0.1.0" })),
    plan: vi.fn(),
    deploy: vi.fn(),
    purge: vi.fn(),
    stagingStatus: vi.fn(),
    installApply: vi.fn(),
    archiveList: vi.fn(),
    archiveExtract: vi.fn(),
    hashFiles: vi.fn(),
    depsResolve: vi.fn(),
    collectionPlan: vi.fn(),
    pluginsScan: vi.fn(),
    pluginsOrderRead: vi.fn(),
    pluginsOrderWrite: vi.fn(),
    collectionStart: vi.fn(),
    collectionStatus: vi.fn(),
    collectionNext: vi.fn(),
    collectionMember: vi.fn(),
    collectionPause: vi.fn(),
    collectionCancel: vi.fn(),
    downloadAdd: vi.fn(),
    downloadStatus: vi.fn(),
    downloadList: vi.fn(),
    downloadPause: vi.fn(),
    downloadResume: vi.fn(),
    downloadCancel: vi.fn(),
  };
  installApi(nativeDeploy);
  resetNativeDeployStatus();
});

afterEach(() => {
  resetNativeDeployStatus();
  vi.clearAllMocks();
});

describe("native deployment adapter", () => {
  it("stays disabled for methods the core does not implement", async () => {
    expect(await canDeployNatively({ id: "move_activator" } as never)).toBe(false);
    expect(nativeDeploy.status).not.toHaveBeenCalled();
  });

  it("enables symlink deployment when the core advertises it", async () => {
    nativeDeploy.status.mockResolvedValue({
      enabled: true,
      available: true,
      capabilities: ["deploy.symlink"],
    });
    expect(await canDeployNatively({ id: "symlink_activator" } as never)).toBe(true);
  });

  it("keeps symlink deployment on the TS path without the capability", async () => {
    nativeDeploy.status.mockResolvedValue({ enabled: true, available: true, capabilities: [] });
    expect(await canDeployNatively({ id: "symlink_activator" } as never)).toBe(false);
  });

  it("maps Vortex mods and the merge source onto the wire request", async () => {
    const { api, dispatch } = makeApi();
    const captured: NativeDeployRequest[] = [];
    nativeDeploy.deploy.mockImplementation((request: NativeDeployRequest) => {
      captured.push(request);
      return Promise.resolve({
        operation: "op-1",
        report: {
          linked: 1,
          removed: 0,
          backedUp: 0,
          restored: 0,
          skipped: 0,
          failures: [
            {
              source: "ModA",
              relPath: "locked.txt",
              outputPath: "/game/locked.txt",
              message: "busy",
            },
          ],
        },
        files: [
          { relPath: "a.txt", source: "ModA", target: "Data", time: 123 },
          { relPath: "merged.txt", source: "__merged", time: 456 },
        ],
      });
    });

    const result = await deployWithNativeCore(
      api as never,
      "skyrimse",
      "/staging",
      "/game/Data",
      [
        {
          id: "mod-a",
          installationPath: "ModA",
          fileOverrides: ["/game/Data/overridden.txt"],
        } as never,
      ],
      { id: "hardlink_activator" } as never,
      [],
      "textures",
      new Set(["mod-a/used-in-merge.dds"]),
      ((mod: { id?: string } | null) => (mod === null ? "zzz_merge" : "Data")) as never,
    );

    expect(captured).toHaveLength(1);
    expect(captured[0]).toMatchObject({
      staging: "/staging",
      target: "/game/Data",
      manifest: path.join("/game/Data", "vortex.deployment.textures.json"),
      instance: "instance-1",
      method: "hardlink_activator",
      game: "skyrimse",
      options: { blacklist: ["mod-a/used-in-merge.dds"] },
      mods: [
        {
          installationPath: "ModA",
          sourceName: "ModA",
          target: "Data",
          fileOverrides: ["/game/Data/overridden.txt"],
        },
        {
          installationPath: "__merged.textures",
          sourceName: "__merged.textures",
          target: "zzz_merge",
        },
      ],
    });

    expect(result).toEqual([
      { relPath: "a.txt", source: "ModA", target: "Data", time: 123, merged: undefined },
      { relPath: "merged.txt", source: "__merged", time: 456, merged: undefined },
    ]);
    expect(dispatch).toHaveBeenCalledWith({
      type: "ADD_DEPLOYMENT_FAILURES",
      payload: {
        gameId: "skyrimse",
        failures: [
          {
            source: "ModA",
            relPath: "locked.txt",
            outputPath: "/game/locked.txt",
            message: "busy",
          },
        ],
      },
    });
  });

  it("purges every mod type and keeps the entries the core could not remove", async () => {
    const { api } = makeApi();
    nativeDeploy.purge
      .mockResolvedValueOnce({
        operation: "op-1",
        report: { linked: 0, removed: 1, backedUp: 0, restored: 1, skipped: 0, failures: [] },
        files: [],
      })
      .mockResolvedValueOnce({
        operation: "op-2",
        report: {
          linked: 0,
          removed: 0,
          backedUp: 0,
          restored: 0,
          skipped: 0,
          failures: [
            { source: "ModB", relPath: "b.txt", outputPath: "/game/b.txt", message: "busy" },
          ],
        },
        files: [{ relPath: "b.txt", source: "ModB", time: 1 }],
      });

    const lastDeployment = { Data: [{ relPath: "a.txt", source: "ModA", time: 1 }], Textures: [] };

    await expect(
      purgeWithNativeCore(
        api as never,
        "skyrimse",
        "/staging",
        { Data: "/game/Data", Textures: "/game/Textures" },
        ["Data", "Textures"],
        { id: "hardlink_activator" } as never,
        lastDeployment,
        () => undefined,
      ),
    ).rejects.toThrow(/Native purge failed/);

    expect(lastDeployment.Data).toEqual([]);
    expect(lastDeployment.Textures).toEqual([
      { relPath: "b.txt", source: "ModB", target: undefined, time: 1, merged: undefined },
    ]);
  });
});

describe("native staging adapter", () => {
  it("returns undefined when the native path is disabled", async () => {
    nativeDeploy.status.mockResolvedValue({ enabled: false, available: false });
    expect(await inspectStagingNatively("/staging", [{ id: "mod-a" }])).toBeUndefined();
    expect(nativeDeploy.stagingStatus).not.toHaveBeenCalled();
  });

  it("batches staged mods and maps their status", async () => {
    nativeDeploy.stagingStatus.mockResolvedValue({
      mods: [
        { id: "mod-a", installationPath: "ModA", status: "ok", entries: 2 },
        { id: "mod-b", installationPath: "ModB", status: "missing", entries: 0 },
      ],
    });

    const result = await inspectStagingNatively("/staging", [
      { id: "mod-a", installationPath: "ModA" },
      { id: "mod-b", installationPath: "ModB" },
      { id: "downloaded-only" },
    ]);

    expect(nativeDeploy.stagingStatus).toHaveBeenCalledWith({
      staging: "/staging",
      mods: [
        { id: "mod-a", installationPath: "ModA" },
        { id: "mod-b", installationPath: "ModB" },
      ],
    });
    expect(result?.get("mod-a")).toBe("ok");
    expect(result?.get("mod-b")).toBe("missing");
  });

  it("applies install instructions through the core", async () => {
    nativeDeploy.installApply.mockResolvedValue({
      operation: "op-install",
      report: { directories: 1, linked: 1, copied: 0, written: 0, failures: [] },
    });

    const result = await applyInstallNatively({
      temp: "/temp",
      destination: "/staging/ModA",
      instructions: [{ type: "hardlink", source: "a.txt", destination: "a.txt" }],
    });

    expect(nativeDeploy.installApply).toHaveBeenCalledWith({
      temp: "/temp",
      destination: "/staging/ModA",
      instructions: [{ type: "hardlink", source: "a.txt", destination: "a.txt" }],
    });
    expect(result?.report.linked).toBe(1);
  });

  it("falls back when the core predates install.apply", async () => {
    nativeDeploy.installApply.mockRejectedValue(
      new Error("internal: unknown method: install.apply"),
    );

    await expect(
      applyInstallNatively({
        temp: "/temp",
        destination: "/staging/ModA",
        instructions: [],
      }),
    ).resolves.toBeUndefined();
  });

  it("hashes files in the core", async () => {
    nativeDeploy.hashFiles.mockResolvedValue({
      algorithm: "md5",
      results: [
        { path: "/temp/a.txt", hash: "abc", bytes: 1 },
        { path: "/temp/b.txt", hash: "def", bytes: 2 },
      ],
      failures: [],
    });

    const result = await hashFilesNatively(["/temp/a.txt", "/temp/b.txt"], "md5");

    expect(nativeDeploy.hashFiles).toHaveBeenCalledWith({
      files: ["/temp/a.txt", "/temp/b.txt"],
      algorithm: "md5",
    });
    expect(result?.get("/temp/a.txt")).toBe("abc");
    expect(result?.get("/temp/b.txt")).toBe("def");
  });

  it("leaves encrypted archives to the TypeScript password prompt", async () => {
    nativeDeploy.archiveList.mockResolvedValue({
      code: 0,
      entries: [],
      errors: [],
      encrypted: true,
    });

    await expect(extractArchiveNatively("/downloads/a.7z", "/temp/a")).resolves.toBeUndefined();
    expect(nativeDeploy.archiveExtract).not.toHaveBeenCalled();
  });

  it("extracts unencrypted archives through the core", async () => {
    nativeDeploy.archiveList.mockResolvedValue({
      code: 0,
      entries: [],
      errors: [],
      encrypted: false,
    });
    nativeDeploy.archiveExtract.mockResolvedValue({ code: 0, entries: [], errors: [] });

    await expect(extractArchiveNatively("/downloads/a.7z", "/temp/a")).resolves.toEqual({
      code: 0,
      errors: [],
    });
    expect(nativeDeploy.archiveExtract).toHaveBeenCalledWith({
      archive: "/downloads/a.7z",
      destination: "/temp/a",
    });
  });

  it("resolves dependency rules in the core", async () => {
    const rule = {
      type: "requires",
      reference: { tag: "tag-1" },
    } as never;
    const mod = {
      id: "mod-a",
      state: "installed",
      attributes: { referenceTag: "tag-1", game: ["skyrimse"] },
    } as never;
    nativeDeploy.depsResolve.mockResolvedValue({
      dependencies: [
        {
          ruleIndex: 0,
          ruleId: "requires_tag-1",
          type: "requires",
          phase: 0,
          ignored: false,
          modId: "mod-a",
          status: "installed",
        },
      ],
    });

    const result = await resolveDependenciesNatively([rule], { "mod-a": mod }, {});

    expect(nativeDeploy.depsResolve).toHaveBeenCalledWith({
      rules: [
        {
          index: 0,
          type: "requires",
          reference: { tag: "tag-1" },
          phase: undefined,
          ignored: undefined,
          localPath: undefined,
          installerChoices: undefined,
          fileList: undefined,
          patches: undefined,
        },
      ],
      mods: [
        {
          id: "mod-a",
          state: "installed",
          fileMD5: undefined,
          fileName: undefined,
          name: undefined,
          logicalFileName: undefined,
          customFileName: undefined,
          version: undefined,
          fileId: undefined,
          modId: undefined,
          source: undefined,
          referenceTag: "tag-1",
          referenceTags: undefined,
          additionalLogicalFileNames: undefined,
          game: ["skyrimse"],
          installerChoices: undefined,
          fileList: undefined,
          patches: undefined,
        },
      ],
      downloads: [],
    });
    expect(result?.[0].modId).toBe("mod-a");
  });

  it("builds collection plans in the core", async () => {
    nativeDeploy.collectionPlan.mockResolvedValue({
      members: [],
      phases: [1, 666],
      totalRequired: 1,
      totalOptional: 1,
    });

    const result = await planCollectionNatively([], {}, {});

    expect(nativeDeploy.collectionPlan).toHaveBeenCalledWith({
      rules: [],
      mods: [],
      downloads: [],
    });
    expect(result?.phases).toEqual([1, 666]);
  });

  it("scans plugin directories in the core", async () => {
    nativeDeploy.pluginsScan.mockResolvedValue({
      directories: [
        {
          path: "/game/Data",
          files: [{ name: "Mod.esp", pluginId: "mod.esp", ghost: false }],
        },
      ],
    });

    const result = await scanPluginsNatively([
      { path: "/game/Data", extensions: [".esm", ".esp", ".esl"] },
    ]);

    expect(nativeDeploy.pluginsScan).toHaveBeenCalledWith({
      directories: [{ path: "/game/Data", extensions: [".esm", ".esp", ".esl"] }],
    });
    expect(result?.get("/game/Data")?.files[0].pluginId).toBe("mod.esp");
  });

  it("writes and reads plugin order files in the core", async () => {
    nativeDeploy.pluginsOrderWrite.mockResolvedValue({
      pluginsFile: "plugins.txt",
      loadOrderFile: "loadorder.txt",
      plugins: ["*Skyrim.esm"],
      loadOrder: ["Skyrim.esm"],
    });
    nativeDeploy.pluginsOrderRead.mockResolvedValue({
      pluginsFile: "plugins.txt",
      loadOrderFile: "loadorder.txt",
      plugins: ["*Skyrim.esm"],
      loadOrder: ["Skyrim.esm"],
    });

    const written = await writePluginOrderNatively(
      "/game/AppData",
      ["*Skyrim.esm"],
      ["Skyrim.esm"],
    );
    const read = await readPluginOrderNatively("/game/AppData");

    expect(nativeDeploy.pluginsOrderWrite).toHaveBeenCalledWith({
      directory: "/game/AppData",
      plugins: ["*Skyrim.esm"],
      loadOrder: ["Skyrim.esm"],
    });
    expect(written?.pluginsFile).toBe("plugins.txt");
    expect(read?.loadOrder).toEqual(["Skyrim.esm"]);
  });

  it("starts and advances a collection job in the core", async () => {
    nativeDeploy.collectionStart.mockResolvedValue({ jobId: "job-1", status: {} } as never);
    nativeDeploy.collectionNext.mockResolvedValue({
      members: [{ ruleId: "requires_a" }],
      status: {},
    } as never);
    nativeDeploy.collectionMember.mockResolvedValue({} as never);
    nativeDeploy.collectionStatus.mockResolvedValue({ complete: false } as never);
    nativeDeploy.collectionPause.mockResolvedValue({} as never);
    nativeDeploy.collectionCancel.mockResolvedValue({} as never);

    const started = await startCollectionNatively(
      "collection",
      "skyrimse",
      [
        {
          ruleIndex: 0,
          ruleId: "requires_a",
          type: "requires",
          phase: 1,
          ignored: false,
          status: "pending",
        },
      ],
      { concurrency: 2 },
    );
    expect(started?.jobId).toBe("job-1");

    const next = await nextCollectionMembersNatively("job-1");
    expect(next?.members[0].ruleId).toBe("requires_a");

    await updateCollectionMemberNatively("job-1", "requires_a", "installed", {
      modId: "mod-a",
    });
    await collectionStatusNatively("job-1");
    await pauseCollectionJobNatively("job-1");
    await cancelCollectionJobNatively("job-1");

    expect(nativeDeploy.collectionStart).toHaveBeenCalledWith({
      collectionId: "collection",
      gameId: "skyrimse",
      concurrency: 2,
      members: [
        {
          ruleIndex: 0,
          ruleId: "requires_a",
          type: "requires",
          phase: 1,
          ignored: false,
          status: "pending",
        },
      ],
    });
    expect(nativeDeploy.collectionMember).toHaveBeenCalledWith({
      jobId: "job-1",
      ruleId: "requires_a",
      status: "installed",
      modId: "mod-a",
    });
  });

  it("downloads a direct URL through the core queue", async () => {
    nativeDeploy.downloadAdd.mockResolvedValue({ id: "dl-1", state: "queued" });
    nativeDeploy.downloadStatus
      .mockResolvedValueOnce({ id: "dl-1", state: "downloading", bytes: 10 })
      .mockResolvedValueOnce({ id: "dl-1", state: "finished", bytes: 20 });

    const result = await downloadFileNatively(
      {
        directory: "/downloads/.native/1",
        fileName: "mod.zip",
        urls: ["https://example.com/mod.zip"],
      },
      1,
    );

    expect(nativeDeploy.downloadAdd).toHaveBeenCalledWith({
      directory: "/downloads/.native/1",
      fileName: "mod.zip",
      urls: ["https://example.com/mod.zip"],
    });
    expect(result?.path).toBe("/downloads/.native/1/mod.zip");
    expect(result?.status.state).toBe("finished");
  });

  it("cleans up a failed core download and lets the caller fall back", async () => {
    nativeDeploy.downloadAdd.mockResolvedValue({ id: "dl-2", state: "queued" });
    nativeDeploy.downloadStatus.mockResolvedValue({ id: "dl-2", state: "failed" });
    nativeDeploy.downloadCancel.mockResolvedValue({ id: "dl-2", state: "cancelled" });

    await expect(
      downloadFileNatively({
        directory: "/downloads/.native/2",
        fileName: "bad.zip",
        urls: ["https://example.com/bad.zip"],
      }),
    ).resolves.toBeUndefined();
    expect(nativeDeploy.downloadCancel).toHaveBeenCalledWith("dl-2");
  });
});
