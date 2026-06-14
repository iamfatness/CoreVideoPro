export type ZoomWindowsSdkArchitecture = "x64" | "arm64";

export type ZoomWindowsSdkEntry = {
  fullName: string;
  length: number;
};

export type ZoomWindowsSdkPackageInput = {
  entries: ZoomWindowsSdkEntry[];
  architecture?: ZoomWindowsSdkArchitecture;
  expectedVersion?: string;
};

export type ZoomWindowsSdkRequiredFile = {
  id: string;
  relativePath: string;
  purpose: string;
};

export type ZoomWindowsSdkPackageReport = {
  status: "ready" | "blocked";
  architecture: ZoomWindowsSdkArchitecture;
  rootFolder?: string;
  version?: string;
  requiredFiles: Array<ZoomWindowsSdkRequiredFile & { present: boolean; sizeBytes?: number }>;
  runtimeDllCount: number;
  runtimeExeCount: number;
  rawMediaHeaderCount: number;
  blockers: string[];
  warnings: string[];
  copyPlan: {
    includeRoot: string;
    runtimeGlob: string;
    includeGlob: string;
    libraryPath: string;
    helperRuntimeTarget: string;
  };
  summary: string;
};

export const ZOOM_WINDOWS_SDK_REQUIRED_FILES: ZoomWindowsSdkRequiredFile[] = [
  {
    id: "sdk-dll",
    relativePath: "bin/sdk.dll",
    purpose: "Runtime DLL loaded by the helper process."
  },
  {
    id: "sdk-lib",
    relativePath: "lib/sdk.lib",
    purpose: "Import library used by the Windows helper build."
  },
  {
    id: "zoom-sdk-header",
    relativePath: "h/zoom_sdk.h",
    purpose: "SDK initialization header."
  },
  {
    id: "meeting-service-header",
    relativePath: "h/meeting_service_interface.h",
    purpose: "Meeting join/status service header."
  },
  {
    id: "raw-video-api-header",
    relativePath: "h/rawdata/zoom_rawdata_api.h",
    purpose: "Raw media API entry points."
  },
  {
    id: "raw-video-renderer-header",
    relativePath: "h/rawdata/rawdata_renderer_interface.h",
    purpose: "Raw participant video and share renderer callbacks."
  },
  {
    id: "raw-audio-header",
    relativePath: "h/rawdata/rawdata_audio_helper_interface.h",
    purpose: "Raw mixed and per-participant audio callbacks."
  }
];

export function inspectZoomWindowsSdkPackage(input: ZoomWindowsSdkPackageInput): ZoomWindowsSdkPackageReport {
  const architecture = input.architecture ?? "x64";
  const normalizedEntries = input.entries.map((entry) => ({
    ...entry,
    fullName: normalizeZipPath(entry.fullName)
  }));
  const rootFolder = findRootFolder(normalizedEntries);
  const version = rootFolder ? versionFromRoot(rootFolder) : undefined;
  const architectureRoot = rootFolder ? `${rootFolder}/${architecture}` : undefined;
  const requiredFiles = ZOOM_WINDOWS_SDK_REQUIRED_FILES.map((file) => {
    const fullName = architectureRoot ? `${architectureRoot}/${file.relativePath}` : "";
    const entry = normalizedEntries.find((candidate) => candidate.fullName === fullName);

    return {
      ...file,
      present: Boolean(entry && entry.length > 0),
      sizeBytes: entry?.length
    };
  });
  const blockers = [
    rootFolder ? undefined : "SDK archive root folder was not found.",
    input.expectedVersion && version && version !== input.expectedVersion ? `SDK version ${version} does not match expected ${input.expectedVersion}.` : undefined,
    ...requiredFiles.filter((file) => !file.present).map((file) => `${architecture}/${file.relativePath} is missing.`)
  ].filter(Boolean) as string[];
  const runtimePrefix = architectureRoot ? `${architectureRoot}/bin/` : "";
  const headerPrefix = architectureRoot ? `${architectureRoot}/h/rawdata/` : "";
  const runtimeDllCount = normalizedEntries.filter((entry) => entry.fullName.startsWith(runtimePrefix) && entry.fullName.toLowerCase().endsWith(".dll")).length;
  const runtimeExeCount = normalizedEntries.filter((entry) => entry.fullName.startsWith(runtimePrefix) && entry.fullName.toLowerCase().endsWith(".exe")).length;
  const rawMediaHeaderCount = normalizedEntries.filter((entry) => entry.fullName.startsWith(headerPrefix) && entry.fullName.toLowerCase().endsWith(".h")).length;
  const warnings = [
    runtimeDllCount < 20 ? `Only ${runtimeDllCount} runtime DLLs found; packaged helper may be missing Zoom dependencies.` : undefined,
    runtimeExeCount === 0 ? "No runtime EXE helpers found; zTscoder/crash/helper tools may be missing." : undefined,
    rawMediaHeaderCount < 3 ? "Raw media headers are incomplete." : undefined,
    version ? undefined : "Could not infer SDK version from archive root folder."
  ].filter(Boolean) as string[];
  const status = blockers.length ? "blocked" : "ready";

  return {
    status,
    architecture,
    rootFolder,
    version,
    requiredFiles,
    runtimeDllCount,
    runtimeExeCount,
    rawMediaHeaderCount,
    blockers,
    warnings,
    copyPlan: {
      includeRoot: architectureRoot ? `${architectureRoot}/h` : `${architecture}/h`,
      runtimeGlob: architectureRoot ? `${architectureRoot}/bin/*` : `${architecture}/bin/*`,
      includeGlob: architectureRoot ? `${architectureRoot}/h/**/*` : `${architecture}/h/**/*`,
      libraryPath: architectureRoot ? `${architectureRoot}/lib/sdk.lib` : `${architecture}/lib/sdk.lib`,
      helperRuntimeTarget: "native-core/zoom-runtime/windows/x64"
    },
    summary:
      status === "ready"
        ? `Zoom Windows Meeting SDK ${version ?? "unknown"} ${architecture} package is ready for helper packaging.`
        : `Zoom Windows Meeting SDK package blocked by ${blockers.length} issue${blockers.length === 1 ? "" : "s"}.`
  };
}

function normalizeZipPath(value: string) {
  return value.replace(/\\/g, "/").replace(/^\/+/, "").replace(/\/+$/, "");
}

function findRootFolder(entries: ZoomWindowsSdkEntry[]) {
  const roots = new Set<string>();
  entries.forEach((entry) => {
    const [root] = entry.fullName.split("/");
    if (root?.startsWith("zoom-sdk-windows-")) {
      roots.add(root);
    }
  });

  return [...roots].sort()[0];
}

function versionFromRoot(rootFolder: string) {
  const match = /^zoom-sdk-windows-(.+)$/.exec(rootFolder);
  return match?.[1];
}
