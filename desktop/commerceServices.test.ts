import { afterEach, describe, expect, it } from "vitest";
import { handlePushChunk, handleStartSession } from "../services/caption-broker/lib/handlers.mjs";
import { resetCaptionSessions } from "../services/caption-broker/lib/session.mjs";
import {
  handleActivate as activateLicense,
  handleStartTrial as startTrial,
  handleUpgrade,
  handleVerify as verifyLicense
} from "../services/licensing-api/lib/handlers.mjs";
import { resetLicenseStore } from "../services/licensing-api/lib/store.mjs";

describe("licensing-api handlers", () => {
  afterEach(() => {
    resetLicenseStore();
  });

  it("issues a 14-day trial with 720p + watermark entitlements", async () => {
    const trial = await startTrial({ email: "ops@example.com", deviceId: "device-a" }, {});
    expect(trial.licenseKey).toMatch(/^cv-trial-/);
    expect(trial.entitlements.tier).toBe("trial");
    expect(trial.entitlements.maxOutputHeight).toBe(720);
    expect(trial.entitlements.watermarkAfterMinutes).toBe(30);

    const activated = await activateLicense({ licenseKey: trial.licenseKey, deviceId: "device-a" }, {});
    expect(activated.ok).toBe(true);

    const verified = await verifyLicense(new URLSearchParams({ licenseKey: trial.licenseKey, deviceId: "device-a" }), {});
    expect(verified.valid).toBe(true);
  });

  it("upgrades a trial license to Pro entitlements", async () => {
    const trial = await startTrial({ deviceId: "device-b" }, {});
    const upgraded = await handleUpgrade({ licenseKey: trial.licenseKey, tier: "pro" }, {});
    expect(upgraded.ok).toBe(true);
    expect(upgraded.entitlements.tier).toBe("pro");
    expect(upgraded.entitlements.chromaKey).toBe(true);
    expect(upgraded.entitlements.captionsLevel).toBe("advanced");
  });
});

describe("caption-broker handlers", () => {
  afterEach(() => {
    resetCaptionSessions();
  });

  it("returns attributed cues with usable latency for audio chunks", () => {
    const session = handleStartSession({ productionSessionId: "webinar-1" });
    expect(session.ok).toBe(true);

    const chunk = handlePushChunk(session.brokerSessionId, {
      atMs: 1500,
      speakerId: "p1",
      speakerName: "Priya Shah",
      audioLevel: 55
    });
    expect(chunk.ok).toBe(true);
    expect(chunk.latencyMs).toBeLessThanOrEqual(250);
    expect(chunk.cues[0]?.speaker).toBe("Priya Shah");
    expect(chunk.cues[0]?.text).toContain("Priya Shah");
  });
});