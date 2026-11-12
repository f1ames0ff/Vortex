/**
 * Native core collection E2E.
 *
 * Launches a fully isolated Vortex instance (temp ELECTRON_USERDATA and
 * ELECTRON_APPDATA, no real profile and no game) and runs a prebuilt
 * collection task through the real main-process IPC and vortex-core process.
 */
import { execFileSync } from "node:child_process";
import fs from "node:fs";
import { createServer } from "node:http";
import os from "node:os";
import path from "node:path";

import { test, expect } from "../fixtures/vortex-app";

process.env.VORTEX_NATIVE_DEPLOY = "1";

const sevenZip = process.env.VORTEX_7Z_PATH ?? "7z";

test.describe("Native collection core", () => {
  test("installs a staged collection member through collection.run", async ({
    vortexApp,
    vortexWindow,
  }) => {
    const work = fs.mkdtempSync(path.join(os.tmpdir(), "vortex-e2e-collection-"));
    let server: ReturnType<typeof createServer> | undefined;
    try {
      const source = path.join(work, "source");
      const archive = path.join(work, "fixture.zip");
      const downloads = path.join(work, "downloads");
      const extract = path.join(work, "extract");
      const staging = path.join(work, "staging");
      fs.mkdirSync(source, { recursive: true });
      fs.writeFileSync(path.join(source, "hello.txt"), "collection-e2e");

      execFileSync(sevenZip, ["a", "-tzip", archive, "hello.txt"], {
        cwd: source,
        stdio: "ignore",
      });
      const archiveBytes = fs.readFileSync(archive);
      let authorization: string | undefined;
      let cookie: string | undefined;
      server = createServer((request, response) => {
        authorization = request.headers.authorization;
        cookie = request.headers.cookie;
        if (authorization !== "Bearer collection-e2e" || cookie !== "session=e2e") {
          response.writeHead(401).end();
          return;
        }
        response.writeHead(200, { "content-type": "application/zip" });
        response.end(archiveBytes);
      });
      await new Promise<void>((resolve) => server!.listen(0, "127.0.0.1", resolve));
      const address = server.address();
      if (address === null || typeof address === "string") {
        throw new Error("failed to bind collection download server");
      }

      await vortexApp.evaluate(async (electron: typeof import("electron"), url: string) => {
        await electron.session.defaultSession.cookies.set({
          url,
          name: "session",
          value: "e2e",
        });
      }, `http://127.0.0.1:${address.port}/`);

      await vortexWindow.waitForFunction(
        () =>
          (globalThis as unknown as { api?: { nativeDeploy?: unknown } }).api?.nativeDeploy !==
          undefined,
      );

      const result = await vortexWindow.evaluate(
        async ({ url, downloads, extract, staging }) => {
          const api = (
            globalThis as unknown as {
              api: {
                nativeDeploy: {
                  collectionStart: (request: unknown) => Promise<{
                    jobId: string;
                    status: { complete: boolean; members: Array<{ status: string }> };
                  }>;
                  collectionRun: (jobId: string, taskOnly?: boolean) => Promise<unknown>;
                  collectionStatus: (jobId: string) => Promise<{
                    complete: boolean;
                    members: Array<{ status: string; error?: string; result?: unknown }>;
                  }>;
                };
              };
            }
          ).api;

          const job = await api.nativeDeploy.collectionStart({
            collectionId: `e2e-${Date.now()}`,
            gameId: "skyrimse",
            concurrency: 1,
            maxRetries: 0,
            members: [
              {
                ruleIndex: 0,
                ruleId: "requires_e2e",
                type: "requires",
                phase: 1,
                ignored: false,
                status: "pending",
                task: {
                  modId: "E2ECollectionMod",
                  download: {
                    directory: downloads,
                    fileName: "fixture.zip",
                    urls: [url],
                    headers: { Authorization: "Bearer collection-e2e" },
                    maxRetries: 0,
                  },
                  extract: { destination: extract },
                  install: { temp: extract, destination: staging, instructions: [] },
                  copyAll: true,
                },
              },
            ],
          });
          await api.nativeDeploy.collectionRun(job.jobId, true);

          const deadline = Date.now() + 20_000;
          let status = await api.nativeDeploy.collectionStatus(job.jobId);
          while (!status.complete && Date.now() < deadline) {
            await new Promise((resolve) => setTimeout(resolve, 100));
            status = await api.nativeDeploy.collectionStatus(job.jobId);
          }
          return status;
        },
        {
          url: `http://127.0.0.1:${address.port}/fixture.zip`,
          downloads,
          extract,
          staging,
        },
      );

      expect(authorization).toBe("Bearer collection-e2e");
      expect(cookie).toBe("session=e2e");
      expect(result.members[0]?.status).toBe("installed");
      expect(result.complete).toBe(true);
      expect(fs.readFileSync(path.join(staging, "hello.txt"), "utf8")).toBe("collection-e2e");
    } finally {
      server?.close();
      fs.rmSync(work, { recursive: true, force: true });
    }
  });
});
