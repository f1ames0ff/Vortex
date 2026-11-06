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

export interface NativeDeploySingleRequest {
  staging: string;
  target: string;
  manifest: string;
  instance?: string;
  method: string;
  game: string;
  enable?: boolean;
  mod: {
    installationPath: string;
    sourceName?: string;
    target?: string;
    fileOverrides?: string[];
  };
  options?: {
    caseInsensitive?: boolean;
    blacklist?: string[];
  };
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
  /** Protocol generation; used to reject stale binaries for newer methods. */
  protocol?: number;
  /** Methods this core binary supports, when it reports them. */
  capabilities?: string[];
  error?: string;
}

/** Staging-folder state reported by the core. */
export type NativeModStatus = "ok" | "empty" | "missing" | "error";

export interface NativeStagingMod {
  /** Caller's mod id; defaults to `installationPath` in the core. */
  id?: string;
  /** Path relative to `staging`. */
  installationPath: string;
}

export interface NativeStagingRequest {
  staging: string;
  mods: NativeStagingMod[];
}

export interface NativeStagingModResult {
  id: string;
  installationPath: string;
  status: NativeModStatus;
  entries: number;
  error?: string;
}

export interface NativeStagingResult {
  mods: NativeStagingModResult[];
}

export type NativeInstallInstructionType = "mkdir" | "copy" | "hardlink" | "write";

export interface NativeInstallInstruction {
  type: NativeInstallInstructionType;
  /** Relative to the extracted archive directory for copy/hardlink. */
  source?: string;
  /** Relative to the mod's staging destination. */
  destination: string;
  /** Raw content for write instructions. */
  data?: string;
  /** Skip a missing source instead of reporting a failure. */
  optional?: boolean;
}

export interface NativeInstallRequest {
  temp: string;
  destination: string;
  instructions: NativeInstallInstruction[];
  /** Directory for the JSONL install journal; omitted keeps no journal. */
  journalDir?: string;
  concurrency?: number;
}

export interface NativeInstallFailure {
  source: string;
  destination: string;
  message: string;
}

export interface NativeInstallReport {
  directories: number;
  linked: number;
  copied: number;
  written: number;
  failures: NativeInstallFailure[];
}

export interface NativeInstallResult {
  report: NativeInstallReport;
  operation: string;
}

export interface NativeArchiveEntry {
  path: string;
  size: number;
  directory: boolean;
  encrypted: boolean;
  symlink: boolean;
}

export interface NativeArchiveListRequest {
  archive: string;
  /** Injected by main when omitted. */
  sevenZip?: string;
  password?: string;
}

export interface NativeArchiveListResult {
  code: number;
  entries: NativeArchiveEntry[];
  errors: string[];
  encrypted: boolean;
}

export interface NativeArchiveExtractRequest {
  archive: string;
  destination: string;
  /** Injected by main when omitted. */
  sevenZip?: string;
  password?: string;
}

export interface NativeArchiveExtractResult {
  code: number;
  entries: NativeArchiveEntry[];
  errors: string[];
}

export type NativeHashAlgorithm = "md5" | "xxh64";

export interface NativeHashFilesRequest {
  files: string[];
  algorithm: NativeHashAlgorithm;
  concurrency?: number;
}

export interface NativeHashFileResult {
  path: string;
  hash: string;
  bytes: number;
}

export interface NativeHashFailure {
  path: string;
  message: string;
}

export interface NativeHashFilesResult {
  algorithm: NativeHashAlgorithm;
  results: NativeHashFileResult[];
  failures: NativeHashFailure[];
}

export interface NativeModReference {
  id?: string;
  idHint?: string;
  md5Hint?: string;
  archiveId?: string;
  fileMD5?: string;
  fileExpression?: string;
  logicalFileName?: string;
  tag?: string;
  gameId?: string;
  versionMatch?: string;
  repo?: {
    repository: string;
    modId?: string;
    fileId?: string;
  };
}

export interface NativeLookupMod {
  id: string;
  state?: string;
  fileMD5?: string;
  fileName?: string;
  name?: string;
  logicalFileName?: string;
  customFileName?: string;
  version?: string;
  fileId?: string;
  modId?: string;
  source?: string;
  referenceTag?: string;
  referenceTags?: string[];
  additionalLogicalFileNames?: string[];
  game?: string[];
  installerChoices?: unknown;
  fileList?: unknown;
  patches?: unknown;
}

export interface NativeDownloadInfo {
  id: string;
  state: string;
  fileMD5?: string;
  localPath?: string;
  logicalFileName?: string;
  version?: string;
  fileId?: string;
  modId?: string;
  source?: string;
  referenceTag?: string;
  referenceTags?: string[];
  game?: string[];
  fileTime?: number;
}

export interface NativeRuleInfo {
  index: number;
  type: string;
  reference: NativeModReference;
  phase?: number;
  ignored?: boolean;
  localPath?: string;
  installerChoices?: unknown;
  fileList?: unknown;
  patches?: unknown;
}

export interface NativeDependencyEntry {
  ruleIndex: number;
  ruleId: string;
  type: string;
  phase: number;
  ignored: boolean;
  modId?: string;
  downloadId?: string;
  status: string;
}

export interface NativeCollectionPlan {
  members: NativeDependencyEntry[];
  phases: number[];
  totalRequired: number;
  totalOptional: number;
}

