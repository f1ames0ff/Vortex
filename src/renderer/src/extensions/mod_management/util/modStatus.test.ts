import path from "node:path";

import { describe, expect, it } from "vitest";

import type { IDeploymentFailure } from "../actions/session";
import type { IModWithState } from "../types/IModProps";
import { hasDeploymentFailure, modStagingFolder, modStatus } from "./modStatus";

const mod = (extra: Partial<IModWithState> = {}): IModWithState =>
  ({
    id: "mod-a",
    state: "installed",
    type: "",
    installationPath: "ModA",
    enabled: true,
    ...extra,
  }) as IModWithState;

const failure = (source: string): IDeploymentFailure => ({
  source,
  relPath: "fomod/ModuleConfig.xml",
  outputPath: "/game/Data/fomod/ModuleConfig.xml",
  message: "a .vortex_backup already exists",
});

describe("modStatus", () => {
  it("reports an enabled, installed mod with files as enabled", () => {
    expect(modStatus(mod({ installationState: "ok" }), [])).toBe("enabled");
  });

  it("reports an enabled mod with an empty staging folder as an error", () => {
    expect(modStatus(mod({ installationState: "empty" }), [])).toBe("error");
  });

  it("reports an enabled mod whose staging folder is gone as an error", () => {
    expect(modStatus(mod({ installationState: "missing" }), [])).toBe("error");
  });

  it("reports a staging inspection error as an error", () => {
    expect(modStatus(mod({ installationState: "error" }), [])).toBe("error");
  });

  it("reports a disabled mod with missing files as an error too, so it can be filtered", () => {
    expect(modStatus(mod({ enabled: false, installationState: "empty" }), [])).toBe("error");
  });

  it("reports a deployment failure even when the folder still has files", () => {
    const failures = [failure("ModA")];
    expect(hasDeploymentFailure(mod(), failures)).toBe(true);
    expect(modStatus(mod({ installationState: "ok" }), failures)).toBe("error");
  });

  it("ignores failures belonging to another mod", () => {
    expect(modStatus(mod({ installationState: "ok" }), [failure("ModB")])).toBe("enabled");
  });

  it("keeps the downloaded states before looking at files", () => {
    expect(modStatus(mod({ state: "downloaded", attributes: {} }), [])).toBe("neverinstalled");
    expect(modStatus(mod({ state: "downloaded", attributes: { wasInstalled: true } }), [])).toBe(
      "uninstalled",
    );
  });

  it("builds the staging path only for installed mods", () => {
    expect(modStagingFolder(mod(), "/staging")).toBe(path.join("/staging", "ModA"));
    expect(modStagingFolder(mod({ state: "downloaded" }), "/staging")).toBeUndefined();
    expect(modStagingFolder(mod(), undefined)).toBeUndefined();
  });
});
