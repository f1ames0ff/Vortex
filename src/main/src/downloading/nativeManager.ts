import { randomUUID } from "node:crypto";
import * as path from "node:path";

import { VortexError } from "@vortex/shared";
import type {
  Chunker,
  DownloadCheckpoint,
  DownloadState,
  PauseResult,
  ResolvedResource,
  RetryStrategy,
} from "@vortex/shared/download";
import { staticChunker } from "@vortex/shared/download";
import type { CookieJar } from "tough-cookie";

import { log } from "../logging";
import type { NativeCoreClient } from "../nativeDeploy/service";
import type { DownloadHandle } from "./manager";
import { DownloadManager } from "./manager";
import { ProgressReporter } from "./progress";

type NativeStatus = Awaited<ReturnType<NativeCoreClient["downloadStatus"]>>;

const POLL_INTERVAL_MS = 150;

function delay(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function probeEndpoint(resource: ResolvedResource): {
  url: string;
  headers: Record<string, string> | undefined;
} {
  const endpoint = "url" in resource ? resource : resource.probeEndpoint;
  return { url: endpoint.url.toString(), headers: endpoint.headers };
}

class NativeDownloadHandle<T = unknown> implements DownloadHandle<T> {
  readonly downloadId: string;

  readonly #client: NativeCoreClient;
  readonly #resource: T;
  readonly #resolved: ResolvedResource;
  readonly #dest: string;
  readonly #reporter = new ProgressReporter();
  readonly #progress;
  #current: Promise<void>;
  #lastStatus: NativeStatus | undefined;
  #adding = true;
  #cancelled = false;

  constructor(
    client: NativeCoreClient,
    downloadId: string,
    resource: T,
    resolved: ResolvedResource,
    dest: string,
    options: {
      headers?: Record<string, string>;
      userAgent?: string;
      proxy?: string;
      maxRetries?: number;
      concurrency?: number;
    },
  ) {
    this.#client = client;
    this.downloadId = downloadId;
    this.#resource = resource;
    this.#resolved = resolved;
    this.#dest = dest;
    this.#progress = this.#reporter.init(undefined);
    this.#reporter.fileName = path.basename(dest);
    this.#current = this.#start(options);
  }

  get promise(): Promise<void> {
    return this.#current;
  }

  async #start(options: {
    headers?: Record<string, string>;
    userAgent?: string;
    proxy?: string;
    maxRetries?: number;
    concurrency?: number;
  }): Promise<void> {
    try {
      const endpoint = probeEndpoint(this.#resolved);
      const status = await this.#client.downloadAdd({
        id: this.downloadId,
        directory: path.dirname(this.#dest),
        fileName: path.basename(this.#dest),
        urls: [endpoint.url],
        headers: options.headers,
        userAgent: options.userAgent,
        proxy: options.proxy,
        maxRetries: options.maxRetries ?? 3,
        concurrency: options.concurrency,
        // The main-process downloader resumes a partial destination itself and
        // expects a failed/paused transfer to leave that file in place.
        atomic: false,
      });
      this.#adding = false;
      this.#apply(status);
      await this.#poll();
    } catch (err) {
      this.#adding = false;
      this.#reporter.status = "failed";
      throw err;
    }
  }

  async #poll(): Promise<void> {
    for (;;) {
      const status = this.#lastStatus;
      if (status === undefined || !["queued", "downloading"].includes(status.state)) {
        break;
      }
      await delay(POLL_INTERVAL_MS);
      this.#apply(await this.#client.downloadStatus(this.downloadId));
    }

    const status = this.#lastStatus;
    if (status?.state === "failed") {
      throw new VortexError(
        status.error ?? "Download failed",
        { kind: "http:generic", url: status.url },
        { cause: new Error(status.error ?? "Download failed") },
      );
    }
    if (status?.state === "cancelled") {
      throw new VortexError("Download cancelled", { kind: "user-canceled", skipped: false });
    }
  }

  #apply(status: NativeStatus): void {
    this.#lastStatus = status;
    this.#reporter.status =
      status.state === "finished"
        ? "completed"
        : status.state === "downloading"
          ? "running"
          : status.state === "cancelled"
            ? "canceled"
            : status.state;
    this.#progress.bytesReceived = status.bytes;
    this.#progress.bytesWritten = status.bytes;
  }

  getState(): DownloadState {
    const state = this.#reporter.getProgress();
    const status = this.#lastStatus;
    const mapped =
      status?.state === "finished"
        ? "completed"
        : status?.state === "downloading"
          ? "running"
          : (status?.state ?? "queued");
    if (mapped === "failed") {
      return {
        ...state,
        status: "failed",
        error: new VortexError(
          status?.error ?? "Download failed",
          { kind: "http:generic", url: status?.url ?? "" },
          { cause: new Error(status?.error ?? "Download failed") },
        ),
      };
    }
    return { ...state, status: mapped } as DownloadState;
  }

  cancel(): DownloadState {
    this.#cancelled = true;
    this.#reporter.status = "canceled";
    void this.#client.downloadCancel(this.downloadId).catch(() => undefined);
    return this.getState();
  }

  async pause(): Promise<PauseResult<T>> {
    if (this.#adding) {
      // The add request is still in flight; cancel it and report a checkpoint
      // with no completed ranges rather than racing the core.
      this.cancel();
      return {
        ...this.getState(),
        status: "paused",
        checkpoint: {
          downloadId: this.downloadId,
          resource: this.#resource,
          dest: this.#dest,
          completedRanges: [],
          etag: undefined,
        },
      };
    }

    let status = await this.#client.downloadPause(this.downloadId);
    while (!["paused", "failed", "cancelled"].includes(status.state)) {
      await delay(50);
      status = await this.#client.downloadStatus(this.downloadId);
    }
    this.#apply(status);
    this.#reporter.status = "paused";
    const completedRanges = status.bytes > 0 ? [{ start: 0, end: status.bytes }] : [];
    return {
      ...this.getState(),
      status: "paused",
      checkpoint: {
        downloadId: this.downloadId,
        resource: this.#resource,
        dest: this.#dest,
        completedRanges,
        etag: undefined,
      },
    };
  }

  async resume(): Promise<void> {
    if (this.#cancelled) {
      throw new VortexError("Download cancelled", { kind: "user-canceled", skipped: false });
    }
    this.#reporter.status = "running";
    this.#apply(await this.#client.downloadResume(this.downloadId));
    this.#current = this.#poll();
  }
}

