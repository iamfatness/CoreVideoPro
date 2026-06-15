import { app, ipcMain } from "electron";
import { join } from "node:path";
import { createLicenseApiClient, resolveLicenseApiConfig } from "./licenseApiClient.ts";
import { readStoredLicense, writeStoredLicense } from "./licenseStore.ts";
import type { LicenseEntitlements } from "./licenseTypes.ts";

const EXPIRED_ENTITLEMENTS: LicenseEntitlements = {
  licenseKey: "",
  tier: "expired",
  status: "none",
  expiresAt: null,
  maxParticipants: 0,
  maxOutputHeight: 720,
  watermarkAfterMinutes: 0,
  captionsLevel: "none",
  chromaKey: false,
  setAndForget: false,
  magicSceneLevel: "none",
  multiDestination: false,
  ndiSrt: false
};

export type LicenseService = {
  getEntitlements(): Promise<LicenseEntitlements>;
  isConfigured(): boolean;
};

export async function createLicenseService(env: NodeJS.ProcessEnv = process.env): Promise<LicenseService> {
  const licensePath = join(app.getPath("userData"), "license.json");
  const deviceId = app.getPath("userData");
  const config = resolveLicenseApiConfig(env, deviceId);
  const client = createLicenseApiClient(config);
  let cachedEntitlements: LicenseEntitlements | undefined;

  async function refreshEntitlements(): Promise<LicenseEntitlements> {
    if (!config.endpoint) {
      cachedEntitlements = EXPIRED_ENTITLEMENTS;
      return cachedEntitlements;
    }

    const stored = await readStoredLicense(licensePath);
    if (!stored?.licenseKey) {
      cachedEntitlements = EXPIRED_ENTITLEMENTS;
      return cachedEntitlements;
    }

    try {
      const verified = await client.verifyLicense(stored.licenseKey);
      cachedEntitlements = verified.entitlements;
      return cachedEntitlements;
    } catch (error: unknown) {
      const message = error instanceof Error ? error.message : "License verification failed.";
      console.warn(`[license] ${message}`);
      cachedEntitlements = EXPIRED_ENTITLEMENTS;
      return cachedEntitlements;
    }
  }

  ipcMain.handle("corevideo:license-get-entitlements", async () => refreshEntitlements());
  ipcMain.handle("corevideo:license-start-trial", async (_event, email?: string) => {
    if (!config.endpoint) {
      return { ok: false, message: "License API not configured (stub mode)." };
    }
    const trial = await client.startTrial(email);
    await writeStoredLicense(licensePath, {
      licenseKey: trial.licenseKey,
      deviceId,
      email,
      updatedAt: new Date().toISOString()
    });
    cachedEntitlements = trial.entitlements;
    return { ok: true, ...trial };
  });
  ipcMain.handle("corevideo:license-activate", async (_event, payload: { licenseKey: string; email?: string }) => {
    if (!config.endpoint) {
      return { ok: false, message: "License API not configured (stub mode)." };
    }
    const activated = await client.activateLicense(payload.licenseKey, payload.email);
    await writeStoredLicense(licensePath, {
      licenseKey: payload.licenseKey,
      deviceId,
      email: payload.email,
      updatedAt: new Date().toISOString()
    });
    cachedEntitlements = activated.entitlements;
    return { ok: true, ...activated };
  });
  ipcMain.handle(
    "corevideo:license-checkout",
    async (_event, payload: { tier: "creator" | "pro" | "team"; email?: string; successUrl?: string }) => {
      if (!config.endpoint) {
        return { ok: false, message: "License API not configured (stub mode)." };
      }
      const session = await client.createCheckoutSession(payload.tier, payload.email, payload.successUrl);
      return { ok: true, ...session };
    }
  );

  return {
    getEntitlements: refreshEntitlements,
    isConfigured: () => Boolean(config.endpoint)
  };
}