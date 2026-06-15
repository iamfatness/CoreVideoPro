import { mkdtemp } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { describe, expect, it } from "vitest";
import { readStoredLicense, writeStoredLicense } from "./licenseStore.ts";

describe("licenseStore", () => {
  it("persists and reloads the active license key", async () => {
    const dir = await mkdtemp(join(tmpdir(), "corevideo-license-"));
    const path = join(dir, "license.json");
    await writeStoredLicense(path, {
      licenseKey: "cv-trial-test",
      deviceId: "device-1",
      email: "ops@example.com",
      updatedAt: new Date().toISOString()
    });
    const loaded = await readStoredLicense(path);
    expect(loaded?.licenseKey).toBe("cv-trial-test");
    expect(loaded?.email).toBe("ops@example.com");
  });
});