export type HybridDownloadOptions = {
  userAgent?: string;
  cookieJar?: CookieJar;
  proxy?: string;
};

/**
 * Routes main-process downloads through the core download queue when it is
 * available, falling back to the existing got-based manager otherwise.
 * The core handles the transfer, retries and pause/cancel; this adapter maps
 * its state onto Vortex's DownloadHandle so the IPC contract is unchanged.
 */
export class HybridDownloadManager {
  readonly #fallback: DownloadManager;
  readonly #client: NativeCoreClient;
  readonly #options: HybridDownloadOptions;
  readonly #native = new Map<string, NativeDownloadHandle>();
  #concurrency = 1;
  #bandwidthLimited = false;

  constructor(
    fallback: DownloadManager,
    client: NativeCoreClient,
    options: HybridDownloadOptions = {},
  ) {
    this.#fallback = fallback;
    this.#client = client;
    this.#options = options;
  }

  configure(options: { concurrency?: number; bytesPerSecond?: number }): void {
    if (options.concurrency !== undefined) {
      this.#concurrency = options.concurrency;
    }
    if (options.bytesPerSecond !== undefined) {
      // The core queue has no token-bucket limiter yet; keep the TypeScript
      // manager in charge when the user asked for a bandwidth cap.
      this.#bandwidthLimited = options.bytesPerSecond > 0;
    }
    this.#fallback.configure(options);
  }

  async download<T>(
    resource: T,
    dest: string,
    resolver: (resource: T) => Promise<ResolvedResource>,
    _chunker?: Chunker<T>,
    retry?: RetryStrategy,
    downloadId?: string,
  ): Promise<DownloadHandle<T>> {
    if (this.#bandwidthLimited) {
      return this.#fallback.download(resource, dest, resolver, undefined, retry, downloadId);
    }

    try {
      const handle = await this.#createNative(resource, dest, resolver, downloadId);
      if (handle !== undefined) {
        return handle;
      }
    } catch (err) {
      log("warn", "native download start failed, falling back", {
        error: err instanceof Error ? err.message : String(err),
      });
    }

    return this.#fallback.download(resource, dest, resolver, undefined, retry, downloadId);
  }

  async #createNative<T>(
    resource: T,
    dest: string,
    resolver: (resource: T) => Promise<ResolvedResource>,
    downloadId?: string,
  ): Promise<DownloadHandle<T> | undefined> {
    const status = await this.#client.status();
    if (
      !status.enabled ||
      !status.available ||
      status.capabilities === undefined ||
      !status.capabilities.includes("download.queue")
    ) {
      return undefined;
    }

    const resolved = await resolver(resource);
    const endpoint = probeEndpoint(resolved);
    if (!/^https?:/i.test(endpoint.url)) {
      return undefined;
    }

    const headers = { ...(endpoint.headers ?? {}) };
    if (this.#options.cookieJar !== undefined) {
      const cookie = await this.#options.cookieJar.getCookieString(endpoint.url);
      if (cookie.length > 0) {
        headers["Cookie"] = cookie;
      }
    }

    const id = downloadId ?? randomUUID();
    log("debug", "routing download through the native core", {
      downloadId: id,
      url: endpoint.url,
      dest,
    });
    const handle = new NativeDownloadHandle(this.#client, id, resource, resolved, dest, {
      headers: Object.keys(headers).length > 0 ? headers : undefined,
      userAgent: this.#options.userAgent,
      proxy: this.#options.proxy,
      concurrency: this.#concurrency,
    });
    this.#native.set(id, handle);
    return handle;
  }

  async resume<T>(
    checkpoint: DownloadCheckpoint<T>,
    resolver: (resource: T) => Promise<ResolvedResource>,
    chunker?: Chunker<T>,
    retry?: RetryStrategy,
  ): Promise<DownloadHandle<T>> {
    const native = this.#native.get(checkpoint.downloadId);
    if (native !== undefined) {
      try {
        await native.resume();
        return native as DownloadHandle<T>;
      } catch (err) {
        // The core may have restarted between pause and resume; its queue is
        // in-memory, so continue from the partial destination in TypeScript.
        this.#native.delete(checkpoint.downloadId);
        log("warn", "native download resume failed, falling back", {
          error: err instanceof Error ? err.message : String(err),
        });
      }
    }
    return this.#fallback.resume(checkpoint, resolver, chunker ?? staticChunker(), retry);
  }

  get(downloadId: string): DownloadHandle | undefined {
    return this.#native.get(downloadId) ?? this.#fallback.get(downloadId);
  }

  cancel(downloadId: string): DownloadState {
    const native = this.#native.get(downloadId);
    if (native !== undefined) {
      return native.cancel();
    }
    return this.#fallback.cancel(downloadId);
  }

  pause(downloadId: string): Promise<PauseResult<unknown>> {
    const native = this.#native.get(downloadId);
    if (native !== undefined) {
      return native.pause();
    }
    return this.#fallback.pause(downloadId);
  }
}
