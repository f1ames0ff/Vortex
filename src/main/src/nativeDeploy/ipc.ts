/** Main-process IPC surface for the native deployment core. */

import type {
  NativeDeployRequest,
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
}

/** Stop the core process during application shutdown. */
export async function shutdownNativeDeploy(): Promise<void> {
  const running = client;
  client = undefined;
  await running?.shutdown();
}
