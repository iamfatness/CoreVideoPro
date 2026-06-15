/**
 * License / account seam.
 *
 * This is the typed boundary the renderer calls to learn what the current user
 * is entitled to (tier, trial state) and to drive sign-in / activation / upgrade.
 * The renderer only ever talks to the {@link LicenseClient} interface and the
 * pure {@link deriveEntitlements} mapping — it never knows whether entitlements
 * come from an in-memory stub or a real billing backend.
 *
 * Lane ownership (see docs/agent-briefs/08-launch-plan-v1.md): Claude owns this
 * interface + the {@link StubLicenseClient}; Grok (C6) implements a real
 * Supabase/Stripe-backed `LicenseClient` and injects it in `engineBundle.ts`
 * with zero renderer changes. The stub is the in-container-green default.
 */

export type LicenseTier = "trial" | "creator" | "pro" | "team";

export type LicenseStatus = "trial" | "active" | "expired" | "unlicensed";

export type LicenseState = {
  tier: LicenseTier;
  status: LicenseStatus;
  /** Epoch ms when the trial ends. Present only while `status === "trial"`. */
  trialEndsAtMs?: number;
  /** Signed-in account email, when known. */
  accountEmail?: string;
};

/**
 * Concrete capability limits derived from a {@link LicenseState}. The renderer
 * composes these with the native core's announced capabilities — a feature is
 * available only when entitlement AND capability both allow it.
 */
export type Entitlements = {
  /** Max Zoom participants that may be composited into the program. */
  maxZoomParticipants: number;
  /** Max output height in pixels (720 / 1080 / 2160). */
  maxOutputHeight: number;
  /** Whether a watermark must be applied to the output. */
  watermark: boolean;
  /**
   * When set, the watermark only engages after this many minutes of a session
   * (the trial pattern: clean for the first 30 minutes, watermarked after).
   */
  watermarkAfterMinutes?: number;
  /** Set & Forget auto-director. */
  setAndForget: boolean;
  /** Chroma-key participant effect. */
  chromaKey: boolean;
  /** Advanced (speaker-attributed, styled) captions vs. basic captions. */
  advancedCaptions: boolean;
  /** Multiple simultaneous output destinations. */
  multiDestination: boolean;
  /** Phase-2 output protocols (NDI / SRT / WebRTC), still core-capability gated. */
  phase2Outputs: boolean;
};

export const TRIAL_LENGTH_DAYS = 14;
export const TRIAL_WATERMARK_AFTER_MINUTES = 30;

/** Tier labels + monthly price for UI (per the product spec's Monetization). */
export const TIER_INFO: Record<LicenseTier, { label: string; pricePerMonthUsd: number }> = {
  trial: { label: "Free Trial", pricePerMonthUsd: 0 },
  creator: { label: "Creator", pricePerMonthUsd: 19 },
  pro: { label: "Pro", pricePerMonthUsd: 49 },
  team: { label: "Team", pricePerMonthUsd: 99 }
};

const CREATOR_ENTITLEMENTS: Entitlements = {
  maxZoomParticipants: 4,
  maxOutputHeight: 1080,
  watermark: false,
  setAndForget: false,
  chromaKey: false,
  advancedCaptions: false,
  multiDestination: false,
  phase2Outputs: false
};

const PRO_ENTITLEMENTS: Entitlements = {
  maxZoomParticipants: 8,
  maxOutputHeight: 1080,
  watermark: false,
  setAndForget: true,
  chromaKey: true,
  advancedCaptions: true,
  multiDestination: true,
  phase2Outputs: true
};

/**
 * Locked nag state for expired / unlicensed users: enough to look around, not
 * enough to produce a real show.
 */
const LOCKED_ENTITLEMENTS: Entitlements = {
  maxZoomParticipants: 2,
  maxOutputHeight: 720,
  watermark: true,
  setAndForget: false,
  chromaKey: false,
  advancedCaptions: false,
  multiDestination: false,
  phase2Outputs: false
};

/**
 * Pure mapping of license state → concrete limits.
 *
 * Trial policy (per spec: "14 days, watermark after 30 minutes, 720p"): a trial
 * gets **full Pro features for evaluation**, constrained to 720p output and a
 * watermark that engages after the first 30 minutes, and expires after 14 days.
 * Once `trialEndsAtMs` has passed it collapses to the locked state.
 */
