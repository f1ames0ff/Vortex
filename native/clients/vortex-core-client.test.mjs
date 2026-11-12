import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import {
  mkdtempSync,
  mkdirSync,
  readdirSync,
  rmSync,
  writeFileSync,
  readFileSync,
  existsSync,
} from "node:fs";
import { createServer } from "node:http";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
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

test("executes a collection member through core workers", async () => {
  const work = join(root, "collection-run");
  const source = join(work, "source");
  const downloadsDir = join(work, "downloads");
  const temp = join(work, "temp");
  const destination = join(work, "staging", "ModA");
  mkdirSync(source, { recursive: true });
  mkdirSync(downloadsDir, { recursive: true });
  writeFileSync(join(source, "a.txt"), "worker-content");
  const archive = join(work, "task.zip");
  execFileSync(sevenZip, ["a", "-tzip", archive, "a.txt"], { cwd: source, stdio: "ignore" });
  const payload = readFileSync(archive);

  const server = createServer((request, response) => {
    response.writeHead(200, { "content-length": payload.length });
    response.end(payload);
  });
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  const url = `http://127.0.0.1:${server.address().port}/task.zip`;

  try {
    const started = await client.collectionStart({
      collectionId: "collection",
      gameId: "skyrimse",
      concurrency: 1,
      maxRetries: 0,
      members: [
        {
          ruleIndex: 0,
          ruleId: "requires_a",
          type: "requires",
          phase: 1,
          ignored: false,
          status: "pending",
          task: {
            modId: "ModA",
            download: {
              directory: downloadsDir,
              fileName: "task.zip",
              urls: [url],
              curl: process.env.VORTEX_CURL_PATH ?? "curl",
              maxRetries: 0,
            },
            extract: { destination: temp, sevenZip },
            copyAll: true,
            install: { temp, destination, instructions: [] },
          },
        },
      ],
    });
    assert.ok(started.jobId);

    await client.collectionRun(started.jobId);
    const deadline = Date.now() + 15000;
    let status = await client.collectionStatus(started.jobId);
    while (!status.complete && Date.now() < deadline) {
      await new Promise((resolve) => setTimeout(resolve, 50));
      status = await client.collectionStatus(started.jobId);
    }

    assert.equal(status.complete, true);
    assert.equal(status.members[0].status, "installed", status.members[0].error);
    assert.equal(status.members[0].modId, "ModA");
    assert.equal(status.members[0].result.modId, "ModA");
    assert.ok(status.members[0].result.files.includes("a.txt"));
    assert.equal(status.counts.installed, 1);
    assert.equal(readFileSync(join(destination, "a.txt"), "utf8"), "worker-content");
  } finally {
    server.close();
  }
});

test("attaches and executes a task on a collection member", async () => {
  const work = join(root, "collection-attach");
  const source = join(work, "source");
  const temp = join(work, "temp");
  const destination = join(work, "staging", "ModA");
  mkdirSync(source, { recursive: true });
  writeFileSync(join(source, "b.txt"), "attached-content");
  const archive = join(work, "attached.zip");
  execFileSync(sevenZip, ["a", "-tzip", archive, "b.txt"], { cwd: source, stdio: "ignore" });

  const started = await client.collectionStart({
    collectionId: "collection",
    gameId: "skyrimse",
    concurrency: 1,
    maxRetries: 0,
    members: [
      {
        ruleIndex: 0,
        ruleId: "requires_attached",
        type: "requires",
        phase: 1,
        ignored: false,
        status: "pending",
      },
    ],
  });

  const attached = await client.collectionTask({
    jobId: started.jobId,
    ruleId: "requires_attached",
    task: {
      modId: "ModA",
      extract: { archive, destination: temp, sevenZip },
      install: {
        temp,
        destination,
        instructions: [{ type: "copy", source: "b.txt", destination: "b.txt" }],
      },
    },
  });
  assert.equal(attached.members[0].status, "pending");
  assert.equal(attached.members[0].hasTask, true);

  const executed = await client.collectionExecute({
    jobId: started.jobId,
    ruleId: "requires_attached",
  });
  assert.equal(executed.complete, true);
  assert.equal(executed.members[0].status, "installed", executed.members[0].error);
  assert.equal(executed.members[0].result.modId, "ModA");
  assert.equal(executed.members[0].result.installationPath, destination);
  assert.ok(executed.members[0].result.files.includes("b.txt"));
  assert.equal(readFileSync(join(destination, "b.txt"), "utf8"), "attached-content");
});

