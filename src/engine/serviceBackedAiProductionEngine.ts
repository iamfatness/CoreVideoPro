import type { AutoProductionState, ProductionState } from "../domain/production";
import type {
  AiProductionEngine,
  AutoProductionSignals,
  MagicSceneRequest,
  MagicSceneResult,
  ZoomSessionSnapshot
} from "./contracts";
import { planAutoProduction } from "./autoProductionDirector";
import {
  StaticDirectorStrategy,
  heuristicDirectorStrategy,
  type DirectorProposal
} from "./directorStrategy";
import type { DirectorIntelligenceService } from "./directorIntelligenceService";
import { buildMagicScene } from "./mockEngines";

/**
 * Telemetry record for one auto-production decision: what the model proposed vs.
 * what the deterministic director actually took (after gating). Used for tuning
 * — comparing model proposals against the safe, taken decision.
 */
export type AutoProductionDecisionTelemetry = {
  /** Telemetry id of the strategy that produced the gated proposal. */
  strategyId: string;
  /** Did the intelligence service return a usable proposal this tick? */
  serviceProposed: boolean;
  /** What the service proposed (scene + confidence), if anything. */
  proposedSceneId?: string;
  proposedConfidence?: number;
  /** What the director actually took/queued/held, after gating. */
  takenSceneId: string;
  takenAction: AutoProductionState["action"];
  takenConfidence: number;
  /** True when the gate overrode the proposal (held a different scene). */
  overridden: boolean;
  /** Model/provider id, when the service supplied one. */
  modelId?: string;
};

export type AutoProductionTelemetrySink = (record: AutoProductionDecisionTelemetry) => void;

/**
 * Feature-flag + entitlement gate for the optional intelligence service. The
 * engine consults the service ONLY when both are true; otherwise it runs the
 * pure heuristic. Gating both the feature flag and the `setAndForget`
 * entitlement keeps cloud inference off by default and tier-restricted.
 *
 * Each field is a snapshot value OR a (possibly async) resolver so the engine
 * can be wired synchronously while config/entitlement resolve later at runtime.
 */
export type GateValue = boolean | (() => boolean | Promise<boolean>);

export type IntelligenceServiceGate = {
  /** Master feature flag (config). */
  enabled: GateValue;
  /** `setAndForget` entitlement from the license. */
  setAndForgetEntitled: GateValue;
};

async function resolveGateValue(value: GateValue): Promise<boolean> {
  return typeof value === "function" ? Boolean(await value()) : Boolean(value);
}

export type ServiceBackedAiProductionEngineOptions = {
  service?: DirectorIntelligenceService;
  gate?: IntelligenceServiceGate;
  telemetry?: AutoProductionTelemetrySink;
};

/**
 * `AiProductionEngine` that consults an optional intelligence service, gates the
 * proposal through the always-on heuristic stabilizer, and falls back to the
 * pure heuristic on any timeout/error/disable.
 *
 * Pipeline:
 *   1. If gated off (flag/entitlement) or no service → heuristic only.
 *   2. Else `await service.propose(...)` with a bounded timeout (in the client).
 *   3. Wrap any returned proposal in a `StaticDirectorStrategy` and run it
 *      through `planAutoProduction` — the deterministic anti-thrash holds +
 *      confidence gate STILL decide take/queue/hold, overriding unsafe proposals.
 *   4. Log proposal-vs-taken telemetry.
 */
export class ServiceBackedAiProductionEngine implements AiProductionEngine {
  private readonly service?: DirectorIntelligenceService;
  private readonly gate: IntelligenceServiceGate;
  private readonly telemetry?: AutoProductionTelemetrySink;

  constructor(options: ServiceBackedAiProductionEngineOptions = {}) {
    this.service = options.service;
    this.gate = options.gate ?? { enabled: false, setAndForgetEntitled: false };
    this.telemetry = options.telemetry;
  }

  async buildMagicScene(request: MagicSceneRequest): Promise<MagicSceneResult> {
    return buildMagicScene(request);
  }

  async recommendAutoProduction(
    state: ProductionState,
    snapshot: ZoomSessionSnapshot,
    signals?: AutoProductionSignals
  ): Promise<AutoProductionState> {
    const { proposal, modelId } = await this.fetchProposal(state, snapshot, signals);

    // The proposal (if any) is gated by the always-on heuristic stabilizer. With
    // no proposal we pass the heuristic strategy directly. `planAutoProduction`
    // sanitizes + scene-checks the strategy proposal and silently degrades to the
    // heuristic on any invalid/unsafe proposal, so this can never bypass safety.
    const strategy = proposal
      ? new StaticDirectorStrategy(proposal, this.service?.id ?? "intelligence-service")
      : heuristicDirectorStrategy;

    const result = planAutoProduction(state, snapshot, snapshot.elapsedSeconds * 1000, strategy);

    this.emitTelemetry(proposal, result, modelId);
    return result;
  }

  private async fetchProposal(
    state: ProductionState,
    snapshot: ZoomSessionSnapshot,
    signals?: AutoProductionSignals
  ): Promise<{ proposal?: DirectorProposal; modelId?: string }> {
    if (!this.service) {
      return {};
    }
    const [enabled, entitled] = await Promise.all([
      resolveGateValue(this.gate.enabled),
      resolveGateValue(this.gate.setAndForgetEntitled)
    ]);
    if (!enabled || !entitled) {
      return {};
    }

    try {
      const response = await this.service.propose({ state, snapshot, signals });
      return { proposal: response.proposal, modelId: response.modelId };
    } catch {
      // Defensive: the HTTP client already swallows failures, but a custom
      // service must never break the always-on director.
      return {};
    }
  }

  private emitTelemetry(
    proposal: DirectorProposal | undefined,
    result: AutoProductionState,
    modelId: string | undefined
  ): void {
    if (!this.telemetry) {
      return;
    }
    const overridden = proposal ? proposal.recommendedSceneId !== result.recommendedSceneId : false;
    this.telemetry({
      strategyId: proposal ? this.service?.id ?? "intelligence-service" : heuristicDirectorStrategy.id,
      serviceProposed: Boolean(proposal),
      proposedSceneId: proposal?.recommendedSceneId,
      proposedConfidence: proposal?.confidence,
      takenSceneId: result.recommendedSceneId,
      takenAction: result.action,
      takenConfidence: result.confidence,
      overridden,
      modelId
    });
  }
}
