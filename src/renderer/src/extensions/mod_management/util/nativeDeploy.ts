/**
 * Renderer-side adapter for the native deployment core.
 *
 * The core itself lives in the main process. This module only maps Vortex's
 * mod/deployment types to the wire protocol and decides whether the
 * feature-flagged native path may be used for the current deployment method.
 */

import { getErrorMessageOrDefault } from "@vortex/shared";
import type {
  NativeCollectionJobMember,
  NativeCollectionJobStatus,
  NativeCollectionNextResult,
  NativeCollectionPlan,
  NativeCollectionStartResult,
  NativeDepsRequest,
  NativeDependencyEntry,
  NativeDeployRequest,
  NativeDeployStatus,
  NativeDeployedFile,
  NativeDownloadInfo,
  NativeHashAlgorithm,
  NativeInstallRequest,
  NativeInstallResult,
  NativeLookupMod,
  NativeModReference,
  NativeModStatus,
  NativePluginOrderResult,
  NativePluginScanDirectory,
  NativeRuleInfo,
} from "@vortex/shared/ipc";

import { log } from "../../../logging";
import type { IExtensionApi } from "../../../types/IExtensionContext";
import * as fs from "../../../util/fs";
import type { IDownload } from "../../download_management/types/IDownload";
import { addDeploymentFailures } from "../actions/session";
import { mergedSourceName } from "../modMerging";
import type { IDeployedFile, IDeploymentMethod } from "../types/IDeploymentMethod";
import type { IMod, IModReference, IModRule } from "../types/IMod";
import { manifestPaths } from "./activationStore";

/** The deployment method whose on-disk behaviour the native core implements. */
export const NATIVE_DEPLOY_METHOD_ID = "hardlink_activator";
const NATIVE_DEPLOY_METHOD_IDS = new Set([NATIVE_DEPLOY_METHOD_ID, "symlink_activator"]);

let statusPromise: Promise<NativeDeployStatus> | undefined;

function unavailable(error: string): NativeDeployStatus {
  return { enabled: false, available: false, error };
}

export function nativeDeployStatus(): Promise<NativeDeployStatus> {
  if (statusPromise !== undefined) {
    return statusPromise;
  }

  const pending = (async () => {
    if (typeof window === "undefined" || window.api?.nativeDeploy === undefined) {
      return unavailable("native deployment IPC is not available");
    }
    try {
      return await window.api.nativeDeploy.status();
    } catch (err) {
      return unavailable(getErrorMessageOrDefault(err));
    }
  })();
  statusPromise = pending;
  // A transient failure (core not built yet, IPC starting) must not disable
  // the native path for the rest of the session.
  void pending.then((status) => {
    if ((!status.enabled || !status.available) && statusPromise === pending) {
      statusPromise = undefined;
    }
  });
  return pending;
}

/** Forget a cached unavailable status (used by tests and manual retries). */
export function resetNativeDeployStatus(): void {
  statusPromise = undefined;
}

export async function canDeployNatively(method: IDeploymentMethod): Promise<boolean> {
  if (!NATIVE_DEPLOY_METHOD_IDS.has(method.id)) {
    return false;
  }
  const status = await nativeDeployStatus();
  if (!status.enabled || !status.available) {
    return false;
  }
  return method.id !== "symlink_activator" || coreSupports(status, "deploy.symlink");
}

/** A core from before the capability handshake has no capability list. */
function coreSupports(status: NativeDeployStatus, capability: string): boolean {
  return status.capabilities === undefined || status.capabilities.includes(capability);
}

function isUnknownMethod(err: unknown): boolean {
  return getErrorMessageOrDefault(err).includes("unknown method");
}

function toNativeReference(reference: IModReference): NativeModReference {
  return {
    id: reference.id,
    idHint: reference.idHint,
    md5Hint: reference.md5Hint,
    archiveId: reference.archiveId,
    fileMD5: reference.fileMD5,
    fileExpression: reference.fileExpression,
    logicalFileName: reference.logicalFileName,
    tag: reference.tag,
    gameId: reference.gameId,
    versionMatch: reference.versionMatch,
    repo:
      reference.repo === undefined
        ? undefined
        : {
            repository: reference.repo.repository,
            modId: reference.repo.modId?.toString(),
            fileId: reference.repo.fileId?.toString(),
          },
  };
}

