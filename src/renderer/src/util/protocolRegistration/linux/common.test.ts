import { beforeEach, describe, expect, it, vi } from "vitest";

const mocks = vi.hoisted(() => ({
  spawnSync: vi.fn(() => ({ status: 0, stdout: "", stderr: "" })),
}));

vi.mock("child_process", () => ({
  spawnSync: mocks.spawnSync,
  default: { spawnSync: mocks.spawnSync },
}));
vi.mock("../../log", () => ({ log: vi.fn() }));

import { setDefaultMimeHandler } from "./common";

describe("linux protocol registration helpers", () => {
  beforeEach(() => {
    mocks.spawnSync.mockClear();
  });

  it("sets a URL scheme through xdg-mime instead of xdg-settings", () => {
    setDefaultMimeHandler("nxm", "com.nexusmods.vortex.desktop");

    expect(mocks.spawnSync).toHaveBeenCalledWith(
      "xdg-mime",
      ["default", "com.nexusmods.vortex.desktop", "x-scheme-handler/nxm"],
      expect.objectContaining({ encoding: "utf8" }),
    );
  });
});
