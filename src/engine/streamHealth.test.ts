import { describe, expect, it } from "vitest";
import {
  scoreStreamHealth,
  gradeFromScore,
  summarizeStreamHealth,
  aggregateStreamHealth,
  formatBitrate,
  type StreamMetricSample,
} from "./streamHealth";

const goodSample: StreamMetricSample = {
  bitrateKbps: 6000,
  droppedFrames: 0,
  rttMs: 50,
  encoderLoadPct: 40,
  timestampMs: Date.now(),
};

describe("gradeFromScore", () => {
  it("maps score ranges to grades", () => {
    expect(gradeFromScore(100)).toBe("excellent");
    expect(gradeFromScore(85)).toBe("excellent");
    expect(gradeFromScore(84)).toBe("good");
    expect(gradeFromScore(65)).toBe("good");
    expect(gradeFromScore(64)).toBe("degraded");
    expect(gradeFromScore(40)).toBe("degraded");
    expect(gradeFromScore(39)).toBe("critical");
    expect(gradeFromScore(0)).toBe("critical");
  });
});

describe("scoreStreamHealth", () => {
  it("returns excellent for a healthy stream", () => {
    const report = scoreStreamHealth(goodSample, 6000);
    expect(report.score).toBe(100);
    expect(report.grade).toBe("excellent");
    expect(report.warnings).toHaveLength(0);
  });

  it("deducts points for low bitrate", () => {
    const sample = { ...goodSample, bitrateKbps: 3000 };
    const report = scoreStreamHealth(sample, 6000);
    expect(report.score).toBeLessThan(100);
    expect(report.warnings.some((w) => w.includes("itrate"))).toBe(true);
  });

  it("gives degraded grade and critical-low warning when bitrate is very low", () => {
    const sample = { ...goodSample, bitrateKbps: 500 };
    const report = scoreStreamHealth(sample, 6000);
    expect(report.grade).toBe("degraded");
    expect(report.warnings.some((w) => w.includes("critically"))).toBe(true);
  });

  it("deducts for dropped frames", () => {
    const sample = { ...goodSample, droppedFrames: 5 };
    const report = scoreStreamHealth(sample, 6000);
    expect(report.score).toBe(95);
    expect(report.warnings).toContain("Frame drops detected");
  });

  it("deducts more for significant dropped frames", () => {
    const sample = { ...goodSample, droppedFrames: 15 };
    const report = scoreStreamHealth(sample, 6000);
    expect(report.score).toBe(85);
    expect(report.warnings).toContain("Significant frame drops");
  });

  it("deducts for high RTT", () => {
    const sample = { ...goodSample, rttMs: 600 };
    const report = scoreStreamHealth(sample, 6000);
    expect(report.score).toBeLessThan(100);
    expect(report.warnings).toContain("High network latency");
  });

  it("deducts for encoder overload", () => {
    const sample = { ...goodSample, encoderLoadPct: 98 };
    const report = scoreStreamHealth(sample, 6000);
    expect(report.score).toBeLessThan(100);
    expect(report.warnings).toContain("Encoder overloaded");
  });

  it("clamps score to 0 for catastrophic streams", () => {
    const sample = { ...goodSample, bitrateKbps: 0, droppedFrames: 30, rttMs: 1000, encoderLoadPct: 100 };
    const report = scoreStreamHealth(sample, 6000);
    expect(report.score).toBe(0);
    expect(report.grade).toBe("critical");
  });
});

describe("summarizeStreamHealth", () => {
  it("returns grade and score when no warnings", () => {
    const report = scoreStreamHealth(goodSample, 6000);
    expect(summarizeStreamHealth(report)).toBe("Excellent — 100/100");
  });

  it("returns grade and first warning when issues exist", () => {
    const sample = { ...goodSample, droppedFrames: 5 };
    const report = scoreStreamHealth(sample, 6000);
    expect(summarizeStreamHealth(report)).toContain("Frame drops detected");
  });
});

describe("aggregateStreamHealth", () => {
  it("returns excellent for empty list", () => {
    const agg = aggregateStreamHealth([]);
    expect(agg.worstGrade).toBe("excellent");
    expect(agg.avgScore).toBe(100);
    expect(agg.allHealthy).toBe(true);
  });

  it("reports worst grade across destinations", () => {
    const r1 = scoreStreamHealth(goodSample, 6000); // excellent
    const r2 = scoreStreamHealth({ ...goodSample, bitrateKbps: 500 }, 6000); // degraded
    const agg = aggregateStreamHealth([r1, r2]);
    expect(agg.worstGrade).toBe("degraded");
    expect(agg.allHealthy).toBe(false);
  });

  it("computes average score", () => {
    const r1 = scoreStreamHealth(goodSample, 6000); // 100
    const r2 = scoreStreamHealth({ ...goodSample, droppedFrames: 10 }, 6000); // 90
    const agg = aggregateStreamHealth([r1, r2]);
    expect(agg.avgScore).toBe(95);
  });
});

describe("formatBitrate", () => {
  it("formats kbps below 1000", () => {
    expect(formatBitrate(500)).toBe("500 kbps");
  });

  it("formats Mbps for high bitrates", () => {
    expect(formatBitrate(6000)).toBe("6.0 Mbps");
    expect(formatBitrate(1500)).toBe("1.5 Mbps");
  });
});
