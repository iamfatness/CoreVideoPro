import { afterEach, describe, expect, it, vi } from "vitest";
import { initialProduction, type Participant, type ProductionState } from "../domain/production";
import { mapCaptureSnapshot } from "./captureSnapshotMapper";
import type { ZoomSessionSnapshot } from "./contracts";
import {
  HeuristicDirectorStrategy,
  StaticDirectorStrategy,
  heuristicDirectorStrategy,
  proposeWithFallback,
  proposeWithProviderAsync,
  sanitizeDirectorProposal,
  selectBaseProposal,
  selectHeuristicProposal,
  strategyFromResolution,
  type AiDirectorProvider,
  type DirectorIntelligenceSignals,
  type DirectorProposal,
  type DirectorSignals
} from "./directorStrategy";

function snapshot(options: {
  screenShare?: boolean;
  count?: number;
  activeSpeakerId?: string;
  muted?: (id: string) => boolean;
}): ZoomSessionSnapshot {
  const count = options.count ?? initialProduction.participants.length;
  return mapCaptureSnapshot({
    meetingState: "in_meeting",
    tick: 1,
    activeSpeakerId: options.activeSpeakerId ?? "p1",
    participants: initialProduction.participants.slice(0, count).map((participant, index) => ({
      userId: participant.id,
      displayName: participant.name,
      role: participant.role,
      talking: participant.id === (options.activeSpeakerId ?? "p1"),
      sharingScreen: Boolean(options.screenShare) && index === 1,
      muted: options.muted ? options.muted(participant.id) : false,
      networkQuality: "good"
    }))
  });
}

function signals(snap: ZoomSessionSnapshot, state: ProductionState = initialProduction): DirectorSignals {
  return {
    state,
    snapshot: snap,
    liveParticipants: snap.participants.filter((participant) => participant.health !== "video-off"),
    elapsedMs: snap.elapsedSeconds * 1000
  };
}

describe("sanitizeDirectorProposal", () => {
  it("accepts a well-formed proposal and clamps confidence to 0-100", () => {
    const sanitized = sanitizeDirectorProposal({
      ruleId: "panel-discussion",
      recommendedSceneId: "panel",
      confidence: 250,
      rationale: "ok"
    });
    expect(sanitized).toEqual({
      ruleId: "panel-discussion",
      recommendedSceneId: "panel",
      confidence: 100,
      rationale: "ok"
    });
  });

  it("rejects unknown rule ids, unknown scenes, and non-finite confidence", () => {
    expect(sanitizeDirectorProposal({ ruleId: "made-up", recommendedSceneId: "panel", confidence: 90 })).toBeUndefined();
    expect(
      sanitizeDirectorProposal({ ruleId: "panel-discussion", recommendedSceneId: "nope", confidence: 90 })
    ).toBeUndefined();
    expect(
      sanitizeDirectorProposal({ ruleId: "panel-discussion", recommendedSceneId: "panel", confidence: Number.NaN })
    ).toBeUndefined();
    expect(sanitizeDirectorProposal(null)).toBeUndefined();
    expect(sanitizeDirectorProposal("string")).toBeUndefined();
  });
});

