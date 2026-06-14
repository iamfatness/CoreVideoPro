import { describe, expect, it } from "vitest";
import {
  captionQualityLabel,
  computeCaptionQuality,
  decideCaptionVisibility,
  detectDeadAir,
  summarizeCaptionQuality,
  type CaptionSignal,
} from "./captionQuality";

const liveSignal: CaptionSignal = {
  confidencePct: 94,
  msSinceLastWord: 200,
  text: "Welcome to the show.",
  activeSpeakerPresent: true,
  screenShareActive: false,
};

describe("detectDeadAir", () => {
  it("returns live within 1.5s", () => {
    expect(detectDeadAir(0)).toBe("live");
    expect(detectDeadAir(1400)).toBe("live");
  });

  it("returns brief-pause between 1.5s and 4s", () => {
    expect(detectDeadAir(1500)).toBe("brief-pause");
    expect(detectDeadAir(3999)).toBe("brief-pause");
  });

  it("returns dead-air between 4s and 10s", () => {
    expect(detectDeadAir(4000)).toBe("dead-air");
    expect(detectDeadAir(9999)).toBe("dead-air");
  });

  it("returns silence at 10s or more", () => {
    expect(detectDeadAir(10000)).toBe("silence");
    expect(detectDeadAir(60000)).toBe("silence");
  });
});

describe("decideCaptionVisibility", () => {
  it("returns visible for active live speech with high confidence", () => {
    const decision = decideCaptionVisibility(liveSignal);
    expect(decision.state).toBe("visible");
    expect(decision.shouldShow).toBe(true);
    expect(decision.fadeDelayMs).toBe(0);
  });

  it("returns hidden when silence is detected", () => {
    const decision = decideCaptionVisibility({
      ...liveSignal,
      msSinceLastWord: 15000,
      activeSpeakerPresent: false,
    });
    expect(decision.state).toBe("hidden");
    expect(decision.shouldShow).toBe(false);
  });

  it("returns hidden when no active speaker and dead air > 1.5s", () => {
    const decision = decideCaptionVisibility({
      ...liveSignal,
      msSinceLastWord: 2000,
      activeSpeakerPresent: false,
    });
    expect(decision.state).toBe("hidden");
    expect(decision.shouldShow).toBe(false);
  });

  it("returns pending when speaker is present but no text yet", () => {
    const decision = decideCaptionVisibility({
      ...liveSignal,
      text: "",
      msSinceLastWord: 2000,
      activeSpeakerPresent: true,
    });
    expect(decision.state).toBe("pending");
    expect(decision.shouldShow).toBe(false);
  });

  it("returns visible with reason when confidence is low but text is present", () => {
    const decision = decideCaptionVisibility({ ...liveSignal, confidencePct: 60 });
    expect(decision.state).toBe("visible");
    expect(decision.shouldShow).toBe(true);
    expect(decision.reason).toMatch(/60%/);
  });

  it("returns fading for brief-pause", () => {
    const decision = decideCaptionVisibility({
      ...liveSignal,
      msSinceLastWord: 2000,
    });
    expect(decision.state).toBe("fading");
    expect(decision.shouldShow).toBe(true);
    expect(decision.fadeDelayMs).toBe(1000);
  });

  it("returns fading with shorter delay for dead-air", () => {
    const decision = decideCaptionVisibility({
      ...liveSignal,
      msSinceLastWord: 5000,
    });
    expect(decision.state).toBe("fading");
    expect(decision.fadeDelayMs).toBe(500);
  });

  it("reason contains pause duration in seconds", () => {
    const decision = decideCaptionVisibility({
      ...liveSignal,
      msSinceLastWord: 2500,
    });
    expect(decision.reason).toMatch(/2\.5s/);
  });
});

describe("computeCaptionQuality", () => {
  it("returns poor tier with no samples", () => {
    const report = computeCaptionQuality([]);
    expect(report.tier).toBe("poor");
    expect(report.sampleCount).toBe(0);
    expect(report.warnings).toHaveLength(1);
  });

  it("returns excellent for average ≥ 92", () => {
    const report = computeCaptionQuality([95, 94, 93]);
    expect(report.tier).toBe("excellent");
    expect(report.avgConfidence).toBe(94);
    expect(report.warnings).toHaveLength(0);
  });

  it("returns good for average 80–91", () => {
    const report = computeCaptionQuality([85, 82, 84]);
    expect(report.tier).toBe("good");
    expect(report.warnings).toHaveLength(0);
  });

  it("returns degraded for average 65–79 and warns", () => {
    const report = computeCaptionQuality([75, 70, 68]);
    expect(report.tier).toBe("degraded");
    expect(report.warnings.some((w) => w.includes("degraded"))).toBe(true);
  });

  it("returns poor for average below 65 and warns", () => {
    const report = computeCaptionQuality([60, 55, 50]);
    expect(report.tier).toBe("poor");
    expect(report.warnings.some((w) => w.includes("poor"))).toBe(true);
  });

  it("adds an extra warning when min confidence drops below 60", () => {
    const report = computeCaptionQuality([90, 90, 55]);
    expect(report.minConfidence).toBe(55);
    expect(report.warnings.some((w) => w.includes("55%"))).toBe(true);
  });

  it("computes correct min confidence", () => {
    const report = computeCaptionQuality([95, 80, 72]);
    expect(report.minConfidence).toBe(72);
  });
});

describe("captionQualityLabel", () => {
  it("maps all tiers to labels", () => {
    expect(captionQualityLabel("excellent")).toBe("Excellent");
    expect(captionQualityLabel("good")).toBe("Good");
    expect(captionQualityLabel("degraded")).toBe("Degraded");
    expect(captionQualityLabel("poor")).toBe("Poor");
  });
});

describe("summarizeCaptionQuality", () => {
  it("returns a human-readable summary with sample count", () => {
    const report = computeCaptionQuality([94, 93, 95]);
    expect(summarizeCaptionQuality(report)).toMatch(/Excellent/);
    expect(summarizeCaptionQuality(report)).toMatch(/94%/);
    expect(summarizeCaptionQuality(report)).toMatch(/3 samples/);
  });

  it("returns no-data string for empty report", () => {
    expect(summarizeCaptionQuality(computeCaptionQuality([]))).toBe("No caption data");
  });
});
