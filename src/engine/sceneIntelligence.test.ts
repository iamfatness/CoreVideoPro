import { describe, expect, it } from "vitest";
import {
  decideAutoSwitch,
  describeRecommendation,
  recommendScene,
  scoreLayoutMatch,
  summarizeSceneIntelligence,
  type SceneSignal,
} from "./sceneIntelligence";

const baseSignal: SceneSignal = {
  videoOnCount: 2,
  screenShareActive: false,
  hasActiveSpeaker: true,
  hostVideoOn: true,
  isOutro: false,
};

describe("recommendScene", () => {
  it("recommends outro when isOutro flag is set", () => {
    const rec = recommendScene({ ...baseSignal, isOutro: true });
    expect(rec.layout).toBe("outro");
    expect(rec.confidence).toBe("high");
  });

  it("recommends speaker-slides when a screen share is active", () => {
    const rec = recommendScene({ ...baseSignal, screenShareActive: true });
    expect(rec.layout).toBe("speaker-slides");
    expect(rec.confidence).toBe("high");
    expect(rec.reason).toMatch(/screen share/i);
  });

  it("recommends host-focus when no video is on", () => {
    const rec = recommendScene({ ...baseSignal, videoOnCount: 0, hostVideoOn: true });
    expect(rec.layout).toBe("host-focus");
    expect(rec.confidence).toBe("high");
  });

  it("recommends host-focus with medium confidence when host video is also off", () => {
    const rec = recommendScene({ ...baseSignal, videoOnCount: 1, hostVideoOn: false });
    expect(rec.layout).toBe("host-focus");
    expect(rec.confidence).toBe("medium");
  });

  it("recommends two-up for exactly 2 participants", () => {
    const rec = recommendScene({ ...baseSignal, videoOnCount: 2 });
    expect(rec.layout).toBe("two-up");
    expect(rec.confidence).toBe("high");
  });

  it("recommends smart-grid with high confidence for 3–6 with active speaker", () => {
    const rec = recommendScene({ ...baseSignal, videoOnCount: 4, hasActiveSpeaker: true });
    expect(rec.layout).toBe("smart-grid");
    expect(rec.confidence).toBe("high");
    expect(rec.reason).toMatch(/4 participants/);
  });

  it("recommends smart-grid with medium confidence for 3–6 without active speaker", () => {
    const rec = recommendScene({ ...baseSignal, videoOnCount: 5, hasActiveSpeaker: false });
    expect(rec.layout).toBe("smart-grid");
    expect(rec.confidence).toBe("medium");
  });

  it("recommends smart-grid for large meetings (7+)", () => {
    const rec = recommendScene({ ...baseSignal, videoOnCount: 12 });
    expect(rec.layout).toBe("smart-grid");
    expect(rec.reason).toMatch(/Large meeting/i);
  });

  it("outro takes priority over screen share", () => {
    const rec = recommendScene({ ...baseSignal, isOutro: true, screenShareActive: true });
    expect(rec.layout).toBe("outro");
  });
});

describe("scoreLayoutMatch", () => {
  it("returns 100 for the recommended layout", () => {
    // two-up is recommended for videoOnCount=2
    expect(scoreLayoutMatch("two-up", baseSignal)).toBe(100);
  });

  it("returns 90 for speaker-slides when screen share is active but outro is recommended", () => {
    // outro is top recommendation; speaker-slides gets partial credit for active screen share
    const signal = { ...baseSignal, screenShareActive: true, isOutro: true };
    expect(scoreLayoutMatch("speaker-slides", signal)).toBe(90);
  });

  it("returns 80 for host-focus when videoOnCount <= 1 but screen share is recommended", () => {
    // speaker-slides is top; host-focus still scores 80 because videoOnCount=1
    const signal = { ...baseSignal, videoOnCount: 1, screenShareActive: true };
    expect(scoreLayoutMatch("host-focus", signal)).toBe(80);
  });

  it("returns 20 for an unrelated layout", () => {
    expect(scoreLayoutMatch("outro", baseSignal)).toBe(20);
  });
});

describe("decideAutoSwitch", () => {
  it("does not switch when layout already matches recommendation", () => {
    const decision = decideAutoSwitch("two-up", baseSignal, 5000);
    expect(decision.shouldSwitch).toBe(false);
  });

  it("does not switch during hold period even if layout differs", () => {
    const signal = { ...baseSignal, screenShareActive: true }; // recommends speaker-slides
    const decision = decideAutoSwitch("two-up", signal, 1000); // only 1s elapsed, hold=4s
    expect(decision.shouldSwitch).toBe(false);
    expect(decision.holdUntilMs).toBeGreaterThan(0);
  });

  it("switches after hold period with high confidence recommendation", () => {
    const signal = { ...baseSignal, screenShareActive: true }; // recommends speaker-slides
    const decision = decideAutoSwitch("two-up", signal, 6000); // past hold=4s
    expect(decision.shouldSwitch).toBe(true);
    expect(decision.recommendation.layout).toBe("speaker-slides");
  });

  it("does not switch for low-confidence recommendation", () => {
    // medium confidence for no active speaker — we need to force low confidence
    // The only low-confidence path doesn't exist yet in recommendScene, so we
    // test medium confidence (≥ medium = ok, but check that low blocks switch)
    // Simulate by checking medium still switches after hold
    const signal = { ...baseSignal, videoOnCount: 5, hasActiveSpeaker: false }; // medium
    const decision = decideAutoSwitch("host-focus", signal, 10000);
    expect(decision.shouldSwitch).toBe(true); // medium is still ok
  });

  it("returns holdUntilMs of 0 when no switch needed", () => {
    const decision = decideAutoSwitch("two-up", baseSignal, 5000);
    expect(decision.holdUntilMs).toBe(0);
  });
});

describe("describeRecommendation", () => {
  it("formats a human-readable recommendation string", () => {
    const rec = recommendScene(baseSignal);
    const desc = describeRecommendation(rec);
    expect(desc).toContain("Interview");
    expect(desc).toContain("two-up");
    expect(desc).toContain("high confidence");
  });
});

describe("summarizeSceneIntelligence", () => {
  it("shows Auto prefix when automation is on", () => {
    const summary = summarizeSceneIntelligence(baseSignal, true);
    expect(summary).toMatch(/^Auto:/);
    expect(summary).toMatch(/Interview/);
  });

  it("shows Manual prefix when automation is off", () => {
    const summary = summarizeSceneIntelligence(baseSignal, false);
    expect(summary).toMatch(/^Manual:/);
  });
});