function toNativeLookupMod(mod: IMod): NativeLookupMod {
  const attrs = (mod.attributes ?? {}) as Record<string, unknown>;
  const stringValue = (value: unknown): string | undefined =>
    value === undefined || value === null ? undefined : String(value);
  return {
    id: mod.id,
    state: mod.state,
    fileMD5: stringValue(attrs.fileMD5),
    fileName: stringValue(attrs.fileName),
    name: stringValue(attrs.modName ?? attrs.name),
    logicalFileName: stringValue(attrs.logicalFileName),
    customFileName: stringValue(attrs.customFileName),
    version: stringValue(attrs.version),
    fileId: stringValue(attrs.fileId),
    modId: stringValue(attrs.modId),
    source: stringValue(attrs.source),
    referenceTag: stringValue(attrs.referenceTag),
    referenceTags: Array.isArray(attrs.referenceTags)
      ? attrs.referenceTags.filter((tag): tag is string => typeof tag === "string")
      : undefined,
    additionalLogicalFileNames: Array.isArray(attrs.additionalLogicalFileNames)
      ? attrs.additionalLogicalFileNames.filter((name): name is string => typeof name === "string")
      : undefined,
    game: Array.isArray(attrs.game)
      ? attrs.game.filter((game): game is string => typeof game === "string")
      : undefined,
    installerChoices: attrs.installerChoices,
    fileList: attrs.fileList,
    patches: attrs.patches,
  };
}

function toNativeDownload(download: IDownload): NativeDownloadInfo {
  const modInfo = download.modInfo ?? {};
  const nexusIds = modInfo.nexus?.ids;
  const details = modInfo.meta?.details;
  const stringValue = (value: unknown): string | undefined =>
    value === undefined || value === null ? undefined : String(value);
  return {
    id: download.id,
    state: download.state,
    fileMD5: download.fileMD5,
    localPath: download.localPath,
    logicalFileName: modInfo.name ?? modInfo.meta?.logicalFileName ?? download.localPath,
    version: modInfo.version ?? modInfo.meta?.fileVersion,
    fileId: stringValue(nexusIds?.fileId ?? details?.fileId),
    modId: stringValue(nexusIds?.modId ?? details?.modId),
    source: modInfo.source,
    referenceTag: modInfo.referenceTag,
    referenceTags: modInfo.referenceTags,
    game: download.game,
    fileTime: download.fileTime,
  };
}

function toNativeRule(rule: IModRule, index: number): NativeRuleInfo {
  return {
    index,
    type: rule.type,
    reference: toNativeReference(rule.reference),
    phase: rule.phase ?? (rule.extra?.phase as number | undefined),
    ignored: rule.ignored,
    localPath: rule.extra?.localPath,
    installerChoices: rule.installerChoices,
    fileList: rule.fileList,
    patches: rule.patches ?? rule.extra?.patches,
  };
}

function toNativeDepsRequest(
  rules: IModRule[],
  mods: Record<string, IMod>,
  downloads: Record<string, IDownload>,
): NativeDepsRequest {
  return {
    rules: rules.map(toNativeRule),
    mods: Object.values(mods).map(toNativeLookupMod),
    downloads: Object.values(downloads).map(toNativeDownload),
  };
}

/** Resolve dependency rules in the core; `undefined` asks the caller to fall back. */
export async function resolveDependenciesNatively(
  rules: IModRule[],
  mods: Record<string, IMod>,
  downloads: Record<string, IDownload>,
): Promise<NativeDependencyEntry[] | undefined> {
  const status = await nativeDeployStatus();
  if (!status.enabled || !status.available || !coreSupports(status, "deps.resolve")) {
    return undefined;
  }
  try {
    const result = await window.api.nativeDeploy.depsResolve(
      toNativeDepsRequest(rules, mods, downloads),
    );
    return result.dependencies;
  } catch (err) {
    if (isUnknownMethod(err)) {
      return undefined;
    }
    throw err;
  }
}