describe("HeuristicDirectorStrategy", () => {
  it("prioritizes speaker-slides whenever a screen share is active", () => {
    const proposal = heuristicDirectorStrategy.propose(signals(snapshot({ screenShare: true })));
    expect(proposal.ruleId).toBe("screen-share-priority");
    expect(proposal.recommendedSceneId).toBe("speaker-slides");
  });

  it("uses an interview for two live participants and a panel for more", () => {
    expect(selectBaseProposal(signals(snapshot({ count: 2 })).liveParticipants, snapshot({ count: 2 })).recommendedSceneId).toBe(
      "interview"
    );
    expect(selectBaseProposal(signals(snapshot({ count: 5 })).liveParticipants, snapshot({ count: 5 })).recommendedSceneId).toBe(
      "panel"
    );
  });

  it("prefers a host-focus shot for a solo presenter", () => {
    const proposal = selectHeuristicProposal(signals(snapshot({ count: 1 })));
    expect(proposal.ruleId).toBe("single-speaker");
    expect(proposal.recommendedSceneId).toBe("intro");
  });

  it("treats a large but all-muted room with one dominant speaker as host-focus, not a sparse panel", () => {
    const snap = snapshot({ count: 6, activeSpeakerId: "p1", muted: (id) => id !== "p1" });
    const proposal = selectHeuristicProposal(signals(snap));
    expect(proposal.recommendedSceneId).toBe("intro");
    expect(proposal.ruleId).toBe("single-speaker");
  });

  it("lowers confidence on cross-talk (multiple simultaneous active speakers)", () => {
    const calm = mapCaptureSnapshot({
      meetingState: "in_meeting",
      tick: 1,
      activeSpeakerId: "p1",
      participants: initialProduction.participants.slice(0, 5).map((participant) => ({
        userId: participant.id,
        displayName: participant.name,
        role: participant.role,
        talking: participant.id === "p1",
        networkQuality: "good"
      }))
    });
    const crossTalk = mapCaptureSnapshot({
      meetingState: "in_meeting",
      tick: 1,
      activeSpeakerId: "p1",
      participants: initialProduction.participants.slice(0, 5).map((participant) => ({
        userId: participant.id,
        displayName: participant.name,
        role: participant.role,
        talking: ["p1", "p2", "p3"].includes(participant.id),
        networkQuality: "good"
      }))
    });

    const calmConfidence = selectHeuristicProposal(signals(calm)).confidence;
    const crossTalkConfidence = selectHeuristicProposal(signals(crossTalk)).confidence;
    expect(crossTalkConfidence).toBeLessThan(calmConfidence);
  });

  it("penalizes confidence when feeds in the proposed layout are degraded", () => {
    const healthy = mapCaptureSnapshot({
      meetingState: "in_meeting",
      tick: 1,
      activeSpeakerId: "p1",
      participants: initialProduction.participants.slice(0, 5).map((participant) => ({
        userId: participant.id,
        displayName: participant.name,
        role: participant.role,
        talking: participant.id === "p1",
        networkQuality: "good"
      }))
    });
    const degraded = mapCaptureSnapshot({
      meetingState: "in_meeting",
      tick: 1,
      activeSpeakerId: "p1",
      participants: initialProduction.participants.slice(0, 5).map((participant) => ({
        userId: participant.id,
        displayName: participant.name,
        role: participant.role,
        talking: participant.id === "p1",
        networkQuality: ["p3", "p4"].includes(participant.id) ? "recovering" : "good"
      }))
    });

    expect(selectHeuristicProposal(signals(degraded)).confidence).toBeLessThan(
      selectHeuristicProposal(signals(healthy)).confidence
    );
  });

  it("raises confidence with program-scene hysteresis when the proposal already matches program", () => {
    const snap = snapshot({ count: 5 });
    const offProgram = selectHeuristicProposal(signals(snap, { ...initialProduction, activeSceneId: "intro" }));
    const onProgram = selectHeuristicProposal(signals(snap, { ...initialProduction, activeSceneId: "panel" }));
    expect(onProgram.confidence).toBeGreaterThan(offProgram.confidence);
  });
});

describe("proposeWithFallback", () => {
  const base = signals(snapshot({ count: 5 }));

  it("uses the heuristic when no strategy is supplied", () => {
    const result = proposeWithFallback(base, undefined);
    expect(result.strategyId).toBe("heuristic");
    expect(result.degraded).toBe(false);
    expect(result.proposal.recommendedSceneId).toBe("panel");
  });

  it("falls back to the heuristic when the strategy throws", () => {
    const throwing = new StaticDirectorStrategy(() => {
      throw new Error("model unavailable");
    }, "throwing");
    const result = proposeWithFallback(base, throwing);
    expect(result.degraded).toBe(true);
    expect(result.strategyId).toBe("heuristic");
    expect(result.proposal.recommendedSceneId).toBe("panel");
  });

  it("falls back to the heuristic when the proposal is invalid", () => {
    const invalid = new StaticDirectorStrategy({ ruleId: "bogus", recommendedSceneId: "panel", confidence: 90 }, "invalid");
    const result = proposeWithFallback(base, invalid);
    expect(result.degraded).toBe(true);
    expect(result.strategyId).toBe("heuristic");
  });

  it("falls back to the heuristic when the proposed scene is not in the show", () => {
    const ghostScene = new StaticDirectorStrategy(
      { ruleId: "panel-discussion", recommendedSceneId: "closing", confidence: 95 },
      "ghost"
    );
    const stateWithoutClosing: ProductionState = {
      ...initialProduction,
      scenes: initialProduction.scenes.filter((scene) => scene.id !== "closing")
    };
    const result = proposeWithFallback(signals(snapshot({ count: 5 }), stateWithoutClosing), ghostScene);
    expect(result.degraded).toBe(true);
    expect(result.strategyId).toBe("heuristic");
    expect(result.proposal.recommendedSceneId).toBe("panel");
  });

  it("uses a valid in-show proposal from the strategy", () => {
    const valid = new StaticDirectorStrategy(
      { ruleId: "single-speaker", recommendedSceneId: "intro", confidence: 77 },
      "valid"
    );
    const result = proposeWithFallback(base, valid);
    expect(result.degraded).toBe(false);
    expect(result.strategyId).toBe("valid");
    expect(result.proposal.recommendedSceneId).toBe("intro");
    expect(result.proposal.confidence).toBe(77);
  });

  it("is deterministic: identical signals produce identical heuristic proposals", () => {
    const strategy = new HeuristicDirectorStrategy();
    const a = strategy.propose(base);
    const b = strategy.propose(base);
    expect(a).toEqual(b);
  });
});

/** A minimal in-memory async provider double — never does any real I/O. */
class FakeAsyncProvider implements AiDirectorProvider {
  readonly id: string;
  lastSignals?: DirectorIntelligenceSignals;
  constructor(
    private readonly behavior: (signals: DirectorIntelligenceSignals) => Promise<unknown>,
    id = "fake-async-provider"
  ) {
    this.id = id;
  }
  propose(signals: DirectorIntelligenceSignals): Promise<unknown> {
    this.lastSignals = signals;
    return this.behavior(signals);
  }
}

