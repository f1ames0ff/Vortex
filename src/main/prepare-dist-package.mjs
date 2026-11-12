import { createWriteStream, existsSync } from "node:fs";
import { access, chmod, copyFile, cp, mkdir, readFile, writeFile } from "node:fs/promises";
import { createRequire } from "node:module";
import { dirname, resolve } from "node:path";
import { Readable } from "node:stream";
import { pipeline } from "node:stream/promises";

const MAIN_DIR = resolve(import.meta.dirname);
const MAIN_PACKAGE_PATH = resolve(MAIN_DIR, "package.json");
const DIST_DIR = resolve(MAIN_DIR, "build");
const DIST_PACKAGE_PATH = resolve(DIST_DIR, "package.json");
// Runtimes bundled into the installer; also declared as winget dependencies by winget-release.yml.
const RUNTIME_DEPS_FILE = "runtime-dependencies.json";

// Walks up because MAIN_DIR is the pnpm-deployed copy (src/main/dist), not src/main.
function findUp(fileName, from) {
  let dir = from;
  for (;;) {
    const candidate = resolve(dir, fileName);
    if (existsSync(candidate)) return candidate;
    const parent = dirname(dir);
    if (parent === dir) throw new Error(`Could not find ${fileName} above ${from}`);
    dir = parent;
  }
}

async function resolveDepVersions(deps, nodeModulesDir) {
  if (!deps) return deps;
  const resolved = { ...deps };
  for (const [name, version] of Object.entries(deps)) {
    if (version === "catalog:" || version.startsWith("workspace:")) {
      try {
        const pkgJson = JSON.parse(
          await readFile(resolve(nodeModulesDir, name, "package.json"), "utf8"),
        );
        resolved[name] = pkgJson.version;
      } catch {
        // leave as-is if not found in node_modules
      }
    }
  }
  return resolved;
}

async function downloadFile(url, dest) {
  await mkdir(resolve(dest, ".."), { recursive: true });
  const response = await fetch(url);
  if (!response.ok) throw new Error(`Failed to download ${url}: ${response.statusText}`);
  await pipeline(Readable.fromWeb(response.body), createWriteStream(dest));
}

// Stage the C++ core next to the packaged app. electron-builder copies
// `temp/native` to `resources/native`, which is where resolveDefaultCorePath
// looks for `vortex-core[.exe]`.
async function prepareNativeCore() {
  if (process.env.VORTEX_SKIP_NATIVE_CORE === "1") {
    console.warn("skipping native core staging (VORTEX_SKIP_NATIVE_CORE=1)");
    return;
  }

  const nativeRoot = dirname(findUp("native/CMakeLists.txt", MAIN_DIR));
  const binaryName = process.platform === "win32" ? "vortex-core.exe" : "vortex-core";
  const source = resolve(nativeRoot, "build", "release", binaryName);
  if (!existsSync(source)) {
    throw new Error(
      `missing native core: ${source}\n` +
        "Build it with `pnpm native:build` before packaging, or set " +
        "VORTEX_SKIP_NATIVE_CORE=1 to package without it.",
    );
  }

  const staged = resolve(MAIN_DIR, "temp", "native", binaryName);
  await mkdir(dirname(staged), { recursive: true });
  await copyFile(source, staged);
  if (process.platform !== "win32") {
    await chmod(staged, 0o755);
  }
  console.log(`staged native core: ${staged}`);

  const moduleName =
    process.platform === "win32"
      ? "vortex_node_helper_module.dll"
      : "libvortex_node_helper_module.so";
  const moduleSource = resolve(nativeRoot, "build", "release", moduleName);
  if (!existsSync(moduleSource)) {
    throw new Error(
      `missing node-helper module: ${moduleSource}\n` +
        "Build it with `pnpm native:build` before packaging.",
    );
  }
  const stagedModule = resolve(MAIN_DIR, "temp", "native", moduleName);
  await copyFile(moduleSource, stagedModule);

  const helperSource = resolve(nativeRoot, "helpers", "vortex-helper.mjs");
  if (!existsSync(helperSource)) {
    throw new Error(`missing node helper script: ${helperSource}`);
  }
  const stagedHelper = resolve(MAIN_DIR, "temp", "native", "helpers", "vortex-helper.mjs");
  await mkdir(dirname(stagedHelper), { recursive: true });
  await copyFile(helperSource, stagedHelper);
  console.log(`staged native helper: ${stagedHelper}`);

  if (process.platform === "win32") {
    const hostSource = resolve(nativeRoot, "fomod-host", "bin", "Release", "net9.0-windows");
    if (!existsSync(resolve(hostSource, "VortexFomodHost.exe"))) {
      throw new Error(
        `missing C# FOMOD host: ${hostSource}\n` +
          "Build it with the Windows FOMOD host workflow before packaging.",
      );
    }
    const stagedHost = resolve(MAIN_DIR, "temp", "native", "fomod-host");
    await cp(hostSource, stagedHost, { recursive: true });
    console.log(`staged C# FOMOD host: ${stagedHost}`);
  }

  // A packaged app runs the helper through the app's Node/Electron runtime, so
  // the FOMOD addon and node-gyp-build must be outside app.asar.
  const require = createRequire(import.meta.url);
  const fomodEntry = require.resolve("@nexusmods/fomod-installer-native");
  const fomodRoot = resolve(dirname(fomodEntry), "..");
  const nodeModules = resolve(MAIN_DIR, "temp", "native", "helpers", "node_modules");
  await cp(fomodRoot, resolve(nodeModules, "@nexusmods", "fomod-installer-native"), {
    recursive: true,
  });
  const gypEntry = createRequire(resolve(fomodRoot, "package.json")).resolve("node-gyp-build");
  await cp(dirname(gypEntry), resolve(nodeModules, "node-gyp-build"), { recursive: true });
}

async function prepareWin() {
  // assert that loot runtime assets exist; fail the build if they don't.
  const lootRelease = resolve(DIST_DIR, "assets", "loot", "build", "Release");
  for (const file of ["node-loot.node", "libloot.dll"]) {
    await access(resolve(lootRelease, file)).catch(() => {
      throw new Error(`missing loot runtime asset: ${resolve(lootRelease, file)}`);
    });
  }

  const tempDir = resolve(MAIN_DIR, "temp");
  const runtimeDeps = JSON.parse(await readFile(findUp(RUNTIME_DEPS_FILE, MAIN_DIR), "utf8"));
  for (const { file, url } of runtimeDeps) {
    await downloadFile(url, resolve(tempDir, file));
  }
}

async function main() {
  const json = await readFile(MAIN_PACKAGE_PATH, "utf8");
  const mainPkg = JSON.parse(json);

  mainPkg["name"] = "Vortex";
  mainPkg["main"] = mainPkg.main.replace(/^build\//, "");

  // NOTE(erri120): this is the minimal amount of bullshit required to get the piece of shit software called "electron-builder" to work with PNPM.
  const nodeModulesDir = resolve(MAIN_DIR, "node_modules");
  mainPkg.dependencies = await resolveDepVersions(mainPkg.dependencies, nodeModulesDir);
  mainPkg.devDependencies = await resolveDepVersions(mainPkg.devDependencies, nodeModulesDir);
  mainPkg.optionalDependencies = await resolveDepVersions(
    mainPkg.optionalDependencies,
    nodeModulesDir,
  );

  await writeFile(DIST_PACKAGE_PATH, JSON.stringify(mainPkg, null, 2) + "\n", "utf8");

  await prepareNativeCore();

  if (process.platform === "win32") {
    await prepareWin();
  }
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
