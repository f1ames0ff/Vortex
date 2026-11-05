/**
 * Client for the native deployment core (`vortex-core`).
 *
 * The core is a separate long-lived process that speaks newline-delimited JSON
 * on stdin/stdout. Keeping it out of the Electron main process means a file
 * operation failure can never take the UI down, and it is the first piece of
 * the future shell/core split.
 */

import { spawn, type ChildProcessWithoutNullStreams } from "node:child_process";
import { existsSync } from "node:fs";
import path from "node:path";
import { createInterface } from "node:readline";

import { getErrorMessageOrDefault } from "@vortex/shared";
import type {
  NativeArchiveExtractRequest,
  NativeArchiveExtractResult,
  NativeArchiveListRequest,
  NativeArchiveListResult,
  NativeCollectionJobStatus,
  NativeCollectionMemberUpdate,
  NativeCollectionNextResult,
  NativeCollectionPlan,
  NativeCollectionStartRequest,
  NativeCollectionStartResult,
  NativeDepsRequest,
  NativeDepsResolveResult,
  NativeDownloadListResult,
  NativeDownloadRequest,
  NativeDownloadStatus,
  NativeDeployPlanSummary,
  NativeDeployRequest,
  NativeDeployResult,
  NativeDeploySingleRequest,
  NativeDeployStatus,
  NativeHashFilesRequest,
  NativeHashFilesResult,
  NativeInstallRequest,
  NativeInstallResult,
  NativePluginOrderRequest,
  NativePluginOrderResult,
  NativePluginOrderWriteRequest,
  NativePluginScanRequest,
  NativePluginScanResult,
  NativePurgeRequest,
  NativePurgeResult,
  NativeStagingRequest,
  NativeStagingResult,
} from "@vortex/shared/ipc";

import { log } from "../logging";

const DEFAULT_REQUEST_TIMEOUT_MS = 10 * 60 * 1000;
const STATUS_TIMEOUT_MS = 5 * 1000;

interface PingResult {
  version: string;
  protocol?: number;
  capabilities?: string[];
}

interface PendingRequest {
  resolve: (value: unknown) => void;
  reject: (err: Error) => void;
  timer: NodeJS.Timeout;
}

export interface NativeCoreClientOptions {
  /** Resolves the core executable; called before each spawn attempt. */
  resolveCorePath: () => string | undefined;
  /** Whether the feature gate allows spawning the core. */
  enabled?: () => boolean;
  requestTimeoutMs?: number;
}

/**
 * Resolve the core binary for a development checkout or a packaged app.
 * `VORTEX_CORE_PATH` always wins so tests and local builds can point anywhere.
 */
export function resolveDefaultCorePath(options: {
  appPath: string;
  resourcesPath?: string;
  env?: NodeJS.ProcessEnv;
}): string | undefined {
  const env = options.env ?? process.env;
  if (env.VORTEX_CORE_PATH !== undefined && env.VORTEX_CORE_PATH !== "") {
    return env.VORTEX_CORE_PATH;
  }

  const executable = process.platform === "win32" ? "vortex-core.exe" : "vortex-core";
  const candidates = [
    // Packaged app: <resources>/native/vortex-core
    path.join(options.resourcesPath ?? "", "native", executable),
    // Development checkout: appPath is usually <repo>/src/main.
    path.join(options.appPath, "native", "build", "dev", executable),
    path.join(options.appPath, "..", "..", "native", "build", "dev", executable),
    path.join(options.appPath, "..", "..", "..", "native", "build", "dev", executable),
    path.join(process.cwd(), "native", "build", "dev", executable),
    // Release build of a checkout.
    path.join(options.appPath, "native", "build", "release", executable),
    path.join(options.appPath, "..", "..", "native", "build", "release", executable),
    path.join(process.cwd(), "native", "build", "release", executable),
  ];

  return candidates.find((candidate) => candidate !== "" && existsSync(candidate));
}

export class NativeCoreClient {
  readonly #resolveCorePath: () => string | undefined;
  readonly #enabled: () => boolean;
  readonly #requestTimeoutMs: number;

  #child: ChildProcessWithoutNullStreams | undefined;
  #nextId = 1;
  #pending = new Map<number, PendingRequest>();
  #lastCorePath: string | undefined;
  #shuttingDown = false;

  constructor(options: NativeCoreClientOptions) {
    this.#resolveCorePath = options.resolveCorePath;
    this.#enabled = options.enabled ?? (() => true);
    this.#requestTimeoutMs = options.requestTimeoutMs ?? DEFAULT_REQUEST_TIMEOUT_MS;
  }

  get spawned(): boolean {
    return this.#child !== undefined;
  }

