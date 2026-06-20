import { describe, expect, it } from "vitest";

import type { DirectorIntelligenceSignals, FeedHealthSummary, SpeakerTurnSummary } from "./directorSignals";
import { sanitizeDirectorProposal } from "./directorStrategy";
import { localDirectorProvider, selectLocalProposal } from "./localDirectorProvider";

const feedHealth = (liveCount: number, degradedCount = 0): FeedHealthSummary => ({
  liveCount,
  degradedCount,
  healthyCount: Math.max(0, liveCount - degradedCount),
  byHealth: { live: Math.max(0, liveCount - degradedCount), "low-resolution": 0, recovering: degradedCount, "video-off": 0 }
});

const speakerTurns = (overrides: Partial<SpeakerTurnSummary> = {}): SpeakerTurnSummary => ({
  activeSpeakerIds: [],
  crossTalk: false,
  recentTurns: [],
  ...overrides
});

const signals = (overrides: Partial<DirectorIntelligenceSignals> = {}): DirectorIntelligenceSignals => ({
  schemaVersion: 1,
  elapsedMs: 1000,
  meetingState: "in-meeting",
  speakerTurns: speakerTurns(),
  screenShare: { active: false },
  engagement: { liveRatio: 1, activeContributorCount: 0, meanAudioLevel: 0 },
  feedHealth: feedHealth(0),
  ...overrides
});

describe("selectLocalProposal", () => {
  it("leads with the shared surface on an active screen share", () => {
    const proposal = selectLocalProposal(
      signals({ screenShare: { active: true, sharerId: "p1", sharerName: "Ada" }, feedHealth: feedHealth(3) })
    );
    expect(proposal.ruleId).toBe("screen-share-priority");
    expect(proposal.recommendedSceneId).toBe("speaker-slides");
  });

  it("uses host-focus intro for a single live feed", () => {
    const proposal = selectLocalProposal(
      signals({ feedHealth: feedHealth(1), engagement: { liveRatio: 1, activeContributorCount: 1, meanAudioLevel: 0.3 } })
    );
    expect(proposal.recommendedSceneId).toBe("intro");
    expect(proposal.ruleId).toBe("single-speaker");
  });

  it("keeps two live contributors two-up", () => {
    const proposal = selectLocalProposal(
      signals({
        feedHealth: feedHealth(2),
        engagement: { liveRatio: 1, activeContributorCount: 2, meanAudioLevel: 0.3 },
        speakerTurns: speakerTurns({ activeSpeakerIds: ["a"], dominantSpeakerId: "a" })
      })
    );
    expect(proposal.recommendedSceneId).toBe("interview");
    expect(proposal.ruleId).toBe("focused-interview");
  });

  it("keeps a contested multi-speaker room as a panel", () => {
    const proposal = selectLocalProposal(
      signals({
        feedHealth: feedHealth(4),
        engagement: { liveRatio: 1, activeContributorCount: 3, meanAudioLevel: 0.5 },
        speakerTurns: speakerTurns({ activeSpeakerIds: ["a", "b"], crossTalk: true })
      })
    );
    expect(proposal.recommendedSceneId).toBe("panel");
    expect(proposal.ruleId).toBe("panel-discussion");
  });

  it("focuses a single dominant speaker carrying a large room (the signal-driven divergence)", () => {
    const proposal = selectLocalProposal(
      signals({
        feedHealth: feedHealth(5),
        engagement: { liveRatio: 1, activeContributorCount: 1, meanAudioLevel: 0.4 },
        speakerTurns: speakerTurns({ activeSpeakerIds: ["a"], dominantSpeakerId: "a" })
      })
    );
    expect(proposal.recommendedSceneId).toBe("intro");
    expect(proposal.ruleId).toBe("single-speaker");
  });

  it("lowers confidence when feeds are degraded", () => {
    const healthy = selectLocalProposal(
      signals({ feedHealth: feedHealth(4, 0), engagement: { liveRatio: 1, activeContributorCount: 3, meanAudioLevel: 0.5 } })
    );
    const degraded = selectLocalProposal(
      signals({ feedHealth: feedHealth(4, 3), engagement: { liveRatio: 1, activeContributorCount: 3, meanAudioLevel: 0.5 } })
    );
    expect(degraded.confidence).toBeLessThan(healthy.confidence);
  });

  it("always produces a sanitizable, 0-100 proposal", () => {
    const proposal = selectLocalProposal(
      signals({ feedHealth: feedHealth(3, 5), engagement: { liveRatio: 1, activeContributorCount: 3, meanAudioLevel: 0.9 } })
    );
    expect(sanitizeDirectorProposal(proposal)).toBeDefined();
    expect(proposal.confidence).toBeGreaterThanOrEqual(0);
    expect(proposal.confidence).toBeLessThanOrEqual(100);
  });
});

describe("localDirectorProvider", () => {
  it("is an async on-device provider that resolves a valid proposal", async () => {
    expect(localDirectorProvider.id).toBe("local-director-v1");
    const proposal = await localDirectorProvider.propose(
      signals({ feedHealth: feedHealth(2), engagement: { liveRatio: 1, activeContributorCount: 2, meanAudioLevel: 0.3 } })
    );
    expect(sanitizeDirectorProposal(proposal)).toBeDefined();
    expect(proposal.recommendedSceneId).toBe("interview");
  });
});
