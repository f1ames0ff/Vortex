import { spawn } from "node:child_process";
import { createInterface } from "node:readline";

/**
 * Minimal client for the line-delimited JSON protocol spoken by vortex-core.
 * The Electron main process can use this as the first step towards the new
 * shell/core boundary.
 */
export class VortexCoreClient {
  #child;
  #nextId = 1;
  #pending = new Map();

  constructor(corePath) {
    this.#child = spawn(corePath, [], {
      stdio: ["pipe", "pipe", "inherit"],
    });

    const lines = createInterface({ input: this.#child.stdout });
    lines.on("line", (line) => {
      let response;
      try {
        response = JSON.parse(line);
      } catch {
        return;
      }
      const pending = this.#pending.get(response.id);
      if (pending === undefined) {
        return;
      }
      this.#pending.delete(response.id);
      if (response.error) {
        pending.reject(new Error(`${response.error.code}: ${response.error.message}`));
      } else {
        pending.resolve(response.result);
      }
    });

    this.#child.on("exit", (code) => {
      const error = new Error(`vortex-core exited with code ${code}`);
      for (const pending of this.#pending.values()) {
        pending.reject(error);
      }
      this.#pending.clear();
    });
  }

  stagingStatus(request) {
    return this.request("staging.status", request);
  }

  gameRegistry(request) {
    return this.request("game.registry", request);
  }

  installApply(request) {
    return this.request("install.apply", request);
  }

  archiveList(request) {
    return this.request("archive.list", request);
  }

  archiveExtract(request) {
    return this.request("archive.extract", request);
  }

  hashFiles(request) {
    return this.request("hash.files", request);
  }

  matchMod(request) {
    return this.request("deps.matchMod", request);
  }

  matchDownload(request) {
    return this.request("deps.matchDownload", request);
  }

  resolveDependencies(request) {
    return this.request("deps.resolve", request);
  }

  planCollection(request) {
    return this.request("collection.plan", request);
  }

  request(method, params = {}) {
    const id = this.#nextId++;
    const request = { id, method, params };
    return new Promise((resolve, reject) => {
      this.#pending.set(id, { resolve, reject });
      this.#child.stdin.write(`${JSON.stringify(request)}\n`);
    });
  }

  async close() {
    try {
      await this.request("shutdown");
    } catch {
      // the process may already be gone
    }
    this.#child.stdin.end();
  }
}