const echoModule =
  process.env.VORTEX_MODULE_PATH ??
  join(
    dirname(corePath),
    process.platform === "win32" ? "vortex_test_echo_module.dll" : "libvortex_test_echo_module.so",
  );

test("loads and calls a C-ABI module", { skip: !existsSync(echoModule) }, async () => {
  const info = await client.moduleLoad(echoModule);
  assert.equal(info.name, "echo");
  assert.equal(info.version, "1.0.0");

  const listed = await client.moduleList();
  assert.ok(listed.modules.some((module) => module.name === "echo"));

  const echoed = await client.moduleCall("echo", "echo", { value: 42 });
  assert.deepEqual(echoed.result, { value: 42 });

  await assert.rejects(client.moduleCall("echo", "missing", {}), /unknown echo-module method/);
});

test("resolves FOMOD choices and metadata through the node-helper C-ABI module", async () => {
  const modulePath =
    process.env.VORTEX_NODE_HELPER_MODULE ??
    join(
      dirname(corePath),
      process.platform === "win32"
        ? "vortex_node_helper_module.dll"
        : "libvortex_node_helper_module.so",
    );
  const helperScript = join(dirname(corePath), "..", "..", "helpers", "vortex-helper.mjs");

  const packageRoot = join(root, "fomod-package");
  const fomodDir = join(packageRoot, "fomod");
  mkdirSync(fomodDir, { recursive: true });
  mkdirSync(join(packageRoot, "Data"), { recursive: true });
  writeFileSync(
    join(fomodDir, "ModuleConfig.xml"),
    `<config xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:noNamespaceSchemaLocation="http://qconsulting.ca/fo3/ModConfig5.0.xsd">
       <moduleName>Parity FOMOD</moduleName>
       <requiredInstallFiles><file source="Data/required.txt" destination="required.txt"/></requiredInstallFiles>
       <installSteps><installStep name="Step"><optionalFileGroups>
         <group name="Group" type="SelectExactlyOne"><plugins>
           <plugin name="Recommended A"><description>Recommended A</description>
             <files><file source="Data/a.txt" destination="a.txt"/></files>
             <typeDescriptor><type name="Recommended"/></typeDescriptor></plugin>
           <plugin name="Optional B"><description>Optional B</description>
             <files><file source="Data/b.txt" destination="b.txt"/></files>
             <typeDescriptor><type name="Optional"/></typeDescriptor></plugin>
         </plugins></group>
       </optionalFileGroups></installStep></installSteps>
     </config>`,
  );
  writeFileSync(join(packageRoot, "Data", "required.txt"), "required");
  writeFileSync(join(packageRoot, "Data", "a.txt"), "a");
  writeFileSync(join(packageRoot, "Data", "b.txt"), "b");

  const nativeRoot = join(dirname(corePath), "..", "..", "..");
  const fomodPackage =
    process.env.VORTEX_FOMOD_NATIVE ??
    (() => {
      const pnpm = join(nativeRoot, "node_modules", ".pnpm");
      const directory = readdirSync(pnpm).find((name) =>
        name.startsWith("@nexusmods+fomod-installer-native@"),
      );
      return directory === undefined
        ? undefined
        : join(
            pnpm,
            directory,
            "node_modules",
            "@nexusmods",
            "fomod-installer-native",
            "dist",
            "index.js",
          );
    })();

  if (!existsSync(modulePath) || !existsSync(helperScript) || fomodPackage === undefined) {
    return;
  }
  await client.moduleLoad(modulePath);

  const supported = await client.moduleCall("node-helper", "call", {
    node: process.execPath,
    script: helperScript,
    request: {
      method: "fomod.testSupported",
      params: {
        nativeLibrary: fomodPackage,
        files: ["fomod/ModuleConfig.xml"],
        allowedTypes: ["XmlScript"],
      },
    },
  });
  assert.equal(supported.result.supported, true);

  const resolved = await client.moduleCall("node-helper", "call", {
    node: process.execPath,
    script: helperScript,
    request: {
      method: "fomod.resolve",
      params: {
        nativeLibrary: fomodPackage,
        files: ["fomod/ModuleConfig.xml", "Data/required.txt", "Data/a.txt", "Data/b.txt"],
        stopPatterns: [],
        pluginPath: packageRoot,
        scriptPath: packageRoot,
        preset: [
          {
            name: "Step",
            groups: [{ name: "Group", choices: [{ name: "Recommended A", idx: 0 }] }],
          },
        ],
        validate: false,
      },
    },
  });
  const sources = resolved.result.instructions
    .filter((instruction) => instruction.type === "copy")
    .map((instruction) => instruction.source)
    .sort();
  assert.deepEqual(sources, ["Data/a.txt", "Data/required.txt"]);

  const noPreset = await client.moduleCall("node-helper", "call", {
    node: process.execPath,
    script: helperScript,
    request: {
      method: "fomod.resolve",
      params: {
        nativeLibrary: fomodPackage,
        files: ["fomod/ModuleConfig.xml", "Data/required.txt", "Data/a.txt", "Data/b.txt"],
        stopPatterns: [],
        pluginPath: packageRoot,
        scriptPath: packageRoot,
        validate: false,
      },
    },
  });
  const defaultSources = noPreset.result.instructions
    .filter((instruction) => instruction.type === "copy")
    .map((instruction) => instruction.source)
    .sort();
  assert.deepEqual(defaultSources, ["Data/a.txt", "Data/required.txt"]);
  assert.deepEqual(noPreset.result.choices, [
    {
      name: "Step",
      groups: [{ name: "Group", choices: [{ name: "Recommended A", idx: 0 }] }],
    },
  ]);
  const defaultOptions = noPreset.result.steps[0].optionalFileGroups.group[0].options;
  const recommended = defaultOptions.find((option) => option.name === "Recommended A");
  const optional = defaultOptions.find((option) => option.name === "Optional B");
  assert.equal(recommended.selected, true);
  assert.equal(optional.selected, false);

  const csharpSupported = await client.moduleCall("node-helper", "call", {
    node: process.execPath,
    script: helperScript,
    request: {
      method: "fomod.testSupported",
      params: {
        nativeLibrary: fomodPackage,
        files: ["fomod/script.cs", "fomod/info.xml"],
        allowedTypes: ["XmlScript", "CSharpScript", "Basic"],
      },
    },
  });
  assert.equal(csharpSupported.result.supported, false);
  await assert.rejects(
    client.moduleCall("node-helper", "call", {
      node: process.execPath,
      script: helperScript,
      request: {
        method: "fomod.resolve",
        params: {
          nativeLibrary: fomodPackage,
          files: ["fomod/script.cs", "fomod/info.xml"],
          scriptPath: packageRoot,
          pluginPath: packageRoot,
          validate: false,
        },
      },
    }),
    /C# FOMOD scripts are not supported/,
  );

  const archive = join(root, "fomod-worker.zip");
  execFileSync(sevenZip, ["a", "-tzip", archive, "fomod", "Data"], {
    cwd: packageRoot,
    stdio: "ignore",
  });
  const coreExtract = join(root, "fomod-worker-extract");
  const coreDestination = join(root, "fomod-worker-staging");
  const started = await client.collectionStart({
    collectionId: "fomod",
    gameId: "skyrimse",
    concurrency: 1,
    maxRetries: 0,
    members: [
      {
        ruleIndex: 0,
        ruleId: "requires_fomod",
        type: "requires",
        phase: 1,
        ignored: false,
        status: "pending",
        task: {
          modId: "FomodWorker",
          extract: { archive, destination: coreExtract, sevenZip },
          install: { temp: coreExtract, destination: coreDestination, instructions: [] },
          fomod: {
            nativeLibrary: fomodPackage,
            node: process.execPath,
            script: helperScript,
            preset: [
              {
                name: "Step",
                groups: [{ name: "Group", choices: [{ name: "Recommended A", idx: 0 }] }],
              },
            ],
            pluginPath: packageRoot,
            scriptPath: packageRoot,
          },
        },
      },
    ],
  });
  const worker = await client.collectionExecute({
    jobId: started.jobId,
    ruleId: "requires_fomod",
  });
  assert.equal(worker.members[0].status, "installed", worker.members[0].error);
  assert.deepEqual(worker.members[0].result.files.sort(), ["a.txt", "required.txt"]);
  assert.equal(readFileSync(join(coreDestination, "a.txt"), "utf8"), "a");
  assert.equal(readFileSync(join(coreDestination, "required.txt"), "utf8"), "required");

  const rawExtract = join(root, "fomod-without-choices-extract");
  const rawDestination = join(root, "fomod-without-choices-staging");
  const rawStart = await client.collectionStart({
    collectionId: "fomod-without-choices",
    gameId: "skyrimse",
    concurrency: 1,
    maxRetries: 0,
    members: [
      {
        ruleIndex: 0,
        ruleId: "requires_fomod_raw",
        type: "requires",
        phase: 1,
        ignored: false,
        status: "pending",
        task: {
          modId: "FomodWithoutChoices",
          extract: { archive, destination: rawExtract, sevenZip },
          install: { temp: rawExtract, destination: rawDestination, instructions: [] },
          copyAll: true,
        },
      },
    ],
  });
  const rawWorker = await client.collectionExecute({
    jobId: rawStart.jobId,
    ruleId: "requires_fomod_raw",
  });
  assert.equal(rawWorker.members[0].status, "pending");
  assert.equal(rawWorker.members[0].fallback, true);
  assert.equal(rawWorker.members[0].executor, "typescript");

  const defaultExtract = join(root, "fomod-defaults-extract");
  const defaultDestination = join(root, "fomod-defaults-staging");
  const defaultStart = await client.collectionStart({
    collectionId: "fomod-defaults",
    gameId: "skyrimse",
    concurrency: 1,
    maxRetries: 0,
    members: [
      {
        ruleIndex: 0,
        ruleId: "requires_fomod_defaults",
        type: "requires",
        phase: 1,
        ignored: false,
        status: "pending",
        task: {
          modId: "FomodDefaults",
          extract: { archive, destination: defaultExtract, sevenZip },
          install: {
            temp: defaultExtract,
            destination: defaultDestination,
            instructions: [],
          },
          fomod: {
            nativeLibrary: fomodPackage,
            node: process.execPath,
            script: helperScript,
            pluginPath: packageRoot,
            scriptPath: packageRoot,
          },
        },
      },
    ],
  });
  const defaultWorker = await client.collectionExecute({
    jobId: defaultStart.jobId,
    ruleId: "requires_fomod_defaults",
  });
  assert.equal(defaultWorker.members[0].status, "installed", defaultWorker.members[0].error);
  assert.deepEqual(defaultWorker.members[0].result.files.sort(), ["a.txt", "required.txt"]);

  const server = createServer((_request, response) => {
    response.writeHead(200, { "content-type": "application/json" });
    response.end(JSON.stringify({ mod_id: 7, file_id: 9 }));
  });
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  try {
    const metadata = await client.moduleCall("node-helper", "call", {
      node: process.execPath,
      script: helperScript,
      request: {
        method: "metadata.lookup",
        params: { endpoint: `http://127.0.0.1:${server.address().port}/meta.json` },
      },
    });
    assert.equal(metadata.result.metadata.mod_id, 7);
  } finally {
    server.close();
  }
});

