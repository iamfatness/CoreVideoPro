import type { BrandKit, GraphicOverlay } from "../domain/production";

const HEX_COLOR = /^#(?:[0-9a-fA-F]{3}|[0-9a-fA-F]{6})$/;
const IMAGE_URL = /^https?:\/\/\S+$/i;

export function isValidHexColor(value: string): boolean {
  return HEX_COLOR.test(value.trim());
}

export function isValidImageUrl(value: string): boolean {
  return IMAGE_URL.test(value.trim());
}

export function normalizeHexColor(value: string, fallback: string): string {
  const trimmed = value.trim();
  if (!isValidHexColor(trimmed)) {
    return fallback;
  }

  return trimmed.toLowerCase();
}

export function normalizeBrandKit(brandKit: BrandKit): BrandKit {
  const logoText = brandKit.logoText.trim() || "CoreVideo Pro";

  return Object.freeze({
    name: brandKit.name.trim() || "Untitled brand",
    logoText,
    brandColor: normalizeHexColor(brandKit.brandColor, "#44c1a1"),
    accentColor: normalizeHexColor(brandKit.accentColor, "#f0a85c"),
    backgroundColor: normalizeHexColor(brandKit.backgroundColor, "#0c1118"),
    backgroundImageUrl: brandKit.backgroundImageUrl.trim(),
    fontFamily: brandKit.fontFamily,
    lowerThirdStyle: brandKit.lowerThirdStyle
  });
}

/**
 * Resolve the program canvas background from the brand kit. A valid image URL
 * is layered under a legibility scrim so participant tiles and captions stay
 * readable; otherwise the canvas falls back to a branded gradient.
 */
export function programBackgroundStyle(brandKit: BrandKit): string {
  const normalized = normalizeBrandKit(brandKit);

  if (isValidImageUrl(normalized.backgroundImageUrl)) {
    return `linear-gradient(rgba(8, 12, 16, 0.55), rgba(8, 12, 16, 0.82)), url("${normalized.backgroundImageUrl}") center / cover no-repeat`;
  }

  return `linear-gradient(135deg, ${normalized.backgroundColor}, ${normalized.brandColor})`;
}

/**
 * Apply brand tokens to the graphic overlays. The brand bug carries the logo
 * text and primary brand color; banners and calls-to-action pick up the accent
 * color so the whole graphics package stays on-brand from a single source.
 */
export function applyBrandKitToGraphics(graphics: GraphicOverlay[], brandKit: BrandKit): GraphicOverlay[] {
  const normalized = normalizeBrandKit(brandKit);

  return graphics.map((graphic) => {
    if (graphic.kind === "bug") {
      return { ...graphic, accent: normalized.brandColor, text: normalized.logoText };
    }

    return { ...graphic, accent: normalized.accentColor };
  });
}

export function summarizeBrandKit(brandKit: BrandKit): string {
  const normalized = normalizeBrandKit(brandKit);
  return `${normalized.name} - ${normalized.fontFamily}, ${normalized.lowerThirdStyle} lower-thirds`;
}
