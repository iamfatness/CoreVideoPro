import { describe, expect, it } from "vitest";
import { initialParticipants } from "../domain/production";
import type { Participant } from "../domain/production";
import { computeAutoCrop, describeFraming, isLowQualityFeed } from "./autoCrop";

function participantWith(overrides: Partial<Participant>): Participant {
  return { ...initialParticipants[0], ...overrides };
}

describe("isLowQualityFeed", () => {
  it("flags weak feeds by health or confidence", () => {
    expect(isLowQualityFeed("low-resolution", 95)).toBe(true);
    expect(isLowQualityFeed("recovering", 95)).toBe(true);
    expect(isLowQualityFeed("video-off", 95)).toBe(true);
    expect(isLowQualityFeed("live", 65)).toBe(true);
    expect(isLowQualityFeed("live", 96)).toBe(false);
  });
});

describe("computeAutoCrop", () => {
  it("crops a confident, healthy face tight with eyeline headroom", () => {
    const framing = computeAutoCrop(participantWith({ cropConfidence: 96, health: "live" }));

    expect(framing.quality).toBe("good");
    expect(framing.zoom).toBeGreaterThan(1.2);
    expect(framing.framingLabel).toBe("tight");
    expect(framing.centerY).toBeLessThan(0.5);
    expect(framing.warning).toBeUndefined();
  });

  it("eases back to a soft frame when confidence is mediocre", () => {
    const framing = computeAutoCrop(participantWith({ cropConfidence: 84, health: "live" }));

    expect(framing.quality).toBe("soft");
    expect(framing.warning).toMatch(/soft/i);
  });

  it("holds a safe wide frame for low-quality feeds and warns", () => {
    const framing = computeAutoCrop(participantWith({ cropConfidence: 88, health: "low-resolution" }));

    expect(framing.quality).toBe("low");
    expect(framing.zoom).toBe(1);
    expect(framing.framingLabel).toBe("wide");
    expect(framing.warning).toMatch(/low-quality/i);
  });
});

describe("describeFraming", () => {
  it("summarizes the framing decision", () => {
    const framing = computeAutoCrop(participantWith({ cropConfidence: 96, health: "live" }));
    expect(describeFraming(framing)).toMatch(/tight frame - \d+% headroom - good feed/);
  });
});
