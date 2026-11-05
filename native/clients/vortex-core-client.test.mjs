import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { mkdtempSync, mkdirSync, rmSync, writeFileSync, readFileSync, existsSync } from "node:fs";
import { createServer } from "node:http";
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

test("runs a collection phase queue", async () => {
  const member = (ruleIndex, ruleId, type, phase, ignored = false, status = "pending") => ({
    ruleIndex,
    ruleId,
    type,
    phase,
    ignored,
    status,
  });
  const started = await client.collectionStart({
    collectionId: "collection",
    gameId: "skyrimse",
    concurrency: 1,
    members: [
      member(0, "requires_a", "requires", 1),
      member(1, "requires_b", "requires", 1),
      member(2, "requires_c", "requires", 2),
      member(3, "recommends_d", "recommends", 666),
      member(4, "requires_ignored", "requires", 1, true, "ignored"),
    ],
  });
  assert.ok(started.jobId);

  let next = await client.collectionNext(started.jobId);
  assert.deepEqual(
    next.members.map((entry) => entry.ruleId),
    ["requires_a"],
  );
  await client.collectionMember({
    jobId: started.jobId,
    ruleId: "requires_a",
    status: "installed",
    modId: "mod-a",
  });

  next = await client.collectionNext(started.jobId);
  assert.deepEqual(
    next.members.map((entry) => entry.ruleId),
    ["requires_b"],
  );
  await client.collectionMember({
    jobId: started.jobId,
    ruleId: "requires_b",
    status: "installed",
    modId: "mod-b",
  });

  next = await client.collectionNext(started.jobId);
  assert.deepEqual(
    next.members.map((entry) => entry.ruleId),
    ["requires_c"],
  );
  await client.collectionMember({
    jobId: started.jobId,
    ruleId: "requires_c",
    status: "installed",
    modId: "mod-c",
  });

  next = await client.collectionNext(started.jobId);
  assert.deepEqual(
    next.members.map((entry) => entry.ruleId),
    ["recommends_d"],
  );
  const done = await client.collectionMember({
    jobId: started.jobId,
    ruleId: "recommends_d",
    status: "installed",
    modId: "mod-d",
  });
  assert.equal(done.complete, true);
  assert.equal(done.counts.installed, 4);
  assert.equal(done.counts.ignored, 1);

  await client.collectionPause(started.jobId);
  assert.deepEqual((await client.collectionNext(started.jobId)).members, []);
  await client.collectionCancel(started.jobId);
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

async function waitForState(downloadId, wanted, timeoutMs = 10000) {
  const deadline = Date.now() + timeoutMs;
  for (;;) {
    const status = await client.downloadStatus(downloadId);
    if (wanted.includes(status.state)) {
      return status;
    }
    if (Date.now() > deadline) {
      throw new Error(`download ${downloadId} stayed ${status.state}: ${status.error ?? ""}`);
    }
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
}

test("downloads over HTTP with pause/resume", async () => {
  const payload = Buffer.alloc(256 * 1024);
  for (let index = 0; index < payload.length; ++index) {
    payload[index] = index % 251;
  }
  const server = createServer((request, response) => {
    const range = /^bytes=(\d+)-/.exec(request.headers.range ?? "");
    const start = range === null ? 0 : Number(range[1]);
    const slice = payload.subarray(start);
    if (range === null) {
      response.writeHead(200, {
        "content-length": slice.length,
        "accept-ranges": "bytes",
      });
      response.write(slice.subarray(0, 32 * 1024));
      setTimeout(() => response.end(slice.subarray(32 * 1024)), 3000);
    } else {
      response.writeHead(206, {
        "content-length": slice.length,
        "accept-ranges": "bytes",
        "content-range": `bytes ${start}-${payload.length - 1}/${payload.length}`,
      });
      response.end(slice);
    }
  });
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  const url = `http://127.0.0.1:${server.address().port}/mod.zip`;

  try {
    const directory = join(root, "downloads");
    const started = await client.downloadAdd({
      id: "dl-http",
      directory,
      fileName: "mod.zip",
      urls: [url],
      curl: process.env.VORTEX_CURL_PATH ?? "curl",
      maxRetries: 1,
    });
    assert.ok(["queued", "downloading"].includes(started.state));

    let status = await waitForState("dl-http", ["downloading", "finished"]);
    const deadline = Date.now() + 5000;
    while (status.bytes === 0 && Date.now() < deadline) {
      await new Promise((resolve) => setTimeout(resolve, 50));
      status = await client.downloadStatus("dl-http");
    }
    assert.ok(status.bytes > 0, "download made progress");

    await client.downloadPause("dl-http");
    status = await waitForState("dl-http", ["paused"]);
    const pausedBytes = status.bytes;
    assert.ok(pausedBytes > 0 && pausedBytes < payload.length, "download is partial");

    await client.downloadResume("dl-http");
    status = await waitForState("dl-http", ["finished"]);
    assert.equal(status.bytes, payload.length);
    assert.equal(status.fromCache, false);
    assert.deepEqual(readFileSync(join(directory, "mod.zip")), payload);
    assert.equal(existsSync(join(directory, "mod.zip.part")), false);

    const list = await client.downloadList();
    assert.equal(list.downloads.find((entry) => entry.id === "dl-http").state, "finished");
  } finally {
    server.close();
  }
});

test("reports failed downloads and cancels in-flight ones", async () => {
  const server = createServer((request, response) => {
    if (request.url === "/slow") {
      response.writeHead(200, { "content-length": 1024 });
      response.write(Buffer.alloc(16));
      return;
    }
    response.writeHead(404);
    response.end("missing");
  });
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  const base = `http://127.0.0.1:${server.address().port}`;

  try {
    const directory = join(root, "downloads-fail");
    await client.downloadAdd({
      id: "dl-404",
      directory,
      fileName: "missing.zip",
      urls: [`${base}/missing`],
      curl: process.env.VORTEX_CURL_PATH ?? "curl",
      maxRetries: 0,
    });
    const failed = await waitForState("dl-404", ["failed"]);
    assert.ok(failed.error.length > 0, "failure carries an error");

    await client.downloadAdd({
      id: "dl-cancel",
      directory,
      fileName: "slow.zip",
      urls: [`${base}/slow`],
      curl: process.env.VORTEX_CURL_PATH ?? "curl",
      maxRetries: 0,
    });
    await new Promise((resolve) => setTimeout(resolve, 150));
    await client.downloadCancel("dl-cancel");
    await waitForState("dl-cancel", ["cancelled"]);
    assert.equal(existsSync(join(directory, "slow.zip.part")), false);
  } finally {
    server.close();
  }
});

test("forwards download headers and supports a non-atomic destination", async () => {
  const payload = Buffer.from("authenticated payload");
  const server = createServer((request, response) => {
    if (request.headers["x-api-key"] !== "secret") {
      response.writeHead(401);
      response.end("unauthorized");
      return;
    }
    response.writeHead(200, { "content-length": payload.length });
    response.end(payload);
  });
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  const url = `http://127.0.0.1:${server.address().port}/direct.bin`;

  try {
    const directory = join(root, "downloads-auth");
    await client.downloadAdd({
      id: "dl-auth",
      directory,
      fileName: "direct.bin",
      urls: [url],
      headers: { "x-api-key": "secret" },
      atomic: false,
      curl: process.env.VORTEX_CURL_PATH ?? "curl",
      maxRetries: 0,
    });
    const status = await waitForState("dl-auth", ["finished", "failed"]);
    assert.equal(status.state, "finished", status.error);
    assert.equal(status.bytes, payload.length);
    assert.deepEqual(readFileSync(join(directory, "direct.bin")), payload);
    assert.equal(existsSync(join(directory, "direct.bin.part")), false);
  } finally {
    server.close();
  }
});

test("activates and deactivates a single mod without disturbing others", async () => {
  const singleStaging = join(root, "single-staging");
  const singleTarget = join(root, "single-target");
  mkdirSync(join(singleStaging, "ModA"), { recursive: true });
  mkdirSync(join(singleStaging, "ModB"), { recursive: true });
  mkdirSync(singleTarget, { recursive: true });
  writeFileSync(join(singleStaging, "ModA", "a.txt"), "mod-a");
  writeFileSync(join(singleStaging, "ModB", "b.txt"), "mod-b");
  const singleManifest = join(singleTarget, "vortex.deployment.json");

  const deployedB = await client.request("deploy", {
    includeFiles: true,
    staging: singleStaging,
    target: singleTarget,
    manifest: singleManifest,
    mods: [{ installationPath: "ModB", sourceName: "ModB", target: "" }],
    instance: "test",
    method: "hardlink_activator",
    game: "skyrimse",
  });
  assert.equal(deployedB.report.failures.length, 0);
  assert.equal(readFileSync(join(singleTarget, "b.txt"), "utf8"), "mod-b");

  const activatedA = await client.request("deploy.single", {
    staging: singleStaging,
    target: singleTarget,
    manifest: singleManifest,
    instance: "test",
    method: "hardlink_activator",
    game: "skyrimse",
    enable: true,
    mod: { installationPath: "ModA", sourceName: "ModA", target: "" },
  });
  assert.equal(activatedA.report.failures.length, 0);
  assert.deepEqual(activatedA.files.map((file) => `${file.source}/${file.relPath}`).sort(), [
    "ModA/a.txt",
    "ModB/b.txt",
  ]);
  assert.equal(readFileSync(join(singleTarget, "a.txt"), "utf8"), "mod-a");
  assert.equal(readFileSync(join(singleTarget, "b.txt"), "utf8"), "mod-b");

  const deactivatedA = await client.request("deploy.single", {
    staging: singleStaging,
    target: singleTarget,
    manifest: singleManifest,
    instance: "test",
    method: "hardlink_activator",
    game: "skyrimse",
    enable: false,
    mod: { installationPath: "ModA", sourceName: "ModA", target: "" },
  });
  assert.equal(deactivatedA.report.failures.length, 0);
  assert.deepEqual(
    deactivatedA.files.map((file) => `${file.source}/${file.relPath}`),
    ["ModB/b.txt"],
  );
  assert.equal(existsSync(join(singleTarget, "a.txt")), false);
  assert.equal(readFileSync(join(singleTarget, "b.txt"), "utf8"), "mod-b");
});

test("moves a mod into the game directory and restores it on purge", async () => {
  const moveStaging = join(root, "move-staging");
  const moveTarget = join(root, "move-target");
  mkdirSync(join(moveStaging, "ModA", "nested"), { recursive: true });
  mkdirSync(moveTarget, { recursive: true });
  writeFileSync(join(moveStaging, "ModA", "a.txt"), "mod-a");
  writeFileSync(join(moveStaging, "ModA", "nested", "b.txt"), "mod-b");
  writeFileSync(join(moveTarget, "a.txt"), "game-a");
  const moveManifest = join(moveTarget, "vortex.deployment.json");

  const moved = await client.request("deploy", {
    includeFiles: true,
    staging: moveStaging,
    target: moveTarget,
    manifest: moveManifest,
    mods: [{ installationPath: "ModA", sourceName: "ModA", target: "" }],
    instance: "test",
    method: "move_activator",
    game: "skyrimse",
  });
  assert.equal(moved.report.failures.length, 0);
  assert.equal(existsSync(join(moveStaging, "ModA", "a.txt")), false);
  assert.ok(existsSync(join(moveStaging, "ModA", "a.txt.vortex_lnk")));
  assert.equal(readFileSync(join(moveTarget, "a.txt"), "utf8"), "mod-a");
  assert.equal(readFileSync(join(moveTarget, "a.txt.vortex_backup"), "utf8"), "game-a");

  const purged = await client.request("purge", {
    staging: moveStaging,
    target: moveTarget,
    manifest: moveManifest,
    instance: "test",
    method: "move_activator",
    game: "skyrimse",
  });
  assert.equal(purged.report.failures.length, 0);
  assert.equal(readFileSync(join(moveStaging, "ModA", "a.txt"), "utf8"), "mod-a");
  assert.equal(existsSync(join(moveStaging, "ModA", "a.txt.vortex_lnk")), false);
  assert.equal(readFileSync(join(moveTarget, "a.txt"), "utf8"), "game-a");
});

test("detects external changes from a manifest", async () => {
  const changeStaging = join(root, "change-staging");
  const changeTarget = join(root, "change-target");
  mkdirSync(join(changeStaging, "ModA"), { recursive: true });
  mkdirSync(changeTarget, { recursive: true });
  writeFileSync(join(changeStaging, "ModA", "a.txt"), "mod-a");
  const changeManifest = join(changeTarget, "vortex.deployment.json");

  await client.request("deploy", {
    staging: changeStaging,
    target: changeTarget,
    manifest: changeManifest,
    mods: [{ installationPath: "ModA", sourceName: "ModA", target: "" }],
    instance: "test",
    method: "hardlink_activator",
    game: "skyrimse",
  });

  const baseRequest = {
    staging: changeStaging,
    target: changeTarget,
    manifest: changeManifest,
    method: "hardlink_activator",
  };
  let result = await client.externalChanges(baseRequest);
  assert.deepEqual(result.changes, []);

  rmSync(join(changeTarget, "a.txt"));
  writeFileSync(join(changeTarget, "a.txt"), "external");
  result = await client.externalChanges(baseRequest);
  assert.equal(result.changes.length, 1);
  assert.equal(result.changes[0].changeType, "refchange");
  assert.ok(result.changes[0].sourceTime > 0);
  assert.ok(result.changes[0].destTime > 0);

  rmSync(join(changeTarget, "a.txt"));
  result = await client.externalChanges(baseRequest);
  assert.equal(result.changes[0].changeType, "deleted");
});