export function deriveEntitlements(state: LicenseState, nowMs: number): Entitlements {
  if (state.status === "expired" || state.status === "unlicensed") {
    return { ...LOCKED_ENTITLEMENTS };
  }

  if (state.status === "trial") {
    if (typeof state.trialEndsAtMs === "number" && nowMs >= state.trialEndsAtMs) {
      return { ...LOCKED_ENTITLEMENTS };
    }
    return {
      ...PRO_ENTITLEMENTS,
      maxOutputHeight: 720,
      watermark: true,
      watermarkAfterMinutes: TRIAL_WATERMARK_AFTER_MINUTES
    };
  }

  // status === "active": entitlements follow the tier.
  switch (state.tier) {
    case "creator":
      return { ...CREATOR_ENTITLEMENTS };
    case "pro":
    case "team":
      return { ...PRO_ENTITLEMENTS };
    case "trial":
      // Active+trial is incoherent; treat as a fresh trial's feature set.
      return { ...PRO_ENTITLEMENTS, maxOutputHeight: 720, watermark: true };
  }
}

/**
 * Whether the trial watermark should currently be showing, given how long the
 * session has been running. Always true for non-trial watermark states.
 */
export function watermarkActiveNow(entitlements: Entitlements, sessionElapsedMs: number): boolean {
  if (!entitlements.watermark) {
    return false;
  }
  if (entitlements.watermarkAfterMinutes === undefined) {
    return true;
  }
  return sessionElapsedMs >= entitlements.watermarkAfterMinutes * 60_000;
}

export type ActivationResult = { ok: true; state: LicenseState } | { ok: false; message: string };

export interface LicenseClient {
  /** Current license state (cached; cheap to call). */
  getState(): Promise<LicenseState>;
  /** Re-fetch from the backend. The stub just returns the cached state. */
  refresh(): Promise<LicenseState>;
  /** Begin / resume an account session by email. */
  signIn(email: string): Promise<LicenseState>;
  /** Redeem a license key, moving from trial/expired to an active tier. */
  activate(key: string): Promise<ActivationResult>;
  /** Open the upgrade/checkout flow for a tier (returns a URL the shell opens). */
  openUpgrade(tier: LicenseTier): Promise<{ checkoutUrl: string }>;
}

/**
 * Deterministic in-memory license client. Default state is a fresh 14-day trial.
 * Activation keys of the form `CVP-<TIER>-...` move to that active tier; anything
 * else is rejected. Replaced by Grok's Supabase/Stripe client for real billing.
 */
export class StubLicenseClient implements LicenseClient {
  private state: LicenseState;

  constructor(initial?: Partial<LicenseState>, private readonly nowMs: () => number = () => Date.now()) {
    this.state = {
      tier: initial?.tier ?? "trial",
      status: initial?.status ?? "trial",
      trialEndsAtMs:
        initial?.trialEndsAtMs ??
        (initial?.status === undefined || initial?.status === "trial"
          ? this.nowMs() + TRIAL_LENGTH_DAYS * 24 * 60 * 60 * 1000
          : undefined),
      accountEmail: initial?.accountEmail
    };
  }

  async getState(): Promise<LicenseState> {
    return { ...this.state };
  }

  async refresh(): Promise<LicenseState> {
    return { ...this.state };
  }

  async signIn(email: string): Promise<LicenseState> {
    this.state = { ...this.state, accountEmail: email };
    return { ...this.state };
  }

  async activate(key: string): Promise<ActivationResult> {
    const tier = parseActivationTier(key);
    if (!tier) {
      return { ok: false, message: "Invalid license key." };
    }
    this.state = { tier, status: "active", accountEmail: this.state.accountEmail };
    return { ok: true, state: { ...this.state } };
  }

  async openUpgrade(tier: LicenseTier): Promise<{ checkoutUrl: string }> {
    return { checkoutUrl: `https://corevideo.pro/checkout?tier=${tier}` };
  }
}

function parseActivationTier(key: string): LicenseTier | undefined {
  const match = /^CVP-(CREATOR|PRO|TEAM)-/i.exec(key.trim());
  if (!match) {
    return undefined;
  }
  return match[1].toLowerCase() as LicenseTier;
}
