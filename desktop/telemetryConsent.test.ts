import { mkdtemp, readFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { afterEach, describe, expect, it } from "vitest";
import { readTelemetryConsent, writeTelemetryConsent } from "./telemetryConsent.ts";

describe("telemetryConsent", () => {
  let tempDir = "";

  afterEach(() => {
    tempDir = "";
  });

  async function consentPath() {
    tempDir = await mkdtemp(join(tmpdir(), "corevideo-telemetry-consent-"));
    return join(tempDir, "telemetry-consent.json");
  }

  it("defaults to analytics disabled when no file exists", async () => {
    const state = await readTelemetryConsent(await consentPath());
    expect(state.analyticsEnabled).toBe(false);
  });

  it("persists opt-in and opt-out choices", async () => {
    const path = await consentPath();
    const enabled = await writeTelemetryConsent(path, true);
    expect(enabled.analyticsEnabled).toBe(true);

    const disabled = await writeTelemetryConsent(path, false);
    expect(disabled.analyticsEnabled).toBe(false);

    const raw = await readFile(path, "utf8");
    expect(JSON.parse(raw)).toMatchObject({ analyticsEnabled: false });
    expect(await readTelemetryConsent(path)).toEqual(disabled);
  });
});