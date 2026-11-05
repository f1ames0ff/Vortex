import assert from "node:assert/strict";
import { mkdtempSync, mkdirSync, rmSync, writeFileSync, readFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { after, test } from "node:test";

import { VortexCoreClient } from "./vortex-core-client.mjs";

const corePath = process.env.VORTEX_CORE_PATH;
assert.ok(corePath, "VORTEX_CORE_PATH must point at vortex-core");

const root = mkdtempSync(join(tmpdir(), "vortex-core-client-"));
const staging = join(root, "staging");
const target = join(root, "target");
const manifest = join(root, "manifest.json");
const mod = join(staging, "ModA");

mkdirSync(mod, { recursive: true });
mkdirSync(target, { recursive: true });
writeFileSync(join(mod, "a.txt"), "mod-a");
writeFileSync(join(target, "a.txt"), "game-a");

const client = new VortexCoreClient(corePath);

after(async () => {
  await client.close();
  rmSync(root, { recursive: true, force: true });
});

test("deploys through the core process and purges", async () => {
  const common = {
    staging,
    target,
    manifest,
    instance: "test",
    method: "hardlink",
    game: "skyrimse",
  };
  const mods = [{ installationPath: "ModA", sourceName: "ModA", target: "" }];

  assert.ok((await client.request("ping")).version);
  assert.equal((await client.request("plan", { ...common, mods })).added, 1);

  const deployed = await client.request("deploy", { ...common, mods });
  assert.equal(deployed.report.linked, 1);
  assert.deepEqual(deployed.report.failures, []);
  assert.equal(readFileSync(join(target, "a.txt"), "utf8"), "mod-a");

  assert.equal((await client.request("plan", { ...common, mods })).added, 0);

  const purged = await client.request("purge", common);
  assert.equal(purged.report.restored, 1);
  assert.equal(readFileSync(join(target, "a.txt"), "utf8"), "game-a");
});
