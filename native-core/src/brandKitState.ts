import type { MediaCoreBrandKit } from "./protocol.js";

export const DEFAULT_BRAND_KIT: MediaCoreBrandKit = {
  name: "CoreVideo Pro House",
  logoText: "CoreVideo Pro",
  brandColor: "#44c1a1",
  accentColor: "#f0a85c",
  backgroundColor: "#0c1118",
  fontFamily: "Inter",
  lowerThirdStyle: "gradient",
  appliedOverlayCount: 0,
  summary: "Brand kit idle.",
  warnings: []
};

export class BrandKitStateModel {
  private brandKit: MediaCoreBrandKit = { ...DEFAULT_BRAND_KIT };

  apply(input: Omit<MediaCoreBrandKit, "appliedOverlayCount" | "summary" | "warnings">, overlayCount = 0) {
    this.brandKit = {
      name: input.name.trim() || DEFAULT_BRAND_KIT.name,
      logoText: input.logoText.trim() || DEFAULT_BRAND_KIT.logoText,
      brandColor: normalizeHex(input.brandColor, DEFAULT_BRAND_KIT.brandColor),
      accentColor: normalizeHex(input.accentColor, DEFAULT_BRAND_KIT.accentColor),
      backgroundColor: normalizeHex(input.backgroundColor, DEFAULT_BRAND_KIT.backgroundColor),
      fontFamily: input.fontFamily,
      lowerThirdStyle: input.lowerThirdStyle,
      appliedOverlayCount: overlayCount,
      summary: buildSummary(input.name, overlayCount),
      warnings: buildWarnings(input)
    };
    return this.snapshot();
  }

  snapshot(): MediaCoreBrandKit {
    return { ...this.brandKit };
  }
}

function normalizeHex(value: string, fallback: string) {
  return /^#(?:[0-9a-fA-F]{3}|[0-9a-fA-F]{6})$/.test(value.trim()) ? value.trim().toLowerCase() : fallback;
}

function buildSummary(name: string, overlayCount: number) {
  const trimmed = name.trim() || DEFAULT_BRAND_KIT.name;
  return overlayCount > 0 ? `${trimmed} applied to ${overlayCount} overlays` : `${trimmed} ready`;
}

function buildWarnings(input: Omit<MediaCoreBrandKit, "appliedOverlayCount" | "summary" | "warnings">) {
  const warnings: string[] = [];
  if (!input.logoText.trim()) {
    warnings.push("Brand logo text was empty; using default bug label.");
  }
  if (!/^#(?:[0-9a-fA-F]{3}|[0-9a-fA-F]{6})$/.test(input.brandColor.trim())) {
    warnings.push("Brand color was invalid; using house default.");
  }
  return warnings;
}