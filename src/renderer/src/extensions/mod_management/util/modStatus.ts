import path from "node:path";

import type { IDeploymentFailure } from "../actions/session";
import type { IModWithState, ModStatus } from "../types/IModProps";

/** The mod's install path, if the game has one and the mod is installed. */
export function modStagingFolder(
  mod: Pick<IModWithState, "state" | "installationPath">,
  stagingPath: string | undefined,
): string | undefined {
  if (
    mod.state !== "installed" ||
    mod.installationPath === undefined ||
    stagingPath === undefined
  ) {
    return undefined;
  }
  return path.join(stagingPath, mod.installationPath);
}

/** Whether a deployment tried and failed to place one of this mod's files. */
export function hasDeploymentFailure(
  mod: IModWithState,
  failures: IDeploymentFailure[] | undefined,
): boolean {
  return (
    mod.installationPath !== undefined &&
    failures?.some((failure) => failure.source === mod.installationPath) === true
  );
}

/**
 * What the mod's status cell shows. `error` wins over `enabled`: a mod whose staging
 * folder disappeared has nothing to enable, and a mod with a recorded deployment
 * failure didn't fully land in the game directory.
 */
export function modStatus(
  mod: IModWithState,
  failures: IDeploymentFailure[] | undefined,
): ModStatus {
  if (mod.state === "downloaded") {
    return mod.attributes?.wasInstalled ? "uninstalled" : "neverinstalled";
  }
  if (mod.state === "installing") {
    return "installing";
  }
  if (
    mod.installationState === "empty" ||
    mod.installationState === "missing" ||
    mod.installationState === "error"
  ) {
    return "error";
  }
  if (hasDeploymentFailure(mod, failures)) {
    return "error";
  }
  return mod.enabled === true ? "enabled" : "disabled";
}
