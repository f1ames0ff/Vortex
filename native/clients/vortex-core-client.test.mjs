import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { mkdtempSync, mkdirSync, rmSync, writeFileSync, readFileSync, existsSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { after, test } from "node:test";

import { VortexCoreClient } from "./vortex-core-client.mjs";

const corePath = process.env.VORTEX_CORE_PATH;
assert.ok(corePath, "VORTEX_CORE_PATH must point at vortex-core");
const sevenZip = process.env.VORTEX_7Z_PATH ?? "/usr/bin/7z";

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

test("hashes files in the core", async () => {
  const result = await client.hashFiles({
    algorithm: "md5",
    files: [join(mod, "a.txt")],
  });

  assert.deepEqual(result.failures, []);
  assert.equal(result.results[0].hash, createHash("md5").update("mod-a").digest("hex"));
  assert.equal(result.results[0].bytes, 5);
});

test("resolves dependencies and plans a collection", async () => {
  const rules = [
    { type: "requires", reference: { tag: "rule-a", gameId: "skyrimse" }, phase: 1 },
    { type: "recommends", reference: { tag: "missing", gameId: "skyrimse" } },
  ];
  const mods = [{ id: "mod-a", state: "installed", referenceTag: "rule-a" }];

  const resolved = await client.resolveDependencies({ rules, mods, downloads: [] });
  assert.equal(resolved.dependencies.length, 2);
  assert.equal(resolved.dependencies[0].modId, "mod-a");
  assert.equal(resolved.dependencies[0].status, "installed");
  assert.equal(resolved.dependencies[1].status, "pending");

  const plan = await client.planCollection({ rules, mods, downloads: [] });
  assert.deepEqual(plan.phases, [1, 666]);
  assert.equal(plan.totalRequired, 1);
  assert.equal(plan.totalOptional, 1);
});

test("scans plugins and reads/writes the load order", async () => {
  const pluginDir = join(root, "plugins");
  mkdirSync(pluginDir, { recursive: true });
  writeFileSync(join(pluginDir, "Skyrim.esm"), "esm");
  writeFileSync(join(pluginDir, "Mod.ESP"), "esp");
  writeFileSync(join(pluginDir, "Disabled.esp.ghost"), "ghost");

  const scan = await client.pluginsScan({
    directories: [{ path: pluginDir, extensions: [".esm", ".esp", ".esl"] }],
  });
  assert.equal(scan.directories.length, 1);
  assert.equal(scan.directories[0].files.length, 3);
  assert.ok(scan.directories[0].files.some((file) => file.pluginId === "mod.esp"));

  const written = await client.pluginsOrderWrite({
    directory: pluginDir,
    plugins: ["*Skyrim.esm", "Mod.esp"],
    loadOrder: ["Skyrim.esm", "Mod.esp"],
  });
  assert.equal(written.error, undefined);
  const read = await client.pluginsOrderRead({ directory: pluginDir });
  assert.deepEqual(read.plugins, ["*Skyrim.esm", "Mod.esp"]);
  assert.deepEqual(read.loadOrder, ["Skyrim.esm", "Mod.esp"]);
});

test("lists and extracts archives through 7-Zip", { skip: !existsSync(sevenZip) }, async () => {
  const archive = join(root, "sample.zip");
  execFileSync(sevenZip, ["a", "-tzip", archive, "a.txt"], {
    cwd: mod,
    stdio: "ignore",
  });

  const listing = await client.archiveList({ archive, sevenZip });
  assert.equal(listing.code, 0);
  assert.deepEqual(listing.errors, []);
  assert.ok(listing.entries.some((entry) => entry.path === "a.txt"));

  const destination = join(root, "archive-out");
  const extraction = await client.archiveExtract({ archive, destination, sevenZip });
  assert.equal(extraction.code, 0);
  assert.deepEqual(extraction.errors, []);
  assert.equal(readFileSync(join(destination, "a.txt"), "utf8"), "mod-a");
});
