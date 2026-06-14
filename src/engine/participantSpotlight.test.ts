import { describe, expect, it } from "vitest";
import {
  explainSpotlight,
  scoreCandidate,
  selectSpotlight,
  spotlightSummary,
  type SpotlightCandidate,
} from "./participantSpotlight";

const host: SpotlightCandidate = {
  id: "p-host",
  name: "Alice (Host)",
  isActiveSpeaker: false,
  hasVideo: true,
  roleWeight: 4, // Host
  secondsSinceLastSpoke: 10,
  totalSpeakingSeconds: 120,
};

const presenter: SpotlightCandidate = {
  id: "p-presenter",
  name: "Bob (Presenter)",
  isActiveSpeaker: true,
  hasVideo: true,
  roleWeight: 3, // Presenter
  secondsSinceLastSpoke: 0,
  totalSpeakingSeconds: 80,
};

const guest: SpotlightCandidate = {
  id: "p-guest",
  name: "Carol (Guest)",
  isActiveSpeaker: false,
  hasVideo: true,
  roleWeight: 1, // Guest
  secondsSinceLastSpoke: 90,
  totalSpeakingSeconds: 20,
};

const noVideo: SpotlightCandidate = {
  id: "p-novideo",
  name: "Dave",
  isActiveSpeaker: false,
  hasVideo: false,
  roleWeight: 2,
  secondsSinceLastSpoke: 5,
  totalSpeakingSeconds: 30,
};

describe("scoreCandidate", () => {
  it("returns score 0 for a participant without video", () => {
    const score = scoreCandidate(noVideo);
    expect(score.score).toBe(0);
    expect(score.reasons).toHaveLength(0);
  });

  it("includes active-speaker reason and 60 pts for the active speaker", () => {
    const score = scoreCandidate(presenter);
    expect(score.reasons).toContain("active-speaker");
    expect(score.score).toBeGreaterThanOrEqual(60);
  });

  it("includes role-priority for non-zero role weight", () => {
    const score = scoreCandidate(host);
    expect(score.reasons).toContain("role-priority");
  });

  it("includes recent-speaker for a participant who spoke within idle threshold", () => {
    const score = scoreCandidate(host, 60);
    expect(score.reasons).toContain("recent-speaker");
  });

  it("does not include recent-speaker for participant beyond idle threshold", () => {
    const score = scoreCandidate(guest, 60); // spoke 90s ago, threshold 60s
    expect(score.reasons).not.toContain("recent-speaker");
  });

  it("scores active speaker higher than non-speaking host", () => {
    const hostScore = scoreCandidate(host).score;
    const presenterScore = scoreCandidate(presenter).score;
    expect(presenterScore).toBeGreaterThan(hostScore);
  });
});

describe("selectSpotlight", () => {
  const candidates = [host, presenter, guest, noVideo];

  it("selects the active speaker as primary in auto mode", () => {
    const decision = selectSpotlight(candidates);
    expect(decision.primaryId).toBe("p-presenter");
    expect(decision.mode).toBe("auto");
  });

  it("picks a secondary candidate when pickSecondary is true", () => {
    const decision = selectSpotlight(candidates, { pickSecondary: true });
    expect(decision.secondaryId).not.toBeNull();
    expect(decision.secondaryId).not.toBe(decision.primaryId);
  });

  it("skips participants without video in secondary", () => {
    const decision = selectSpotlight(candidates, { pickSecondary: true });
    expect(decision.secondaryId).not.toBe("p-novideo");
  });

  it("omits secondary when pickSecondary is false", () => {
    const decision = selectSpotlight(candidates, { pickSecondary: false });
    expect(decision.secondaryId).toBeNull();
  });

  it("honours pinnedPrimaryId and switches to manual mode", () => {
    const decision = selectSpotlight(candidates, { pinnedPrimaryId: "p-host" });
    expect(decision.primaryId).toBe("p-host");
    expect(decision.mode).toBe("manual");
  });

  it("returns null primary when all candidates have no video", () => {
    const decision = selectSpotlight([noVideo]);
    expect(decision.primaryId).toBeNull();
  });

  it("includes all candidates in the scores array", () => {
    const decision = selectSpotlight(candidates);
    expect(decision.scores).toHaveLength(candidates.length);
  });

  it("returns scores sorted descending by score", () => {
    const decision = selectSpotlight(candidates);
    for (let i = 1; i < decision.scores.length; i++) {
      expect(decision.scores[i - 1].score).toBeGreaterThanOrEqual(decision.scores[i].score);
    }
  });
});

describe("spotlightSummary", () => {
  it("includes primary and secondary name in auto mode", () => {
    const decision = selectSpotlight([host, presenter, guest]);
    const summary = spotlightSummary(decision);
    expect(summary).toMatch(/Auto:/);
    expect(summary).toMatch(/Bob \(Presenter\)/);
  });

  it("shows Pinned label for manual mode", () => {
    const decision = selectSpotlight([host, presenter], { pinnedPrimaryId: "p-host" });
    expect(spotlightSummary(decision)).toMatch(/Pinned:/);
  });

  it("returns not-available message when no primary", () => {
    const decision = selectSpotlight([noVideo]);
    expect(spotlightSummary(decision)).toBe("No spotlight candidate available");
  });
});

describe("explainSpotlight", () => {
  it("returns not eligible for zero-score candidate", () => {
    const score = scoreCandidate(noVideo);
    expect(explainSpotlight(score)).toBe("Not eligible (no video)");
  });

  it("mentions speaking now for the active speaker", () => {
    const score = scoreCandidate(presenter);
    expect(explainSpotlight(score)).toContain("speaking now");
  });

  it("mentions role and recency for host who recently spoke", () => {
    const score = scoreCandidate(host, 60);
    const explanation = explainSpotlight(score);
    expect(explanation).toContain("priority role");
    expect(explanation).toContain("recently spoke");
  });
});
