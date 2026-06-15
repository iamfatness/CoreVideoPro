import { describe, expect, it, vi } from "vitest";
import { createLicenseApiClient, resolveLicenseApiConfig } from "./licenseApiClient.ts";

describe("resolveLicenseApiConfig", () => {
  it("reads endpoint and device id from env", () => {
    const config = resolveLicenseApiConfig(
      { COREVIDEO_LICENSE_API_ENDPOINT: "https://license.example.com", COREVIDEO_LICENSE_API_KEY: "secret" },
      "device-1"
    );
    expect(config.endpoint).toBe("https://license.example.com");
    expect(config.deviceId).toBe("device-1");
  });
});

describe("createLicenseApiClient", () => {
  it("throws in stub mode when endpoint is missing", async () => {
    const client = createLicenseApiClient({ deviceId: "device-1" });
    await expect(client.startTrial("ops@example.com")).rejects.toThrow(/stub mode/i);
  });

  it("runs trial → activate → verify with bearer auth", async () => {
    const fetchMock = vi.fn(async (url: string, init?: RequestInit) => {
      if (url.endsWith("/v1/trial/start")) {
        return new Response(
          JSON.stringify({
            licenseKey: "cv-trial-abc",
            entitlements: { licenseKey: "cv-trial-abc", tier: "trial", status: "active", maxOutputHeight: 720 }
          }),
          { status: 200 }
        );
      }
      if (url.endsWith("/v1/license/activate")) {
        return new Response(
          JSON.stringify({
            activated: true,
            entitlements: { licenseKey: "cv-trial-abc", tier: "trial", status: "active", maxOutputHeight: 720 }
          }),
          { status: 200 }
        );
      }
      if (url.includes("/v1/license/verify")) {
        return new Response(
          JSON.stringify({
            valid: true,
            entitlements: { licenseKey: "cv-trial-abc", tier: "trial", status: "active", maxOutputHeight: 720 }
          }),
          { status: 200 }
        );
      }
      return new Response("not found", { status: 404 });
    });

    const client = createLicenseApiClient(
      { endpoint: "https://license.example.com", apiKey: "secret", deviceId: "device-1" },
      fetchMock
    );

    const trial = await client.startTrial("ops@example.com");
    expect(trial.licenseKey).toBe("cv-trial-abc");

    const activated = await client.activateLicense(trial.licenseKey);
    expect(activated.activated).toBe(true);

    const verified = await client.verifyLicense(trial.licenseKey);
    expect(verified.valid).toBe(true);

    const [, trialInit] = fetchMock.mock.calls[0] as [string, RequestInit];
    expect(trialInit.headers).toMatchObject({ authorization: "Bearer secret" });
  });
});