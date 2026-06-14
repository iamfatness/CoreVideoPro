import { describe, expect, it } from "vitest";
import { initialProduction } from "../domain/production";
import type { BrandKit } from "../domain/production";
import {
  applyBrandKitToGraphics,
  isValidHexColor,
  normalizeBrandKit,
  normalizeHexColor,
  summarizeBrandKit
} from "./brandKit";

const sampleBrandKit: BrandKit = {
  name: "Launch Show",
  logoText: "Acme Live",
  brandColor: "#1166FF",
  accentColor: "#FFAA22",
  backgroundColor: "#101418",
  fontFamily: "Poppins",
  lowerThirdStyle: "gradient"
};

describe("brand kit colors", () => {
  it("validates hex colors", () => {
    expect(isValidHexColor("#fff")).toBe(true);
    expect(isValidHexColor("#44C1A1")).toBe(true);
    expect(isValidHexColor("44c1a1")).toBe(false);
    expect(isValidHexColor("#xyzxyz")).toBe(false);
  });

  it("falls back when a color is invalid and lowercases valid ones", () => {
    expect(normalizeHexColor("not-a-color", "#000000")).toBe("#000000");
    expect(normalizeHexColor("#ABCDEF", "#000000")).toBe("#abcdef");
  });
});

describe("normalizeBrandKit", () => {
  it("trims names and supplies defaults for blank fields", () => {
    const normalized = normalizeBrandKit({
      ...sampleBrandKit,
      name: "  ",
      logoText: "   ",
      brandColor: "bogus"
    });

    expect(normalized.name).toBe("Untitled brand");
    expect(normalized.logoText).toBe("CoreVideo Pro");
    expect(normalized.brandColor).toBe("#44c1a1");
  });
});

describe("applyBrandKitToGraphics", () => {
  it("pushes brand color and logo onto the brand bug and accent onto the rest", () => {
    const updated = applyBrandKitToGraphics(initialProduction.graphics, sampleBrandKit);
    const bug = updated.find((graphic) => graphic.kind === "bug");
    const banner = updated.find((graphic) => graphic.kind === "banner");

    expect(bug?.accent).toBe("#1166ff");
    expect(bug?.text).toBe("Acme Live");
    expect(banner?.accent).toBe("#ffaa22");
  });

  it("does not mutate the source graphics", () => {
    const before = initialProduction.graphics.map((graphic) => graphic.accent);
    applyBrandKitToGraphics(initialProduction.graphics, sampleBrandKit);
    const after = initialProduction.graphics.map((graphic) => graphic.accent);

    expect(after).toEqual(before);
  });
});

describe("summarizeBrandKit", () => {
  it("describes the kit's font and lower-third style", () => {
    expect(summarizeBrandKit(sampleBrandKit)).toBe("Launch Show - Poppins, gradient lower-thirds");
  });
});
