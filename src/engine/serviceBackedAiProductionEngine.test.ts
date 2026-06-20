import { describe, expect, it, vi } from "vitest";
import { initialProduction, type ProductionState } from "../domain/production";
import { mapCaptureSnapshot } from "./captureSnapshotMapper";
import type { ZoomSessionSnapshot } from "./contracts";
import type {
  AutoProductionProposalRequest,
  AutoProductionProposalResponse,
  DirectorIntelligenceService
} from "./directorIntelligenceService";
import type { DirectorProposal } from "./directorStrategy";
import {
  ServiceBackedAiProductionEngine,
  type AutoProductionTelemetrySink
} from "./serviceBackedAiProductionEngine";

function snapshot(options: { screenShare?: boolean; count?: number } = {}): ZoomSessionSnapshot {
  const count = options.count ?? 5;
  return mapCaptureSnapshot({
    meetingState: "in_meeting",
    tick: 1,
    activeSpeakerId: "p1",
    participants: initialProduction.participants.slice(0, count).map((participant, index) => ({
      userId: participant.id,
      displayName: participant.name,
      role: participant.role,
      talking: index === 0,
      sharingScreen: Boolean(options.screenShare) && index === 1,
      networkQuality: "good"
    }))
  });
}

/** A panel-shaped show whose program is already the panel scene. */
function panelState(): ProductionState {
  return { ...initialProduction, mode: "set-and-forget", activeSceneId: "panel", previewSceneId: "panel" };
}

class StubService implements DirectorIntelligenceService {
  readonly id = "stub-service";
  constructor(private readonly handler: (request: AutoProductionProposalRequest) => Promise<AutoProductionProposalResponse>) {}
  propose(request: AutoProductionProposalRequest): Promise<AutoProductionProposalResponse> {
    return this.handler(request);
  }
}

const ENABLED_GATE = { enabled: true, setAndForgetEntitled: true };

