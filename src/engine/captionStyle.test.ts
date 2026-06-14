import { describe, expect, it } from "vitest";
import type { CaptionStyle } from "../domain/production";
import {
  captionFontPx,
  captionStyleVars,
  clampOpacity,
  formatCaptionText,
  normalizeCaptionStyle,
  summarizeCaptionStyle
} from "./captionStyle";

const baseStyle: CaptionStyle = {
  fontSize: "large",
  textColor: "#FFEE00",
  backgroundOpacity: 80,
  uppercase: true
};

describe("captionFontPx", () => {
  it("maps named sizes to pixels", () => {
    expect(captionFontPx("small")).toBe(14);
    expect(captionFontPx("medium")).toBe(18);
    expect(captionFontPx("large")).toBe(24);
  });
});

describe("clampOpacity", () => {
  it("clamps to the 0-100 range and rounds", () => {
    expect(clampOpacity(140)).toBe(100);
    expect(clampOpacity(-20)).toBe(0);
    expect(clampOpacity(63.4)).toBe(63);
    expect(clampOpacity(Number.NaN)).toBe(0);
  });
});

describe("normalizeCaptionStyle", () => {
  it("normalizes color and opacity while preserving choices", () => {
    const normalized = normalizeCaptionStyle({ ...baseStyle, textColor: "nope", backgroundOpacity: 200 });
    expect(normalized.textColor).toBe("#f7fbf8");
    expect(normalized.backgroundOpacity).toBe(100);
    expect(normalized.fontSize).toBe("large");
  });
});

describe("formatCaptionText", () => {
  it("uppercases only when requested", () => {
    expect(formatCaptionText("Live now", baseStyle)).toBe("LIVE NOW");
    expect(formatCaptionText("Live now", { ...baseStyle, uppercase: false })).toBe("Live now");
  });
});

describe("captionStyleVars", () => {
  it("produces CSS custom properties for the caption strip", () => {
    expect(captionStyleVars(baseStyle)).toEqual({
      "--caption-font-size": "24px",
      "--caption-text-color": "#ffee00",
      "--caption-bg-opacity": "0.8"
    });
  });
});

describe("summarizeCaptionStyle", () => {
  it("summarizes size, backdrop, and case", () => {
    expect(summarizeCaptionStyle(baseStyle)).toBe("large - 80% backdrop - uppercase");
  });
});
