/** @typedef {"trial" | "creator" | "pro" | "team" | "expired"} LicenseTier */
/** @typedef {"active" | "expired" | "grace" | "none"} LicenseStatus */

export const TRIAL_DAYS = 14;
export const TRIAL_WATERMARK_AFTER_MINUTES = 30;

/** @type {Record<LicenseTier, Omit<import("./types.mjs").LicenseEntitlements, "tier" | "status" | "expiresAt" | "licenseKey" | "email">>} */
export const TIER_DEFAULTS = {
  trial: {
    maxParticipants: 4,
    maxOutputHeight: 720,
    watermarkAfterMinutes: TRIAL_WATERMARK_AFTER_MINUTES,
    captionsLevel: "basic",
    chromaKey: false,
    setAndForget: false,
    magicSceneLevel: "basic",
    multiDestination: false,
    ndiSrt: false
  },
  creator: {
    maxParticipants: 4,
    maxOutputHeight: 1080,
    watermarkAfterMinutes: null,
    captionsLevel: "basic",
    chromaKey: false,
    setAndForget: false,
    magicSceneLevel: "basic",
    multiDestination: false,
    ndiSrt: false
  },
  pro: {
    maxParticipants: 8,
    maxOutputHeight: 1080,
    watermarkAfterMinutes: null,
    captionsLevel: "advanced",
    chromaKey: true,
    setAndForget: true,
    magicSceneLevel: "advanced",
    multiDestination: true,
    ndiSrt: true
  },
  team: {
    maxParticipants: 8,
    maxOutputHeight: 1080,
    watermarkAfterMinutes: null,
    captionsLevel: "advanced",
    chromaKey: true,
    setAndForget: true,
    magicSceneLevel: "advanced",
    multiDestination: true,
    ndiSrt: true
  },
  expired: {
    maxParticipants: 0,
    maxOutputHeight: 720,
    watermarkAfterMinutes: 0,
    captionsLevel: "none",
    chromaKey: false,
    setAndForget: false,
    magicSceneLevel: "none",
    multiDestination: false,
    ndiSrt: false
  }
};

/**
 * @param {LicenseTier} tier
 * @param {LicenseStatus} status
 * @param {string | null} expiresAt
 * @param {string} licenseKey
 * @param {string | undefined} email
 * @returns {import("./types.mjs").LicenseEntitlements}
 */
export function buildEntitlements(tier, status, expiresAt, licenseKey, email) {
  const defaults = TIER_DEFAULTS[tier] ?? TIER_DEFAULTS.expired;
  return {
    licenseKey,
    email,
    tier,
    status,
    expiresAt,
    ...defaults
  };
}

/**
 * @param {string | undefined} tier
 * @returns {tier is "creator" | "pro" | "team"}
 */
export function isPaidTier(tier) {
  return tier === "creator" || tier === "pro" || tier === "team";
}