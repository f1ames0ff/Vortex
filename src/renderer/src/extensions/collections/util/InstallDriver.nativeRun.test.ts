import { expect, vi } from "vitest";

import {
  makeCollectionModInfo,
  makeDownload,
  makeMod,
  makeReference,
  makeRule,
} from "../../../test-utils/builders";
import { test as driverTest } from "../../../test-utils/driverTest";
import type { IProfile } from "../../profile_management/types/IProfile";
import { MOD_TYPE } from "../constants";

const native = vi.hoisted(() => ({
  plan: vi.fn(),
  start: vi.fn(),
  run: vi.fn(),
}));

vi.mock("../../mod_management/util/nativeDeploy", async (importOriginal) => ({
  ...(await importOriginal<typeof import("../../mod_management/util/nativeDeploy")>()),
  planCollectionNatively: native.plan,
  startCollectionNatively: native.start,
  runCollectionJobNatively: native.run,
}));

const GAME_ID = "skyrimse";
const profile = {
  id: "prof-1",
  gameId: GAME_ID,
  modState: {},
  name: "test",
} as unknown as IProfile;

function fixture(hasTask: boolean) {
  const id = "collection-native";
  const collection = makeMod({
    id,
    type: MOD_TYPE,
    archiveId: `dl-${id}`,
    installationPath: `mods/${id}`,
    rules: [
      makeRule({
        type: "requires",
        reference: makeReference({ tag: "mod-a" }),
      }),
    ],
  });
  const download = makeDownload({
    id: `dl-${id}`,
    state: "finished",
    localPath: `${id}.7z`,
    modInfo: makeCollectionModInfo({ collectionId: 1, gameId: GAME_ID }),
  });

  const member = {
    ruleIndex: 0,
    ruleId: "requires_mod-a",
    type: "requires",
    phase: 0,
    ignored: false,
    status: "pending",
    hasTask,
  };
  const status = {
    jobId: "job-1",
    collectionId: id,
    gameId: GAME_ID,
    phase: 0,
    phases: [0],
    members: [member],
    counts: {},
    complete: false,
    paused: false,
    cancelled: false,
    stalled: false,
    startedAt: 0,
    lastProgress: 0,
  };

  native.plan.mockResolvedValue({
    members: [member],
    phases: [0],
    totalRequired: 1,
    totalOptional: 0,
  });
  native.start.mockResolvedValue({ jobId: "job-1", status });
  native.run.mockResolvedValue({ ...status, members: [{ ...member, status: "installing" }] });
  return { collection, download };
}

driverTest(
  "InstallDriver starts collection.run when every member has a task",
  async ({ makeDriver }) => {
    native.plan.mockClear();
    native.start.mockClear();
    native.run.mockClear();
    const data = fixture(true);
    const h = makeDriver({
      mods: { [GAME_ID]: { [data.collection.id]: data.collection } },
      downloads: { [data.download.id]: data.download },
      profiles: { [profile.id]: profile },
    });
    await h.driver.start(profile, data.collection);
    await vi.waitFor(() => expect(native.run).toHaveBeenCalledWith("job-1"));
  },
);

driverTest(
  "InstallDriver keeps pull scheduling when a member has no task",
  async ({ makeDriver }) => {
    native.plan.mockClear();
    native.start.mockClear();
    native.run.mockClear();
    const data = fixture(false);
    const h = makeDriver({
      mods: { [GAME_ID]: { [data.collection.id]: data.collection } },
      downloads: { [data.download.id]: data.download },
      profiles: { [profile.id]: profile },
    });
    await h.driver.start(profile, data.collection);
    await vi.waitFor(() => expect(native.start).toHaveBeenCalled());
    expect(native.run).not.toHaveBeenCalled();
  },
);
