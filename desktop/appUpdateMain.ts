import { app, type BrowserWindow } from "electron";
import { createRequire } from "node:module";
import {
  appUpdateEnabled,
  createAppUpdateController,
  type AppUpdateSnapshot,
  type AppUpdateUpdater
} from "./appUpdate.ts";

const require = createRequire(import.meta.url);

function broadcastUpdateStatus(windows: BrowserWindow[], snapshot: AppUpdateSnapshot): void {
  for (const window of windows) {
    window.webContents.send("corevideo:app-update-status", snapshot);
  }
}

export function createPackagedAppUpdateService(
  windows: () => BrowserWindow[],
  env: NodeJS.ProcessEnv = process.env
) {
  const enabled = appUpdateEnabled(env, app.isPackaged);
  let updater: ReturnType<typeof createAppUpdateController> | undefined;

  if (enabled) {
    try {
      const { autoUpdater } = require("electron-updater") as { autoUpdater: AppUpdateUpdater };
      updater = createAppUpdateController({
        enabled: true,
        currentVersion: app.getVersion(),
        updater: autoUpdater,
        notify: (snapshot) => broadcastUpdateStatus(windows(), snapshot)
      });
    } catch (error: unknown) {
      const message = error instanceof Error ? error.message : "electron-updater unavailable";
      console.warn(`[app-update] disabled: ${message}`);
    }
  }

  const controller =
    updater ??
    createAppUpdateController({
      enabled: false,
      currentVersion: app.getVersion(),
      notify: () => undefined
    });

  return controller;
}