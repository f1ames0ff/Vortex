# Vortex native core

C++ deployment core for the post-Electron Vortex architecture. This is the first
vertical slice: file index, deployment planner, hardlink/symlink executor, journal and
manifest compatibility, plus the feature-flagged Electron integration that runs
hardlink/symlink deployments through the core process.

## Requirements

- CMake 3.28+
- Ninja
- Clang 18+ (the project is configured and tested with clang)
- Linux for the current test suite; the implementation uses `std::filesystem`
  for the hardlink backend and is intended to build on Windows as well.

## Build and test

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev --output-on-failure
```

The CTest suite includes the library tests, a Python protocol smoke test, and a
Node client test. The latter two spawn `vortex-core`, deploy a mod and purge it.

Release and sanitizer builds:

```sh
cmake --preset release && cmake --build --preset release
cmake --preset asan && cmake --build --preset asan
./build/asan/vortex-deployment-tests
```

## Core process

`vortex-core` is a long-lived process that speaks line-delimited JSON on
stdin/stdout. This is the prototype of the UI/core boundary: the shell or
Electron main process spawns it once and exchanges requests without embedding
the deployment logic in the UI process.

Supported requests:

```json
{"id":1,"method":"ping","params":{}}
{"id":2,"method":"scan","params":{"staging":"/staging"}}
{"id":3,"method":"plan","params":{"staging":"/staging","target":"/game/Data","manifest":"/game/Data/vortex.deployment.json","mods":[{"installationPath":"ModA"}]}}
{"id":4,"method":"deploy","params":{"staging":"/staging","target":"/game/Data","manifest":"/game/Data/vortex.deployment.json","mods":[{"installationPath":"ModA"}],"instance":"...","method":"hardlink","game":"skyrimse"}}
{"id":4,"method":"deploy.single","params":{"staging":"/staging","target":"/game/Data","manifest":"/game/Data/vortex.deployment.json","instance":"...","method":"move_activator","game":"skyrimse","enable":true,"mod":{"installationPath":"ModA","target":"Data"}}}
{"id":4,"method":"deployment.externalChanges","params":{"staging":"/staging","target":"/game/Data","manifest":"/game/Data/vortex.deployment.json","method":"hardlink_activator","files":[...]}}
{"id":5,"method":"purge","params":{"staging":"/staging","target":"/game/Data","manifest":"/game/Data/vortex.deployment.json"}}
{"id":6,"method":"staging.status","params":{"staging":"/staging","mods":[{"id":"mod-a","installationPath":"ModA"},{"id":"mod-b","installationPath":"ModB"}]}}
{"id":7,"method":"install.apply","params":{"temp":"/tmp/ModA.extracted","destination":"/staging/ModA","instructions":[{"type":"hardlink","source":"textures/a.dds","destination":"textures/a.dds"},{"type":"write","destination":"meta.ini","data":"..."}]}}
{"id":8,"method":"game.registry","params":{"directory":"/games"}}
{"id":9,"method":"archive.list","params":{"archive":"/downloads/ModA.7z","sevenZip":"/path/to/7z"}}
{"id":10,"method":"archive.extract","params":{"archive":"/downloads/ModA.7z","destination":"/tmp/ModA.extracted","sevenZip":"/path/to/7z"}}
{"id":11,"method":"hash.files","params":{"algorithm":"md5","files":["/tmp/ModA/a.txt"]}}
{"id":12,"method":"deps.matchMod","params":{"reference":{"logicalFileName":"ModA"},"mods":[...]}}
{"id":13,"method":"deps.matchDownload","params":{"reference":{"tag":"rule-tag"},"downloads":[...]}}
{"id":14,"method":"deps.resolve","params":{"rules":[...],"mods":[...],"downloads":[...]}}
{"id":15,"method":"collection.plan","params":{"rules":[...],"mods":[...],"downloads":[...]}}
{"id":16,"method":"plugins.scan","params":{"directories":[{"path":"/game/Data","extensions":[".esm",".esp",".esl"]}]}}
{"id":17,"method":"plugins.loadOrder.read","params":{"directory":"/game/AppData"}}
{"id":18,"method":"plugins.loadOrder.write","params":{"directory":"/game/AppData","plugins":["*Skyrim.esm"],"loadOrder":["Skyrim.esm"]}}
{"id":19,"method":"collection.start","params":{"collectionId":"col","gameId":"skyrimse","concurrency":5,"members":[...]}}
{"id":20,"method":"collection.next","params":{"jobId":"collection_..."}}
{"id":21,"method":"collection.member","params":{"jobId":"collection_...","ruleId":"requires_...","status":"installed","modId":"ModA"}}
{"id":22,"method":"collection.status","params":{"jobId":"collection_..."}}
{"id":23,"method":"collection.pause","params":{"jobId":"collection_..."}}
{"id":24,"method":"collection.cancel","params":{"jobId":"collection_..."}}
{"id":24,"method":"collection.run","params":{"jobId":"collection_..."}}
{"id":24,"method":"module.load","params":{"path":"/modules/libmetadata.so"}}
{"id":24,"method":"module.list","params":{}}
{"id":24,"method":"module.call","params":{"module":"echo","method":"echo","params":{"value":42}}}
{"id":25,"method":"download.add","params":{"directory":"/downloads","fileName":"ModA.7z","urls":["https://example.com/ModA.7z"],"maxRetries":3}}
{"id":26,"method":"download.status","params":{"id":"download_..."}}
{"id":27,"method":"download.pause","params":{"id":"download_..."}}
{"id":28,"method":"download.resume","params":{"id":"download_..."}}
{"id":29,"method":"download.cancel","params":{"id":"download_..."}}
{"id":30,"method":"download.list","params":{}}
{"id":31,"method":"shutdown","params":{}}
```

Every response echoes `id` and contains either `result` or `error`. Commands
that modify files also write a journal under the staging folder. The process
catches per-request exceptions and keeps serving; file errors are returned as
per-file failures in the report. Passing `"includeFiles": true` to `plan` and
`deploy` returns the deployed file list, which the renderer uses to keep its
manifest bookkeeping in sync.

`staging.status` inspects a batch of mod folders in parallel inside the core.
It replaces the renderer's per-mod `readdir` walk, which used to saturate the
UI thread's libuv pool on thousand-mod collections. Absolute paths and `..`
components are rejected.

`install.apply` applies the generic installer's file instructions: `mkdir`,
`copy`, `hardlink` (falling back to copy across devices) and `write`.
`InstallManager` routes the generic installer's `mkdir`/`copy` set through it
when the native path is enabled; FOMOD choice resolution and `generatefile`
still run in the renderer. Sources
are resolved inside `temp`, destinations inside `destination`; absolute paths,
`..`, symlinked components and (on Windows) device/ADS names are rejected.
Hardlink/copy targets are grouped by destination, so duplicate instructions are
applied in request order instead of racing. The install journal records a
durable `install.begin`, per-failure `install.failure` entries, and a durable
`install.complete` or `install.failed`. It defaults to
`<staging>/.vortex-install-journal`. FOMOD script execution stays in the
existing .NET host; the core only receives its resolved instructions.

Deployment supports `hardlink`, `symlink` and `move`. Move deployment writes a
`<source>.vortex_lnk` JSON placeholder into staging, backs up an existing game
file as `.vortex_backup`, then moves the staged file to the target. The planner
rebuilds the desired deployment from those placeholders, so a move deployment
can be re-planned without rescanning the now-empty staging files. Purge moves
each target back to staging, removes the placeholder and restores the backup;
cross-device moves fall back to copy + delete.

`deployment.externalChanges` compares every manifest entry with staging and
the target directory. It reports `srcdeleted` when staging lost a file the
deployment can restore from the target, `deleted` when the target file
disappeared, and `refchange` when both exist but the target no longer links
to (or points at) the staged source. Move placeholders are resolved to their
target, and the result is deduplicated with deleted entries taking priority.
The renderer's external-change scan calls this first and keeps the activator's
TypeScript implementation as fallback.

`deploy.single` merges one mod into the existing manifest and applies only that
mod's added/changed/removed entries, leaving every other source untouched. It
accepts a blacklist of paths owned by later mods in load order, which
`InstallManager`-adjacent activation code uses to preserve priority during
single-mod activation.

`archive.list` and `archive.extract` invoke the existing 7-Zip binary; the core
does not embed or reimplement the archiver. Every listed path is validated
before extraction, and archives containing absolute paths, `..` or symlinks are
rejected. `archive.extract` passes `-snl-` so symlinks are not recreated.
`hash.files` streams MD5 or XXH64 over a batch on worker threads; XXH64 is
returned as the same big-endian base64 digest the renderer uses.

`deps.matchMod`/`deps.matchDownload` implement the TS reference matching
priority: id/idHint, collection tags, file MD5, repo ids, logical file names,
archive-name expressions, version ranges and install-spec equality.
`deps.resolve` fills the per-rule mod/download/status triple from reality;
`collection.plan` groups those rules by authored phase and adds the trailing
`OPTIONAL_PHASE` (666), required/optional counts and the reconstructed statuses.
The matcher covers the common semver range forms (`^`, `~`, comparators,
wildcards, `||`); hyphen ranges and full minimatch character classes are not
implemented yet.

`collection.start` turns a plan into a core-owned phase queue. Members may
carry an execution `task`:

```json
{
    "download": { "directory": "/downloads", "fileName": "ModA.7z", "urls": ["https://..."] },
    "extract": { "destination": "/temp/ModA", "sevenZip": "/usr/bin/7z" },
    "install": {
        "temp": "/temp/ModA",
        "destination": "/staging/ModA",
        "instructions": [{ "type": "copy", "source": "a.txt", "destination": "a.txt" }]
    },
    "modId": "ModA"
}
```

`collection.task` attaches a task to a member of a running job, and
`collection.execute` executes just that member synchronously, for renderer
orchestrators that still drive phases themselves.

`InstallManager` now uses that path for collection dependencies whose archive
has no FOMOD and no override instructions: it lists the archive in the core,
attaches a copy-all task, executes it into a prepared staging directory, then
runs the existing install path only to register the mod record and move the
prepared directory into place. FOMOD, installer-choice, file-list and patched
members stay on the TypeScript path.

`collection.run` starts core workers that repeatedly take members from
`collection.next`, execute download → extract → install, and report back
through the same state machine. Workers honour concurrency, pause/cancel,
per-member retries (`maxRetries`) and a progress-based stall timeout, and
advance phases only when all members are terminal. Without `collection.run`,
renderer executors can keep using `collection.next`/`collection.member`.
`InstallDriver` starts `collection.run` automatically when every non-ignored
member reports `hasTask: true`; otherwise it keeps the pull-based
`collection.next` lifecycle.
`collection.next` returns up to the job's concurrency from the allowed phase
and marks the selected members queued; TypeScript executors report progress
back with `collection.member`. A phase advances only once every member is
terminal (`installed`, `failed` or `ignored`), with `recommends` members in the
trailing phase. `collection.status` exposes counts, completion, pause/cancel
state and a stall flag based on the last progress timestamp. The queue lives in
memory in the core; `InstallDriver` falls back to its own scheduling when the
job cannot be created.

`plugins.scan` scans top-level directories for the requested plugin
extensions, preserving file-name casing while exposing the lower-case plugin
id and ghost flag. `plugins.loadOrder.read`/`write` handle `plugins.txt` and
`loadorder.txt`, including the `Plugins.txt` capitalisation the game creates in
Proton prefixes, and write the Vortex header with CRLF rows atomically.

`download.add` creates a core-owned queue entry; a scheduler thread starts up
to `concurrency` transfers and the platform `curl` binary performs the
transfer. The core owns retries, pause/resume (via `--continue-at`), cancel,
progress bytes and the atomic `.part` -> final rename. Requests accept custom
headers, a user-agent, a proxy URL and `atomic: false` for callers that own the
destination and resume partial files directly. Only `http(s)` URLs and safe
relative file names are accepted; headers are checked for line breaks.

Two callers use it: `InstallManager` for direct HTTP(S) dependency downloads
with a known file name, and the main-process `HybridDownloadManager`, which
routes `download:start` through the core using the probe URL and headers the
renderer resolved. Nexus CDN URLs therefore keep their signed token, and a
configured cookie jar is serialized into a `Cookie` header. The TypeScript
`got` manager remains the fallback when the core is unavailable, a bandwidth
limit is configured, or a resume follows a core restart. Set
`VORTEX_NATIVE_DOWNLOADS=0` to force the TypeScript path. Native downloads
share the `VORTEX_NATIVE_DEPLOY=1` feature gate with the rest of the core.

## C-ABI modules

Shared libraries can expose `vortex_module_entry` returning a
`vortex_module_v1` from `include/vortex/core/module_abi.h`. The ABI is
versioned (`VORTEX_MODULE_ABI_VERSION`), carries a name/version and a single
JSON-in/JSON-out `invoke` function, and owns result/error strings through
`free_string`. The core loads modules with `module.load`, lists them with
`module.list` and calls them with `module.call`.

This is the boundary intended for domain code that must stay outside the core
or be supplied as a plugin. `modules/node_helper.cpp` is the first wrapper: it
loads `libvortex_node_helper_module` and runs `helpers/vortex-helper.mjs`,
which exposes `fomod.testSupported`, `fomod.resolve` and `metadata.lookup`.
The FOMOD helper loads the existing Native AOT
`@nexusmods/fomod-installer-native` addon and resolves a preset headlessly;
metadata lookup performs the Nexus files request. The core still does not embed
the .NET FOMOD installer or the LevelDB metadata store.

`ping` also returns `protocol` and `capabilities`, so the shell can reject a
stale core before calling a method it does not support.

`game.registry` scans the immediate child directories of `directory` for a
`game.toml`, loads each descriptor, and returns them sorted by directory name.
A descriptor uses the game-extension vocabulary:

```toml
id = "skyrimse"
name = "Skyrim Special Edition"
nexusDomain = "skyrimspecialedition"
steamAppId = 489830
requiredFiles = ["SkyrimSE.exe"]
hooks = ["gamebryo-plugin-management"]

