/**
 * @typedef {object} LicenseEntitlements
 * @property {string} licenseKey
 * @property {string | undefined} email
 * @property {"trial" | "creator" | "pro" | "team" | "expired"} tier
 * @property {"active" | "expired" | "grace" | "none"} status
 * @property {string | null} expiresAt
 * @property {number} maxParticipants
 * @property {720 | 1080} maxOutputHeight
 * @property {number | null} watermarkAfterMinutes
 * @property {"none" | "basic" | "advanced"} captionsLevel
 * @property {boolean} chromaKey
 * @property {boolean} setAndForget
 * @property {"none" | "basic" | "advanced"} magicSceneLevel
 * @property {boolean} multiDestination
 * @property {boolean} ndiSrt
 */

/**
 * @typedef {object} StoredLicense
 * @property {string} licenseKey
 * @property {string | undefined} email
 * @property {"trial" | "creator" | "pro" | "team" | "expired"} tier
 * @property {string} createdAt
 * @property {string | null} expiresAt
 * @property {string[]} activatedDevices
 * @property {string | undefined} stripeCustomerId
 * @property {string | undefined} stripeSubscriptionId
 */

export {};