test("runs a collection with prebuilt member tasks", async () => {
  const work = join(root, "prebuilt-run");
  const source = join(work, "source");
  const temp = join(work, "temp");
  const destination = join(work, "staging", "ModA");
  mkdirSync(source, { recursive: true });
  writeFileSync(join(source, "prebuilt.txt"), "prebuilt-content");
  const archive = join(work, "prebuilt.zip");
  execFileSync(sevenZip, ["a", "-tzip", archive, "prebuilt.txt"], {
    cwd: source,
    stdio: "ignore",
  });

  const started = await client.collectionStart({
    collectionId: "prebuilt",
    gameId: "skyrimse",
    concurrency: 1,
    maxRetries: 0,
    members: [
      {
        ruleIndex: 0,
        ruleId: "requires_prebuilt",
        type: "requires",
        phase: 1,
        ignored: false,
        status: "pending",
        task: {
          modId: "PrebuiltMod",
          extract: { archive, destination: temp, sevenZip },
          install: { temp, destination, instructions: [] },
          copyAll: true,
        },
      },
    ],
  });
  assert.equal(started.status.members[0].hasTask, true);

  await client.collectionRun(started.jobId);
  const deadline = Date.now() + 10000;
  let status = await client.collectionStatus(started.jobId);
  while (!status.complete && Date.now() < deadline) {
    await new Promise((resolve) => setTimeout(resolve, 50));
    status = await client.collectionStatus(started.jobId);
  }
  assert.equal(status.complete, true);
  assert.equal(status.members[0].status, "installed", status.members[0].error);
  assert.equal(status.members[0].result.modId, "PrebuiltMod");
  const registration = await client.collectionRegistration(started.jobId);
  assert.equal(registration.members.length, 1);
  assert.equal(registration.members[0].ruleId, "requires_prebuilt");
  assert.equal(registration.members[0].modId, "PrebuiltMod");
  assert.equal(registration.members[0].installationPath, destination);
  assert.deepEqual(registration.members[0].files, ["prebuilt.txt"]);
  assert.equal(readFileSync(join(destination, "prebuilt.txt"), "utf8"), "prebuilt-content");
});

