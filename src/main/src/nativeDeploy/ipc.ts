/** Main-process IPC surface for the native deployment core. */

import path from "node:path";

import type {
  NativeArchiveExtractRequest,
  NativeArchiveListRequest,
  NativeCollectionMemberUpdate,
  NativeCollectionStartRequest,
  NativeDepsRequest,
  NativeDeployRequest,
  NativeDownloadRequest,
  NativePluginOrderRequest,
  NativePluginOrderWriteRequest,
  NativePluginScanRequest,
  NativeHashFilesRequest,
  NativeInstallRequest,
  NativePurgeRequest,
  NativeStagingRequest,
} from "@vortex/shared/ipc";
import { app } from "electron";

import { betterIpcMain } from "../ipc";
import { NativeCoreClient, resolveDefaultCorePath } from "./service";

function nativeDeployEnabled(): boolean {
  const value = (process.env.VORTEX_NATIVE_DEPLOY ?? "").toLowerCase();
  return value === "1" || value === "true" || value === "yes";
}

let client: NativeCoreClient | undefined;

function resolveSevenZipPath(): string | undefined {
  try {
    const packageJson = require.resolve("node-7z/package.json");
    const binaryModule = require.resolve("7z-bin", { paths: [path.dirname(packageJson)] });
    const binary = require(binaryModule) as string;
    // Packaged builds keep the executable outside app.asar.
    return binary.replace(`app.asar${path.sep}`, `app.asar.unpacked${path.sep}`);
  } catch {
    return undefined;
  }
}

export function nativeCoreClient(): NativeCoreClient {
  client ??= new NativeCoreClient({
    enabled: nativeDeployEnabled,
    resolveCorePath: () =>
      resolveDefaultCorePath({
        appPath: app.getAppPath(),
        resourcesPath: process.resourcesPath,
      }),
  });
  return client;
}

/** Register the feature-flagged native deployment channels. */
export function initNativeDeployIpc(): void {
  betterIpcMain.handle("nativeDeploy:status", () => nativeCoreClient().status());

  betterIpcMain.handle("nativeDeploy:plan", (_event, request: NativeDeployRequest) =>
    nativeCoreClient().plan(request),
  );

  betterIpcMain.handle("nativeDeploy:deploy", (_event, request: NativeDeployRequest) =>
    nativeCoreClient().deploy(request),
  );

  betterIpcMain.handle("nativeDeploy:purge", (_event, request: NativePurgeRequest) =>
    nativeCoreClient().purge(request),
  );

  betterIpcMain.handle("nativeDeploy:stagingStatus", (_event, request: NativeStagingRequest) =>
    nativeCoreClient().stagingStatus(request),
  );

  betterIpcMain.handle("nativeDeploy:installApply", (_event, request: NativeInstallRequest) =>
    nativeCoreClient().installApply(request),
  );

  betterIpcMain.handle("nativeDeploy:archiveList", (_event, request: NativeArchiveListRequest) => {
    const sevenZip = request.sevenZip ?? resolveSevenZipPath();
    if (sevenZip === undefined) {
      return Promise.reject(new Error("7-Zip binary was not found"));
    }
    return nativeCoreClient().archiveList({ ...request, sevenZip });
  });

  betterIpcMain.handle(
    "nativeDeploy:archiveExtract",
    (_event, request: NativeArchiveExtractRequest) => {
      const sevenZip = request.sevenZip ?? resolveSevenZipPath();
      if (sevenZip === undefined) {
        return Promise.reject(new Error("7-Zip binary was not found"));
      }
      return nativeCoreClient().archiveExtract({ ...request, sevenZip });
    },
  );

  betterIpcMain.handle("nativeDeploy:hashFiles", (_event, request: NativeHashFilesRequest) =>
    nativeCoreClient().hashFiles(request),
  );

  betterIpcMain.handle("nativeDeploy:depsResolve", (_event, request: NativeDepsRequest) =>
    nativeCoreClient().depsResolve(request),
  );

  betterIpcMain.handle("nativeDeploy:collectionPlan", (_event, request: NativeDepsRequest) =>
    nativeCoreClient().collectionPlan(request),
  );

  betterIpcMain.handle("nativeDeploy:pluginsScan", (_event, request: NativePluginScanRequest) =>
    nativeCoreClient().pluginsScan(request),
  );

  betterIpcMain.handle(
    "nativeDeploy:pluginsOrderRead",
    (_event, request: NativePluginOrderRequest) => nativeCoreClient().pluginsOrderRead(request),
  );

  betterIpcMain.handle(
    "nativeDeploy:pluginsOrderWrite",
    (_event, request: NativePluginOrderWriteRequest) =>
      nativeCoreClient().pluginsOrderWrite(request),
  );

  betterIpcMain.handle(
    "nativeDeploy:collectionStart",
    (_event, request: NativeCollectionStartRequest) => nativeCoreClient().collectionStart(request),
  );

  betterIpcMain.handle("nativeDeploy:collectionStatus", (_event, jobId: string) =>
    nativeCoreClient().collectionStatus(jobId),
  );

  betterIpcMain.handle("nativeDeploy:collectionNext", (_event, jobId: string) =>
    nativeCoreClient().collectionNext(jobId),
  );

  betterIpcMain.handle(
    "nativeDeploy:collectionMember",
    (_event, request: NativeCollectionMemberUpdate) => nativeCoreClient().collectionMember(request),
  );

  betterIpcMain.handle("nativeDeploy:collectionPause", (_event, jobId: string) =>
    nativeCoreClient().collectionPause(jobId),
  );

  betterIpcMain.handle("nativeDeploy:collectionCancel", (_event, jobId: string) =>
    nativeCoreClient().collectionCancel(jobId),
  );

  betterIpcMain.handle("nativeDeploy:downloadAdd", (_event, request: NativeDownloadRequest) =>
    nativeCoreClient().downloadAdd({
      curl: process.env.VORTEX_CURL_PATH ?? "curl",
      ...request,
    }),
  );
  betterIpcMain.handle("nativeDeploy:downloadStatus", (_event, id: string) =>
    nativeCoreClient().downloadStatus(id),
  );
  betterIpcMain.handle("nativeDeploy:downloadList", () => nativeCoreClient().downloadList());
  betterIpcMain.handle("nativeDeploy:downloadPause", (_event, id: string) =>
    nativeCoreClient().downloadPause(id),
  );
  betterIpcMain.handle("nativeDeploy:downloadResume", (_event, id: string) =>
    nativeCoreClient().downloadResume(id),
  );
  betterIpcMain.handle("nativeDeploy:downloadCancel", (_event, id: string) =>
    nativeCoreClient().downloadCancel(id),
  );
}

/** Stop the core process during application shutdown. */
export async function shutdownNativeDeploy(): Promise<void> {
  const running = client;
  client = undefined;
  await running?.shutdown();
}
