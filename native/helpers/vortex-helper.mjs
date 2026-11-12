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

function hasCSharpScript(files) {
  return (files ?? []).some((file) => {
    const normalized = String(file).replace(/\\/g, "/").toLowerCase();
    return normalized === "fomod/script.cs" || normalized.endsWith("/fomod/script.cs");
  });
}

function choicesFromSteps(steps) {
  return (steps ?? []).map((step) => ({
    name: step.name,
    groups: (step.optionalFileGroups?.group ?? []).map((group) => ({
      name: group.name,
      choices: (group.options ?? [])
        .filter((option) => option.selected)
        .map((option, idx) => ({ name: option.name, idx })),
    })),
  }));
}

async function fomodTestSupported(params) {
  if (hasCSharpScript(params.files)) {
    return { supported: false, requiredFiles: [] };
  }
  const fomod = loadFomod(params.nativeLibrary);
  return fomod.NativeModInstaller.testSupported(params.files ?? [], params.allowedTypes ?? []);
}

async function fomodResolve(params) {
  if (hasCSharpScript(params.files)) {
    throw new Error(
      "C# FOMOD scripts are not supported by @nexusmods/fomod-installer-native; " +
        "use @nexusmods/fomod-installer-ipc with the .NET host",
    );
  }

  const fomod = loadFomod(params.nativeLibrary);
  fomod.NativeFileSystem.setDefaultCallbacks();
  const logger = new fomod.NativeLogger(() => {});
  logger.setCallbacks();

  const autoDefaults = params.preset === undefined;
  let capturedSteps;
  let continueCallback;
  let selectCallback;
  let advancing = false;
  let selectedForGroup;

  const installer = new fomod.NativeModInstaller(
    () => "[]",
    () => "0.1.0",
    () => "1.0.0",
    () => "",
    (_name, _image, select, cont) => {
      selectCallback = select;
      continueCallback = cont;
    },
    () => {},
    (steps, currentStep) => {
      capturedSteps = steps;
      if (!autoDefaults || advancing) {
        return;
      }

      // SelectAtLeastOne groups have no default option. Pick the first one so
      // a headless run can advance instead of waiting for a UI selection.
      for (const step of steps ?? []) {
        for (const group of step.optionalFileGroups?.group ?? []) {
          if (group.type !== "SelectAtLeastOne") {
            continue;
          }
          const selected = (group.options ?? []).some((option) => option.selected);
          const key = `${step.id}:${group.id}`;
          if (!selected && selectedForGroup !== key && (group.options ?? []).length > 0) {
            selectedForGroup = key;
            selectCallback?.(step.id, group.id, [group.options[0].id]);
            return;
          }
        }
      }

      advancing = true;
      setImmediate(() => {
        advancing = false;
        continueCallback?.(true, currentStep);
      });
    },
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
  if (!autoDefaults) {
    return result;
  }
  return {
    ...result,
    steps: capturedSteps ?? [],
    choices: choicesFromSteps(capturedSteps ?? []),
  };
}

async function nxmResolve(params) {
  if (params.type !== "mod" && params.type !== "collection") {
    throw new Error(`nxm.resolve does not support type: ${params.type}`);
  }
  const requireFrom = params.nexusApiModule ? createRequire(params.nexusApiModule) : require;
  if (params.apiUrls !== undefined) {
    const parameters = requireFrom("./parameters");
    if (params.apiUrls.base !== undefined) parameters.BASE_URL = params.apiUrls.base;
    if (params.apiUrls.api !== undefined) parameters.API_URL = params.apiUrls.api;
    if (params.apiUrls.graphql !== undefined) parameters.GRAPHQL_URL = params.apiUrls.graphql;
    if (params.apiUrls.userService !== undefined) {
      parameters.USER_SERVICE_API_URL = params.apiUrls.userService;
    }
  }
  const nexusModule = requireFrom("./index");
  const Nexus = nexusModule.default ?? nexusModule;
  const nexus = new Nexus("Vortex", params.appVersion ?? "0.1.0", params.gameDomain, 30000);
  if (params.accessToken !== undefined) {
    await nexus.setTokenProvider(async () => params.accessToken);
  } else if (params.apiKey !== undefined) {
    await nexus.setKey(params.apiKey);
  } else {
    throw new Error("nxm.resolve requires accessToken or apiKey");
  }
  if (params.type === "mod") {
    const urls = await nexus.getDownloadURLs(
      params.modId,
      params.fileId,
      params.key,
      params.expires,
      params.gameDomain,
      params.source,
    );
    return {
      urls: (urls ?? []).map((entry) => entry.URI),
      ids: { modId: params.modId, fileId: params.fileId },
    };
  }

  const collection = params.collection ?? {};
  if (collection.slug === undefined) {
    throw new Error("nxm.resolve collection requires collection.slug");
  }
  const revision = await nexus.getCollectionRevisionGraph(
    { id: true, revisionNumber: true, downloadLink: true, collection: { id: true } },
    collection.slug,
    collection.revisionNumber,
  );
  const urls = await nexus.getCollectionDownloadLink(revision.downloadLink);
  return {
    urls: (urls ?? []).map((entry) => entry.URI),
    ids: {
      collectionId: revision.collection.id,
      revisionId: revision.id,
      collectionSlug: collection.slug,
      revisionNumber: revision.revisionNumber ?? collection.revisionNumber ?? 0,
    },
  };
}

async function metadataLookup(params) {
  let endpoint = params.endpoint;
  if (endpoint === undefined && params.md5 !== undefined) {
    if (params.gameDomain === undefined) {
      throw new Error("metadata lookup by md5 requires gameDomain");
    }
    endpoint = `https://api.nexusmods.com/v1/games/${params.gameDomain}/mods/md5_search/${params.md5}.json`;
  }
  endpoint ??= `https://api.nexusmods.com/v1/games/${params.gameDomain}/mods/${params.modId}/files/${params.fileId}.json`;
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
    case "nxm.resolve":
      return nxmResolve(request.params ?? {});
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
