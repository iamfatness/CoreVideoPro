import { describe, expect, it } from "vitest";
import {
  evaluateLoudness,
  formatDbtp,
  formatLufs,
  getLoudnessTarget,
  loudnessTargets,
  planLoudnessNormalisation,
  type LoudnessReading,
} from "./audioLoudness";

const goodReading: LoudnessReading = {
  integratedLufs: -14.2,
  shortTermLufs: -13.8,
  truePeakDbtp: -4.1,
  loudnessRangeLu: 8,
};

describe("loudnessTargets", () => {
  it("includes at least 4 targets", () => {
    expect(loudnessTargets.length).toBeGreaterThanOrEqual(4);
  });

  it("has Zoom and EBU R128 targets", () => {
    expect(getLoudnessTarget("zoom")).toBeDefined();
    expect(getLoudnessTarget("ebu-r128")).toBeDefined();
  });

  it("returns undefined for unknown id", () => {
    expect(getLoudnessTarget("unknown")).toBeUndefined();
  });
});

describe("evaluateLoudness", () => {
  const zoomTarget = getLoudnessTarget("zoom")!;

  it("returns ok when within ±1 LU of target", () => {
    const status = evaluateLoudness(goodReading, zoomTarget);
    expect(status.level).toBe("ok");
    expect(status.warnings).toHaveLength(0);
    expect(status.summary).toContain("on target");
  });

  it("returns too-loud when significantly above target", () => {
    const reading = { ...goodReading, integratedLufs: -10 };
    const status = evaluateLoudness(reading, zoomTarget);
    expect(status.level).toBe("too-loud");
    expect(status.deviationLu).toBeCloseTo(4, 1);
    expect(status.gainAdjustmentDb).toBeCloseTo(-4, 1);
    expect(status.warnings.some((w) => w.includes("above target"))).toBe(true);
  });

  it("returns too-quiet when significantly below target", () => {
    const reading = { ...goodReading, integratedLufs: -20 };
    const status = evaluateLoudness(reading, zoomTarget);
    expect(status.level).toBe("too-quiet");
    expect(status.gainAdjustmentDb).toBeCloseTo(6, 1);
    expect(status.warnings.some((w) => w.includes("below target"))).toBe(true);
  });

  it("returns clipping when true peak exceeds limit", () => {
    const reading = { ...goodReading, truePeakDbtp: -0.5 };
    const status = evaluateLoudness(reading, zoomTarget);
    expect(status.level).toBe("clipping");
    expect(status.peakClipping).toBe(true);
    expect(status.warnings.some((w) => w.includes("limiter"))).toBe(true);
  });

  it("warns on wide loudness range", () => {
    const reading = { ...goodReading, loudnessRangeLu: 25 };
    const status = evaluateLoudness(reading, zoomTarget);
    expect(status.warnings.some((w) => w.includes("Loudness range"))).toBe(true);
  });

  it("deviation is positive when too loud, negative when too quiet", () => {
    const tooLoud = evaluateLoudness({ ...goodReading, integratedLufs: -12 }, zoomTarget);
    const tooQuiet = evaluateLoudness({ ...goodReading, integratedLufs: -18 }, zoomTarget);
    expect(tooLoud.deviationLu).toBeGreaterThan(0);
    expect(tooQuiet.deviationLu).toBeLessThan(0);
  });

  it("uses EBU R128 target correctly (target -23 LUFS)", () => {
    const ebuTarget = getLoudnessTarget("ebu-r128")!;
    const reading = { ...goodReading, integratedLufs: -23.2 };
    const status = evaluateLoudness(reading, ebuTarget);
    expect(status.level).toBe("ok");
  });
});

describe("planLoudnessNormalisation", () => {
  it("returns a plan with ratio 1 for narrow dynamic range", () => {
    const plan = planLoudnessNormalisation({ ...goodReading, loudnessRangeLu: 4 }, "zoom");
    expect(plan.suggestedRatio).toBe(1);
    expect(plan.limiterCeilingDbtp).toBe(-3);
    expect(plan.target.id).toBe("zoom");
  });

  it("suggests a 2:1 ratio for moderate dynamic range", () => {
    const plan = planLoudnessNormalisation({ ...goodReading, loudnessRangeLu: 10 }, "zoom");
    expect(plan.suggestedRatio).toBe(2);
  });

  it("suggests a 4:1 ratio for very wide dynamic range", () => {
    const plan = planLoudnessNormalisation({ ...goodReading, loudnessRangeLu: 25 }, "zoom");
    expect(plan.suggestedRatio).toBe(4);
  });

  it("falls back to Zoom target when id is unknown", () => {
    const plan = planLoudnessNormalisation(goodReading, "nonexistent");
    expect(plan.target.id).toBe("zoom");
  });
});

describe("format helpers", () => {
  it("formats LUFS", () => {
    expect(formatLufs(-14.0)).toBe("-14.0 LUFS");
    expect(formatLufs(-23.5)).toBe("-23.5 LUFS");
  });

  it("formats dBTP with sign", () => {
    expect(formatDbtp(-1.0)).toBe("-1.0 dBTP");
    expect(formatDbtp(0.5)).toBe("+0.5 dBTP");
  });
});
