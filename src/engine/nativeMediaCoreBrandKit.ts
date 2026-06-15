import type { BrandKit } from "../domain/production";
import type { NativeMediaCoreBrandKit } from "./nativeMediaCoreProtocol";

export const IDLE_NATIVE_BRAND_KIT: NativeMediaCoreBrandKit = {
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

export class NativeBrandKitSimulator {
  private brandKit: NativeMediaCoreBrandKit = { ...IDLE_NATIVE_BRAND_KIT };

  apply(brandKit: BrandKit, overlayCount = 0) {
    const normalized = normalizeBrandKitSnapshot(brandKit);
    this.brandKit = {
      ...normalized,
      appliedOverlayCount: overlayCount,
      summary: overlayCount > 0 ? `${normalized.name} applied to ${overlayCount} overlays` : `${normalized.name} ready`,
      warnings: []
    };
    return this.snapshot();
  }

  snapshot(): NativeMediaCoreBrandKit {
    return { ...this.brandKit };
  }
}

function normalizeBrandKitSnapshot(brandKit: BrandKit): Omit<NativeMediaCoreBrandKit, "appliedOverlayCount" | "summary" | "warnings"> {
  return {
    name: brandKit.name.trim() || IDLE_NATIVE_BRAND_KIT.name,
    logoText: brandKit.logoText.trim() || IDLE_NATIVE_BRAND_KIT.logoText,
    brandColor: normalizeHex(brandKit.brandColor, IDLE_NATIVE_BRAND_KIT.brandColor),
    accentColor: normalizeHex(brandKit.accentColor, IDLE_NATIVE_BRAND_KIT.accentColor),
    backgroundColor: normalizeHex(brandKit.backgroundColor, IDLE_NATIVE_BRAND_KIT.backgroundColor),
    fontFamily: brandKit.fontFamily,
    lowerThirdStyle: brandKit.lowerThirdStyle
  };
}

function normalizeHex(value: string, fallback: string) {
  return /^#(?:[0-9a-fA-F]{3}|[0-9a-fA-F]{6})$/.test(value.trim()) ? value.trim().toLowerCase() : fallback;
}