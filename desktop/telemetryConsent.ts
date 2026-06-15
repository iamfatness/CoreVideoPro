import { mkdir, readFile, writeFile } from "node:fs/promises";
import { dirname } from "node:path";

export type TelemetryConsentState = {
  analyticsEnabled: boolean;
  updatedAt: string;
};

const DEFAULT_CONSENT: TelemetryConsentState = {
  analyticsEnabled: false,
  updatedAt: new Date(0).toISOString()
};

export async function readTelemetryConsent(filePath: string): Promise<TelemetryConsentState> {
  try {
    const raw = await readFile(filePath, "utf8");
    const parsed = JSON.parse(raw) as Partial<TelemetryConsentState>;
    return {
      analyticsEnabled: parsed.analyticsEnabled === true,
      updatedAt: typeof parsed.updatedAt === "string" ? parsed.updatedAt : DEFAULT_CONSENT.updatedAt
    };
  } catch {
    return { ...DEFAULT_CONSENT };
  }
}

export async function writeTelemetryConsent(
  filePath: string,
  analyticsEnabled: boolean
): Promise<TelemetryConsentState> {
  const state: TelemetryConsentState = {
    analyticsEnabled,
    updatedAt: new Date().toISOString()
  };
  await mkdir(dirname(filePath), { recursive: true });
  await writeFile(filePath, `${JSON.stringify(state, null, 2)}\n`, "utf8");
  return state;
}