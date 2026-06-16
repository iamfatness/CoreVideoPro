/**
 * Electron main process. Owns the window, the media-core child-process
 * supervisor, and the IPC router that turns renderer bridge commands into
 * responses. No nodeIntegration — the renderer only talks to us through the
 * preload's contextBridge surface.
 *
 * Not exercised by the CI test suite (no Electron in the container); the
 * router/supervisor logic it wires is covered by desktop/*.test.ts and the
 * integration gate. Launch with `npm run desktop`.
 */
import { app, BrowserWindow, ipcMain } from "electron";
import { existsSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import { appendCrashRecoveryEvent, readCrashRecoveryEvents } from "./crashRecoveryLog.ts";
import { setupCrashReporting } from "./crashReporterMain.ts";
import { createIpcRouter } from "./ipcRouter.ts";
import { MediaCoreSupervisor } from "./mediaCoreClient.ts";
import { mediaCoreSupervisorOptionsForApp } from "./packagedRuntime.ts";
import { createPackagedAppUpdateService } from "./appUpdateMain.ts";
import { createCaptionBrokerService } from "./captionBrokerMain.ts";
import { createLicenseService } from "./licenseMain.ts";
import { createTelemetryService } from "./telemetryMain.ts";
import { createDesktopZoomOAuthService } from "./zoomOAuthFactory.ts";
import type { NativeBridgeCommand } from "../src/engine/nativeBridgeProtocol";

const here = dirname(fileURLToPath(import.meta.url));
const repoRoot = dirname(here);
const zoomOAuth = createDesktopZoomOAuthService();
const crashLogPath = join(app.getPath("userData"), "crash-recovery.json");
const diagnosticsDirectory = join(app.getPath("userData"), "diagnostics");
const supervisor = new MediaCoreSupervisor({
  ...mediaCoreSupervisorOptionsForApp(process.env, process.resourcesPath, process.platform, app.isPackaged),
  onCrash: (info) => {
    console.warn(`[media-core] crashed (code ${info.code}); restart #${info.restartCount}`);
    void appendCrashRecoveryEvent(crashLogPath, {
      at: new Date().toISOString(),
      exitCode: info.code,
      restartCount: info.restartCount
    })
      .then(() => {
        if (telemetryRef.service) {
          void telemetryRef.service.client.uploadCrashReport({
            reason: "media-core-crash",
            detail: `exit ${info.code ?? "unknown"}; restart #${info.restartCount}`
          });
        }
      })
      .catch((error: unknown) => {
        const message = error instanceof Error ? error.message : "Failed to persist crash recovery log.";
        console.warn(`[media-core] ${message}`);
      });
  },
  onProfile: (profile) => console.info(`[media-core] profile: ${profile.name} (${profile.renderer})`),
  onZoomVideoFrame: (frame) => {
    for (const window of BrowserWindow.getAllWindows()) {
      window.webContents.send("corevideo:zoom-video-frame", frame);
    }
  }
});
const telemetryRef: { service?: Awaited<ReturnType<typeof createTelemetryService>> } = {};

const route = createIpcRouter({
  mediaCore: supervisor,
  zoomOAuth,
  diagnosticsDirectory,
  readCrashEvents: () => readCrashRecoveryEvents(crashLogPath)
});

const RENDERER_DEV_URL = process.env.COREVIDEO_RENDERER_URL ?? "http://127.0.0.1:5173";
const RENDERER_FILE = join(repoRoot, "dist", "index.html");
const OAUTH_PROTOCOL = "corevideopro";

function resolvePreloadPath(): string {
  const candidates = [join(here, "preload.cjs"), join(here, "preload.ts")];
  return candidates.find((candidate) => existsSync(candidate)) ?? join(here, "preload.ts");
}

async function createWindow(): Promise<void> {
  const window = new BrowserWindow({
    width: 1480,
    height: 900,
    title: "CoreVideo Pro",
    backgroundColor: "#0a0f16",
    webPreferences: {
      preload: resolvePreloadPath(),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: false
    }
  });

  const devServerUrl = process.env.COREVIDEO_RENDERER_URL?.trim();
  if (devServerUrl) {
    await window.loadURL(devServerUrl);
    return;
  }

  if (app.isPackaged || existsSync(RENDERER_FILE)) {
    await window.loadFile(RENDERER_FILE);
    return;
  }

  await window.loadURL(RENDERER_DEV_URL);
}

function handleOAuthCallbackUrl(url: string): void {
  void zoomOAuth
    .handleRedirectUrl(url)
    .then(() => {
      for (const window of BrowserWindow.getAllWindows()) {
        window.webContents.send("corevideo:zoom-oauth-updated");
      }
    })
    .catch((error: unknown) => {
      const message = error instanceof Error ? error.message : "Zoom OAuth callback failed.";
      console.warn(`[zoom-oauth] ${message}`);
      for (const window of BrowserWindow.getAllWindows()) {
        window.webContents.send("corevideo:zoom-oauth-error", message);
      }
    });
}

function extractOAuthCallbackUrl(argv: string[]): string | undefined {
  return argv.find((arg) => arg.startsWith(`${OAUTH_PROTOCOL}://`));
}

// Synchronous handshake channel: the preload reads the cached profile before it
// exposes window.coreVideoNative, so the renderer's runtime status is correct on
// first paint.
ipcMain.on("corevideo:handshake", (event) => {
  event.returnValue = supervisor.getProfile() ?? null;
});

ipcMain.handle("corevideo:request", (_event, command: NativeBridgeCommand) => route(command));

const appUpdate = createPackagedAppUpdateService(() => BrowserWindow.getAllWindows());

ipcMain.handle("corevideo:check-for-updates", async () => appUpdate.checkForUpdates());
ipcMain.handle("corevideo:download-update", async () => appUpdate.downloadUpdate());
ipcMain.handle("corevideo:install-update", async () => {
  appUpdate.quitAndInstall();
});
ipcMain.handle("corevideo:get-update-status", async () => appUpdate.getSnapshot());

if (process.defaultApp) {
  if (process.argv.length >= 2) {
    app.setAsDefaultProtocolClient(OAUTH_PROTOCOL, process.execPath, [process.argv[1] ?? ""]);
  }
} else {
  app.setAsDefaultProtocolClient(OAUTH_PROTOCOL);
}

app.whenReady().then(async () => {
  const callbackUrl = extractOAuthCallbackUrl(process.argv);
  if (callbackUrl) {
    handleOAuthCallbackUrl(callbackUrl);
  }

  telemetryRef.service = await createTelemetryService();
  await createLicenseService();
  createCaptionBrokerService();
  setupCrashReporting({
    windows: () => BrowserWindow.getAllWindows(),
    telemetry: telemetryRef.service.client,
    env: process.env
  });

  await supervisor.start();
  await createWindow();
  appUpdate.startBackgroundChecks();

  app.on("activate", () => {
    if (BrowserWindow.getAllWindows().length === 0) {
      void createWindow();
    }
  });
});

app.on("open-url", (...args: unknown[]) => {
  const event = args[0] as { preventDefault?: () => void };
  const url = args[1] as string;
  event.preventDefault?.();
  if (typeof url === "string" && url.startsWith(`${OAUTH_PROTOCOL}://`)) {
    handleOAuthCallbackUrl(url);
  }
});

const isE2e = process.env.COREVIDEO_E2E === "1";
const gotLock = isE2e || app.requestSingleInstanceLock();
if (!gotLock) {
  app.quit();
} else {
  app.on("second-instance", (...args: unknown[]) => {
    const argv = args[1] as string[];
    const callbackUrl = extractOAuthCallbackUrl(argv);
    if (callbackUrl) {
      handleOAuthCallbackUrl(callbackUrl);
    }
    const [window] = BrowserWindow.getAllWindows();
    window?.focus();
  });
}

app.on("window-all-closed", () => {
  supervisor.stop();
  if (process.platform !== "darwin") {
    app.quit();
  }
});

app.on("before-quit", () => supervisor.stop());