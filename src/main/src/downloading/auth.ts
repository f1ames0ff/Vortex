import type { NativeDownloadRequest } from "@vortex/shared/ipc";
import type { Session } from "electron";

let downloadSession: Session | undefined;

/** Called once the main window (and therefore its Electron session) exists. */
export function configureDownloadAuth(session: Session): void {
  downloadSession = session;
}

export function resetDownloadAuthForTests(): void {
  downloadSession = undefined;
}

function isLoopback(hostname: string): boolean {
  return (
    hostname === "localhost" ||
    hostname === "127.0.0.1" ||
    hostname === "::1" ||
    hostname === "[::1]"
  );
}

function hasHeader(headers: Record<string, string>, name: string): boolean {
  const lower = name.toLowerCase();
  return Object.keys(headers).some((key) => key.toLowerCase() === lower);
}

async function cookieHeaderFor(url: string): Promise<string | undefined> {
  if (downloadSession === undefined) {
    return undefined;
  }
  const cookies = await downloadSession.cookies.get({ url });
  if (cookies.length === 0) {
    return undefined;
  }
  return cookies.map((cookie) => `${cookie.name}=${cookie.value}`).join("; ");
}

async function proxyFor(url: string): Promise<string | undefined> {
  if (downloadSession === undefined) {
    return undefined;
  }
  const parsed = new URL(url);
  // Local test servers and the E2E fixtures must stay direct; Chromium may
  // still report a system proxy for the loopback host.
  if (isLoopback(parsed.hostname)) {
    return undefined;
  }
  const resolved = await downloadSession.resolveProxy(url);
  const first = resolved.split(";")[0]?.trim();
  if (first === undefined || first === "" || first === "DIRECT") {
    return undefined;
  }
  const [scheme, address] = first.split(/\s+/, 2);
  if (scheme === undefined || address === undefined) {
    return undefined;
  }
  switch (scheme.toUpperCase()) {
    case "PROXY":
    case "HTTP":
      return `http://${address}`;
    case "HTTPS":
      return `https://${address}`;
    case "SOCKS":
    case "SOCKS4":
    case "SOCKS5":
      return `socks5://${address}`;
    default:
      return undefined;
  }
}

/**
 * Adds the Electron session cookie and system proxy to a core download
 * request. Explicit caller headers/proxy always win.
 */
export async function applyDownloadAuth(
  request: NativeDownloadRequest,
): Promise<NativeDownloadRequest> {
  const url = request.urls[0];
  if (url === undefined || downloadSession === undefined) {
    return request;
  }

  const headers = { ...(request.headers ?? {}) };
  if (!hasHeader(headers, "cookie")) {
    const cookie = await cookieHeaderFor(url);
    if (cookie !== undefined) {
      headers["Cookie"] = cookie;
    }
  }

  const proxy = request.proxy ?? (await proxyFor(url));
  return {
    ...request,
    headers: Object.keys(headers).length > 0 ? headers : undefined,
    proxy,
  };
}
