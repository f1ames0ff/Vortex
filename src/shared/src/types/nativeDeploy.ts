/** Wire types for the native deployment core (`vortex-core`). */

/**
 * A mod as the native planner sees it. Paths are relative to `staging`.
 * `target` is the optional subdirectory inside the deployment target, and
 * `fileOverrides` are absolute paths in the target directory that this mod
 * must not provide.
 */
export interface NativeDeployMod {
  installationPath: string;
  sourceName?: string;
  target?: string;
  fileOverrides?: string[];
}

export interface NativeDeployOptions {
  /** Force case-insensitive path keys; defaults to the target filesystem. */
  caseInsensitive?: boolean;
  /** Exact `<source>/<relPath>` entries the planner must skip. */
  blacklist?: string[];
}

export interface NativeDeployRequest {
  /** Mod staging folder. */
  staging: string;
  /** Root deployment directory for this mod type. */
  target: string;
  /** Manifest path the core reads and writes, e.g. `vortex.deployment.json`. */
  manifest: string;
  mods: NativeDeployMod[];
  instance?: string;
  /** Deployment method stored in the manifest, `hardlink` by default. */
  method?: string;
  game?: string;
  options?: NativeDeployOptions;
  journalDir?: string;
}

export interface NativePurgeRequest {
  staging: string;
  target: string;
  manifest: string;
  instance?: string;
  method?: string;
  game?: string;
  caseInsensitive?: boolean;
  journalDir?: string;
}

export interface NativeDeployFailure {
  source: string;
  relPath: string;
  outputPath: string;
  message: string;
}

export interface NativeDeployReport {
  linked: number;
  removed: number;
  backedUp: number;
  restored: number;
  skipped: number;
  failures: NativeDeployFailure[];
}

export interface NativeDeployedFile {
  relPath: string;
  source: string;
  target?: string;
  time: number;
  merged?: string[];
}

export interface NativeDeployPlanSummary {
  desired: number;
  added: number;
  removed: number;
  changed: number;
  operation: string;
  /** Present for `includeFiles` requests. */
  files?: NativeDeployedFile[];
}

export interface NativeDeployResult {
  report: NativeDeployReport;
  operation: string;
  plan?: Omit<NativeDeployPlanSummary, "operation" | "files">;
  /** Deployment state after the operation, when `includeFiles` was requested. */
  files?: NativeDeployedFile[];
}

export interface NativePurgeResult {
  report: NativeDeployReport;
  operation: string;
  /** Entries the core could not remove; they stay recorded for a later purge. */
  files?: NativeDeployedFile[];
}

export interface NativeDeployStatus {
  /** Whether `VORTEX_NATIVE_DEPLOY` turned the native path on. */
  enabled: boolean;
  /** Whether the core binary was found and answered `ping`. */
  available: boolean;
  corePath?: string;
  version?: string;
  error?: string;
}