[deployment]
targets = ["Data"]
pluginPath = "Data"
iniPath = "SkyrimPrefs.ini"
```

The reader also accepts `[game]` and `[requirements]` grouping (`game.id`,
`requirements.files`), dotted keys, comments, booleans and string/integer
arrays. Every descriptor is returned with its `sourcePath`; optional fields are
omitted from the JSON when they are not set.

## Electron integration

The main process owns one long-lived `vortex-core` child and exposes it to the
renderer over typed IPC:

- `nativeDeploy:status`
- `nativeDeploy:plan`
- `nativeDeploy:deploy`
- `nativeDeploy:purge`
- `nativeDeploy:stagingStatus`
- `nativeDeploy:installApply`

The preload API is `window.api.nativeDeploy`. The feature gate is
`VORTEX_NATIVE_DEPLOY=1`; `VORTEX_CORE_PATH` overrides the binary path. Without
`VORTEX_CORE_PATH`, the app looks for `native/build/dev/vortex-core` (then
`release`) in a checkout and `resources/native/vortex-core` in a packaged app.

Build the core and start the app:

```sh
cd native
cmake --preset dev
cmake --build --preset dev

cd ..
VORTEX_NATIVE_DEPLOY=1 pnpm run start
```

To operate on the installed app's profiles, mods and staging folder instead of
the dev build's empty state, use the production-data launcher:

```sh
pnpm run start:prod          # installed app's data, TypeScript deployment
pnpm run start:prod:native   # installed app's data, native deployment via vortex-core
```

`VORTEX_PROD_DATA=1` resolves the production user-data directory
(`~/.config/Vortex` on Linux, `%APPDATA%\Vortex` on Windows). An explicit value
is treated as a path, e.g. `VORTEX_PROD_DATA=/path/to/user-data`. The existing
`--user-data <path>` CLI flag still works too.

When enabled, `hardlink_activator` and `symlink_activator` deployments and purges are routed through
the core, and `ModList` asks the core for staging status instead of walking
every installed mod in the renderer. Other deployment methods, a missing
binary, or a disabled flag keep the existing TypeScript implementation. The
renderer still writes its own manifest/backup copies afterwards, so manifests
stay compatible with the old path.

Collection resolution is also moving across: `gatherDependencies` asks
`deps.resolve` for the top-level rule matches before its metadata lookups, and
`InstallDriver` uses `collection.plan` to compute the missing-member set, then
starts a `collection.start` phase queue and reports installed members back to
it. `InstallManager` creates the required-phase job as well: downloaded
dependencies are reported as `downloaded`, `collection.next` selects the work
that may start, and completion/failure/ignore statuses flow back through
`collection.member`. The TypeScript phase tracker remains the fallback, and the
actual download/install executor is still TypeScript.

Single-mod activation now calls `deploy.single` with the activator id and the
later-source blacklist; full deployment already routes `move_activator`,
`symlink_activator` and `hardlink_activator` through the same executor.
The gamebryo plugin manager sends its Data-folder and enabled-mod directory
scans to `plugins.scan`; `PluginPersistor` reads and writes the order files
through `plugins.loadOrder.*`. The TypeScript implementations remain the
fallback when the core is disabled or lacks a capability.

Known integration gaps for the next slice:

- `InstallDriver` uses `collection.run` only for jobs whose members all carry
  tasks; current collections still attach tasks per member in `InstallManager`,
  so phase scheduling normally stays on `collection.next`. Complex members
  remain in TS.
- Single-mod activation and move deployment use the core when its
  `deploy.single`/`deploy.move` capabilities are present; otherwise they fall
  back to the TypeScript activator.
- The `node-helper` C-ABI module resolves FOMOD presets and metadata requests.
  `InstallManager` now uses `fomod.resolve` for collection members with a
  FOMOD preset: it extracts the archive, resolves copy instructions, attaches a
  `collection.task` and executes it in the core. Members without a preset or
  with unsupported instructions still use the TypeScript installer.
- The packaged app runs the helper through Electron-as-Node and ships the
  Native AOT FOMOD addon plus `node-gyp-build` under `resources/native/helpers`.
  `VORTEX_NODE_PATH` can point at a standalone Node binary for environments
  where Electron-as-Node is unavailable.
- External-change detection is in the core for hardlink/symlink/move; the
  other deployment methods still use their TypeScript activator.
- Downloads use the external `curl` binary. Linux distributions ship it;
  Windows 10+ includes `curl.exe`. Set `VORTEX_CURL_PATH` to override the
  binary. Curl honours the standard `http_proxy`/`https_proxy` environment
  variables; an explicit proxy can be passed per request.
- The core download queue is in-memory and has no token-bucket bandwidth
  limiter; downloads started while a bandwidth cap is configured stay on the
  TypeScript `got` manager.
- Exact skip-list entries are passed to the core; minimatch patterns from
  `ignoreDeploy` still rely on the TypeScript path.
- The packaged app does not embed a Windows build of the core yet.

## Packaging

The release core is built and staged into the packaged app automatically:

```sh
pnpm native:build          # cmake --preset release + build
pnpm package:nosign
pnpm verify:packaged-native
```

`prepare-dist-package.mjs` copies `native/build/release/vortex-core[.exe]`
into `src/main/dist/temp/native`, and electron-builder's `extraResources`
places it at `resources/native/vortex-core[.exe]` in the packaged app. That is
the path `resolveDefaultCorePath` probes. Set `VORTEX_SKIP_NATIVE_CORE=1` only
for a deliberate TypeScript-only package; normal packaging fails if the core
is missing.

## First slice

- Manifest read/write in the existing Vortex JSON format.
- Per-mod file index with directory-mtime invalidation and persisted cache.
- Planner producing the desired deployment and `added` / `removed` / `changed`
  differences against the previous manifest.
- Hardlink deploy, no-op deploy, source/content replacement, backups and purge.
- `.vortex_backup` restore on purge.
- Write-ahead JSONL journal for every deployment/purge.
- Batched staging-folder status (`ok` / `empty` / `missing` / `error`) with
  path-traversal rejection.
- Generic installer instruction executor (`mkdir`, `copy`, `hardlink`,
  `write`) with bounded concurrency and its own JSONL journal.
- CLI for manual testing.

The file index validates previously deployed files against the manifest, so an
in-place edit of a deployed file is detected even when the mod directory mtime
does not change. New files added inside a nested directory still require the mod
directory mtime to change; a filesystem watcher or a background revalidation
pass is the next step.

## CLI

```sh
./build/dev/vortex-deploy plan \
  --staging /path/to/staging \
  --target /path/to/game/Data \
  --manifest /path/to/vortex.deployment.json \
  --mod ModA