/** Build the collection phase/member plan in the core; `undefined` falls back. */
export async function planCollectionNatively(
  rules: IModRule[],
  mods: Record<string, IMod>,
  downloads: Record<string, IDownload>,
): Promise<NativeCollectionPlan | undefined> {
  const status = await nativeDeployStatus();
  if (!status.enabled || !status.available || !coreSupports(status, "collection.plan")) {
    return undefined;
  }
  try {
    return await window.api.nativeDeploy.collectionPlan(
      toNativeDepsRequest(rules, mods, downloads),
    );
  } catch (err) {
    if (isUnknownMethod(err)) {
      return undefined;
    }
    throw err;
  }
}

/** Scan plugin directories in the core, keyed by directory path. */
export async function scanPluginsNatively(
  directories: Array<{ path: string; extensions: string[] }>,
): Promise<Map<string, NativePluginScanDirectory> | undefined> {
  const status = await nativeDeployStatus();
  if (!status.enabled || !status.available || !coreSupports(status, "plugins.scan")) {
    return undefined;
  }
  try {
    const result = await window.api.nativeDeploy.pluginsScan({ directories });
    return new Map(result.directories.map((directory) => [directory.path, directory]));
  } catch (err) {
    if (isUnknownMethod(err)) {
      return undefined;
    }
    throw err;
  }
}

/** Read the raw plugin order rows in the core. */
export async function readPluginOrderNatively(
  directory: string,
): Promise<NativePluginOrderResult | undefined> {
  const status = await nativeDeployStatus();
  if (!status.enabled || !status.available || !coreSupports(status, "plugins.loadOrder.read")) {
    return undefined;
  }
  try {
    return await window.api.nativeDeploy.pluginsOrderRead({ directory });
  } catch (err) {
    if (isUnknownMethod(err)) {
      return undefined;
    }
    throw err;
  }
}

/** Write plugins.txt/loadorder.txt in the core. */
export async function writePluginOrderNatively(
  directory: string,
  plugins: string[],
  loadOrder: string[],
): Promise<NativePluginOrderResult | undefined> {
  const status = await nativeDeployStatus();
  if (!status.enabled || !status.available || !coreSupports(status, "plugins.loadOrder.write")) {
    return undefined;
  }
  try {
    return await window.api.nativeDeploy.pluginsOrderWrite({ directory, plugins, loadOrder });
  } catch (err) {
    if (isUnknownMethod(err)) {
      return undefined;
    }
    throw err;
  }
}

/** Create a core-owned phase queue for a collection install. */
export async function startCollectionNatively(
  collectionId: string,
  gameId: string,
  members: NativeCollectionJobMember[],
  options: { concurrency?: number; maxRetries?: number; stallMs?: number } = {},
): Promise<NativeCollectionStartResult | undefined> {
  const status = await nativeDeployStatus();
  if (!status.enabled || !status.available || !coreSupports(status, "collection.start")) {
    return undefined;
  }
  try {
    return await window.api.nativeDeploy.collectionStart({
      collectionId,
      gameId,
      members,
      ...options,
    });
  } catch (err) {
    if (isUnknownMethod(err)) {
      return undefined;
    }
    throw err;
  }
}

export async function collectionStatusNatively(
  jobId: string,
): Promise<NativeCollectionJobStatus | undefined> {
  const status = await nativeDeployStatus();
  if (!status.enabled || !status.available || !coreSupports(status, "collection.status")) {
    return undefined;
  }
  return window.api.nativeDeploy.collectionStatus(jobId);
}

export async function nextCollectionMembersNatively(
  jobId: string,
): Promise<NativeCollectionNextResult | undefined> {
  const status = await nativeDeployStatus();
  if (!status.enabled || !status.available || !coreSupports(status, "collection.next")) {
    return undefined;
  }
  return window.api.nativeDeploy.collectionNext(jobId);
}

export async function updateCollectionMemberNatively(
  jobId: string,
  ruleId: string,
  status: string,
  details: { modId?: string; downloadId?: string; error?: string; retry?: boolean } = {},
): Promise<NativeCollectionJobStatus | undefined> {
  const native = await nativeDeployStatus();
  if (!native.enabled || !native.available || !coreSupports(native, "collection.member")) {
    return undefined;
  }
  return window.api.nativeDeploy.collectionMember({ jobId, ruleId, status, ...details });
}

export async function pauseCollectionJobNatively(
  jobId: string,
): Promise<NativeCollectionJobStatus | undefined> {
  const status = await nativeDeployStatus();
  if (!status.enabled || !status.available || !coreSupports(status, "collection.pause")) {
    return undefined;
  }
  return window.api.nativeDeploy.collectionPause(jobId);
}

