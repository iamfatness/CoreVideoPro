import { describe, expect, it, vi } from "vitest";
import { appUpdateEnabled, createAppUpdateController, type AppUpdateUpdater } from "./appUpdate.ts";

function createUpdaterStub(): AppUpdateUpdater & { listeners: Record<string, Array<(...args: unknown[]) => void>> } {
  const listeners: Record<string, Array<(...args: unknown[]) => void>> = {};
  return {
    listeners,
    autoDownload: false,
    autoInstallOnAppQuit: false,
    allowDowngrade: false,
    fullChangelog: false,
    on(event, listener) {
      listeners[event] ??= [];
      listeners[event].push(listener as (...args: unknown[]) => void);
    },
    checkForUpdates: vi.fn(async () => undefined),
    downloadUpdate: vi.fn(async () => undefined),
    quitAndInstall: vi.fn()
  };
}

describe("appUpdateEnabled", () => {
  it("is disabled outside packaged builds", () => {
    expect(appUpdateEnabled({}, false)).toBe(false);
  });

  it("is disabled during e2e", () => {
    expect(appUpdateEnabled({ COREVIDEO_E2E: "1" }, true)).toBe(false);
  });

  it("is enabled for packaged production builds", () => {
    expect(appUpdateEnabled({}, true)).toBe(true);
  });
});

describe("createAppUpdateController", () => {
  it("returns a disabled snapshot when updates are off", async () => {
    const controller = createAppUpdateController({
      enabled: false,
      currentVersion: "0.1.0",
      notify: () => undefined
    });

    expect(controller.getSnapshot().phase).toBe("disabled");
    await expect(controller.checkForUpdates()).resolves.toMatchObject({ phase: "disabled" });
  });

  it("tracks the update lifecycle and configures the updater", async () => {
    const updates: string[] = [];
    const updater = createUpdaterStub();
    const controller = createAppUpdateController({
      enabled: true,
      currentVersion: "0.1.0",
      updater,
      notify: (snapshot) => updates.push(snapshot.phase)
    });

    expect(updater.autoDownload).toBe(true);
    expect(updater.autoInstallOnAppQuit).toBe(true);

    updater.listeners["checking-for-update"][0]();
    updater.listeners["update-available"][0]({ version: "0.1.1", releaseNotes: "Fixes" });
    updater.listeners["download-progress"][0]({ percent: 42 });
    updater.listeners["update-downloaded"][0]({ version: "0.1.1" });

    expect(updates).toEqual(["checking", "available", "downloading", "downloaded"]);
    expect(controller.getSnapshot()).toMatchObject({
      phase: "downloaded",
      availableVersion: "0.1.1",
      percent: 100
    });

    await controller.checkForUpdates();
    expect(updater.checkForUpdates).toHaveBeenCalled();
    controller.quitAndInstall();
    expect(updater.quitAndInstall).toHaveBeenCalled();
  });
});