describe("ServiceBackedAiProductionEngine", () => {
  it("falls back to the heuristic when the service REJECTS (failure)", async () => {
    const service = new StubService(() => Promise.reject(new Error("service down")));
    const telemetry = vi.fn<AutoProductionTelemetrySink>();
    const engine = new ServiceBackedAiProductionEngine({ service, gate: ENABLED_GATE, telemetry });

    const result = await engine.recommendAutoProduction(panelState(), snapshot({ count: 5 }));

    // Heuristic for a 5-up live room with no screen share is the panel.
    expect(result.recommendedSceneId).toBe("panel");
    expect(telemetry).toHaveBeenCalledTimes(1);
    expect(telemetry.mock.calls[0][0].serviceProposed).toBe(false);
    expect(telemetry.mock.calls[0][0].strategyId).toBe("heuristic");
  });

  it("falls back to the heuristic when the service TIMES OUT (empty proposal)", async () => {
    // The HTTP client converts a timeout/abort into an empty proposal response.
    const service = new StubService(() => Promise.resolve({}));
    const telemetry = vi.fn<AutoProductionTelemetrySink>();
    const engine = new ServiceBackedAiProductionEngine({ service, gate: ENABLED_GATE, telemetry });

    const result = await engine.recommendAutoProduction(panelState(), snapshot({ count: 5 }));

    expect(result.recommendedSceneId).toBe("panel");
    expect(telemetry.mock.calls[0][0].serviceProposed).toBe(false);
  });

  it("uses a valid in-show model proposal when it is safe", async () => {
    // Propose intro (a real scene) for a calm 5-up room; the gate runs in
    // set-and-forget and the proposal is the strategy the stabilizer consumes.
    const proposal: DirectorProposal = {
      ruleId: "single-speaker",
      recommendedSceneId: "intro",
      confidence: 95
    };
    const service = new StubService(() => Promise.resolve({ proposal, modelId: "claude-opus-4-8" }));
    const telemetry = vi.fn<AutoProductionTelemetrySink>();
    // Start from intro on program so there is no anti-thrash hold to clear.
    const state: ProductionState = {
      ...initialProduction,
      mode: "set-and-forget",
      activeSceneId: "intro",
      previewSceneId: "intro"
    };
    const engine = new ServiceBackedAiProductionEngine({ service, gate: ENABLED_GATE, telemetry });

    const result = await engine.recommendAutoProduction(state, snapshot({ count: 5 }));

    expect(result.recommendedSceneId).toBe("intro");
    expect(telemetry.mock.calls[0][0].serviceProposed).toBe(true);
    expect(telemetry.mock.calls[0][0].proposedSceneId).toBe("intro");
    expect(telemetry.mock.calls[0][0].modelId).toBe("claude-opus-4-8");
    expect(telemetry.mock.calls[0][0].overridden).toBe(false);
  });

  it("the stabilizer OVERRIDES an unsafe model proposal (anti-thrash hold)", async () => {
    // Program is on panel; the model proposes an immediate cut to speaker-slides
    // with a (fabricated) max confidence. In set-and-forget the director must
    // still hold for the scene-change window rather than take the proposal.
    const unsafe: DirectorProposal = {
      ruleId: "screen-share-priority",
      recommendedSceneId: "speaker-slides",
      confidence: 100
    };
    const service = new StubService(() => Promise.resolve({ proposal: unsafe, modelId: "claude-opus-4-8" }));
    const telemetry = vi.fn<AutoProductionTelemetrySink>();
    const engine = new ServiceBackedAiProductionEngine({ service, gate: ENABLED_GATE, telemetry });

    // No screen share is actually active, and elapsed time is ~0, so the hold
    // window has not elapsed: the director must HOLD on the current program.
    const result = await engine.recommendAutoProduction(panelState(), snapshot({ count: 5, screenShare: false }));

    expect(result.action).toBe("hold");
    expect(result.heldSceneId).toBe("panel");
    // Telemetry records that the model proposed speaker-slides but the director
    // did not take it.
    const record = telemetry.mock.calls[0][0];
    expect(record.proposedSceneId).toBe("speaker-slides");
    expect(record.takenAction).toBe("hold");
  });

  it("rejects an out-of-show proposal and degrades to the heuristic", async () => {
    const ghost: DirectorProposal = {
      ruleId: "panel-discussion",
      recommendedSceneId: "closing",
      confidence: 99
    };
    const service = new StubService(() => Promise.resolve({ proposal: ghost }));
    const stateWithoutClosing: ProductionState = {
      ...initialProduction,
      mode: "set-and-forget",
      activeSceneId: "panel",
      previewSceneId: "panel",
      scenes: initialProduction.scenes.filter((scene) => scene.id !== "closing")
    };
    const engine = new ServiceBackedAiProductionEngine({ service, gate: ENABLED_GATE });

    const result = await engine.recommendAutoProduction(stateWithoutClosing, snapshot({ count: 5 }));
    // planAutoProduction rejects the ghost scene and uses the heuristic (panel).
    expect(result.recommendedSceneId).toBe("panel");
  });

  it("does not call the service when the feature flag is off", async () => {
    const propose = vi.fn(() => Promise.resolve({}));
    const service = new StubService(propose);
    const engine = new ServiceBackedAiProductionEngine({
      service,
      gate: { enabled: false, setAndForgetEntitled: true }
    });

    await engine.recommendAutoProduction(panelState(), snapshot({ count: 5 }));
    expect(propose).not.toHaveBeenCalled();
  });

  it("does not call the service without the setAndForget entitlement", async () => {
    const propose = vi.fn(() => Promise.resolve({}));
    const service = new StubService(propose);
    const engine = new ServiceBackedAiProductionEngine({
      service,
      gate: { enabled: true, setAndForgetEntitled: false }
    });

    await engine.recommendAutoProduction(panelState(), snapshot({ count: 5 }));
    expect(propose).not.toHaveBeenCalled();
  });

  it("resolves async/function gate values", async () => {
    const propose = vi.fn(() => Promise.resolve({}));
    const service = new StubService(propose);
    const engine = new ServiceBackedAiProductionEngine({
      service,
      gate: { enabled: async () => true, setAndForgetEntitled: () => true }
    });

    await engine.recommendAutoProduction(panelState(), snapshot({ count: 5 }));
    expect(propose).toHaveBeenCalledTimes(1);
  });

  it("buildMagicScene still delegates to the heuristic builder", async () => {
    const engine = new ServiceBackedAiProductionEngine();
    const result = await engine.buildMagicScene({
      participants: initialProduction.participants.slice(0, 3),
      currentScenes: initialProduction.scenes,
      screenShareActive: false
    });
    expect(result.scenes.length).toBeGreaterThan(0);
  });
});