export async function cancelCollectionJobNatively(
  jobId: string,
): Promise<NativeCollectionJobStatus | undefined> {
  const status = await nativeDeployStatus();
  if (!status.enabled || !status.available || !coreSupports(status, "collection.cancel")) {
    return undefined;
  }
  return window.api.nativeDeploy.collectionCancel(jobId);
}

/**
 * Check many staging folders in the core process. Returns `undefined` when
 * the native path is disabled, so callers can fall back to their local walk.
 */
export async function inspectStagingNatively(
  stagingPath: string,
  mods: Array<{ id: string; installationPath?: string }>,
): Promise<Map<string, NativeModStatus> | undefined> {
  const status = await nativeDeployStatus();
  if (!status.enabled || !status.available || !coreSupports(status, "staging.status")) {
    return undefined;
  }

  const queries = mods
    .filter(
      (mod): mod is { id: string; installationPath: string } =>
        mod.installationPath !== undefined && mod.installationPath !== "",
    )
    .map((mod) => ({ id: mod.id, installationPath: mod.installationPath }));
  if (queries.length === 0) {
    return new Map();
  }

  try {
    const result = await window.api.nativeDeploy.stagingStatus({
      staging: stagingPath,
      mods: queries,
    });
    return new Map(result.mods.map((mod) => [mod.id, mod.status]));
  } catch (err) {
    // A stale core may predate the method; let the caller use its local walk.
    if (isUnknownMethod(err)) {
      return undefined;
    }
    throw err;
  }
}

/**
 * Apply generic installer instructions in the core. Returns `undefined` when
 * the native path is disabled; FOMOD choice resolution stays in the caller.
 */
export async function applyInstallNatively(
  request: NativeInstallRequest,
): Promise<NativeInstallResult | undefined> {
  const status = await nativeDeployStatus();
  if (!status.enabled || !status.available || !coreSupports(status, "install.apply")) {
    return undefined;
  }
  try {
    return await window.api.nativeDeploy.installApply(request);
  } catch (err) {
    if (isUnknownMethod(err)) {
      return undefined;
    }
    throw err;
  }
}

/** Hash a batch of files in the core, returning path -> digest. */
export async function hashFilesNatively(
  files: string[],
  algorithm: NativeHashAlgorithm,
): Promise<Map<string, string> | undefined> {
  const status = await nativeDeployStatus();
  if (!status.enabled || !status.available || !coreSupports(status, "hash.files")) {
    return undefined;
  }

  try {
    const result = await window.api.nativeDeploy.hashFiles({ files, algorithm });
    return new Map(result.results.map((entry) => [entry.path, entry.hash]));
  } catch (err) {
    if (isUnknownMethod(err)) {
      return undefined;
    }
    throw err;
  }
}

/**
 * Extract an archive through the core. Returns `undefined` when the native
 * path is disabled, the archive is encrypted (the renderer prompts for a
 * password), or listing failed, so the caller can use its existing path.
 */
export async function extractArchiveNatively(
  archive: string,
  destination: string,
): Promise<{ code: number; errors: string[] } | undefined> {
  const status = await nativeDeployStatus();
  if (
    !status.enabled ||
    !status.available ||
    !coreSupports(status, "archive.list") ||
    !coreSupports(status, "archive.extract")
  ) {
    return undefined;
  }

  try {
    const listing = await window.api.nativeDeploy.archiveList({ archive });
    if (listing.code !== 0 || listing.encrypted) {
      return undefined;
    }
    const result = await window.api.nativeDeploy.archiveExtract({ archive, destination });
    return { code: result.code, errors: result.errors };
  } catch (err) {
    if (isUnknownMethod(err)) {
      return undefined;
    }
    throw err;
  }
}

function toDeployedFile(file: NativeDeployedFile): IDeployedFile {
  return {
    relPath: file.relPath,
    source: file.source,
    target: file.target,
    time: file.time,
    merged: file.merged,
  };
}

/**
 * Deploy one mod type with the native core. Returns `undefined` when the
 * feature flag, method or binary makes the native path inapplicable, so the
 * caller can fall back to the TypeScript implementation.
 */