  /** Report whether the native path is enabled and the core responds. */
  async status(): Promise<NativeDeployStatus> {
    if (!this.#enabled()) {
      return {
        enabled: false,
        available: false,
        corePath: this.#resolveCorePath(),
        error: "native deployment is disabled (set VORTEX_NATIVE_DEPLOY=1)",
      };
    }

    const corePath = this.#resolveCorePath();
    if (corePath === undefined) {
      return {
        enabled: true,
        available: false,
        error:
          "vortex-core was not found; build it with " +
          "`cmake --preset dev && cmake --build --preset dev` in native/ " +
          "or set VORTEX_CORE_PATH",
      };
    }

    try {
      const pong = await this.#request<PingResult>("ping", {}, STATUS_TIMEOUT_MS);
      return {
        enabled: true,
        available: true,
        corePath,
        version: pong.version,
        protocol: pong.protocol,
        capabilities: pong.capabilities,
      };
    } catch (err) {
      return {
        enabled: true,
        available: false,
        corePath,
        error: getErrorMessageOrDefault(err),
      };
    }
  }

  plan(request: NativeDeployRequest): Promise<NativeDeployPlanSummary> {
    return this.#request("plan", { ...request, includeFiles: true });
  }

  deploy(request: NativeDeployRequest): Promise<NativeDeployResult> {
    return this.#request("deploy", { ...request, includeFiles: true });
  }

  deploySingle(request: NativeDeploySingleRequest): Promise<NativeDeployResult> {
    return this.#request("deploy.single", { ...request, includeFiles: true });
  }

  purge(request: NativePurgeRequest): Promise<NativePurgeResult> {
    return this.#request("purge", request);
  }

  stagingStatus(request: NativeStagingRequest): Promise<NativeStagingResult> {
    return this.#request("staging.status", request);
  }

  installApply(request: NativeInstallRequest): Promise<NativeInstallResult> {
    return this.#request("install.apply", request);
  }

  archiveList(request: NativeArchiveListRequest): Promise<NativeArchiveListResult> {
    return this.#request("archive.list", request);
  }

  archiveExtract(request: NativeArchiveExtractRequest): Promise<NativeArchiveExtractResult> {
    return this.#request("archive.extract", request);
  }

  hashFiles(request: NativeHashFilesRequest): Promise<NativeHashFilesResult> {
    return this.#request("hash.files", request);
  }

  depsResolve(request: NativeDepsRequest): Promise<NativeDepsResolveResult> {
    return this.#request("deps.resolve", request);
  }

  collectionPlan(request: NativeDepsRequest): Promise<NativeCollectionPlan> {
    return this.#request("collection.plan", request);
  }

  pluginsScan(request: NativePluginScanRequest): Promise<NativePluginScanResult> {
    return this.#request("plugins.scan", request);
  }

  pluginsOrderRead(request: NativePluginOrderRequest): Promise<NativePluginOrderResult> {
    return this.#request("plugins.loadOrder.read", request);
  }

  pluginsOrderWrite(request: NativePluginOrderWriteRequest): Promise<NativePluginOrderResult> {
    return this.#request("plugins.loadOrder.write", request);
  }

  collectionStart(request: NativeCollectionStartRequest): Promise<NativeCollectionStartResult> {
    return this.#request("collection.start", request);
  }

  collectionStatus(jobId: string): Promise<NativeCollectionJobStatus> {
    return this.#request("collection.status", { jobId });
  }

  collectionNext(jobId: string): Promise<NativeCollectionNextResult> {
    return this.#request("collection.next", { jobId });
  }

  collectionMember(request: NativeCollectionMemberUpdate): Promise<NativeCollectionJobStatus> {
    return this.#request("collection.member", request);
  }

  collectionPause(jobId: string): Promise<NativeCollectionJobStatus> {
    return this.#request("collection.pause", { jobId });
  }

  collectionCancel(jobId: string): Promise<NativeCollectionJobStatus> {
    return this.#request("collection.cancel", { jobId });
  }

  downloadAdd(request: NativeDownloadRequest): Promise<NativeDownloadStatus> {
    return this.#request("download.add", request);
  }

  downloadStatus(id: string): Promise<NativeDownloadStatus> {
    return this.#request("download.status", { id });
  }

  downloadList(): Promise<NativeDownloadListResult> {
    return this.#request("download.list", {});
  }

  downloadPause(id: string): Promise<NativeDownloadStatus> {
    return this.#request("download.pause", { id });
  }

  downloadResume(id: string): Promise<NativeDownloadStatus> {
    return this.#request("download.resume", { id });
  }

  downloadCancel(id: string): Promise<NativeDownloadStatus> {
    return this.#request("download.cancel", { id });
  }

