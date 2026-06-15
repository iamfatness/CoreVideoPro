import { app, type BrowserWindow } from "electron";
import type { SupportBundle } from "../src/domain/production.ts";
import type { TelemetryClient } from "./telemetryClient.ts";

type ElectronAppEvents = {
  on(event: "browser-window-created", listener: (event: unknown, window: BrowserWindow) => void): void;
};

export function setupCrashReporting(options: {
  windows: () => BrowserWindow[];
  telemetry: TelemetryClient;
  onRendererCrashed?: (detail: string) => Promise<SupportBundle | undefined>;
  env?: NodeJS.ProcessEnv;
  electronApp?: ElectronAppEvents;
}) {
  const env = options.env ?? process.env;
  const electronApp = options.electronApp ?? app;

  const uploadCrash = (reason: string, bundle?: SupportBundle, detail?: string) => {
    void options.telemetry.uploadCrashReport({ reason, bundle, detail }).then((result) => {
      if (result.uploaded) {
        console.info(`[crash-report] ${result.message}`);
      }
    });
  };

  process.on("uncaughtException", (error) => {
    console.error("[crash-report] uncaught exception", error);
    uploadCrash("main-uncaught", undefined, error.message);
  });

  process.on("unhandledRejection", (reason) => {
    const message = reason instanceof Error ? reason.message : String(reason);
    console.error("[crash-report] unhandled rejection", message);
    uploadCrash("main-unhandled-rejection", undefined, message);
  });

  for (const window of options.windows()) {
    attachRendererCrashHandlers(window, options, uploadCrash);
  }

  electronApp.on("browser-window-created", (_event, window) => {
    attachRendererCrashHandlers(window, options, uploadCrash);
  });

  if (env.COREVIDEO_FORCE_CRASH === "1") {
    setTimeout(() => {
      throw new Error("COREVIDEO_FORCE_CRASH triggered");
    }, 250);
  }
}

function attachRendererCrashHandlers(
  window: BrowserWindow,
  options: {
    telemetry: TelemetryClient;
    onRendererCrashed?: (detail: string) => Promise<SupportBundle | undefined>;
  },
  uploadCrash: (reason: string, bundle?: SupportBundle, detail?: string) => void
) {
  window.webContents.on("render-process-gone", (_event, details) => {
    const detail = `${details.reason} (exit ${details.exitCode})`;
    console.error(`[crash-report] renderer gone: ${detail}`);
    void (async () => {
      const bundle = await options.onRendererCrashed?.(detail);
      uploadCrash("renderer-crash", bundle, detail);
    })();
  });

  window.webContents.on("unresponsive", () => {
    uploadCrash("renderer-unresponsive", undefined, "Renderer became unresponsive.");
  });
}