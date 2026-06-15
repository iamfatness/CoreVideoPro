export type LicenseTier = "trial" | "creator" | "pro" | "team" | "expired";

export type LicenseStatus = "active" | "expired" | "grace" | "none";

export type CaptionsLevel = "none" | "basic" | "advanced";

export type MagicSceneLevel = "none" | "basic" | "advanced";

export type LicenseEntitlements = {
  licenseKey: string;
  email?: string;
  tier: LicenseTier;
  status: LicenseStatus;
  expiresAt: string | null;
  maxParticipants: number;
  maxOutputHeight: 720 | 1080;
  watermarkAfterMinutes: number | null;
  captionsLevel: CaptionsLevel;
  chromaKey: boolean;
  setAndForget: boolean;
  magicSceneLevel: MagicSceneLevel;
  multiDestination: boolean;
  ndiSrt: boolean;
};

export type StoredLicenseRecord = {
  licenseKey: string;
  deviceId: string;
  email?: string;
  updatedAt: string;
};