import type { ActivationResult, LicenseClient, LicenseState, LicenseTier } from "./licenseClient";

type DesktopLicenseEntitlements = {
  licenseKey?: string;
  email?: string;
  tier: "trial" | "creator" | "pro" | "team" | "expired";
  status: "active" | "expired" | "grace" | "none";
  expiresAt: string | null;
};

type LicenseBridge = {
  getLicenseEntitlements(): Promise<DesktopLicenseEntitlements>;
  startLicenseTrial?(email?: string): Promise<{
    ok: boolean;
    licenseKey?: string;
    entitlements?: DesktopLicenseEntitlements;
    message?: string;
  }>;
  activateLicense?(licenseKey: string, email?: string): Promise<{
    ok: boolean;
    activated?: boolean;
    entitlements?: DesktopLicenseEntitlements;
    message?: string;
  }>;
  createLicenseCheckout?(
    tier: "creator" | "pro" | "team",
    email?: string,
    successUrl?: string
  ): Promise<{ ok: boolean; checkoutUrl?: string; sessionId?: string; message?: string }>;
};

function toLicenseTier(tier: DesktopLicenseEntitlements["tier"]): LicenseTier {
  return tier === "expired" ? "trial" : tier;
}

export function isLicenseBridge(value: unknown): value is LicenseBridge {
  return (
    typeof value === "object" &&
    value !== null &&
    "getLicenseEntitlements" in value &&
    typeof (value as LicenseBridge).getLicenseEntitlements === "function"
  );
}

export function mapDesktopEntitlementsToLicenseState(entitlements: DesktopLicenseEntitlements): LicenseState {
  if (entitlements.status === "none" || entitlements.tier === "expired") {
    return { tier: "trial", status: "unlicensed", accountEmail: entitlements.email };
  }

  if (entitlements.status === "expired") {
    const tier: LicenseTier = toLicenseTier(entitlements.tier);
    return {
      tier,
      status: "expired",
      trialEndsAtMs: entitlements.expiresAt ? Date.parse(entitlements.expiresAt) : undefined,
      accountEmail: entitlements.email
    };
  }

  if (entitlements.tier === "trial") {
    return {
      tier: "trial",
      status: "trial",
      trialEndsAtMs: entitlements.expiresAt ? Date.parse(entitlements.expiresAt) : undefined,
      accountEmail: entitlements.email
    };
  }

  return {
    tier: toLicenseTier(entitlements.tier),
    status: "active",
    accountEmail: entitlements.email
  };
}

export class RemoteLicenseClient implements LicenseClient {
  private cached: LicenseState = { tier: "trial", status: "unlicensed" };

  constructor(private readonly bridge: LicenseBridge) {}

  async getState(): Promise<LicenseState> {
    return { ...this.cached };
  }

  async refresh(): Promise<LicenseState> {
    const entitlements = await this.bridge.getLicenseEntitlements();
    this.cached = mapDesktopEntitlementsToLicenseState(entitlements);
    return { ...this.cached };
  }

  async signIn(email: string): Promise<LicenseState> {
    if (this.bridge.startLicenseTrial) {
      const result = await this.bridge.startLicenseTrial(email);
      if (result.ok && result.entitlements) {
        this.cached = mapDesktopEntitlementsToLicenseState(result.entitlements);
        return { ...this.cached };
      }
    }
    this.cached = { ...this.cached, accountEmail: email };
    return { ...this.cached };
  }

  async activate(key: string): Promise<ActivationResult> {
    if (!this.bridge.activateLicense) {
      return { ok: false, message: "License activation is unavailable in this host." };
    }
    const result = await this.bridge.activateLicense(key, this.cached.accountEmail);
    if (!result.ok || !result.entitlements) {
      return { ok: false, message: result.message ?? "Activation failed." };
    }
    this.cached = mapDesktopEntitlementsToLicenseState(result.entitlements);
    return { ok: true, state: { ...this.cached } };
  }

  async openUpgrade(tier: LicenseTier): Promise<{ checkoutUrl: string }> {
    if (!this.bridge.createLicenseCheckout || tier === "trial") {
      return { checkoutUrl: `https://corevideo.pro/checkout?tier=${tier}` };
    }
    const paidTier = tier as "creator" | "pro" | "team";
    const result = await this.bridge.createLicenseCheckout(paidTier, this.cached.accountEmail);
    return { checkoutUrl: result.checkoutUrl ?? `https://corevideo.pro/checkout?tier=${tier}` };
  }
}