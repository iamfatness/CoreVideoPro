import { app, ipcMain } from "electron";
import { join } from "node:path";
import {
  createTelemetryClient,
  resolveTelemetryConfig,
  type TelemetryClient,
  type TelemetryEvent
} from "./telemetryClient.ts";
import { readTelemetryConsent, writeTelemetryConsent } from "./telemetryConsent.ts";

export type TelemetryService = {
  client: TelemetryClient;
  consentPath: string;
  refreshClient(): Promise<void>;
};

export async function createTelemetryService(env: NodeJS.ProcessEnv = process.env): Promise<TelemetryService> {
  const consentPath = join(app.getPath("userData"), "telemetry-consent.json");
  let client = createTelemetryClient(
    resolveTelemetryConfig(env, await readTelemetryConsent(consentPath), app.getVersion(), process.platform)
  );

  const refreshClient = async () => {
    const consent = await readTelemetryConsent(consentPath);
    client = createTelemetryClient(resolveTelemetryConfig(env, consent, app.getVersion(), process.platform));
  };

  ipcMain.handle("corevideo:get-telemetry-consent", async () => readTelemetryConsent(consentPath));
  ipcMain.handle("corevideo:set-telemetry-consent", async (_event, analyticsEnabled: boolean) => {
    await writeTelemetryConsent(consentPath, analyticsEnabled === true);
    await refreshClient();
    return readTelemetryConsent(consentPath);
  });
  ipcMain.handle("corevideo:record-telemetry-event", async (_event, event: TelemetryEvent) => client.recordEvent(event));
  ipcMain.handle("corevideo:renderer-error", async (_event, payload: { message: string; source?: string }) =>
    client.uploadCrashReport({
      reason: "renderer-error",
      detail: payload.source ? `${payload.source}: ${payload.message}` : payload.message
    })
  );

  return {
    get client() {
      return client;
    },
    consentPath,
    refreshClient
  };
}