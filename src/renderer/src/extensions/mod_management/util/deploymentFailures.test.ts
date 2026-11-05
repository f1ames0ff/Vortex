import { describe, expect, it, vi } from "vitest";

import type { IExtensionApi } from "../../../types/IExtensionContext";
import type { IDeploymentFailure } from "../actions/session";
import { reportDeploymentFailures } from "./deploymentFailures";

interface CapturedDialog {
  type: string;
  title: string;
  content: { bbcode?: string };
}

function makeApi() {
  let dialog: CapturedDialog | undefined;
  const api = {
    translate: (key: string) => key,
    sendNotification: vi.fn(),
    showDialog: vi.fn(
      (type: string, title: string, content: { bbcode?: string }): Promise<unknown> => {
        dialog = { type, title, content };
        return Promise.resolve({ action: "Close" });
      },
    ),
  };
  return { api, dialog: () => dialog };
}

function openFailureDialog(api: ReturnType<typeof makeApi>["api"]): void {
  const notification = api.sendNotification.mock.calls[0]?.[0] as {
    actions: Array<{ action: (dismiss: () => void) => void }>;
  };
  notification.actions[0].action(vi.fn());
}

describe("reportDeploymentFailures", () => {
  it("shows the deployment engine reason instead of the generic lock hint", () => {
    const { api, dialog } = makeApi();
    const failures: IDeploymentFailure[] = [
      {
        source: "No NPC Greetings",
        relPath: "fomod/ModuleConfig.xml",
        outputPath: "/game/Data/fomod/ModuleConfig.xml",
        message:
          "a .vortex_backup already exists but the target is not a hardlink; " +
          "remove the stale backup or resolve the file manually",
      },
    ];

    reportDeploymentFailures(api as unknown as IExtensionApi, failures);
    openFailureDialog(api);

    const captured = dialog();
    expect(captured?.title).toBe("Files could not be deployed");
    expect(captured?.content.bbcode).toContain("deployment engine");
    expect(captured?.content.bbcode).toContain("[i]a .vortex_backup already exists");
    expect(captured?.content.bbcode).not.toContain("<i>");
    expect(captured?.content.bbcode).not.toContain("another application had them open");
    expect(captured?.content.bbcode).toContain(
      "[url=cb://reveal/%2Fgame%2FData%2Ffomod%2FModuleConfig.xml]fomod/ModuleConfig.xml[/url]",
    );
  });

  it("keeps the generic hint when the engine reported no reason", () => {
    const { api, dialog } = makeApi();

    reportDeploymentFailures(api as unknown as IExtensionApi, [
      {
        source: "ModA",
        relPath: "a.txt",
        outputPath: "/game/a.txt",
      },
    ]);
    openFailureDialog(api);

    expect(dialog()?.content.bbcode).toContain("another application had them open");
  });
});
