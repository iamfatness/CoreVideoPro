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
import type { NativeBridgeCommand } from "../src/engine/nativeBridgeProtocol";

const here = dirname(fileURLToPath(import.meta.url));
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
const route = createIpcRouter({ mediaCore: supervisor });

const RENDERER_DEV_URL = process.env.COREVIDEO_RENDERER_URL ?? "http://127.0.0.1:5173";
const RENDERER_FILE = join(here, "..", "dist", "index.html");

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

// Synchronous handshake channel: the preload reads the cached profile before it
// exposes window.coreVideoNative, so the renderer's runtime status is correct on
// first paint.
ipcMain.on("corevideo:handshake", (event) => {
  event.returnValue = supervisor.getProfile() ?? null;
});

ipcMain.handle("corevideo:request", (_event, command: NativeBridgeCommand) => route(command));

app.whenReady().then(async () => {
  await supervisor.start();
  await createWindow();

  app.on("activate", () => {
    if (BrowserWindow.getAllWindows().length === 0) {
      void createWindow();
    }
  });
});

app.on("window-all-closed", () => {
  supervisor.stop();
  if (process.platform !== "darwin") {
    app.quit();
  }
});

app.on("before-quit", () => supervisor.stop());
