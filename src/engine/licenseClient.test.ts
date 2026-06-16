import { describe, expect, it } from "vitest";
import {
  StubLicenseClient,
  TRIAL_LENGTH_DAYS,
  TRIAL_WATERMARK_AFTER_MINUTES,
  deriveEntitlements,
  watermarkActiveNow,
  type LicenseState
} from "./licenseClient";

const NOW = 1_700_000_000_000;
const DAY = 24 * 60 * 60 * 1000;

describe("deriveEntitlements", () => {
  it("gives a trial full Pro features but caps at 720p with a delayed watermark", () => {
    const state: LicenseState = { tier: "trial", status: "trial", trialEndsAtMs: NOW + 5 * DAY };
    const ent = deriveEntitlements(state, NOW);
    expect(ent.maxZoomParticipants).toBe(8);
    expect(ent.setAndForget).toBe(true);
    expect(ent.chromaKey).toBe(true);
    expect(ent.maxOutputHeight).toBe(720);
    expect(ent.watermark).toBe(true);
    expect(ent.watermarkAfterMinutes).toBe(TRIAL_WATERMARK_AFTER_MINUTES);
  });

  it("collapses an expired trial to the locked state", () => {
    const state: LicenseState = { tier: "trial", status: "trial", trialEndsAtMs: NOW - DAY };
    const ent = deriveEntitlements(state, NOW);
    expect(ent.maxZoomParticipants).toBe(2);
    expect(ent.maxOutputHeight).toBe(720);
    expect(ent.watermark).toBe(true);
    expect(ent.watermarkAfterMinutes).toBeUndefined();
    expect(ent.setAndForget).toBe(false);
  });

  it("limits Creator to 4 participants, 1080p, no Pro features", () => {
    const ent = deriveEntitlements({ tier: "creator", status: "active" }, NOW);
    expect(ent.maxZoomParticipants).toBe(4);
    expect(ent.maxOutputHeight).toBe(1080);
    expect(ent.watermark).toBe(false);
    expect(ent.setAndForget).toBe(false);
    expect(ent.chromaKey).toBe(false);
    expect(ent.multiDestination).toBe(false);
    expect(ent.phase2Outputs).toBe(false);
  });

  it("unlocks Pro and Team to 8 participants and all features", () => {
    for (const tier of ["pro", "team"] as const) {
      const ent = deriveEntitlements({ tier, status: "active" }, NOW);
      expect(ent.maxZoomParticipants).toBe(8);
      expect(ent.setAndForget).toBe(true);
      expect(ent.chromaKey).toBe(true);
      expect(ent.advancedCaptions).toBe(true);
      expect(ent.multiDestination).toBe(true);
      expect(ent.phase2Outputs).toBe(true);
      expect(ent.watermark).toBe(false);
    }
  });

  it("locks expired and unlicensed states", () => {
    for (const status of ["expired", "unlicensed"] as const) {
      const ent = deriveEntitlements({ tier: "pro", status }, NOW);
      expect(ent.maxZoomParticipants).toBe(2);
      expect(ent.watermark).toBe(true);
      expect(ent.setAndForget).toBe(false);
    }
  });
});

describe("watermarkActiveNow", () => {
  it("is false when there is no watermark entitlement", () => {
    const ent = deriveEntitlements({ tier: "pro", status: "active" }, NOW);
    expect(watermarkActiveNow(ent, 60 * 60_000)).toBe(false);
  });

  it("delays the trial watermark until after the grace window", () => {
    const ent = deriveEntitlements({ tier: "trial", status: "trial", trialEndsAtMs: NOW + DAY }, NOW);
    expect(watermarkActiveNow(ent, 10 * 60_000)).toBe(false);
    expect(watermarkActiveNow(ent, (TRIAL_WATERMARK_AFTER_MINUTES + 1) * 60_000)).toBe(true);
  });

  it("is always on for the locked state", () => {
    const ent = deriveEntitlements({ tier: "creator", status: "expired" }, NOW);
    expect(watermarkActiveNow(ent, 0)).toBe(true);
  });
});

describe("StubLicenseClient", () => {
  it("defaults to a 14-day trial", async () => {
    const client = new StubLicenseClient(undefined, () => NOW);
    const state = await client.getState();
    expect(state.tier).toBe("trial");
    expect(state.status).toBe("trial");
    expect(state.trialEndsAtMs).toBe(NOW + TRIAL_LENGTH_DAYS * DAY);
  });

  it("records the signed-in email", async () => {
    const client = new StubLicenseClient(undefined, () => NOW);
    const state = await client.signIn("producer@example.com");
    expect(state.accountEmail).toBe("producer@example.com");
  });

  it("activates a valid key into the matching active tier", async () => {
    const client = new StubLicenseClient(undefined, () => NOW);
    const result = await client.activate("CVP-PRO-ABC123");
    expect(result.ok).toBe(true);
    if (result.ok) {
      expect(result.state.tier).toBe("pro");
      expect(result.state.status).toBe("active");
    }
  });

  it("rejects an invalid key", async () => {
    const client = new StubLicenseClient(undefined, () => NOW);
    const result = await client.activate("not-a-key");
    expect(result.ok).toBe(false);
    expect((await client.getState()).status).toBe("trial");
  });

  it("returns a tier-specific checkout URL", async () => {
    const client = new StubLicenseClient(undefined, () => NOW);
    const { checkoutUrl } = await client.openUpgrade("team");
    expect(checkoutUrl).toContain("tier=team");
  });
});
