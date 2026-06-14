import { describe, expect, it } from "vitest";
import {
  advanceAnimation,
  animationPresets,
  animationTotalDurationMs,
  applyEasing,
  computeAnimationFrame,
  dismissAnimation,
  getAnimationPreset,
  interpolateTransform,
  startAnimation,
} from "./graphicAnimator";

const SLIDE_UP = animationPresets.find((p) => p.id === "slide-up")!;
const FADE = animationPresets.find((p) => p.id === "fade")!;

describe("animationPresets", () => {
  it("includes at least 5 presets", () => {
    expect(animationPresets.length).toBeGreaterThanOrEqual(5);
  });

  it("each preset has required fields", () => {
    for (const p of animationPresets) {
      expect(p.id).toBeTruthy();
      expect(p.introDurationMs).toBeGreaterThan(0);
      expect(p.holdDurationMs).toBeGreaterThan(0);
      expect(p.outroDurationMs).toBeGreaterThan(0);
    }
  });
});

describe("getAnimationPreset", () => {
  it("returns the preset by id", () => {
    expect(getAnimationPreset("slide-up")?.name).toBe("Slide Up");
  });

  it("returns undefined for unknown id", () => {
    expect(getAnimationPreset("nope")).toBeUndefined();
  });
});

describe("startAnimation", () => {
  it("starts in intro phase at 0ms", () => {
    const state = startAnimation(SLIDE_UP);
    expect(state.phase).toBe("intro");
    expect(state.phaseElapsedMs).toBe(0);
    expect(state.totalElapsedMs).toBe(0);
  });
});

describe("advanceAnimation", () => {
  it("stays in intro before introDurationMs elapsed", () => {
    let state = startAnimation(SLIDE_UP);
    state = advanceAnimation(state, SLIDE_UP.introDurationMs / 2);
    expect(state.phase).toBe("intro");
  });

  it("transitions to hold after intro completes", () => {
    let state = startAnimation(SLIDE_UP);
    state = advanceAnimation(state, SLIDE_UP.introDurationMs + 1);
    expect(state.phase).toBe("hold");
  });

  it("transitions to outro after hold completes", () => {
    let state = startAnimation(SLIDE_UP);
    state = advanceAnimation(state, SLIDE_UP.introDurationMs + SLIDE_UP.holdDurationMs + 1);
    expect(state.phase).toBe("outro");
  });

  it("transitions to done after outro completes", () => {
    let state = startAnimation(SLIDE_UP);
    const total = animationTotalDurationMs(SLIDE_UP) + 1;
    state = advanceAnimation(state, total);
    expect(state.phase).toBe("done");
  });

  it("does not advance past done", () => {
    let state = startAnimation(SLIDE_UP);
    state = advanceAnimation(state, animationTotalDurationMs(SLIDE_UP) + 1000);
    const phase = state.phase;
    state = advanceAnimation(state, 1000);
    expect(state.phase).toBe(phase);
  });

  it("accumulates totalElapsedMs", () => {
    let state = startAnimation(SLIDE_UP);
    state = advanceAnimation(state, 100);
    state = advanceAnimation(state, 200);
    expect(state.totalElapsedMs).toBe(300);
  });
});

describe("dismissAnimation", () => {
  it("switches intro to outro", () => {
    let state = startAnimation(SLIDE_UP);
    state = advanceAnimation(state, 100);
    state = dismissAnimation(state);
    expect(state.phase).toBe("outro");
    expect(state.phaseElapsedMs).toBe(0);
  });

  it("switches hold to outro", () => {
    let state = startAnimation(SLIDE_UP);
    state = advanceAnimation(state, SLIDE_UP.introDurationMs + 100);
    state = dismissAnimation(state);
    expect(state.phase).toBe("outro");
  });

  it("does not change outro phase", () => {
    let state = startAnimation(SLIDE_UP);
    state = advanceAnimation(state, SLIDE_UP.introDurationMs + SLIDE_UP.holdDurationMs + 1);
    expect(state.phase).toBe("outro");
    state = dismissAnimation(state);
    expect(state.phase).toBe("outro");
  });
});