./build/dev/vortex-deploy deploy \
  --staging /path/to/staging \
  --target /path/to/game/Data \
  --manifest /path/to/vortex.deployment.json \
  --mod ModA

./build/dev/vortex-deploy purge \
  --staging /path/to/staging \
  --target /path/to/game/Data \
  --manifest /path/to/vortex.deployment.json
```

`--mod` can be repeated. An optional target subdirectory is appended after a
colon, for games that deploy into separate mod-type folders:

```sh
--mod ModA:textures
```

The journal is written to `<staging>/.vortex-journal` unless `--journal-dir` is
given.

## Layout

```text
include/vortex/deployment/   deployment types, planner, executor, manifest
include/vortex/core/         staging, install, archive, hash, deps and plugin APIs
include/vortex/game/         game.toml reader and game descriptor registry
src/                         implementation
cli/                         standalone test CLI
core/                        long-lived JSON-over-stdio core process
clients/                     Node client for the core protocol
tests/                       CTest executable and protocol smoke tests
third_party/nlohmann/        vendored JSON header
```

## Next steps

1. Add the Windows-specific hardlink/symlink, long-path and plugin-file
   backends and run the suite in a Windows CI runner.
2. Route single-mod activation and move deployment through the core.
3. Move the actual download/install executor behind `collection.next`
   workers, leaving `InstallDriver` as a view model.
4. Route the main-process `DownloadManager` and proxy/cookie-aware Nexus
   transfers through the core queue once the core can reuse the downloader's
   authentication and header pipeline.
5. Add the first-party module/plugin host on top of the game descriptors.
