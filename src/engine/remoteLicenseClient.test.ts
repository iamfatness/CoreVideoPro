import { describe, expect, it } from "vitest";
import { mapDesktopEntitlementsToLicenseState, RemoteLicenseClient } from "./remoteLicenseClient";

describe("mapDesktopEntitlementsToLicenseState", () => {
  it("maps an active trial from the licensing API", () => {
    const state = mapDesktopEntitlementsToLicenseState({
      tier: "trial",
      status: "active",
      expiresAt: "2026-07-01T00:00:00.000Z",
      email: "ops@example.com"
    });
    expect(state).toEqual({
      tier: "trial",
      status: "trial",
      trialEndsAtMs: Date.parse("2026-07-01T00:00:00.000Z"),
      accountEmail: "ops@example.com"
    });
  });

  it("maps unconfigured desktop hosts to unlicensed", () => {
    const state = mapDesktopEntitlementsToLicenseState({
      tier: "expired",
      status: "none",
      expiresAt: null
    });
    expect(state.status).toBe("unlicensed");
  });
});

describe("RemoteLicenseClient", () => {
  it("refreshes license state from the electron bridge", async () => {
    const client = new RemoteLicenseClient({
      async getLicenseEntitlements() {
        return { tier: "pro", status: "active", expiresAt: null, email: "ops@example.com" };
      }
    });
    const state = await client.refresh();
    expect(state).toEqual({ tier: "pro", status: "active", accountEmail: "ops@example.com" });
  });
});