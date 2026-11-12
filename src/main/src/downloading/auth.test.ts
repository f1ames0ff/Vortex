import type { NativeDownloadRequest } from "@vortex/shared/ipc";
import type { Session } from "electron";
import { afterEach, describe, expect, it, vi } from "vitest";

import { applyDownloadAuth, configureDownloadAuth, resetDownloadAuthForTests } from "./auth";

function fakeSession(options?: {
  cookies?: Array<{ name: string; value: string }>;
  proxy?: string;
}): Session {
  return {
    cookies: {
      get: vi.fn().mockResolvedValue(options?.cookies ?? []),
    },
    resolveProxy: vi.fn().mockResolvedValue(options?.proxy ?? "DIRECT"),
  } as unknown as Session;
}

function request(overrides: Partial<NativeDownloadRequest> = {}): NativeDownloadRequest {
  return {
    directory: "/tmp",
    fileName: "file.bin",
    urls: ["https://cdn.example.com/file.bin"],
    ...overrides,
  };
}

afterEach(() => {
  resetDownloadAuthForTests();
});

describe("download auth", () => {
  it("adds session cookies and the resolved proxy", async () => {
    configureDownloadAuth(
      fakeSession({
        cookies: [
          { name: "session", value: "abc" },
          { name: "premium", value: "1" },
        ],
        proxy: "PROXY proxy.local:8080; DIRECT",
      }),
    );

    const result = await applyDownloadAuth(request());

    expect(result.headers).toEqual({ Cookie: "session=abc; premium=1" });
    expect(result.proxy).toBe("http://proxy.local:8080");
  });

  it("keeps explicit cookies and proxy", async () => {
    configureDownloadAuth(
      fakeSession({
        cookies: [{ name: "session", value: "from-session" }],
        proxy: "PROXY proxy.local:8080",
      }),
    );

    const result = await applyDownloadAuth(
      request({
        headers: { Cookie: "explicit=1" },
        proxy: "http://explicit-proxy:3128",
      }),
    );

    expect(result.headers).toEqual({ Cookie: "explicit=1" });
    expect(result.proxy).toBe("http://explicit-proxy:3128");
  });

  it("does not proxy loopback downloads", async () => {
    configureDownloadAuth(
      fakeSession({
        cookies: [{ name: "session", value: "abc" }],
        proxy: "PROXY proxy.local:8080",
      }),
    );

    const result = await applyDownloadAuth(request({ urls: ["http://127.0.0.1:1234/file.bin"] }));

    expect(result.headers).toEqual({ Cookie: "session=abc" });
    expect(result.proxy).toBeUndefined();
  });

  it("leaves requests untouched without a configured session", async () => {
    const original = request();
    expect(await applyDownloadAuth(original)).toBe(original);
  });
});
