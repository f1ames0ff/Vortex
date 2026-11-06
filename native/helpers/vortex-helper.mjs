#!/usr/bin/env node
import { readFileSync } from "node:fs";
import { createRequire } from "node:module";

const requestIndex = process.argv.indexOf("--request");
if (requestIndex < 0 || process.argv[requestIndex + 1] === undefined) {
  process.stderr.write("usage: vortex-helper.mjs --request <json-file>\n");
  process.exit(2);
}

const request = JSON.parse(readFileSync(process.argv[requestIndex + 1], "utf8"));
const require = createRequire(import.meta.url);

function loadFomod(library) {
  return require(library);
}

async function fomodTestSupported(params) {
  const fomod = loadFomod(params.nativeLibrary);
  return fomod.NativeModInstaller.testSupported(params.files ?? [], params.allowedTypes ?? []);
}

async function fomodResolve(params) {
  const fomod = loadFomod(params.nativeLibrary);
  fomod.NativeFileSystem.setDefaultCallbacks();
  const logger = new fomod.NativeLogger(() => {});
  logger.setCallbacks();

  const installer = new fomod.NativeModInstaller(
    () => "[]",
    () => "0.1.0",
    () => "1.0.0",
    () => "",
    () => {},
    () => {},
    () => {},
  );

  const result = await installer.install(
    params.files ?? [],
    params.stopPatterns ?? [],
    params.pluginPath ?? null,
    params.scriptPath ?? "",
    params.preset ?? undefined,
    false,
    params.validate === true,
  );
  if (result == null) {
    throw new Error("FOMOD installation returned no result");
  }
  return result;
}

async function metadataLookup(params) {
  const endpoint =
    params.endpoint ??
    `https://api.nexusmods.com/v1/games/${params.gameDomain}/mods/${params.modId}/files/${params.fileId}.json`;
  const headers = {};
  if (params.apiKey !== undefined) {
    headers.apikey = params.apiKey;
  }
  const response = await fetch(endpoint, { headers });
  if (!response.ok) {
    throw new Error(`metadata lookup failed: ${response.status} ${response.statusText}`);
  }
  return { metadata: await response.json() };
}

async function run() {
  switch (request.method) {
    case "ping":
      return { version: "1.0.0" };
    case "fomod.testSupported":
      return fomodTestSupported(request.params ?? {});
    case "fomod.resolve":
      return fomodResolve(request.params ?? {});
    case "metadata.lookup":
      return metadataLookup(request.params ?? {});
    default:
      throw new Error(`unknown helper method: ${request.method}`);
  }
}

run()
  .then((result) => {
    process.stdout.write(JSON.stringify({ result }));
    process.exit(0);
  })
  .catch((err) => {
    process.stdout.write(
      JSON.stringify({ error: err instanceof Error ? err.message : String(err) }),
    );
    process.exit(1);
  });
