import { createServer } from "node:http";
import * as path from "node:path";

import { afterEach, describe, expect, it } from "vitest";

import { NativeCoreClient } from "../nativeDeploy/service";

const corePath =
  process.env.VORTEX_CORE_PATH ??
  path.resolve(process.cwd(), "../../native/build/release/vortex-core");
const helperScript = path.resolve(process.cwd(), "../../native/helpers/vortex-helper.mjs");

describe("native OAuth refresh", () => {
  let client: NativeCoreClient | undefined;

  afterEach(async () => {
    await client?.shutdown();
    client = undefined;
  });

  it("trades a refresh token through the core helper", async () => {
    let body = "";
    const server = createServer((request, response) => {
      request.on("data", (chunk) => (body += chunk));
      request.on("end", () => {
        response.writeHead(200, { "content-type": "application/json" });
        response.end(
          JSON.stringify({
            access_token: "new-access",
            refresh_token: "new-refresh",
            expires_in: 3600,
            token_type: "Bearer",
            scope: "profile",
          }),
        );
      });
    });
    await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", resolve));
    const address = server.address();
    if (address === null || typeof address === "string") {
      throw new Error("failed to bind mock OAuth server");
    }

    client = new NativeCoreClient({ resolveCorePath: () => corePath });
    try {
      const result = await client.oauthRefresh({
        tokenUrl: `http://127.0.0.1:${address.port}/oauth/token`,
        clientId: "vortex_loopback",
        refreshToken: "old-refresh",
        node: process.execPath,
        script: helperScript,
        env: { ELECTRON_RUN_AS_NODE: "1" },
      });

      expect(body).toBe(
        "grant_type=refresh_token&client_id=vortex_loopback&refresh_token=old-refresh",
      );
      expect(result).toEqual({
        ok: true,
        token: {
          access_token: "new-access",
          refresh_token: "new-refresh",
          expires_in: 3600,
          token_type: "Bearer",
          scope: "profile",
        },
      });
    } finally {
      server.close();
    }
  });

  it("returns the OAuth refusal code", async () => {
    const server = createServer((_request, response) => {
      response.writeHead(400, { "content-type": "application/json" });
      response.end(JSON.stringify({ error: "invalid_grant", error_description: "expired" }));
    });
    await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", resolve));
    const address = server.address();
    if (address === null || typeof address === "string") {
      throw new Error("failed to bind mock OAuth server");
    }

    client = new NativeCoreClient({ resolveCorePath: () => corePath });
    try {
      const result = await client.oauthRefresh({
        tokenUrl: `http://127.0.0.1:${address.port}/oauth/token`,
        clientId: "vortex_loopback",
        refreshToken: "dead-refresh",
        node: process.execPath,
        script: helperScript,
        env: { ELECTRON_RUN_AS_NODE: "1" },
      });

      expect(result).toEqual({
        ok: false,
        code: "invalid_grant",
        message: "expired",
      });
    } finally {
      server.close();
    }
  });
});
