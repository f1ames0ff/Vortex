# Vortex native core

C++ deployment core for the post-Electron Vortex architecture. This is the first
vertical slice: file index, deployment planner, hardlink executor, journal and
manifest compatibility, plus the feature-flagged Electron integration that runs
hardlink deployments through the core process.

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
{"id":5,"method":"purge","params":{"staging":"/staging","target":"/game/Data","manifest":"/game/Data/vortex.deployment.json"}}
{"id":6,"method":"staging.status","params":{"staging":"/staging","mods":[{"id":"mod-a","installationPath":"ModA"},{"id":"mod-b","installationPath":"ModB"}]}}
{"id":7,"method":"install.apply","params":{"temp":"/tmp/ModA.extracted","destination":"/staging/ModA","instructions":[{"type":"hardlink","source":"textures/a.dds","destination":"textures/a.dds"},{"type":"write","destination":"meta.ini","data":"..."}]}}
{"id":8,"method":"shutdown","params":{}}
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
`copy`, `hardlink` (falling back to copy across devices) and `write`. Sources
are resolved inside `temp`, destinations inside `destination`; absolute paths,
`..`, symlinked components and (on Windows) device/ADS names are rejected.
Hardlink/copy targets are grouped by destination, so duplicate instructions are
applied in request order instead of racing. The install journal records a
durable `install.begin`, per-failure `install.failure` entries, and a durable
`install.complete` or `install.failed`. It defaults to
`<staging>/.vortex-install-journal`. FOMOD script execution stays in the
existing .NET host; the core only receives its resolved instructions.

`ping` also returns `protocol` and `capabilities`, so the shell can reject a
stale core before calling a method it does not support.

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
pnpm run start:prod:native   # installed app's data, hardlink via vortex-core
```

`VORTEX_PROD_DATA=1` resolves the production user-data directory
(`~/.config/Vortex` on Linux, `%APPDATA%\Vortex` on Windows). An explicit value
is treated as a path, e.g. `VORTEX_PROD_DATA=/path/to/user-data`. The existing
`--user-data <path>` CLI flag still works too.

When enabled, `hardlink_activator` deployments and purges are routed through
the core, and `ModList` asks the core for staging status instead of walking
every installed mod in the renderer. Other deployment methods, a missing
binary, or a disabled flag keep the existing TypeScript implementation. The
renderer still writes its own manifest/backup copies afterwards, so manifests
stay compatible with the old path.

Known integration gaps for the next slice:

- Only hardlink deployment is routed; symlink/move and single-mod activation
  still use the TypeScript path.
- Exact skip-list entries are passed to the core; minimatch patterns from
  `ignoreDeploy` still rely on the TypeScript path.
- The packaged app does not embed a Windows build of the core yet.

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
include/vortex/core/         staging status and install instruction APIs
src/                         implementation
cli/                         standalone test CLI
core/                        long-lived JSON-over-stdio core process
clients/                     Node client for the core protocol
tests/                       CTest executable and protocol smoke tests
third_party/nlohmann/        vendored JSON header
```

## Next steps

1. Add the Windows-specific hardlink and long-path backend and run the suite in
   a Windows CI runner.
2. Route single-mod activation, symlink and move deployment through the core.
3. Move archive extraction into the core (spawning the existing 7-Zip binary)
   so `InstallManager` no longer orchestrates extraction in the renderer.
4. Move dependency resolution and collection phase scheduling into the core,
   leaving the renderer with UI state only.
5. Add the `game.toml` descriptor loader and the first-party module/plugin host.
