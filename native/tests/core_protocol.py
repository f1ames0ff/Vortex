#!/usr/bin/env python3
"""Smoke test for the line-delimited JSON core protocol."""

import json
import os
import shutil
import subprocess
import sys
import tempfile


def main() -> int:
    core = sys.argv[1]
    root = tempfile.mkdtemp(prefix="vortex-core-protocol-")
    staging = os.path.join(root, "staging")
    target = os.path.join(root, "target")
    manifest = os.path.join(root, "manifest.json")
    mod = os.path.join(staging, "ModA")
    os.makedirs(mod)
    os.makedirs(target)
    with open(os.path.join(mod, "a.txt"), "w", encoding="utf-8") as stream:
        stream.write("mod-a")
    with open(os.path.join(target, "a.txt"), "w", encoding="utf-8") as stream:
        stream.write("game-a")

    process = subprocess.Popen(
        [core],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        text=True,
        encoding="utf-8",
    )
    requests = []

    def raw(rpc_method: str, **params):
        request = {"id": len(requests) + 1, "method": rpc_method, "params": params}
        requests.append(request)
        process.stdin.write(json.dumps(request) + "\n")
        process.stdin.flush()
        return json.loads(process.stdout.readline())

    def call(rpc_method: str, **params):
        response = raw(rpc_method, **params)
        if "error" in response:
            raise AssertionError(f"{rpc_method} failed: {response['error']}")
        return response["result"]

    try:
        common = {
            "staging": staging,
            "target": target,
            "manifest": manifest,
            "instance": "test",
            "method": "hardlink",
            "game": "skyrimse",
        }
        mods = [{"installationPath": "ModA", "sourceName": "ModA", "target": ""}]

        assert call("ping")["version"]

        process.stdin.write("not json\n")
        process.stdin.flush()
        malformed = json.loads(process.stdout.readline())
        assert "error" in malformed
        assert call("ping")["version"]

        unknown = raw("unknown-method")
        assert "error" in unknown
        deploy = call("deploy", **common, mods=mods, includeFiles=True)
        assert deploy["report"]["linked"] == 1
        assert not deploy["report"]["failures"]
        assert deploy["files"] == [
            {"relPath": "a.txt", "source": "ModA", "time": deploy["files"][0]["time"]}
        ]
        with open(os.path.join(target, "a.txt"), encoding="utf-8") as stream:
            assert stream.read() == "mod-a"

        assert call("plan", **common, mods=mods)["added"] == 0
        purge = call("purge", **common)
        assert purge["report"]["restored"] == 1
        with open(os.path.join(target, "a.txt"), encoding="utf-8") as stream:
            assert stream.read() == "game-a"

        call("shutdown")
        process.wait(timeout=5)
        return 0
    finally:
        if process.poll() is None:
            process.kill()
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
