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
  canDeployNatively,
  deployWithNativeCore,
  purgeWithNativeCore,
  resetNativeDeployStatus,
} from "./nativeDeploy";

interface FakeNativeDeploy {
  status: ReturnType<typeof vi.fn>;
  plan: ReturnType<typeof vi.fn>;
  deploy: ReturnType<typeof vi.fn>;
  purge: ReturnType<typeof vi.fn>;
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
  };
  installApi(nativeDeploy);
  resetNativeDeployStatus();
});

afterEach(() => {
  resetNativeDeployStatus();
  vi.clearAllMocks();
});

describe("native deployment adapter", () => {
  it("stays disabled for non-hardlink deployment methods", async () => {
    expect(await canDeployNatively({ id: "symlink_activator" } as never)).toBe(false);
    expect(nativeDeploy.status).not.toHaveBeenCalled();
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
