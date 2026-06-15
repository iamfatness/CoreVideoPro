import { mkdir, readFile, writeFile } from "node:fs/promises";
import { dirname } from "node:path";
import type { StoredLicenseRecord } from "./licenseTypes.ts";

export async function readStoredLicense(filePath: string): Promise<StoredLicenseRecord | undefined> {
  try {
    const raw = await readFile(filePath, "utf8");
    const parsed = JSON.parse(raw) as Partial<StoredLicenseRecord>;
    if (typeof parsed.licenseKey !== "string" || typeof parsed.deviceId !== "string") {
      return undefined;
    }
    return {
      licenseKey: parsed.licenseKey,
      deviceId: parsed.deviceId,
      email: typeof parsed.email === "string" ? parsed.email : undefined,
      updatedAt: typeof parsed.updatedAt === "string" ? parsed.updatedAt : new Date(0).toISOString()
    };
  } catch {
    return undefined;
  }
}

export async function writeStoredLicense(filePath: string, record: StoredLicenseRecord): Promise<StoredLicenseRecord> {
  await mkdir(dirname(filePath), { recursive: true });
  await writeFile(filePath, `${JSON.stringify(record, null, 2)}\n`, "utf8");
  return record;
}