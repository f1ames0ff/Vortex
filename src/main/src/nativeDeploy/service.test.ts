import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";

import { afterAll, beforeAll, describe, expect, it, vi } from "vitest";

vi.mock("../logging", () => ({ log: vi.fn() }));

import { NativeCoreClient } from "./service";

function findCore(): string | undefined {
  const candidates = [
    process.env.VORTEX_CORE_PATH,
    path.resolve(__dirname, "..", "..", "..", "..", "native", "build", "dev", "vortex-core"),
    path.resolve(__dirname, "..", "..", "..", "..", "native", "build", "release", "vortex-core"),
  ];
  return candidates.find((candidate) => candidate !== undefined && existsSync(candidate));
}

const corePath = findCore();

describe.skipIf(corePath === undefined)("NativeCoreClient", () => {
  let root: string;
  let staging: string;
  let target: string;
  let manifest: string;
  let client: NativeCoreClient;

  beforeAll(() => {
    root = mkdtempSync(path.join(tmpdir(), "vortex-native-core-client-"));
    staging = path.join(root, "staging");
    target = path.join(root, "target");
    manifest = path.join(target, "vortex.deployment.json");

    mkdirSync(path.join(staging, "ModA"), { recursive: true });
    mkdirSync(target, { recursive: true });
    writeFileSync(path.join(staging, "ModA", "a.txt"), "mod-a");
    writeFileSync(path.join(target, "a.txt"), "game-a");

    client = new NativeCoreClient({
      resolveCorePath: () => corePath,
      enabled: () => true,
      requestTimeoutMs: 30_000,
    });
  });

  afterAll(async () => {
    await client?.shutdown();
    if (root !== undefined) {
      rmSync(root, { recursive: true, force: true });
    }
  });

  it("reports an available core", async () => {
    const status = await client.status();
    expect(status).toMatchObject({ enabled: true, available: true, corePath });
    expect(status.version).toBeTruthy();
  });

  it("plans, deploys, re-plans and purges through the core process", async () => {
    const common = {
      staging,
      target,
      manifest,
      instance: "test",
      method: "hardlink",
      game: "skyrimse" as const,
    };
    const mods = [{ installationPath: "ModA", sourceName: "ModA" }];

    expect(await client.plan({ ...common, mods })).toMatchObject({ desired: 1, added: 1 });

    const deployed = await client.deploy({ ...common, mods });
    expect(deployed.report.linked).toBe(1);
    expect(deployed.report.failures).toEqual([]);
    expect(deployed.files).toEqual([expect.objectContaining({ relPath: "a.txt", source: "ModA" })]);
    expect(readFileSync(path.join(target, "a.txt"), "utf8")).toBe("mod-a");
    expect(readFileSync(path.join(target, "a.txt.vortex_backup"), "utf8")).toBe("game-a");

    expect(await client.plan({ ...common, mods })).toMatchObject({
      desired: 1,
      added: 0,
      removed: 0,
      changed: 0,
    });

    const purged = await client.purge(common);
    expect(purged.report.restored).toBe(1);
    expect(purged.report.failures).toEqual([]);
    expect(readFileSync(path.join(target, "a.txt"), "utf8")).toBe("game-a");
    expect(existsSync(path.join(target, "a.txt.vortex_backup"))).toBe(false);
  });
});
