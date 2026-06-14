import { describe, expect, it } from "vitest";
import {
  advanceStinger,
  describeStinger,
  getStingerPreset,
  isStingerComplete,
  startStinger,
  stingerPhaseAt,
  stingerPhaseProgress,
  stingerPresets,
  stingerSceneSwitchMs,
  stingerTotalDurationMs,
} from "./stingerEngine";

const brandWipe = getStingerPreset("brand-wipe")!;
const logoReveal = getStingerPreset("logo-reveal")!;
const smoothDissolve = getStingerPreset("smooth-dissolve")!;

describe("stingerPresets", () => {
  it("returns at least 4 built-in stingers", () => {
    expect(stingerPresets().length).toBeGreaterThanOrEqual(4);
  });

  it("returns undefined for unknown id", () => {
    expect(getStingerPreset("nonexistent")).toBeUndefined();
  });

  it("brand-wipe switches scene on hold start", () => {
    expect(brandWipe.sceneSwitchPoint).toBe("on-hold-start");
  });
});

describe("stingerTotalDurationMs", () => {
  it("sums in + hold + out durations", () => {
    expect(stingerTotalDurationMs(brandWipe)).toBe(300 + 200 + 300); // 800ms
  });

  it("smooth-dissolve has no hold phase", () => {
    expect(smoothDissolve.holdDurationMs).toBe(0);
    expect(stingerTotalDurationMs(smoothDissolve)).toBe(1000);
  });
});

describe("stingerSceneSwitchMs", () => {
  it("fires at inDuration for on-hold-start", () => {
    expect(stingerSceneSwitchMs(brandWipe)).toBe(300);
  });

  it("fires at inDuration + half-hold for on-hold-midpoint", () => {
    // logoReveal: in=400, hold=400 → switch at 400 + 200 = 600
    expect(stingerSceneSwitchMs(logoReveal)).toBe(600);
  });

  it("fires at inDuration + hold for on-out-start", () => {
    // smoothDissolve: in=500, hold=0 → switch at 500
    expect(stingerSceneSwitchMs(smoothDissolve)).toBe(500);
  });
});

describe("stingerPhaseAt", () => {
  it("returns idle at t=0 or negative", () => {
    expect(stingerPhaseAt(brandWipe, 0)).toBe("idle");
    expect(stingerPhaseAt(brandWipe, -1)).toBe("idle");
  });

  it("returns in during in-phase", () => {
    expect(stingerPhaseAt(brandWipe, 1)).toBe("in");
    expect(stingerPhaseAt(brandWipe, 299)).toBe("in");
  });

  it("returns hold during hold-phase", () => {
    expect(stingerPhaseAt(brandWipe, 300)).toBe("hold");
    expect(stingerPhaseAt(brandWipe, 499)).toBe("hold");
  });

  it("returns out during out-phase", () => {
    expect(stingerPhaseAt(brandWipe, 500)).toBe("out");
    expect(stingerPhaseAt(brandWipe, 799)).toBe("out");
  });

  it("returns complete at or past total duration", () => {
    expect(stingerPhaseAt(brandWipe, 800)).toBe("complete");
    expect(stingerPhaseAt(brandWipe, 9999)).toBe("complete");
  });
});

describe("startStinger", () => {
  it("starts in the in-phase", () => {
    const pb = startStinger(brandWipe);
    expect(pb.phase).toBe("in");
    expect(pb.elapsedMs).toBe(0);
  });

  it("computes sceneSwitchAtMs correctly", () => {
    const pb = startStinger(brandWipe);
    expect(pb.sceneSwitchAtMs).toBe(300);
  });

  it("sceneSwitched is false initially for non-zero switch point", () => {
    const pb = startStinger(brandWipe);
    expect(pb.sceneSwitched).toBe(false);
  });
});

describe("advanceStinger", () => {
  it("advances elapsedMs", () => {
    let pb = startStinger(brandWipe);
    pb = advanceStinger(pb, 150);
    expect(pb.elapsedMs).toBe(150);
    expect(pb.phase).toBe("in");
  });

  it("transitions to hold phase when in-phase ends", () => {
    let pb = startStinger(brandWipe);
    pb = advanceStinger(pb, 300);
    expect(pb.phase).toBe("hold");
  });

  it("sets sceneSwitched when elapsed reaches sceneSwitchAtMs", () => {
    let pb = startStinger(brandWipe);
    expect(pb.sceneSwitched).toBe(false);
    pb = advanceStinger(pb, 300);
    expect(pb.sceneSwitched).toBe(true);
  });

  it("does not unset sceneSwitched once true", () => {
    let pb = startStinger(brandWipe);
    pb = advanceStinger(pb, 300);
    pb = advanceStinger(pb, 400);
    expect(pb.sceneSwitched).toBe(true);
  });

  it("clamps elapsed at total duration", () => {
    let pb = startStinger(brandWipe);
    pb = advanceStinger(pb, 5000);
    expect(pb.elapsedMs).toBe(800);
    expect(pb.phase).toBe("complete");
  });

  it("completes after advancing full duration in steps", () => {
    let pb = startStinger(brandWipe);
    pb = advanceStinger(pb, 400);
    pb = advanceStinger(pb, 400);
    expect(isStingerComplete(pb)).toBe(true);
  });
});

describe("stingerPhaseProgress", () => {
  it("returns 0 progress at start of in-phase", () => {
    const pb = startStinger(brandWipe);
    expect(stingerPhaseProgress(pb)).toBe(0);
  });

  it("returns 0.5 progress at midpoint of in-phase", () => {
    let pb = startStinger(brandWipe);
    pb = advanceStinger(pb, 150); // 150/300 = 0.5
    expect(stingerPhaseProgress(pb)).toBeCloseTo(0.5, 2);
  });

  it("returns 1 for complete stinger", () => {
    let pb = startStinger(brandWipe);
    pb = advanceStinger(pb, 5000);
    expect(stingerPhaseProgress(pb)).toBe(1);
  });
});

describe("describeStinger", () => {
  it("includes name, total duration and switch time", () => {
    const desc = describeStinger(brandWipe);
    expect(desc).toContain("Brand Wipe");
    expect(desc).toContain("800ms");
    expect(desc).toContain("300ms");
  });
});
