import assert from "node:assert/strict";
import { mkdtempSync, mkdirSync, rmSync, writeFileSync, readFileSync, existsSync } from "node:fs";
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

test("reports staging status without walking it in the renderer", async () => {
  const result = await client.stagingStatus({
    staging,
    mods: [
      { id: "mod-a", installationPath: "ModA" },
      { id: "missing", installationPath: "MissingMod" },
    ],
  });

  assert.equal(result.mods.length, 2);
  assert.equal(result.mods[0].status, "ok");
  assert.equal(result.mods[0].entries, 1);
  assert.equal(result.mods[1].status, "missing");
});

test("loads game descriptors through the core", async () => {
  const games = join(root, "games");
  mkdirSync(join(games, "skyrimse"), { recursive: true });
  mkdirSync(join(games, "alpha"), { recursive: true });
  mkdirSync(join(games, "zeta"), { recursive: true });
  mkdirSync(join(games, "ignored"), { recursive: true });
  writeFileSync(
    join(games, "skyrimse", "game.toml"),
    [
      'id = "skyrimse"',
      'name = "Skyrim Special Edition"',
      'nexusDomain = "skyrimspecialedition"',
      "steamAppId = 489830",
      'requiredFiles = ["SkyrimSE.exe"]',
      'hooks = ["gamebryo-plugin-management"]',
      "",
      "[deployment]",
      'targets = ["Data"]',
      'pluginPath = "Data"',
      'iniPath = "SkyrimPrefs.ini"',
    ].join("\n"),
  );
  writeFileSync(
    join(games, "alpha", "game.toml"),
    'id = "alpha"\nname = "Alpha"\nnexusDomain = "alphatestgame"\n',
  );
  writeFileSync(
    join(games, "zeta", "game.toml"),
    'id = "zeta"\nname = "Zeta"\nnexusDomain = "zetatestgame"\n',
  );

  const result = await client.gameRegistry({ directory: games });
  assert.deepEqual(
    result.games.map((game) => game.id),
    ["alpha", "skyrimse", "zeta"],
  );
  const skyrim = result.games[1];
  assert.equal(skyrim.name, "Skyrim Special Edition");
  assert.equal(skyrim.nexusDomain, "skyrimspecialedition");
  assert.equal(skyrim.steamAppId, 489830);
  assert.deepEqual(skyrim.requiredFiles, ["SkyrimSE.exe"]);
  assert.deepEqual(skyrim.deploymentTargets, ["Data"]);
  assert.equal(skyrim.pluginPath, "Data");
  assert.equal(skyrim.iniPath, "SkyrimPrefs.ini");
  assert.deepEqual(skyrim.hooks, ["gamebryo-plugin-management"]);
  assert.equal(skyrim.sourcePath, join(games, "skyrimse", "game.toml"));

  await assert.rejects(
    client.gameRegistry({ directory: join(root, "missing-games") }),
    /does not exist/,
  );
});

test("applies generic installer instructions in the core", async () => {
  const temp = join(root, "install-temp");
  const destination = join(root, "install-destination");
  const journalDir = join(root, "install-journal");
  mkdirSync(join(temp, "nested"), { recursive: true });
  writeFileSync(join(temp, "a.txt"), "a");
  writeFileSync(join(temp, "nested", "b.txt"), "b");

  const result = await client.installApply({
    temp,
    destination,
    journalDir,
    instructions: [
      { type: "hardlink", source: "a.txt", destination: "a.txt" },
      { type: "copy", source: "nested/b.txt", destination: "nested/b.txt" },
      { type: "write", destination: "generated.txt", data: "hello" },
    ],
  });

  assert.deepEqual(result.report.failures, []);
  assert.equal(result.report.linked, 1);
  assert.equal(result.report.copied, 1);
  assert.equal(result.report.written, 1);
  assert.equal(readFileSync(join(destination, "a.txt"), "utf8"), "a");
  assert.equal(readFileSync(join(destination, "nested", "b.txt"), "utf8"), "b");
  assert.equal(readFileSync(join(destination, "generated.txt"), "utf8"), "hello");
  assert.ok(existsSync(join(journalDir, `${result.operation}.jsonl`)));
});
