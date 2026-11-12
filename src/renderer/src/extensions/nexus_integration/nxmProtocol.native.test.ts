import { expect, vi } from "vitest";

import type { INxmFixtures } from "@/test-utils/nxmTest";
import { COLLECTION_URL, FREE, MOD_URL, test } from "@/test-utils/nxmTest";

import {
  addNxmQueueEntry,
  clearNxmQueue,
  removeNxmQueueEntry,
  resolveNxmNatively,
  updateNxmQueueEntry,
} from "../mod_management/util/nativeDeploy";
import { getAccessToken } from "./util/oauthSession";

vi.mock("./util/oauthSession", () => ({
  getAccessToken: vi.fn(),
}));

vi.mock("../mod_management/util/nativeDeploy", () => ({
  resolveNxmNatively: vi.fn(),
  addNxmQueueEntry: vi.fn(),
  clearNxmQueue: vi.fn(),
  listNxmQueue: vi.fn(),
  matchNxmQueueEntry: vi.fn(),
  removeNxmQueueEntry: vi.fn(),
  updateNxmQueueEntry: vi.fn(),
}));

vi.mock("./util", async (importOriginal) => ({
  ...(await importOriginal<typeof import("./util")>()),
  getInfoGraphQL: vi.fn().mockResolvedValue({ modInfo: { direct_download_enabled: false } }),
}));

const nativeResolve = vi.mocked(resolveNxmNatively);
const accessToken = vi.mocked(getAccessToken);
const queueAdd = vi.mocked(addNxmQueueEntry);
const queueClear = vi.mocked(clearNxmQueue);
const queueRemove = vi.mocked(removeNxmQueueEntry);
const queueUpdate = vi.mocked(updateNxmQueueEntry);

const downloadLink = (uri: string) => [{ URI: uri, short_name: "cdn", name: "CDN" }];

test.describe("NxmProtocol native resolution", () => {
  test.beforeEach(() => {
    accessToken.mockReset();
    nativeResolve.mockReset();
    queueAdd.mockReset();
    queueClear.mockReset();
    queueRemove.mockReset();
    queueUpdate.mockReset();
  });

  test("uses the core helper for authenticated mod links", async ({ makeNxm }: INxmFixtures) => {
    accessToken.mockResolvedValue("native-token");
    nativeResolve.mockResolvedValue({
      urls: ["https://cdn.example.com/native.zip"],
      ids: { modId: 100, fileId: 500 },
    });
    const { harness, resolve } = makeNxm();

    const result = await resolve(MOD_URL);

    expect(nativeResolve).toHaveBeenCalledWith(
      expect.objectContaining({
        type: "mod",
        gameDomain: "skyrimspecialedition",
        modId: 100,
        fileId: 500,
        accessToken: "native-token",
      }),
    );
    expect(result.urls).toEqual(["https://cdn.example.com/native.zip"]);
    expect(result.meta).toEqual({
      source: "nexus",
      nexus: { ids: { modId: 100, fileId: 500 } },
    });
    expect(harness.getDownloadURLs).not.toHaveBeenCalled();
  });

  test("uses the core helper for collections", async ({ makeNxm }) => {
    accessToken.mockResolvedValue("native-token");
    nativeResolve.mockResolvedValue({
      urls: ["https://cdn.example.com/collection.zip"],
      ids: {
        collectionId: 10,
        revisionId: 11,
        collectionSlug: "abcdef",
        revisionNumber: 3,
      },
    });
    const { harness, resolve } = makeNxm();

    const result = await resolve(COLLECTION_URL);

    expect(nativeResolve).toHaveBeenCalledWith(
      expect.objectContaining({
        type: "collection",
        collection: { slug: "abcdef", revisionNumber: 3 },
        accessToken: "native-token",
      }),
    );
    expect(result.urls).toEqual(["https://cdn.example.com/collection.zip"]);
    expect(result.meta).toEqual({
      source: "nexus",
      nexus: {
        ids: {
          collectionId: 10,
          revisionId: 11,
          collectionSlug: "abcdef",
          revisionNumber: 3,
        },
      },
    });
    expect(harness.getCollectionRevisionGraph).not.toHaveBeenCalled();
  });

  test("uses the core helper with an API key", async ({ makeNxm }) => {
    accessToken.mockResolvedValue(undefined);
    nativeResolve.mockResolvedValue({
      urls: ["https://cdn.example.com/api-key.zip"],
      ids: { modId: 100, fileId: 500 },
    });
    const { harness, resolve } = makeNxm();
    harness.setState((draft) => {
      (draft.confidential.account as { nexus?: { APIKey?: string } }).nexus = {
        APIKey: "api-key",
      };
    });

    const result = await resolve(MOD_URL);

    expect(nativeResolve).toHaveBeenCalledWith(
      expect.objectContaining({ type: "mod", apiKey: "api-key" }),
    );
    expect(result.urls).toEqual(["https://cdn.example.com/api-key.zip"]);
    expect(harness.getDownloadURLs).not.toHaveBeenCalled();
  });

  test("mirrors the free-user dialog queue into the core", async ({ makeNxm }) => {
    accessToken.mockResolvedValue(undefined);
    queueAdd.mockResolvedValue({
      input: MOD_URL,
      gameDomain: "skyrimspecialedition",
      type: "mod",
      modId: 100,
      fileId: 500,
      collectionSlug: "",
      awaitingLink: false,
      addedAt: 1,
    });
    queueUpdate.mockResolvedValue(undefined);
    queueClear.mockResolvedValue(true);
    queueRemove.mockResolvedValue(true);
    const { nxm, resolve } = makeNxm({ userInfo: FREE });

    const pending = resolve(MOD_URL);
    await vi.waitFor(() => expect(queueAdd).toHaveBeenCalled());

    expect(queueAdd).toHaveBeenCalledWith({
      input: MOD_URL,
      gameDomain: "skyrimspecialedition",
      type: "mod",
      modId: 100,
      fileId: 500,
      collectionSlug: undefined,
    });
    expect(nxm.dialogHandlers.onCancel(MOD_URL)).toBe(true);
    expect(queueClear).toHaveBeenCalled();
    await expect(pending).rejects.toBeTruthy();
  });

  test("falls back to nexus-api when the core helper is unavailable", async ({
    makeNxm,
  }: INxmFixtures) => {
    accessToken.mockResolvedValue("native-token");
    nativeResolve.mockResolvedValue(undefined);
    const { harness, resolve } = makeNxm();
    harness.getDownloadURLs.mockResolvedValue(downloadLink("https://cdn.example.com/fallback.zip"));

    const result = await resolve(MOD_URL);

    expect(harness.getDownloadURLs).toHaveBeenCalled();
    expect(result.urls).toEqual(["https://cdn.example.com/fallback.zip"]);
  });

  test("does not call the core helper without an access token", async ({
    makeNxm,
  }: INxmFixtures) => {
    accessToken.mockResolvedValue(undefined);
    const { harness, resolve } = makeNxm();
    harness.getDownloadURLs.mockResolvedValue(downloadLink("https://cdn.example.com/api-key.zip"));

    await resolve(MOD_URL);

    expect(nativeResolve).not.toHaveBeenCalled();
    expect(harness.getDownloadURLs).toHaveBeenCalled();
  });
});
