import { describe, expect, it } from "vitest";
import {
  buildWatermarkSpec,
  classificationColor,
  confidentialityLevels,
  createWatermark,
  positionToCSS,
  updateWatermark,
  validateWatermark,
  watermarkPositions,
} from "./outputWatermark";

describe("createWatermark", () => {
  it("creates a disabled watermark by default", () => {
    const wm = createWatermark();
    expect(wm.mode).toBe("disabled");
  });

  it("applies overrides correctly", () => {
    const wm = createWatermark({ mode: "live", text: "DRAFT", opacityPct: 50 });
    expect(wm.mode).toBe("live");
    expect(wm.text).toBe("DRAFT");
    expect(wm.opacityPct).toBe(50);
  });

  it("uses sensible defaults", () => {
    const wm = createWatermark();
    expect(wm.position).toBe("bottom-right");
    expect(wm.opacityPct).toBe(30);
    expect(wm.color).toBe("#ffffff");
  });
});

describe("updateWatermark", () => {
  it("updates individual fields", () => {
    const wm = createWatermark({ mode: "live", text: "DRAFT" });
    const next = updateWatermark(wm, { text: "FINAL" });
    expect(next.text).toBe("FINAL");
    expect(next.mode).toBe("live"); // unchanged
  });

  it("clamps opacity to 0–100", () => {
    const wm = createWatermark();
    expect(updateWatermark(wm, { opacityPct: 150 }).opacityPct).toBe(100);
    expect(updateWatermark(wm, { opacityPct: -10 }).opacityPct).toBe(0);
  });

  it("clamps sizePx to 8–48", () => {
    const wm = createWatermark();
    expect(updateWatermark(wm, { sizePx: 1 }).sizePx).toBe(8);
    expect(updateWatermark(wm, { sizePx: 100 }).sizePx).toBe(48);
  });
});

describe("positionToCSS", () => {
  it("returns absolute position for top-left", () => {
    const css = positionToCSS("top-left");
    expect(css.position).toBe("absolute");
    expect(css.top).toBeTruthy();
    expect(css.left).toBeTruthy();
  });

  it("returns bottom-right positioning", () => {
    const css = positionToCSS("bottom-right");
    expect(css.bottom).toBeTruthy();
    expect(css.right).toBeTruthy();
  });

  it("returns transform for centered positions", () => {
    const css = positionToCSS("center");
    expect(css.transform).toContain("translate");
  });

  it("returns transform for top-center", () => {
    const css = positionToCSS("top-center");
    expect(css.transform).toContain("translateX");
  });

  it("covers all 7 positions without error", () => {
    for (const pos of watermarkPositions) {
      expect(() => positionToCSS(pos)).not.toThrow();
    }
  });
});

describe("buildWatermarkSpec", () => {
  it("returns active=false when mode is disabled", () => {
    const wm = createWatermark({ mode: "disabled", text: "Test" });
    const spec = buildWatermarkSpec(wm);
    expect(spec.active).toBe(false);
  });

  it("returns active=false when text is empty", () => {
    const wm = createWatermark({ mode: "live", text: "" });
    const spec = buildWatermarkSpec(wm);
    expect(spec.active).toBe(false);
  });

  it("returns active=true for live mode with text", () => {
    const wm = createWatermark({ mode: "live", text: "DRAFT" });
    const spec = buildWatermarkSpec(wm);
    expect(spec.active).toBe(true);
  });

  it("includes opacity in cssStyle when active", () => {
    const wm = createWatermark({ mode: "live", text: "X", opacityPct: 40 });
    const spec = buildWatermarkSpec(wm);
    expect(spec.cssStyle.opacity).toBe("0.4");
  });

  it("classificationBanner is null when showClassification is false", () => {
    const wm = createWatermark({ mode: "live", text: "DRAFT", showClassification: false });
    expect(buildWatermarkSpec(wm).classificationBanner).toBeNull();
  });

  it("classificationBanner contains classification label when enabled", () => {
    const wm = createWatermark({ mode: "live", text: "DRAFT", showClassification: true, classification: "confidential" });
    const spec = buildWatermarkSpec(wm);
    expect(spec.classificationBanner).toContain("CONFIDENTIAL");
  });

  it("summary says 'disabled' when not active", () => {
    const spec = buildWatermarkSpec(createWatermark());
    expect(spec.summary).toContain("disabled");
  });

  it("summary contains mode and text when active", () => {
    const wm = createWatermark({ mode: "burn-in", text: "DRAFT" });
    const spec = buildWatermarkSpec(wm);
    expect(spec.summary).toContain("Burn-in");
    expect(spec.summary).toContain("DRAFT");
  });
});

describe("validateWatermark", () => {
  it("returns error when mode is live but text is empty", () => {
    const wm = createWatermark({ mode: "live", text: "" });
    const issues = validateWatermark(wm);
    expect(issues.some((i) => i.includes("empty"))).toBe(true);
  });

  it("warns when opacity is high for live mode", () => {
    const wm = createWatermark({ mode: "live", text: "X", opacityPct: 80 });
    const issues = validateWatermark(wm);
    expect(issues.some((i) => i.includes("High opacity"))).toBe(true);
  });

  it("warns on burn-in + public classification", () => {
    const wm = createWatermark({ mode: "burn-in", text: "X", classification: "public" });
    const issues = validateWatermark(wm);
    expect(issues.some((i) => i.includes("public"))).toBe(true);
  });

  it("returns no issues for valid live watermark at 30% opacity", () => {
    const wm = createWatermark({ mode: "live", text: "DRAFT", opacityPct: 30, classification: "internal" });
    expect(validateWatermark(wm)).toHaveLength(0);
  });

  it("disabled mode never produces issues", () => {
    const wm = createWatermark({ mode: "disabled", text: "" });
    expect(validateWatermark(wm)).toHaveLength(0);
  });
});

describe("classificationColor", () => {
  it("returns a non-empty hex color for each level", () => {
    for (const level of confidentialityLevels) {
      const color = classificationColor(level);
      expect(color).toMatch(/^#[0-9a-f]{6}$/i);
    }
  });

  it("restricted is red-ish (high R component)", () => {
    const color = classificationColor("restricted");
    // #ef4444 — red
    expect(parseInt(color.slice(1, 3), 16)).toBeGreaterThan(200);
  });

  it("public is green-ish", () => {
    const color = classificationColor("public");
    // #22c55e — green dominant
    expect(parseInt(color.slice(3, 5), 16)).toBeGreaterThan(150);
  });
});
