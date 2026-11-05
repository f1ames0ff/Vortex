import { randomBytes } from "node:crypto";
import { existsSync } from "node:fs";
import { mkdtemp, readFile, rm } from "node:fs/promises";
import * as os from "node:os";
import * as path from "node:path";

import { describe, expect, it } from "vitest";

import { NativeCoreClient } from "../nativeDeploy/service";
import { DownloadManager } from "./manager";
import { HybridDownloadManager } from "./nativeManager";
import {
  createSharedTestServer,
  delayBeforeChunk,
  serveFile,
  withHooks,
  writeResponse,
  type TestServer,
} from "./test-server";

const corePath =
  process.env.VORTEX_CORE_PATH ??
  path.resolve(process.cwd(), "../../native/build/release/vortex-core");

function makeClient(): NativeCoreClient {
  return new NativeCoreClient({ resolveCorePath: () => corePath });
}

async function withServer<T>(
  body: Buffer,
  options: { acceptRanges?: boolean; chunkSize?: number; delayIndex?: number } = {},
  run: (url: URL, server: TestServer) => Promise<T>,
): Promise<T> {
  const server = await createSharedTestServer();
  const route = server.route(
    withHooks(
      serveFile({
        body,
        acceptRanges: options.acceptRanges ?? false,
        chunkSize: options.chunkSize,
      }),
      options.delayIndex === undefined ? {} : delayBeforeChunk(options.delayIndex, 2500),
    ),
  );
  try {
    return await run(route.url, server);
  } finally {
    route.deregister();
    await server.close();
  }
}

describe.skipIf(!existsSync(corePath))("HybridDownloadManager", () => {
  it("uses the core queue and forwards endpoint headers", async () => {
    const body = randomBytes(512 * 1024);
    const server = await createSharedTestServer();
    const route = server.route(async (ctx) => {
      if (ctx.req.headers["x-api-key"] !== "secret") {
        await writeResponse(ctx, 401, {});
        return;
      }
      await serveFile({ body })(ctx);
    });
    const tmp = await mkdtemp(path.join(os.tmpdir(), "native-download-"));
    const client = makeClient();
    try {
      const manager = new HybridDownloadManager(new DownloadManager({ concurrency: 1 }), client);
      const dest = path.join(tmp, "output.bin");
      const handle = await manager.download(route.url, dest, () =>
        Promise.resolve({
          url: new URL(route.url),
          headers: { "x-api-key": "secret" },
        }),
      );

      await handle.promise;

      expect(client.spawned).toBe(true);
      expect(handle.getState().status).toBe("completed");
      expect(handle.getState().bytesReceived).toBe(body.length);
      expect(Buffer.compare(await readFile(dest), body)).toBe(0);
    } finally {
      await client.shutdown();
      route.deregister();
      await server.close();
      await rm(tmp, { recursive: true, force: true });
    }
  });

  it("pauses to a resumable destination and resumes through the core", async () => {
    const body = randomBytes(2 * 1024 * 1024);
    const tmp = await mkdtemp(path.join(os.tmpdir(), "native-download-"));
    const client = makeClient();
    try {
      await withServer(
        body,
        { acceptRanges: true, chunkSize: 64 * 1024, delayIndex: 2 },
        async (url) => {
          const manager = new HybridDownloadManager(
            new DownloadManager({ concurrency: 1 }),
            client,
          );
          const dest = path.join(tmp, "paused.bin");
          const handle = await manager.download(url, dest, () =>
            Promise.resolve({ url: new URL(url) }),
          );

          // Wait until the first chunks landed, then pause mid-transfer.
          for (let attempt = 0; attempt < 200 && handle.getState().bytesReceived === 0; ++attempt) {
            await new Promise((resolve) => setTimeout(resolve, 25));
          }
          const paused = await handle.pause();
          expect(paused.status).toBe("paused");
          if (paused.status !== "paused") throw new Error("not paused");
          expect(paused.checkpoint.completedRanges.length).toBeGreaterThan(0);
          expect(existsSync(dest)).toBe(true);
          const pausedBytes = handle.getState().bytesReceived;
          expect(pausedBytes).toBeGreaterThan(0);
          expect(pausedBytes).toBeLessThan(body.length);

          const resumed = await manager.resume(paused.checkpoint, () =>
            Promise.resolve({ url: new URL(url) }),
          );
          await resumed.promise;

          expect(resumed.getState().status).toBe("completed");
          expect(Buffer.compare(await readFile(dest), body)).toBe(0);
        },
      );
    } finally {
      await client.shutdown();
      await rm(tmp, { recursive: true, force: true });
    }
  });
});
