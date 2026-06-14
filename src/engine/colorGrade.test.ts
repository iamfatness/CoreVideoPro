import { describe, expect, it } from "vitest";
import type { ColorGrade } from "../domain/production";
import { colorGradeFilter, isNeutralGrade, resolveColorGrade, summarizeColorGrade } from "./colorGrade";

function grade(overrides: Partial<ColorGrade> = {}): ColorGrade {
  return { lut: "none", exposure: 0, contrast: 0, saturation: 0, temperature: 0, ...overrides };
}

describe("resolveColorGrade", () => {
  it("combines LUT preset with manual offsets and clamps to +/-50", () => {
    const resolved = resolveColorGrade(grade({ lut: "warm-film", contrast: 60, temperature: 10 }));
    expect(resolved.temperature).toBe(34);
    expect(resolved.contrast).toBe(50);
  });
});

describe("colorGradeFilter", () => {
  it("is an identity-ish filter for a neutral grade", () => {
    expect(colorGradeFilter(grade())).toBe(
      "brightness(1.00) contrast(1.00) saturate(1.00) sepia(0.00) hue-rotate(0deg)"
    );
  });

  it("warms via sepia and cools via hue rotation", () => {
    expect(colorGradeFilter(grade({ lut: "warm-film" }))).toContain("sepia(0.12)");
    expect(colorGradeFilter(grade({ lut: "cool-broadcast" }))).toContain("hue-rotate(4deg)");
  });
});

describe("isNeutralGrade", () => {
  it("detects a passthrough grade", () => {
    expect(isNeutralGrade(grade())).toBe(true);
    expect(isNeutralGrade(grade({ lut: "punch" }))).toBe(false);
  });
});

describe("summarizeColorGrade", () => {
  it("describes passthrough and active grades", () => {
    expect(summarizeColorGrade(grade())).toBe("No grade - source passthrough");
    expect(summarizeColorGrade(grade({ lut: "punch" }))).toMatch(/^punch - exp \+6 \/ con \+22 \/ sat \+22 \/ temp \+4$/);
  });
});
