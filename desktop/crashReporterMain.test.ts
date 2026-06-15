import { afterEach, describe, expect, it, vi } from "vitest";
import { setupCrashReporting } from "./crashReporterMain.ts";
import type { TelemetryClient } from "./telemetryClient.ts";

describe("setupCrashReporting", () => {
  afterEach(() => {
    vi.restoreAllMocks();
  });

  it("uploads main uncaught exceptions", async () => {
    const uploadCrashReport = vi.fn(async () => ({ uploaded: true, message: "ok" }));
    const telemetry = { uploadCrashReport } as unknown as TelemetryClient;

    setupCrashReporting({
      windows: () => [],
      telemetry,
      env: {},
      electronApp: { on: () => undefined }
    });

    const listeners = process.listeners("uncaughtException");
    const handler = listeners[listeners.length - 1] as (error: Error) => void;
    handler(new Error("boom"));

    await vi.waitFor(() => {
      expect(uploadCrashReport).toHaveBeenCalledWith({
        reason: "main-uncaught",
        bundle: undefined,
        detail: "boom"
      });
    });
  });
});