export async function deployWithNativeCore(
  api: IExtensionApi,
  gameId: string,
  stagingPath: string,
  destinationPath: string,
  mods: IMod[],
  method: IDeploymentMethod,
  lastActivation: IDeployedFile[],
  typeId: string,
  skipFiles: Set<string>,
  subDir: (mod: IMod) => string,
  progressCB?: (name: string, progress: number) => void,
): Promise<IDeployedFile[] | undefined> {
  if (!(await canDeployNatively(method))) {
    return undefined;
  }

  progressCB?.("Preparing native deployment", 0);

  const state = api.store.getState();
  const mergeSource = mergedSourceName(typeId);
  const manifest = manifestPaths(destinationPath, stagingPath, typeId).manifestPath;

  const request: NativeDeployRequest = {
    staging: stagingPath,
    target: destinationPath,
    manifest,
    mods: [
      ...mods
        .filter((mod) => mod.installationPath !== undefined && mod.installationPath !== "")
        .map((mod) => ({
          installationPath: mod.installationPath,
          sourceName: mod.installationPath,
          target: subDir(mod),
          fileOverrides: mod.fileOverrides,
        })),
      {
        installationPath: mergeSource,
        sourceName: mergeSource,
        target: subDir(null),
      },
    ],
    instance: state.app.instanceId,
    method: method.id,
    game: gameId,
    options: {
      // Exact entries only; name patterns are handled by the TS fallback.
      blacklist: [...skipFiles],
    },
  };

  log("info", "deploying through native core", {
    gameId,
    typeId,
    stagingPath,
    destinationPath,
    mods: mods.length,
    previousFiles: lastActivation.length,
  });

  const result = await window.api.nativeDeploy.deploy(request);
  progressCB?.("Native deployment complete", 100);

  if (result.report.failures.length > 0) {
    log("warn", "native deployment reported file failures", {
      count: result.report.failures.length,
      failures: result.report.failures,
    });
    api.store.dispatch(
      addDeploymentFailures(
        gameId,
        result.report.failures.map((failure) => ({
          source: failure.source,
          relPath: failure.relPath,
          outputPath: failure.outputPath,
          message: failure.message,
        })),
      ),
    );
  }

  return (result.files ?? []).map(toDeployedFile);
}

/**
 * Purge all native-deployed mod types. Returns `false` when the native path
 * does not apply; otherwise mutates `lastDeployment` to the entries the core
 * could not remove and returns `true`.
 */
export async function purgeWithNativeCore(
  api: IExtensionApi,
  gameId: string,
  stagingPath: string,
  modPaths: { [typeId: string]: string },
  modTypes: string[],
  method: IDeploymentMethod,
  lastDeployment: { [typeId: string]: IDeployedFile[] },
  onProgress: (progress: number, message: string) => void,
): Promise<boolean> {
  if (!(await canDeployNatively(method))) {
    return false;
  }

  const state = api.store.getState();
  const instance = state.app.instanceId;

  for (const [index, typeId] of modTypes.entries()) {
    onProgress(25 + Math.floor((index * 50) / modTypes.length), "Removing links");

    const targetPath = modPaths[typeId];
    if (targetPath === undefined || targetPath === "") {
      lastDeployment[typeId] = [];
      continue;
    }

    const manifest = manifestPaths(targetPath, stagingPath, typeId).manifestPath;
    const manifestExists = await fs
      .statAsync(manifest)
      .then(() => true)
      .catch(() => false);
    if (!manifestExists) {
      lastDeployment[typeId] = [];
      continue;
    }

    log("info", "purging through native core", {
      gameId,
      typeId,
      targetPath,
      manifest,
      previousFiles: (lastDeployment[typeId] ?? []).length,
    });

    const result = await window.api.nativeDeploy.purge({
      staging: stagingPath,
      target: targetPath,
      manifest,
      instance,
      method: method.id,
      game: gameId,
    });

    lastDeployment[typeId] = (result.files ?? []).map(toDeployedFile);

    if (result.report.failures.length > 0) {
      throw new Error(
        `Native purge failed for ${result.report.failures.length} file(s): ` +
          result.report.failures
            .slice(0, 5)
            .map((failure) => `${failure.relPath}: ${failure.message}`)
            .join("; "),
      );
    }
  }

  return true;
}
