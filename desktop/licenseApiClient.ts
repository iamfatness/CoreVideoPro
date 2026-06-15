import type { LicenseEntitlements } from "./licenseTypes.ts";

export type LicenseApiConfig = {
  endpoint?: string;
  apiKey?: string;
  deviceId: string;
};

export type StartTrialResult = {
  licenseKey: string;
  entitlements: LicenseEntitlements;
};

export type ActivateLicenseResult = {
  activated: boolean;
  entitlements: LicenseEntitlements;
};

export type VerifyLicenseResult = {
  valid: boolean;
  entitlements: LicenseEntitlements;
};

export type CheckoutSessionResult = {
  sessionId: string;
  checkoutUrl: string;
  mode: "stub" | "stripe";
  tier: string;
};

type FetchLike = typeof fetch;

export function resolveLicenseApiConfig(env: NodeJS.ProcessEnv, deviceId: string): LicenseApiConfig {
  return {
    endpoint: env.COREVIDEO_LICENSE_API_ENDPOINT?.trim() || undefined,
    apiKey: env.COREVIDEO_LICENSE_API_KEY?.trim() || undefined,
    deviceId
  };
}

export function createLicenseApiClient(config: LicenseApiConfig, fetchImpl: FetchLike = fetch) {
  const endpoint = config.endpoint?.replace(/\/$/, "");

  async function request(path: string, init?: RequestInit) {
    if (!endpoint) {
      throw new Error("License API endpoint not configured (stub mode).");
    }

    const headers: Record<string, string> = {
      "content-type": "application/json",
      ...(init?.headers as Record<string, string> | undefined)
    };
    if (config.apiKey) {
      headers.authorization = `Bearer ${config.apiKey}`;
    }

    const response = await fetchImpl(`${endpoint}${path}`, {
      ...init,
      headers
    });

    let body: unknown;
    try {
      body = await response.json();
    } catch {
      body = undefined;
    }

    if (!response.ok) {
      const message =
        typeof body === "object" && body && "error" in body && typeof body.error === "string"
          ? body.error
          : `License API failed (${response.status}).`;
      throw new Error(message);
    }

    return body;
  }

  return {
    getConfig: () => config,
    async startTrial(email?: string): Promise<StartTrialResult> {
      return (await request("/v1/trial/start", {
        method: "POST",
        body: JSON.stringify({ email, deviceId: config.deviceId })
      })) as StartTrialResult;
    },
    async activateLicense(licenseKey: string, email?: string): Promise<ActivateLicenseResult> {
      const result = (await request("/v1/license/activate", {
        method: "POST",
        body: JSON.stringify({ licenseKey, deviceId: config.deviceId, email })
      })) as { activated: boolean; entitlements: LicenseEntitlements };
      return { activated: result.activated === true, entitlements: result.entitlements };
    },
    async verifyLicense(licenseKey: string): Promise<VerifyLicenseResult> {
      const params = new URLSearchParams({
        licenseKey,
        deviceId: config.deviceId
      });
      const result = (await request(`/v1/license/verify?${params.toString()}`)) as VerifyLicenseResult;
      return { valid: result.valid === true, entitlements: result.entitlements };
    },
    async createCheckoutSession(tier: "creator" | "pro" | "team", email?: string, successUrl?: string): Promise<CheckoutSessionResult> {
      const result = (await request("/v1/checkout/session", {
        method: "POST",
        body: JSON.stringify({ tier, email, successUrl, cancelUrl: successUrl })
      })) as CheckoutSessionResult;
      return result;
    },
    async upgradeLicense(licenseKey: string, tier: "creator" | "pro" | "team"): Promise<LicenseEntitlements> {
      const result = (await request("/v1/license/upgrade", {
        method: "POST",
        body: JSON.stringify({ licenseKey, tier })
      })) as { entitlements: LicenseEntitlements };
      return result.entitlements;
    }
  };
}

export type LicenseApiClient = ReturnType<typeof createLicenseApiClient>;