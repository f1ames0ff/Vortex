import type { IDownload } from "../../download_management/types/IDownload";
import type { IProfileMod } from "../../profile_management/types/IProfile";
import type { IMod } from "./IMod";

export interface IModProps {
  mods: { [modId: string]: IMod };
  modState: { [modId: string]: IProfileMod };
  downloads: { [downloadId: string]: IDownload };
}

export type ModInstallationState = "ok" | "empty" | "missing";

/** The mod's row status, which can differ from its enabled flag when its files are gone. */
export type ModStatus =
  | "error"
  | "enabled"
  | "disabled"
  | "uninstalled"
  | "neverinstalled"
  | "installing";

export type IModWithState = IMod &
  IProfileMod & {
    /** Whether the staging folder was found and has any entries. */
    installationState?: ModInstallationState;
    /** The status the table shows; kept in sync with installationState and deployments. */
    status?: ModStatus;
  };