describe("proposeWithProviderAsync", () => {
  afterEach(() => {
    vi.useRealTimers();
  });

  const base = signals(snapshot({ count: 5 }));

  it("uses the heuristic (not degraded) when no provider is configured", async () => {
    const result = await proposeWithProviderAsync(base, undefined);
    expect(result.strategyId).toBe("heuristic");
    expect(result.degraded).toBe(false);
    expect(result.degradeReason).toBeUndefined();
    expect(result.proposal.recommendedSceneId).toBe("panel");
  });

  it("honors a valid in-show proposal from the provider and passes it the rich signals", async () => {
    const provider = new FakeAsyncProvider(
      async () => ({ ruleId: "single-speaker", recommendedSceneId: "intro", confidence: 80 }),
      "good"
    );
    const result = await proposeWithProviderAsync(base, provider);
    expect(result.degraded).toBe(false);
    expect(result.strategyId).toBe("good");
    expect(result.proposal.recommendedSceneId).toBe("intro");
    expect(provider.lastSignals?.schemaVersion).toBe(1);
  });

  it("falls back to the heuristic when the provider rejects (error)", async () => {
    const provider = new FakeAsyncProvider(async () => {
      throw new Error("model down");
    }, "boom");
    const result = await proposeWithProviderAsync(base, provider);
    expect(result.degraded).toBe(true);
    expect(result.degradeReason).toBe("error");
    expect(result.strategyId).toBe("heuristic");
    expect(result.proposal.recommendedSceneId).toBe("panel");
  });

  it("falls back to the heuristic when the provider returns an invalid proposal", async () => {
    const provider = new FakeAsyncProvider(async () => ({ ruleId: "nope", recommendedSceneId: "panel", confidence: 9 }));
    const result = await proposeWithProviderAsync(base, provider);
    expect(result.degraded).toBe(true);
    expect(result.degradeReason).toBe("invalid");
    expect(result.strategyId).toBe("heuristic");
  });

  it("falls back to the heuristic when the proposed scene is not in the show", async () => {
    const stateWithoutIntro: ProductionState = {
      ...initialProduction,
      scenes: initialProduction.scenes.filter((scene) => scene.id !== "intro")
    };
    const provider = new FakeAsyncProvider(async () => ({
      ruleId: "single-speaker",
      recommendedSceneId: "intro",
      confidence: 95
    }));
    const result = await proposeWithProviderAsync(signals(snapshot({ count: 5 }), stateWithoutIntro), provider);
    expect(result.degraded).toBe(true);
    expect(result.degradeReason).toBe("ghost-scene");
    expect(result.strategyId).toBe("heuristic");
  });

  it("falls back to the heuristic when the provider exceeds its timeout budget", async () => {
    vi.useFakeTimers();
    const provider = new FakeAsyncProvider(
      () => new Promise(() => {}), // never resolves
      "slow"
    );
    const pending = proposeWithProviderAsync(base, provider, { timeoutMs: 50 });
    await vi.advanceTimersByTimeAsync(60);
    const result = await pending;
    expect(result.degraded).toBe(true);
    expect(result.degradeReason).toBe("timeout");
    expect(result.strategyId).toBe("heuristic");
    expect(result.proposal.recommendedSceneId).toBe("panel");
  });

  it("strategyFromResolution adapts a resolved proposal into a sync strategy", () => {
    const resolution = {
      proposal: { ruleId: "single-speaker", recommendedSceneId: "intro", confidence: 70 } as DirectorProposal,
      strategyId: "good",
      degraded: false
    };
    const strategy = strategyFromResolution(resolution);
    expect(strategy.id).toBe("good");
    expect(strategy.propose(base)).toEqual(resolution.proposal);
  });
});

// Smoke check the shape the seam exports so a future model-backed strategy has a
// clear contract to implement.
describe("DirectorStrategy contract", () => {
  it("exposes an id and a pure propose() returning a sanitizable proposal", () => {
    const strategy = new HeuristicDirectorStrategy();
    expect(typeof strategy.id).toBe("string");
    const proposal: DirectorProposal = strategy.propose(signals(snapshot({ count: 4 })));
    expect(sanitizeDirectorProposal(proposal)).toBeDefined();
  });

  it("ignores participants without live video when counting", () => {
    const withVideoOff = mapCaptureSnapshot({
      meetingState: "in_meeting",
      tick: 1,
      activeSpeakerId: "p1",
      participants: initialProduction.participants.slice(0, 4).map((participant, index) => ({
        userId: participant.id,
        displayName: participant.name,
        role: participant.role,
        talking: participant.id === "p1",
        videoOn: index < 2,
        networkQuality: "good"
      }))
    });
    const live: Participant[] = withVideoOff.participants.filter((participant) => participant.health !== "video-off");
    expect(live.length).toBe(2);
    expect(selectBaseProposal(live, withVideoOff).recommendedSceneId).toBe("interview");
  });
});
