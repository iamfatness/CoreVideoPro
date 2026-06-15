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
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import { createIpcRouter } from "./ipcRouter.ts";
import { MediaCoreSupervisor } from "./mediaCoreClient.ts";
import { mediaCoreSupervisorOptionsFromEnv } from "./mediaCoreRuntime.ts";
import { createDesktopZoomOAuthService } from "./zoomOAuthFactory.ts";
import type { NativeBridgeCommand } from "../src/engine/nativeBridgeProtocol";

const here = dirname(fileURLToPath(import.meta.url));
const zoomOAuth = createDesktopZoomOAuthService();
const supervisor = new MediaCoreSupervisor({
  ...mediaCoreSupervisorOptionsFromEnv(process.env),
  onCrash: (info) => console.warn(`[media-core] crashed (code ${info.code}); restart #${info.restartCount}`),
  onProfile: (profile) => console.info(`[media-core] profile: ${profile.name} (${profile.renderer})`),
  onZoomVideoFrame: (frame) => {
    for (const window of BrowserWindow.getAllWindows()) {
      window.webContents.send("corevideo:zoom-video-frame", frame);
    }
  }
});
const route = createIpcRouter({ mediaCore: supervisor, zoomOAuth });

const RENDERER_DEV_URL = process.env.COREVIDEO_RENDERER_URL ?? "http://127.0.0.1:5173";
const RENDERER_FILE = join(here, "..", "dist", "index.html");
const OAUTH_PROTOCOL = "corevideopro";

async function createWindow(): Promise<void> {
  const window = new BrowserWindow({
    width: 1480,
    height: 900,
    backgroundColor: "#0a0f16",
    webPreferences: {
      preload: join(here, "preload.cjs"),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: false
    }
  });

  if (process.env.COREVIDEO_RENDERER_URL || !app.isPackaged) {
    await window.loadURL(RENDERER_DEV_URL);
  } else {
    await window.loadFile(RENDERER_FILE);
  }
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

  await supervisor.start();
  await createWindow();

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

const gotLock = app.requestSingleInstanceLock();
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