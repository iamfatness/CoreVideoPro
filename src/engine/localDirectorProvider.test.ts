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
  floor: [],
  ...overrides
});

describe("selectLocalProposal", () => {
  it("uses scored distinct guest identities instead of padding a two-up with the host", () => {
    const proposal = selectLocalProposal(signals({ elapsedMs: 10_000, floor: [
      { id: "guest", sourceId: "zoom:guest", isHost: false, hasVideo: true, talkingNow: true, lastSpokeAtMs: 10_000, score: 5 },
      { id: "host", sourceId: "zoom:host", isHost: true, hasVideo: true, talkingNow: true, lastSpokeAtMs: 10_000, score: 9 }
    ] }));
    expect(proposal.recommendedSceneId).toBe("intro");
    expect(proposal.slotBindings?.map((binding) => binding.personId)).toEqual(["guest"]);
  });

  it("collapses duplicate source rows for one person before assigning seats", () => {
    const proposal = selectLocalProposal(signals({ elapsedMs: 10_000, floor: [
      { id: "a", sourceId: "capture:a", isHost: false, hasVideo: true, talkingNow: true, lastSpokeAtMs: 10_000, score: 5 },
      { id: "a", sourceId: "zoom:a", isHost: false, hasVideo: true, talkingNow: true, lastSpokeAtMs: 10_000, score: 5 }
    ] }));
    expect(proposal.recommendedSceneId).toBe("intro");
    expect(proposal.slotBindings).toEqual([{ slotIndex: 0, personId: "a", sourceId: "zoom:a" }]);
  });

  it("binds a two-up only when the second guest spoke recently", () => {
    const floor = [
      { id: "a", sourceId: "zoom:a", isHost: false, hasVideo: true, talkingNow: true, lastSpokeAtMs: 20_000, score: 5 },
      { id: "b", sourceId: "zoom:b", isHost: false, hasVideo: true, talkingNow: false, lastSpokeAtMs: 12_000, score: 1 }
    ];
    expect(selectLocalProposal(signals({ elapsedMs: 20_000, floor })).slotBindings?.map((binding) => binding.personId))
      .toEqual(["a", "b"]);
    expect(selectLocalProposal(signals({ elapsedMs: 25_000, floor })).recommendedSceneId).toBe("intro");
  });
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
        speakerTurns: speakerTurns({ activeSpeakerIds: ["a"], dominantSpeakerId: "a" }),
        floor: [
          { id: "a", sourceId: "zoom:a", isHost: false, hasVideo: true, talkingNow: true, lastSpokeAtMs: 1000, score: 5 },
          { id: "b", sourceId: "zoom:b", isHost: false, hasVideo: true, talkingNow: false, lastSpokeAtMs: 500, score: 2 }
        ]
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
        speakerTurns: speakerTurns({ activeSpeakerIds: ["a", "b"], crossTalk: true }),
        floor: ["a", "b", "c", "d"].map((id, index) => ({
          id, sourceId: `zoom:${id}`, isHost: false, hasVideo: true,
          talkingNow: index < 2, lastSpokeAtMs: 1000, score: 4 - index
        }))
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
    const floor = ["a", "b", "c"].map((id, index) => ({
      id, sourceId: `zoom:${id}`, isHost: false, hasVideo: true,
      talkingNow: index === 0, lastSpokeAtMs: 1000, score: 4 - index
    }));
    const healthy = selectLocalProposal(
      signals({ floor, feedHealth: feedHealth(4, 0), engagement: { liveRatio: 1, activeContributorCount: 3, meanAudioLevel: 0.5 } })
    );
    const degraded = selectLocalProposal(
      signals({ floor, feedHealth: feedHealth(4, 3), engagement: { liveRatio: 1, activeContributorCount: 3, meanAudioLevel: 0.5 } })
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
      signals({ feedHealth: feedHealth(2), engagement: { liveRatio: 1, activeContributorCount: 2, meanAudioLevel: 0.3 }, floor: [
        { id: "a", sourceId: "zoom:a", isHost: false, hasVideo: true, talkingNow: true, lastSpokeAtMs: 1000, score: 5 },
        { id: "b", sourceId: "zoom:b", isHost: false, hasVideo: true, talkingNow: false, lastSpokeAtMs: 500, score: 2 }
      ] })
    );
    expect(sanitizeDirectorProposal(proposal)).toBeDefined();
    expect(proposal.recommendedSceneId).toBe("interview");
  });
});