describe("computeAnimationFrame — intro", () => {
  it("opacity is 0 at start of intro", () => {
    const state = startAnimation(SLIDE_UP);
    const frame = computeAnimationFrame(state);
    expect(frame.opacity).toBeCloseTo(0, 2);
  });

  it("opacity is 1 at end of intro", () => {
    let state = startAnimation(SLIDE_UP);
    state = { ...state, phaseElapsedMs: SLIDE_UP.introDurationMs };
    const frame = computeAnimationFrame(state);
    expect(frame.opacity).toBeCloseTo(1, 2);
  });

  it("phaseProgress is between 0 and 1 during intro", () => {
    let state = startAnimation(SLIDE_UP);
    state = advanceAnimation(state, SLIDE_UP.introDurationMs / 2);
    const frame = computeAnimationFrame(state);
    expect(frame.phaseProgress).toBeGreaterThan(0);
    expect(frame.phaseProgress).toBeLessThan(1);
  });
});

describe("computeAnimationFrame — hold", () => {
  it("opacity is 1 during hold", () => {
    let state = startAnimation(SLIDE_UP);
    state = advanceAnimation(state, SLIDE_UP.introDurationMs + 100);
    const frame = computeAnimationFrame(state);
    expect(frame.opacity).toBe(1);
    expect(frame.cssTransform).toBe("none");
  });
});

describe("computeAnimationFrame — done", () => {
  it("done flag is true and opacity is 0", () => {
    let state = startAnimation(SLIDE_UP);
    state = advanceAnimation(state, animationTotalDurationMs(SLIDE_UP) + 1);
    const frame = computeAnimationFrame(state);
    expect(frame.done).toBe(true);
    expect(frame.opacity).toBe(0);
  });
});

describe("applyEasing", () => {
  it("linear returns input unchanged", () => {
    expect(applyEasing(0.5, "linear")).toBe(0.5);
    expect(applyEasing(0, "linear")).toBe(0);
    expect(applyEasing(1, "linear")).toBe(1);
  });

  it("ease-out starts fast and decelerates", () => {
    // ease-out should be above linear at t=0.5
    expect(applyEasing(0.5, "ease-out")).toBeGreaterThan(0.5);
  });

  it("ease-in starts slow and accelerates", () => {
    expect(applyEasing(0.5, "ease-in")).toBeLessThan(0.5);
  });

  it("ease-in-out is symmetric", () => {
    const a = applyEasing(0.3, "ease-in-out");
    const b = 1 - applyEasing(0.7, "ease-in-out");
    expect(a).toBeCloseTo(b, 5);
  });

  it("bounce-out ends at 1", () => {
    expect(applyEasing(1, "bounce-out")).toBeCloseTo(1, 3);
  });

  it("spring ends near 1", () => {
    expect(applyEasing(1, "spring")).toBeCloseTo(1, 2);
  });

  it("clamps below 0", () => {
    expect(applyEasing(-0.5, "linear")).toBe(0);
  });

  it("clamps above 1", () => {
    expect(applyEasing(1.5, "linear")).toBe(1);
  });
});

describe("interpolateTransform", () => {
  it("returns 'none' at t=1", () => {
    expect(interpolateTransform("translateY(60px)", "none", 1)).toBe("none");
  });

  it("returns from transform at t=0", () => {
    expect(interpolateTransform("translateY(60px)", "none", 0)).toBe("translateY(60px)");
  });

  it("returns partial translateY at t=0.5", () => {
    const result = interpolateTransform("translateY(60px)", "none", 0.5);
    expect(result).toContain("translateY(30.00px)");
  });

  it("handles translateX offscreen", () => {
    const result = interpolateTransform("translateX(-100px)", "none", 0.5);
    expect(result).toContain("translateX(-50.00px)");
  });
});

describe("animationTotalDurationMs", () => {
  it("sums intro + hold + outro", () => {
    expect(animationTotalDurationMs(SLIDE_UP)).toBe(
      SLIDE_UP.introDurationMs + SLIDE_UP.holdDurationMs + SLIDE_UP.outroDurationMs
    );
  });
});