test("runs task-bearing members and leaves taskless ones for the fallback", async () => {
  const work = join(root, "mixed-run");
  const source = join(work, "source");
  const temp = join(work, "temp");
  const destination = join(work, "staging", "CoreMember");
  mkdirSync(source, { recursive: true });
  writeFileSync(join(source, "core.txt"), "core-content");
  const archive = join(work, "mixed.zip");
  execFileSync(sevenZip, ["a", "-tzip", archive, "core.txt"], {
    cwd: source,
    stdio: "ignore",
  });

  const started = await client.collectionStart({
    collectionId: "mixed",
    gameId: "skyrimse",
    concurrency: 2,
    maxRetries: 0,
    members: [
      {
        ruleIndex: 0,
        ruleId: "requires_core",
        type: "requires",
        phase: 0,
        ignored: false,
        status: "pending",
        task: {
          modId: "CoreMember",
          extract: { archive, destination: temp, sevenZip },
          install: { temp, destination, instructions: [] },
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

  assert.equal(status.runComplete, true);
  assert.equal(status.complete, false);
  assert.equal(status.members[0].status, "installed", status.members[0].error);
  assert.equal(status.members[0].executor, "core");
  assert.equal(status.members[1].status, "pending");
  assert.equal(status.members[1].executor, "typescript");
  const registration = await client.collectionRegistration(started.jobId);
  assert.deepEqual(
    registration.members.map((member) => member.ruleId),
    ["requires_core"],
  );

  const next = await client.collectionNext(started.jobId);
  assert.deepEqual(
    next.members.map((member) => member.ruleId),
    ["requires_typescript"],
  );
  assert.equal(next.members[0].status, "queued");
  assert.equal(next.members[0].executor, "typescript");

  status = await client.collectionMember({
    jobId: started.jobId,
    ruleId: "requires_typescript",
    status: "installed",
  });
  assert.equal(status.complete, true);
});

test("normalizes Nexus md5 metadata into lookup results", async () => {
  const server = createServer((request, response) => {
    assert.match(request.url, /md5_search\/[a-f0-9]{32}\.json/);
    response.writeHead(200, { "content-type": "application/json" });
    response.end(
      JSON.stringify([
        {
          mod: {
            mod_id: 123,
            domain_name: "skyrimspecialedition",
            category_id: 42,
            description: "A test mod",
            author: "Tester",
          },
          file_details: {
            file_id: 456,
            name: "Main File",
            file_name: "test-mod-1.0.zip",
            version: "1.0",
            size: 2,
          },
        },
      ]),
    );
  });
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  try {
    const script = join(dirname(corePath), "..", "..", "helpers", "vortex-helper.mjs");
    const md5 = "0123456789abcdef0123456789abcdef";
    const result = await client.metadataLookup({
      node: process.execPath,
      script,
      env: { ELECTRON_RUN_AS_NODE: "1" },
      endpoint: `http://127.0.0.1:${server.address().port}/md5_search/${md5}.json`,
      md5,
      fileSize: 2048,
      gameId: "skyrimse",
      gameDomain: "skyrimspecialedition",
    });
    assert.equal(result.lookups.length, 1);
    assert.equal(result.lookups[0].key, `hash:${md5}:2048:skyrimse:`);
    assert.equal(result.lookups[0].value.fileName, "test-mod-1.0.zip");
    assert.equal(result.lookups[0].value.source, "nexus");
    assert.equal(result.lookups[0].value.sourceURI, "nxm:/skyrimspecialedition/mods/123/files/456");
    assert.equal(result.lookups[0].value.details.modId, "123");
  } finally {
    server.close();
  }
});

test("forwards auth cookies and proxies downloads through the core", async () => {
  const payload = Buffer.from("proxy-auth-payload");
  let proxiedUrl;
  let authorization;
  let cookie;
  const proxy = createServer((request, response) => {
    proxiedUrl = request.url;
    authorization = request.headers.authorization;
    cookie = request.headers.cookie;
    response.writeHead(200, { "content-length": payload.length });
    response.end(payload);
  });
  await new Promise((resolve) => proxy.listen(0, "127.0.0.1", resolve));

  try {
    const directory = join(root, "downloads-proxy-auth");
    await client.downloadAdd({
      id: "dl-proxy-auth",
      directory,
      fileName: "protected.bin",
      urls: ["http://origin.invalid/protected.bin"],
      proxy: `http://127.0.0.1:${proxy.address().port}`,
      headers: {
        Authorization: "Bearer proxy-token",
        Cookie: "session=proxy-cookie",
      },
      curl: process.env.VORTEX_CURL_PATH ?? "curl",
      maxRetries: 0,
    });
    const status = await waitForState("dl-proxy-auth", ["finished", "failed"]);
    assert.equal(status.state, "finished", status.error);
    assert.equal(
      proxiedUrl,
      "http://origin.invalid/protected.bin",
      "curl used the proxy with an absolute URL",
    );
    assert.equal(authorization, "Bearer proxy-token");
    assert.equal(cookie, "session=proxy-cookie");
    assert.deepEqual(readFileSync(join(directory, "protected.bin")), payload);
  } finally {
    proxy.close();
  }
});

test("collection.next returns only TypeScript members while core tasks are pending", async () => {
  const work = join(root, "executor-filter");
  const started = await client.collectionStart({
    collectionId: "executor-filter",
    gameId: "skyrimse",
    concurrency: 1,
    maxRetries: 0,
    members: [
      {
        ruleIndex: 0,
        ruleId: "requires_core",
        type: "requires",
        phase: 0,
        ignored: false,
        status: "pending",
        task: {
          modId: "CoreOnly",
          install: {
            temp: work,
            destination: join(work, "staging"),
            instructions: [],
          },
          copyAll: true,
        },
      },
      {
        ruleIndex: 1,
        ruleId: "requires_typescript",
        type: "requires",
        phase: 0,
        ignored: false,
        status: "pending",
      },
    ],
  });

  const next = await client.collectionNext(started.jobId);
  assert.deepEqual(
    next.members.map((member) => member.ruleId),
    ["requires_typescript"],
  );
  assert.equal(next.members[0].executor, "typescript");
  await client.collectionCancel(started.jobId);
});

test("hands failed core tasks to the TypeScript fallback", async () => {
  const work = join(root, "failed-fallback");
  const started = await client.collectionStart({
    collectionId: "failed-fallback",
    gameId: "skyrimse",
    concurrency: 1,
    maxRetries: 0,
    members: [
      {
        ruleIndex: 0,
        ruleId: "requires_broken",
        type: "requires",
        phase: 0,
        ignored: false,
        status: "pending",
        task: {
          modId: "Broken",
          extract: {
            archive: join(work, "missing.zip"),
            destination: join(work, "extract"),
            sevenZip,
          },
          install: {
            temp: join(work, "extract"),
            destination: join(work, "staging"),
            instructions: [],
          },
          copyAll: true,
        },
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

  assert.equal(status.runComplete, true);
  assert.equal(status.complete, false);
  assert.equal(status.members[0].status, "pending");
  assert.equal(status.members[0].fallback, true);
  assert.equal(status.members[0].executor, "typescript");

  const next = await client.collectionNext(started.jobId);
  assert.deepEqual(
    next.members.map((member) => member.ruleId),
    ["requires_broken"],
  );
  status = await client.collectionMember({
    jobId: started.jobId,
    ruleId: "requires_broken",
    status: "installed",
  });
  assert.equal(status.complete, true);
});

test("runs C# FOMOD through the external host contract with explicit consent", async () => {
  const work = join(root, "fomod-csharp-host");
  const source = join(work, "source");
  const archive = join(work, "csharp.zip");
  const extract = join(work, "extract");
  const staging = join(work, "staging");
  const helperScript = join(dirname(corePath), "..", "..", "helpers", "vortex-helper.mjs");
  const hostScript = join(work, "fake-csharp-host.mjs");
  mkdirSync(join(source, "fomod"), { recursive: true });
  writeFileSync(join(source, "fomod", "script.cs"), "// scripted fixture");
  writeFileSync(
    hostScript,
    `import { readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
const index = process.argv.indexOf("--request");
const request = JSON.parse(readFileSync(process.argv[index + 1], "utf8"));
writeFileSync(join(request.scriptPath, "generated.txt"), "csharp-output");
process.stdout.write(JSON.stringify({ result: { instructions: [
  { type: "copy", source: "generated.txt", destination: "generated.txt" }
] } }));`,
  );
  execFileSync(sevenZip, ["a", "-tzip", archive, "fomod"], { cwd: source, stdio: "ignore" });

  const start = (allowCSharpScripts) =>
    client.collectionStart({
      collectionId: `fomod-csharp-${allowCSharpScripts}`,
      gameId: "skyrimse",
      concurrency: 1,
      maxRetries: 0,
      members: [
        {
          ruleIndex: 0,
          ruleId: "requires_csharp",
          type: "requires",
          phase: 1,
          ignored: false,
          status: "pending",
          task: {
            modId: "CSharpMod",
            extract: { archive, destination: extract, sevenZip },
            install: { temp: extract, destination: staging, instructions: [] },
            fomod: {
              nativeLibrary: "unused",
              node: process.execPath,
              script: helperScript,
              scriptPath: extract,
              pluginPath: extract,
              allowCSharpScripts,
              csharpHost: process.execPath,
              csharpHostArgs: [hostScript],
            },
          },
        },
      ],
    });

  const denied = await start(false);
  const deniedWorker = await client.collectionExecute({
    jobId: denied.jobId,
    ruleId: "requires_csharp",
  });
  assert.equal(deniedWorker.members[0].status, "pending");
  assert.equal(deniedWorker.members[0].fallback, true);
  assert.match(deniedWorker.members[0].fallbackError, /explicit consent/);

  const allowed = await start(true);
  const allowedWorker = await client.collectionExecute({
    jobId: allowed.jobId,
    ruleId: "requires_csharp",
  });
  assert.equal(allowedWorker.members[0].status, "installed", allowedWorker.members[0].error);
  assert.deepEqual(allowedWorker.members[0].result.files, ["generated.txt"]);
  assert.equal(readFileSync(join(staging, "generated.txt"), "utf8"), "csharp-output");
});

test("owns the free-user NXM queue state", async () => {
  await client.nxmQueueClear();
  const first = await client.nxmQueueAdd({
    input: "nxm://skyrimspecialedition/mods/100/files/500",
    gameDomain: "skyrimspecialedition",
    type: "mod",
    modId: 100,
    fileId: 500,
  });
  assert.equal(first.awaitingLink, false);
  await client.nxmQueueAdd({
    input: "nxm://skyrimspecialedition/mods/200/files/600",
    gameDomain: "skyrimspecialedition",
    type: "mod",
    modId: 200,
    fileId: 600,
  });
  // the same input is one queue row, not a duplicate
  await client.nxmQueueAdd({
    input: "nxm://skyrimspecialedition/mods/100/files/500",
    gameDomain: "skyrimspecialedition",
    type: "mod",
    modId: 100,
    fileId: 500,
  });

  let list = await client.nxmQueueList();
  assert.deepEqual(
    list.entries.map((entry) => entry.input),
    [
      "nxm://skyrimspecialedition/mods/100/files/500",
      "nxm://skyrimspecialedition/mods/200/files/600",
    ],
  );
  assert.equal(await client.nxmQueueMatch("skyrimspecialedition", 100, 500), null);

  await client.nxmQueueUpdate("nxm://skyrimspecialedition/mods/100/files/500", true);
  const match = await client.nxmQueueMatch("skyrimspecialedition", 100, 500);
  assert.equal(match.input, "nxm://skyrimspecialedition/mods/100/files/500");
  assert.equal(match.awaitingLink, true);

  await client.nxmQueueRemove("nxm://skyrimspecialedition/mods/100/files/500");
  list = await client.nxmQueueList();
  assert.deepEqual(
    list.entries.map((entry) => entry.input),
    ["nxm://skyrimspecialedition/mods/200/files/600"],
  );
  await client.nxmQueueClear();
  assert.deepEqual((await client.nxmQueueList()).entries, []);
});