export interface NativeDepsRequest {
  rules: NativeRuleInfo[];
  mods: NativeLookupMod[];
  downloads: NativeDownloadInfo[];
}

export interface NativeDepsResolveResult {
  dependencies: NativeDependencyEntry[];
}

export interface NativePluginFile {
  name: string;
  pluginId: string;
  ghost: boolean;
}

export interface NativePluginScanDirectory {
  path: string;
  files: NativePluginFile[];
  error?: string;
}

export interface NativePluginScanRequest {
  directories: Array<{ path: string; extensions: string[] }>;
}

export interface NativePluginScanResult {
  directories: NativePluginScanDirectory[];
}

export interface NativePluginOrderRequest {
  directory: string;
}

export interface NativePluginOrderResult {
  pluginsFile: string;
  loadOrderFile: string;
  pluginsHeader?: string;
  loadOrderHeader?: string;
  plugins: string[];
  loadOrder: string[];
  error?: string;
}

export interface NativePluginOrderWriteRequest extends NativePluginOrderRequest {
  plugins: string[];
  loadOrder: string[];
}

export interface NativeCollectionJobMember {
  ruleIndex: number;
  ruleId: string;
  type: string;
  phase: number;
  ignored: boolean;
  status: string;
  modId?: string;
  downloadId?: string;
  error?: string;
  attempts?: number;
  /** Optional prebuilt execution task; when present, collection.run owns it. */
  task?: NativeCollectionTask;
  /** True when the member carries a core execution task. */
  hasTask?: boolean;
  /** Set once a core-driven task completes successfully. */
  result?: NativeCollectionMemberResult;
}

export interface NativeCollectionMemberResult {
  modId: string;
  installationPath: string;
  downloadId?: string;
  archivePath?: string;
  files: string[];
}

export interface NativeCollectionJobStatus {
  jobId: string;
  collectionId: string;
  gameId: string;
  phase: number;
  phases: number[];
  members: NativeCollectionJobMember[];
  counts: Record<string, number>;
  complete: boolean;
  paused: boolean;
  cancelled: boolean;
  stalled: boolean;
  startedAt: number;
  lastProgress: number;
}

export interface NativeCollectionStartRequest {
  collectionId: string;
  gameId: string;
  concurrency?: number;
  maxRetries?: number;
  stallMs?: number;
  members: NativeCollectionJobMember[];
}

export interface NativeCollectionStartResult {
  jobId: string;
  status: NativeCollectionJobStatus;
}

export interface NativeCollectionNextResult {
  members: NativeCollectionJobMember[];
  status: NativeCollectionJobStatus;
}

export interface NativeCollectionMemberUpdate {
  jobId: string;
  ruleId: string;
  status: string;
  modId?: string;
  downloadId?: string;
  error?: string;
  retry?: boolean;
}

export interface NativeDownloadRequest {
  id?: string;
  directory: string;
  fileName: string;
  urls: string[];
  headers?: Record<string, string>;
  referer?: string;
  userAgent?: string;
  proxy?: string;
  /** Write directly to the destination instead of the default .part file. */
  atomic?: boolean;
  /** Injected by main; defaults to curl on PATH. */
  curl?: string;
  maxRetries?: number;
  concurrency?: number;
}

export type NativeDownloadState =
  | "queued"
  | "downloading"
  | "paused"
  | "finished"
  | "failed"
  | "cancelled";

export interface NativeDownloadStatus {
  id: string;
  state: NativeDownloadState;
  fileName: string;
  url: string;
  bytes: number;
  attempts: number;
  fromCache: boolean;
  error?: string;
  startedAt: number;
  finishedAt: number;
}

export interface NativeDownloadListResult {
  downloads: NativeDownloadStatus[];
}

export interface NativeExternalChange {
  filePath: string;
  source: string;
  target?: string;
  changeType: "refchange" | "valchange" | "deleted" | "srcdeleted";
  sourceTime?: number;
  destTime?: number;
}

export interface NativeExternalChangesRequest {
  staging: string;
  target: string;
  manifest: string;
  method: string;
  files?: Array<{
    relPath: string;
    source: string;
    target?: string;
    time?: number;
    merged?: string[];
  }>;
  caseInsensitive?: boolean;
}

export interface NativeExternalChangesResult {
  changes: NativeExternalChange[];
}

export interface NativeCollectionTask {
  download?: NativeDownloadRequest;
  extract?: {
    archive?: string;
    destination: string;
    /** Injected by main when omitted. */
    sevenZip?: string;
    password?: string;
  };
  install?: {
    temp?: string;
    destination: string;
    instructions: NativeInstallInstruction[];
    concurrency?: number;
  };
  modId?: string;
}

export interface NativeCollectionTaskRequest {
  jobId: string;
  ruleId: string;
  task: NativeCollectionTask;
}

export interface NativeFomodResolveRequest {
  files: string[];
  stopPatterns?: string[];
  pluginPath?: string;
  scriptPath?: string;
  preset?: unknown;
  validate?: boolean;
}

export interface NativeFomodInstruction {
  type: string;
  source?: string;
  destination?: string;
  priority?: number;
  [key: string]: unknown;
}

export interface NativeFomodResolveResult {
  message?: string;
  instructions: NativeFomodInstruction[];
}
