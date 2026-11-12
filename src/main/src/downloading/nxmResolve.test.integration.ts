import { createServer } from "node:http";
import { createRequire } from "node:module";
import * as path from "node:path";

import { afterEach, describe, expect, it } from "vitest";

import { NativeCoreClient } from "../nativeDeploy/service";

const corePath =
  process.env.VORTEX_CORE_PATH ??
  path.resolve(process.cwd(), "../../native/build/release/vortex-core");
const helperScript = path.resolve(process.cwd(), "../../native/helpers/vortex-helper.mjs");
const requireFromMain = createRequire(import.meta.url);
const nexusApiModule = requireFromMain.resolve("@nexusmods/nexus-api");

describe("native nxm resolution", () => {
  let client: NativeCoreClient | undefined;

  afterEach(async () => {
    await client?.shutdown();
    client = undefined;
  });

  it("resolves an authenticated Nexus mod link through the core helper", async () => {
    let requestUrl: URL | undefined;
    let authorization: string | undefined;
    const server = createServer((request, response) => {
      requestUrl = new URL(request.url ?? "/", "http://localhost");
      authorization = request.headers.authorization;
      response.writeHead(200, { "content-type": "application/json" });
      response.end(
        JSON.stringify([
          { URI: "https://cdn.example.com/file.zip", name: "file", short_name: "cdn" },
        ]),
      );
    });
    await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", resolve));
    const address = server.address();
    if (address === null || typeof address === "string") {
      throw new Error("failed to bind mock Nexus API");
    }
    const base = `http://127.0.0.1:${address.port}`;

    client = new NativeCoreClient({ resolveCorePath: () => corePath });
    try {
      const result = await client.downloadResolve({
        type: "mod",
        gameDomain: "skyrimspecialedition",
        modId: 100,
        fileId: 500,
        key: "abc",
        expires: 123,
        source: { placement: "collection", position: 1 },
        accessToken: "test-token",
        node: process.execPath,
        script: helperScript,
        env: { ELECTRON_RUN_AS_NODE: "1" },
        appVersion: "0.1.0",
        nexusApiModule,
        apiUrls: {
          base,
          api: `${base}/v1`,
          graphql: `${base}/v2/graphql`,
          userService: base,
        },
      });

      expect(authorization).toBe("Bearer test-token");
      expect(requestUrl?.pathname).toBe(
        "/v1/games/skyrimspecialedition/mods/100/files/500/download_link",
      );
      expect(requestUrl?.searchParams.get("key")).toBe("abc");
      expect(requestUrl?.searchParams.get("expires")).toBe("123");
      expect(requestUrl?.searchParams.get("placement")).toBe("collection");
      expect(requestUrl?.searchParams.get("position")).toBe("1");
      expect(result).toEqual({
        urls: ["https://cdn.example.com/file.zip"],
        ids: { modId: 100, fileId: 500 },
      });
    } finally {
      server.close();
    }
  });

  it("resolves an API-key authenticated mod link", async () => {
    let apiKeyHeader: string | undefined;
    const server = createServer((request, response) => {
      const url = new URL(request.url ?? "/", "http://localhost");
      if (url.pathname === "/v1/users/validate") {
        apiKeyHeader = request.headers.apikey as string | undefined;
        response.writeHead(200, { "content-type": "application/json" });
        response.end(JSON.stringify({ user_id: 1 }));
        return;
      }
      apiKeyHeader = request.headers.apikey as string | undefined;
      response.writeHead(200, { "content-type": "application/json" });
      response.end(
        JSON.stringify([
          { URI: "https://cdn.example.com/api-key.zip", name: "file", short_name: "cdn" },
        ]),
      );
    });
    await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", resolve));
    const address = server.address();
    if (address === null || typeof address === "string") {
      throw new Error("failed to bind mock Nexus API");
    }
    const base = `http://127.0.0.1:${address.port}`;

    client = new NativeCoreClient({ resolveCorePath: () => corePath });
    try {
      const result = await client.downloadResolve({
        type: "mod",
        gameDomain: "skyrimspecialedition",
        modId: 100,
        fileId: 500,
        apiKey: "api-key",
        node: process.execPath,
        script: helperScript,
        env: { ELECTRON_RUN_AS_NODE: "1" },
        appVersion: "0.1.0",
        nexusApiModule,
        apiUrls: {
          base,
          api: `${base}/v1`,
          graphql: `${base}/v2/graphql`,
          userService: base,
        },
      });

      expect(apiKeyHeader).toBe("api-key");
      expect(result).toEqual({
        urls: ["https://cdn.example.com/api-key.zip"],
        ids: { modId: 100, fileId: 500 },
      });
    } finally {
      server.close();
    }
  });

  it("resolves an authenticated collection revision", async () => {
    let graphqlVariables: Record<string, unknown> | undefined;
    let authorization: string | undefined;
    const server = createServer((request, response) => {
      const url = new URL(request.url ?? "/", "http://localhost");
      if (url.pathname === "/v2/graphql") {
        authorization = request.headers.authorization;
        let body = "";
        request.on("data", (chunk) => (body += chunk));
        request.on("end", () => {
          graphqlVariables = (JSON.parse(body) as { variables?: Record<string, unknown> })
            .variables;
          response.writeHead(200, { "content-type": "application/json" });
          response.end(
            JSON.stringify({
              data: {
                collectionRevision: {
                  id: 11,
                  revisionNumber: 3,
                  downloadLink: "/download/rev3",
                  collection: { id: 10 },
                },
              },
            }),
          );
        });
        return;
      }
      if (url.pathname === "/download/rev3") {
        response.writeHead(200, { "content-type": "application/json" });
        response.end(
          JSON.stringify({
            download_links: [
              {
                URI: "https://cdn.example.com/collection.zip",
                name: "collection",
                short_name: "cdn",
              },
            ],
          }),
        );
        return;
      }
      response.writeHead(404).end();
    });
    await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", resolve));
    const address = server.address();
    if (address === null || typeof address === "string") {
      throw new Error("failed to bind mock Nexus API");
    }
    const base = `http://127.0.0.1:${address.port}`;

    client = new NativeCoreClient({ resolveCorePath: () => corePath });
    try {
      const result = await client.downloadResolve({
        type: "collection",
        gameDomain: "skyrimspecialedition",
        collection: { slug: "abcdef", revisionNumber: 3 },
        accessToken: "test-token",
        node: process.execPath,
        script: helperScript,
        env: { ELECTRON_RUN_AS_NODE: "1" },
        appVersion: "0.1.0",
        nexusApiModule,
        apiUrls: {
          base,
          api: `${base}/v1`,
          graphql: `${base}/v2/graphql`,
          userService: base,
        },
      });

      expect(authorization).toBe("Bearer test-token");
      expect(graphqlVariables).toEqual({
        slug: "abcdef",
        revision: 3,
        viewAdultContent: true,
      });
      expect(result).toEqual({
        urls: ["https://cdn.example.com/collection.zip"],
        ids: {
          collectionId: 10,
          revisionId: 11,
          collectionSlug: "abcdef",
          revisionNumber: 3,
        },
      });
    } finally {
      server.close();
    }
  });
});