  /** Stop the core process and reject anything still in flight. */
  async shutdown(): Promise<void> {
    const child = this.#child;
    if (child === undefined) {
      this.#shuttingDown = true;
      return;
    }

    // Ask the core to stop before setting the guard; #request rejects once
    // #shuttingDown is true, which used to make this a dead branch and always
    // kill the process after the timeout instead.
    try {
      await this.#request("shutdown", {}, STATUS_TIMEOUT_MS);
    } catch {
      // The process may already be gone; terminating below is enough.
    }
    this.#shuttingDown = true;

    await new Promise<void>((resolve) => {
      const done = () => resolve();
      const timer = setTimeout(() => {
        child.kill();
        resolve();
      }, 2_000);
      timer.unref();
      child.once("exit", () => {
        clearTimeout(timer);
        done();
      });
      if (child.exitCode !== null) {
        clearTimeout(timer);
        done();
      }
    });

    this.#detachChild(child, new Error("native core shut down"));
  }

  #request<T>(method: string, params: unknown, timeoutMs?: number): Promise<T> {
    if (this.#shuttingDown) {
      return Promise.reject(new Error("native core is shutting down"));
    }

    let child: ChildProcessWithoutNullStreams;
    try {
      child = this.#ensureChild();
    } catch (err) {
      return Promise.reject(err instanceof Error ? err : new Error(String(err)));
    }

    const id = this.#nextId++;
    const timeout = timeoutMs ?? this.#requestTimeoutMs;

    return new Promise<T>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.#pending.delete(id);
        reject(new Error(`native core request '${method}' timed out after ${timeout}ms`));
      }, timeout);
      timer.unref();

      this.#pending.set(id, {
        resolve: (value) => resolve(value as T),
        reject,
        timer,
      });

      const line = `${JSON.stringify({ id, method, params })}\n`;
      child.stdin.write(line, (err) => {
        if (err === null || err === undefined) {
          return;
        }
        const pending = this.#pending.get(id);
        if (pending !== undefined) {
          this.#pending.delete(id);
          clearTimeout(pending.timer);
          pending.reject(err);
        }
      });
    });
  }

  #ensureChild(): ChildProcessWithoutNullStreams {
    if (this.#child !== undefined) {
      return this.#child;
    }

    if (!this.#enabled()) {
      throw new Error("native deployment is disabled (set VORTEX_NATIVE_DEPLOY=1)");
    }

    const corePath = this.#resolveCorePath();
    if (corePath === undefined) {
      throw new Error(
        "vortex-core was not found; build it with " +
          "`cmake --preset dev && cmake --build --preset dev` in native/ " +
          "or set VORTEX_CORE_PATH",
      );
    }

    log("info", "starting native deployment core", { path: corePath });
    const child = spawn(corePath, [], {
      stdio: ["pipe", "pipe", "pipe"],
      windowsHide: true,
    });
    this.#lastCorePath = corePath;
    this.#child = child;

    const stdout = createInterface({ input: child.stdout });
    stdout.on("line", (line) => this.#handleLine(child, line));

    const stderr = createInterface({ input: child.stderr });
    stderr.on("line", (line) => log("debug", "vortex-core", { line }));

    child.on("error", (err) => {
      log("error", "native deployment core failed", {
        path: corePath,
        error: getErrorMessageOrDefault(err),
      });
      this.#detachChild(child, err);
    });

    child.on("exit", (code, signal) => {
      const message = `vortex-core exited (code ${code ?? "null"}, signal ${signal ?? "none"})`;
      log(code === 0 ? "info" : "warn", message, { path: corePath });
      this.#detachChild(child, new Error(message));
    });

    child.stdin.on("error", (err) => {
      log("warn", "native deployment core stdin failed", {
        error: getErrorMessageOrDefault(err),
      });
    });

    return child;
  }

  #handleLine(child: ChildProcessWithoutNullStreams, line: string): void {
    if (child !== this.#child || line.trim() === "") {
      return;
    }

    let response: {
      id?: number;
      result?: unknown;
      error?: { code?: string; message?: string };
    };
    try {
      response = JSON.parse(line) as typeof response;
    } catch {
      log("warn", "unparseable line from vortex-core", { line });
      return;
    }

    if (typeof response.id !== "number") {
      return;
    }
    const pending = this.#pending.get(response.id);
    if (pending === undefined) {
      return;
    }
    this.#pending.delete(response.id);
    clearTimeout(pending.timer);

    if (response.error !== undefined) {
      const code = response.error.code ?? "internal";
      const message = response.error.message ?? "unknown native core error";
      pending.reject(new Error(`${code}: ${message}`));
    } else {
      pending.resolve(response.result);
    }
  }

  #detachChild(child: ChildProcessWithoutNullStreams, reason: Error): void {
    if (this.#child !== child) {
      return;
    }
    this.#child = undefined;

    for (const pending of this.#pending.values()) {
      clearTimeout(pending.timer);
      pending.reject(
        new Error(`${getErrorMessageOrDefault(reason)} (core: ${this.#lastCorePath ?? "unknown"})`),
      );
    }
    this.#pending.clear();
  }
}
