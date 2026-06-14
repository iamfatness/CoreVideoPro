import type { CaptionFontSize, CaptionStyle } from "../domain/production";
import { normalizeHexColor } from "./brandKit";

const FONT_PIXELS: Record<CaptionFontSize, number> = {
  small: 14,
  medium: 18,
  large: 24
};

export function captionFontPx(size: CaptionFontSize): number {
  return FONT_PIXELS[size];
}

export function clampOpacity(value: number): number {
  if (Number.isNaN(value)) {
    return 0;
  }

  return Math.min(100, Math.max(0, Math.round(value)));
}

export function normalizeCaptionStyle(style: CaptionStyle): CaptionStyle {
  return Object.freeze({
    fontSize: style.fontSize,
    textColor: normalizeHexColor(style.textColor, "#f7fbf8"),
    backgroundOpacity: clampOpacity(style.backgroundOpacity),
    uppercase: style.uppercase
  });
}

export function formatCaptionText(text: string, style: CaptionStyle): string {
  return style.uppercase ? text.toUpperCase() : text;
}

/**
 * Resolve a caption style into the CSS custom properties consumed by the
 * caption strip, so the operator's style choices render directly in program.
 */
export function captionStyleVars(style: CaptionStyle): Record<string, string> {
  const normalized = normalizeCaptionStyle(style);
  return {
    "--caption-font-size": `${captionFontPx(normalized.fontSize)}px`,
    "--caption-text-color": normalized.textColor,
    "--caption-bg-opacity": String(normalized.backgroundOpacity / 100)
  };
}

export function summarizeCaptionStyle(style: CaptionStyle): string {
  const normalized = normalizeCaptionStyle(style);
  const caseLabel = normalized.uppercase ? "uppercase" : "sentence case";
  return `${normalized.fontSize} - ${normalized.backgroundOpacity}% backdrop - ${caseLabel}`;
}
