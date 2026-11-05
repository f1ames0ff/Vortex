/**
 * Renderer-side adapter for the native deployment core.
 *
 * The core itself lives in the main process. This module only maps Vortex's
 * mod/deployment types to the wire protocol and decides whether the
 * feature-flagged native path may be used for the current deployment method.
 */

import { getErrorMessageOrDefault } from "@vortex/shared";
import type {
  NativeDeployRequest,
  NativeDeployStatus,
  NativeDeployedFile,
} from "@vortex/shared/ipc";

import { log } from "../../../logging";
import type { IExtensionApi } from "../../../types/IExtensionContext";
import * as fs from "../../../util/fs";
import { addDeploymentFailures } from "../actions/session";
import { mergedSourceName } from "../modMerging";
import type { IDeployedFile, IDeploymentMethod } from "../types/IDeploymentMethod";
import type { IMod } from "../types/IMod";
import { manifestPaths } from "./activationStore";

/** The deployment method whose on-disk behaviour the native core implements. */
export const NATIVE_DEPLOY_METHOD_ID = "hardlink_activator";

let statusPromise: Promise<NativeDeployStatus> | undefined;

function unavailable(error: string): NativeDeployStatus {
  return { enabled: false, available: false, error };
}

export function nativeDeployStatus(): Promise<NativeDeployStatus> {
  statusPromise ??= (async () => {
    if (typeof window === "undefined" || window.api?.nativeDeploy === undefined) {
      return unavailable("native deployment IPC is not available");
    }
    try {
      return await window.api.nativeDeploy.status();
    } catch (err) {
      return unavailable(getErrorMessageOrDefault(err));
    }
  })();
  return statusPromise;
}

/** Forget a cached unavailable status (used by tests and manual retries). */
export function resetNativeDeployStatus(): void {
  statusPromise = undefined;
}

export async function canDeployNatively(method: IDeploymentMethod): Promise<boolean> {
  if (method.id !== NATIVE_DEPLOY_METHOD_ID) {
    return false;
  }
  const status = await nativeDeployStatus();
  return status.enabled && status.available;
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
