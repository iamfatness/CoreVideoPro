/**
 * Packaged-app auto-update controller backed by electron-updater.
 * Disabled in dev shells and e2e so the default in-container gate stays green.
 */

export type AppUpdatePhase =
  | "disabled"
  | "idle"
  | "checking"
  | "available"
  | "not-available"
  | "downloading"
  | "downloaded"
  | "error";

export type AppUpdateSnapshot = {
  phase: AppUpdatePhase;
  currentVersion: string;
  availableVersion?: string;
  percent?: number;
  message?: string;
  releaseNotes?: string;
};

export type AppUpdateInfo = {
  version?: string;
  releaseNotes?: string | Array<{ note: string }>;
};

export type AppUpdateProgress = {
  percent?: number;
};

export interface AppUpdateUpdater {
  autoDownload: boolean;
  autoInstallOnAppQuit: boolean;
  allowDowngrade: boolean;
  fullChangelog: boolean;
  on(event: "checking-for-update", listener: () => void): void;
  on(event: "update-available", listener: (info: AppUpdateInfo) => void): void;
  on(event: "update-not-available", listener: (info: AppUpdateInfo) => void): void;
  on(event: "download-progress", listener: (progress: AppUpdateProgress) => void): void;
  on(event: "update-downloaded", listener: (info: AppUpdateInfo) => void): void;
  on(event: "error", listener: (error: Error) => void): void;
  checkForUpdates(): Promise<unknown>;
  downloadUpdate(): Promise<unknown>;
  quitAndInstall(): void;
}

function formatReleaseNotes(notes: AppUpdateInfo["releaseNotes"]): string | undefined {
  if (!notes) {
    return undefined;
  }
  if (typeof notes === "string") {
    return notes;
  }
  return notes.map((entry) => entry.note).join("\n");
}

export function createAppUpdateController(options: {
  enabled: boolean;
  currentVersion: string;
  updater?: AppUpdateUpdater;
  notify: (snapshot: AppUpdateSnapshot) => void;
}) {
  let snapshot: AppUpdateSnapshot = {
    phase: options.enabled ? "idle" : "disabled",
    currentVersion: options.currentVersion
  };

  const publish = (patch: Partial<AppUpdateSnapshot>) => {
    snapshot = { ...snapshot, ...patch };
    options.notify(snapshot);
  };

  if (!options.enabled || !options.updater) {
    return {
      getSnapshot: () => snapshot,
      checkForUpdates: async () => snapshot,
      downloadUpdate: async () => snapshot,
      quitAndInstall: () => undefined,
      startBackgroundChecks: () => undefined
    };
  }

  const updater = options.updater;
  updater.autoDownload = true;
  updater.autoInstallOnAppQuit = true;
  updater.allowDowngrade = false;
  updater.fullChangelog = false;

  updater.on("checking-for-update", () => {
    publish({ phase: "checking", message: "Checking for updates…" });
  });

  updater.on("update-available", (info) => {
    publish({
      phase: "available",
      availableVersion: info.version,
      releaseNotes: formatReleaseNotes(info.releaseNotes),
      message: info.version ? `Update ${info.version} is available.` : "Update available."
    });
  });

  updater.on("update-not-available", () => {
    publish({ phase: "not-available", message: "You're on the latest version." });
  });

  updater.on("download-progress", (progress) => {
    publish({
      phase: "downloading",
      percent: progress.percent,
      message: `Downloading update… ${Math.round(progress.percent ?? 0)}%`
    });
  });

  updater.on("update-downloaded", (info) => {
    publish({
      phase: "downloaded",
      availableVersion: info.version,
      releaseNotes: formatReleaseNotes(info.releaseNotes),
      percent: 100,
      message: "Update downloaded. Restart to install."
    });
  });

  updater.on("error", (error) => {
    publish({
      phase: "error",
      message: error.message || "Update check failed."
    });
  });

  return {
    getSnapshot: () => snapshot,
    checkForUpdates: async () => {
      await updater.checkForUpdates();
      return snapshot;
    },
    downloadUpdate: async () => {
      await updater.downloadUpdate();
      return snapshot;
    },
    quitAndInstall: () => {
      updater.quitAndInstall();
    },
    startBackgroundChecks: () => {
      void updater.checkForUpdates();
    }
  };
}

export function appUpdateEnabled(env: NodeJS.ProcessEnv, packaged: boolean): boolean {
  if (!packaged || env.COREVIDEO_E2E === "1" || env.COREVIDEO_DISABLE_AUTO_UPDATE === "1") {
    return false;
  }
  return true;
}