import { describe, expect, it } from "vitest";
import {
  clampTransitionDuration,
  describeTransition,
  recommendedTransitionDuration,
  transitionStyles,
  transitionUsesWipe
} from "./transitions";

describe("transitionStyles", () => {
  it("offers cut, fade, slide, wipe, and stinger", () => {
    expect(transitionStyles).toEqual(["cut", "fade", "slide", "wipe", "stinger"]);
  });
});

describe("recommendedTransitionDuration", () => {
  it("returns 0 for a cut and longer defaults for wipe-style transitions", () => {
    expect(recommendedTransitionDuration("cut")).toBe(0);
    expect(recommendedTransitionDuration("wipe")).toBe(600);
    expect(recommendedTransitionDuration("stinger")).toBe(900);
  });
});

describe("transitionUsesWipe", () => {
  it("flags wipe and stinger as overlay transitions", () => {
    expect(transitionUsesWipe("wipe")).toBe(true);
    expect(transitionUsesWipe("stinger")).toBe(true);
    expect(transitionUsesWipe("fade")).toBe(false);
    expect(transitionUsesWipe("cut")).toBe(false);
  });
});

describe("clampTransitionDuration", () => {
  it("clamps to each style's bounds", () => {
    expect(clampTransitionDuration("cut", 500)).toBe(0);
    expect(clampTransitionDuration("stinger", 50)).toBe(400);
    expect(clampTransitionDuration("stinger", 9000)).toBe(2000);
    expect(clampTransitionDuration("fade", 300)).toBe(300);
  });
});

describe("describeTransition", () => {
  it("summarizes a cut as instant and a stinger as a branded wipe", () => {
    expect(describeTransition("cut", 0).summary).toBe("Cut - instant");
    expect(describeTransition("stinger", 900)).toMatchObject({
      label: "Stinger",
      usesWipe: true,
      summary: "Stinger - 900ms branded wipe"
    });
    expect(describeTransition("fade", 420).summary).toBe("Fade - 420ms cross-dissolve");
  });
});
