/** Main-process IPC surface for the native deployment core. */

import { existsSync } from "node:fs";
import path from "node:path";

import { getErrorMessageOrDefault } from "@vortex/shared";
import type {
  NativeArchiveExtractRequest,
  NativeArchiveListRequest,
  NativeCollectionMemberUpdate,
  NativeCollectionTaskRequest,
  NativeCollectionStartRequest,
  NativeDepsRequest,
  NativeDeployRequest,
  NativeDeploySingleRequest,
  NativeExternalChangesRequest,
  NativeFomodResolveRequest,
  NativeFomodResolveResult,
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

function nativeRootDir(): string {
  return app.isPackaged
    ? path.join(process.resourcesPath, "native")
    : path.resolve(app.getAppPath(), "..", "..", "native");
}

function nodeHelperPaths(): { module: string; script: string; fomod: string } {
  const root = nativeRootDir();
  const moduleName =
    process.platform === "win32"
      ? "vortex_node_helper_module.dll"
      : "libvortex_node_helper_module.so";
  const modulePath = app.isPackaged
    ? path.join(root, moduleName)
    : path.join(root, "build", "release", moduleName);
  const scriptPath = app.isPackaged
    ? path.join(root, "helpers", "vortex-helper.mjs")
    : path.join(root, "helpers", "vortex-helper.mjs");
  // Keep the app.asar path: Electron's asar fs shim resolves the package's
  // node-gyp-build dependency and redirects the native .node load itself.
  const packagedFomod = path.join(
    root,
    "helpers",
    "node_modules",
    "@nexusmods",
    "fomod-installer-native",
    "dist",
    "index.js",
  );
  const fomod = existsSync(packagedFomod)
    ? packagedFomod
    : require.resolve("@nexusmods/fomod-installer-native");
  return { module: modulePath, script: scriptPath, fomod };
}

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

  betterIpcMain.handle("nativeDeploy:deploySingle", (_event, request: NativeDeploySingleRequest) =>
    nativeCoreClient().deploySingle(request),
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
    (_event, request: NativeCollectionStartRequest) => {
      const paths = nodeHelperPaths();
      const sevenZip = resolveSevenZipPath();
      const curl = process.env.VORTEX_CURL_PATH ?? "curl";
      const members = request.members.map((member) => {
        if (member.task === undefined) {
          return member;
        }
        return {
          ...member,
          task: {
            ...member.task,
            download:
              member.task.download === undefined
                ? undefined
                : { ...member.task.download, curl: member.task.download.curl ?? curl },
            extract:
              member.task.extract === undefined
                ? undefined
                : {
                    ...member.task.extract,
                    sevenZip: member.task.extract.sevenZip ?? sevenZip,
                  },
            fomod:
              member.task.fomod === undefined
                ? undefined
                : {
                    ...member.task.fomod,
                    node:
                      member.task.fomod.node || process.env.VORTEX_NODE_PATH || process.execPath,
                    script: member.task.fomod.script || paths.script,
                    nativeLibrary: member.task.fomod.nativeLibrary || paths.fomod,
                    pluginPath:
                      member.task.fomod.pluginPath || member.task.extract?.destination || "",
                    scriptPath:
                      member.task.fomod.scriptPath || member.task.extract?.destination || "",
                  },
          },
        };
      });
      return nativeCoreClient().collectionStart({ ...request, members });
    },
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

  betterIpcMain.handle("nativeDeploy:collectionRun", (_event, jobId: string) =>
    nativeCoreClient().collectionRun(jobId),
  );

  betterIpcMain.handle(
    "nativeDeploy:collectionTask",
    (_event, request: NativeCollectionTaskRequest) => {
      const sevenZip = request.task.extract?.sevenZip ?? resolveSevenZipPath();
      const curl = request.task.download?.curl ?? process.env.VORTEX_CURL_PATH ?? "curl";
      return nativeCoreClient().collectionTask({
        ...request,
        task: {
          ...request.task,
          download:
            request.task.download === undefined ? undefined : { ...request.task.download, curl },
          extract:
            request.task.extract === undefined
              ? undefined
              : {
                  ...request.task.extract,
                  sevenZip: request.task.extract.sevenZip ?? sevenZip,
                },
        },
      });
    },
  );

  betterIpcMain.handle(
    "nativeDeploy:collectionExecute",
    (_event, request: { jobId: string; ruleId: string }) =>
      nativeCoreClient().collectionExecute(request),
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

  betterIpcMain.handle(
    "nativeDeploy:externalChanges",
    (_event, request: NativeExternalChangesRequest) => nativeCoreClient().externalChanges(request),
  );

  betterIpcMain.handle(
    "nativeDeploy:fomodResolve",
    async (_event, request: NativeFomodResolveRequest): Promise<NativeFomodResolveResult> => {
      const paths = nodeHelperPaths();
      const core = nativeCoreClient();
      try {
        await core.moduleLoad(paths.module);
      } catch (err) {
        if (!getErrorMessageOrDefault(err).includes("already loaded")) {
          throw err;
        }
      }
      const result = await core.moduleCall("node-helper", "call", {
        node: process.env.VORTEX_NODE_PATH ?? process.execPath,
        env: process.env.VORTEX_NODE_PATH === undefined ? { ELECTRON_RUN_AS_NODE: "1" } : undefined,
        script: paths.script,
        request: {
          method: "fomod.resolve",
          params: {
            nativeLibrary: paths.fomod,
            files: request.files,
            stopPatterns: request.stopPatterns ?? [],
            pluginPath: request.pluginPath ?? null,
            scriptPath: request.scriptPath ?? "",
            preset: request.preset,
            validate: request.validate === true,
          },
        },
      });
      return result.result as NativeFomodResolveResult;
    },
  );
}

/** Stop the core process during application shutdown. */
export async function shutdownNativeDeploy(): Promise<void> {
  const running = client;
  client = undefined;
  await running?.shutdown();
}
