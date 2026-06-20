import type { Participant, ProductionState } from "../domain/production";
import type { ZoomSessionSnapshot } from "./contracts";
import { buildDirectorIntelligenceSignals, type DirectorIntelligenceSignals } from "./directorSignals";

export type { DirectorIntelligenceSignals } from "./directorSignals";

/**
 * Pluggable director-strategy seam.
 *
 * A {@link DirectorStrategy} consumes the live production signals and *proposes*
 * a scene (with a rule id and confidence). It is deliberately the only part of
 * the auto-director that is swappable: the deterministic anti-thrash stabilizer
 * and the safety holds in `autoProductionDirector.ts` always gate whatever a
 * strategy proposes, so a future model-backed strategy can never bypass the
 * director's safety instincts.
 *
 * This file ships ONLY the seam, the always-on heuristic default
 * ({@link HeuristicDirectorStrategy}), and a trivial in-memory test double
 * ({@link StaticDirectorStrategy}). No external model/network is wired here; the
 * plan defers that pending a privacy decision (see
 * `docs/native-production-completion-plan.md` item 10).
 */

/** The canonical rule ids a director proposal can carry. */
export type DirectorProposalRuleId =
  | "screen-share-priority"
  | "single-speaker"
  | "focused-interview"
  | "panel-discussion";

/** The full set of scene ids the heuristic director can target. */
export const DIRECTOR_SCENE_IDS = [
  "intro",
  "interview",
  "speaker-slides",
  "panel",
  "closing"
] as const;

export type DirectorSceneId = (typeof DIRECTOR_SCENE_IDS)[number];

/**
 * The read-only signal bundle a strategy reasons over. Richer fields can be
 * added here over time (speaker turns, sentiment, engagement) without changing
 * the gate; today everything is derived deterministically from the snapshot.
 */
export type DirectorSignals = {
  state: ProductionState;
  snapshot: ZoomSessionSnapshot;
  /** Participants that currently have live (non `video-off`) feeds. */
  liveParticipants: Participant[];
  /** Monotonic clock in ms (defaults to snapshot.elapsedSeconds * 1000). */
  elapsedMs: number;
};

/** A proposal is just a candidate scene plus how strongly the strategy holds it. */
export type DirectorProposal = {
  ruleId: DirectorProposalRuleId;
  recommendedSceneId: DirectorSceneId;
  /** 0-100. The gate uses this for take/queue calibration. */
  confidence: number;
  /** Optional free-text rationale; used for telemetry/explainability only. */
  rationale?: string;
};

export interface DirectorStrategy {
  /** Human-readable id for telemetry (proposal-vs-taken logging). */
  readonly id: string;
  /** Propose a scene from the current signals. MUST be deterministic+pure here. */
  propose(signals: DirectorSignals): DirectorProposal;
}

const VALID_RULE_IDS = new Set<DirectorProposalRuleId>([
  "screen-share-priority",
  "single-speaker",
  "focused-interview",
  "panel-discussion"
]);

const VALID_SCENE_IDS = new Set<string>(DIRECTOR_SCENE_IDS);

/**
 * Validate an arbitrary (possibly model-produced) proposal. Returns a sanitized
 * proposal or `undefined` if it is unusable. Confidence is clamped to 0-100.
 */
export function sanitizeDirectorProposal(value: unknown): DirectorProposal | undefined {
  if (typeof value !== "object" || value === null) {
    return undefined;
  }

  const candidate = value as Partial<DirectorProposal>;
  if (typeof candidate.ruleId !== "string" || !VALID_RULE_IDS.has(candidate.ruleId as DirectorProposalRuleId)) {
    return undefined;
  }

  if (
    typeof candidate.recommendedSceneId !== "string" ||
    !VALID_SCENE_IDS.has(candidate.recommendedSceneId)
  ) {
    return undefined;
  }

  if (typeof candidate.confidence !== "number" || !Number.isFinite(candidate.confidence)) {
    return undefined;
  }

  return {
    ruleId: candidate.ruleId as DirectorProposalRuleId,
    recommendedSceneId: candidate.recommendedSceneId as DirectorSceneId,
    confidence: Math.max(0, Math.min(100, candidate.confidence)),
    rationale: typeof candidate.rationale === "string" ? candidate.rationale : undefined
  };
}

/**
 * Run a candidate strategy with graceful degradation: any throw, invalid
 * proposal, or proposal whose target scene is not present in the show always
 * falls back to the heuristic strategy. This is the seam's safety contract.
 */
export function proposeWithFallback(
  signals: DirectorSignals,
  strategy: DirectorStrategy | undefined,
  fallback: DirectorStrategy = heuristicDirectorStrategy
): { proposal: DirectorProposal; strategyId: string; degraded: boolean } {
  const heuristicProposal = fallback.propose(signals);

  if (!strategy || strategy === fallback) {
    return { proposal: heuristicProposal, strategyId: fallback.id, degraded: false };
  }

  try {
    const raw = strategy.propose(signals);
    const sanitized = sanitizeDirectorProposal(raw);
    if (!sanitized) {
      return { proposal: heuristicProposal, strategyId: fallback.id, degraded: true };
    }

    // Reject proposals that reference a scene the current show does not have.
    const sceneExists = signals.state.scenes.some((scene) => scene.id === sanitized.recommendedSceneId);
    if (!sceneExists) {
      return { proposal: heuristicProposal, strategyId: fallback.id, degraded: true };
    }

    return { proposal: sanitized, strategyId: strategy.id, degraded: false };
  } catch {
    return { proposal: heuristicProposal, strategyId: fallback.id, degraded: true };
  }
}

/**
 * The always-on, offline heuristic. It encodes the director's safety instincts
 * and is also the fallback for every other strategy. The scene-selection logic
 * lives in {@link selectHeuristicProposal} so `autoProductionDirector` can reuse
 * the exact same calibration.
 */
export class HeuristicDirectorStrategy implements DirectorStrategy {
  readonly id = "heuristic";

  propose(signals: DirectorSignals): DirectorProposal {
    return selectHeuristicProposal(signals);
  }
}

export const heuristicDirectorStrategy = new HeuristicDirectorStrategy();

/**
 * A trivial in-memory test double: always returns the configured proposal (or a
 * throw, to exercise the fallback path). This is the seam a future model-backed
 * strategy will plug into; it is intentionally inert and has no I/O.
 */
export class StaticDirectorStrategy implements DirectorStrategy {
  readonly id: string;
  private readonly result: DirectorProposal | (() => never) | unknown;

  constructor(result: DirectorProposal | (() => never) | unknown, id = "static-test-double") {
    this.result = result;
    this.id = id;
  }

  propose(): DirectorProposal {
    if (typeof this.result === "function") {
      return (this.result as () => DirectorProposal)();
    }
    return this.result as DirectorProposal;
  }
}

// ---------------------------------------------------------------------------
// Async intelligence-provider seam (Item 10 scaffold)
// ---------------------------------------------------------------------------

/**
 * Where a future, out-of-process / model-backed director plugs in.
 *
 * An {@link AiDirectorProvider} is the ASYNC analogue of {@link DirectorStrategy}:
 * it consumes the richer {@link DirectorIntelligenceSignals} bundle and MAY take
 * arbitrarily long (network/model latency), so it returns a promise and is run
 * under a timeout. It is deliberately the only async, potentially-remote seam in
 * the director.
 *
 * CRITICAL: no implementation of this interface ships in `src/engine` — the real
 * provider lives behind a `services/` boundary and is injected by the shell once a
 * privacy posture (local vs cloud inference over meeting content) is chosen. The
 * engine here only knows the *contract* and how to fall back. The deterministic
 * stabilizer in `autoProductionDirector.ts` still gates whatever a provider
 * proposes, so a provider can never bypass the director's safety holds.
 */
export interface AiDirectorProvider {
  /** Human-readable id for proposal-vs-taken telemetry. */
  readonly id: string;
  /**
   * Propose a scene from the richer signal bundle. MAY reject or hang; callers
   * MUST run it via {@link proposeWithProviderAsync}, which enforces the timeout
   * and the heuristic fallback. The returned value is untrusted and is sanitized.
   */
  propose(signals: DirectorIntelligenceSignals): Promise<unknown>;
}

/** Default budget for a provider call before the heuristic takes over. */
export const DEFAULT_PROVIDER_TIMEOUT_MS = 1200;

export type ProviderResolution = {
  proposal: DirectorProposal;
  /** Which seam actually produced the taken proposal. */
  strategyId: string;
  /** True when the provider was absent, slow, errored, or returned junk. */
  degraded: boolean;
  /** Why we degraded, for telemetry. Undefined when the provider was honored. */
  degradeReason?: "absent" | "timeout" | "error" | "invalid" | "ghost-scene";
};

/** Wrap a promise so it rejects after `timeoutMs`, identifying a timeout. */
function withTimeout<T>(promise: Promise<T>, timeoutMs: number): Promise<T> {
  return new Promise<T>((resolve, reject) => {
    const timer = setTimeout(() => reject(new ProviderTimeoutError(timeoutMs)), timeoutMs);
    promise.then(
      (value) => {
        clearTimeout(timer);
        resolve(value);
      },
      (error) => {
        clearTimeout(timer);
        reject(error);
      }
    );
  });
}

export class ProviderTimeoutError extends Error {
  constructor(timeoutMs: number) {
    super(`AI director provider exceeded ${timeoutMs}ms budget`);
    this.name = "ProviderTimeoutError";
  }
}

/**
 * Run an optional async provider with the heuristic as the always-on fallback.
 *
 * Falls back to the deterministic heuristic — and reports `degraded` — when the
 * provider is absent, exceeds the timeout, throws, returns an unsanitizable
 * proposal, or proposes a scene the current show does not contain. The returned
 * proposal is the *ungated* candidate; `autoProductionDirector` still applies the
 * anti-thrash holds before any take.
 */
export async function proposeWithProviderAsync(
  signals: DirectorSignals,
  provider: AiDirectorProvider | undefined,
  options: { timeoutMs?: number; fallback?: DirectorStrategy } = {}
): Promise<ProviderResolution> {
  const fallback = options.fallback ?? heuristicDirectorStrategy;
  const timeoutMs = options.timeoutMs ?? DEFAULT_PROVIDER_TIMEOUT_MS;
  const heuristicProposal = fallback.propose(signals);

  if (!provider) {
    return { proposal: heuristicProposal, strategyId: fallback.id, degraded: false };
  }

  const degrade = (degradeReason: ProviderResolution["degradeReason"]): ProviderResolution => ({
    proposal: heuristicProposal,
    strategyId: fallback.id,
    degraded: true,
    degradeReason
  });

  try {
    const intelligenceSignals = buildDirectorIntelligenceSignals(signals);
    const raw = await withTimeout(provider.propose(intelligenceSignals), timeoutMs);
    const sanitized = sanitizeDirectorProposal(raw);
    if (!sanitized) {
      return degrade("invalid");
    }
    const sceneExists = signals.state.scenes.some((scene) => scene.id === sanitized.recommendedSceneId);
    if (!sceneExists) {
      return degrade("ghost-scene");
    }
    return { proposal: sanitized, strategyId: provider.id, degraded: false };
  } catch (error) {
    return degrade(error instanceof ProviderTimeoutError ? "timeout" : "error");
  }
}

/**
 * Adapt a resolved async provider proposal into a synchronous
 * {@link DirectorStrategy} so it can be fed to `planAutoProduction` unchanged.
 * This is how a future provider-driven plan is gated by the same stabilizer the
 * heuristic uses: resolve the proposal once, then hand the frozen result in.
 */
export function strategyFromResolution(resolution: ProviderResolution): DirectorStrategy {
  return new StaticDirectorStrategy(resolution.proposal, resolution.strategyId);
}

// ---------------------------------------------------------------------------
// Heuristic scene selection + confidence calibration
// ---------------------------------------------------------------------------

/**
 * Choose the scene the heuristic director wants, with calibrated confidence.
 *
 * Calibration goals:
 * - Confidence reflects how unambiguous the signal is (a lone clear speaker or a
 *   stable screen share is high; a noisy panel near a participant-count boundary
 *   is lower) so the gate's take/queue threshold behaves sensibly.
 * - Hysteresis: when the proposal matches what is already on program, nudge
 *   confidence up; when we are near a participant-count boundary that recently
 *   flipped, nudge confidence down so the gate prefers to hold rather than
 *   thrash. The hard anti-thrash holds in `autoProductionDirector` remain the
 *   real safety gate; this only shapes take-vs-queue.
 */
export function selectHeuristicProposal(signals: DirectorSignals): DirectorProposal {
  const { liveParticipants, snapshot, state } = signals;
  const base = selectBaseProposal(liveParticipants, snapshot);
  const confidence = calibrateConfidence(base, liveParticipants, snapshot, state);

  return { ...base, confidence };
}

/**
 * The base (uncalibrated) scene selection: which scene the heuristic wants and
 * its nominal confidence, before program-aware calibration. Exported so the
 * back-compat `selectAutomationRule` facade can reuse it without picking up the
 * hysteresis/degraded-feed adjustments.
 */
export function selectBaseProposal(
  participants: Participant[],
  snapshot: ZoomSessionSnapshot
): DirectorProposal {
  const screenShareActive = isScreenShareActive(participants, snapshot);

  if (screenShareActive) {
    return {
      ruleId: "screen-share-priority",
      recommendedSceneId: "speaker-slides",
      confidence: 96,
      rationale: "Active screen share is the safest live layout."
    };
  }

  // Only participants who can actually carry program (live video). A meeting
  // that is all-muted / camera-off was already handled upstream as hold-empty.
  const speakingCount = countSpeakingCandidates(participants);

  if (participants.length === 1) {
    return {
      ruleId: "single-speaker",
      recommendedSceneId: "intro",
      confidence: 91,
      rationale: "One live participant; host-focus framing."
    };
  }

  // A clear dominant speaker among 2 live feeds reads as an interview.
  if (participants.length === 2) {
    return {
      ruleId: "focused-interview",
      recommendedSceneId: "interview",
      confidence: 90,
      rationale: "Two live participants; keep the conversation two-up."
    };
  }

  // 3+ live participants without a screen share is a panel. If essentially one
  // person is carrying the room (everyone else muted), the interview/intro shot
  // is cleaner than a sparse grid.
  if (speakingCount <= 1) {
    return {
      ruleId: "single-speaker",
      recommendedSceneId: "intro",
      confidence: 88,
      rationale: "Larger meeting but a single dominant speaker; host-focus is cleaner than a sparse grid."
    };
  }

  return {
    ruleId: "panel-discussion",
    recommendedSceneId: "panel",
    confidence: 92,
    rationale: "Multiple live speakers without slides; keep everyone visible."
  };
}

/** Screen share is active if the snapshot flags it OR any live feed shares. */
export function isScreenShareActive(participants: Participant[], snapshot: ZoomSessionSnapshot): boolean {
  return snapshot.screenShareActive || participants.some((participant) => participant.isScreenSharing);
}

/**
 * How many live participants are plausibly contributing (unmuted, or actively
 * speaking even if the mute flag is stale). Used to distinguish a true panel
 * from a room where one person is presenting to a muted audience.
 */
export function countSpeakingCandidates(participants: Participant[]): number {
  return participants.filter((participant) => !participant.isMuted || participant.isActiveSpeaker).length;
}

function calibrateConfidence(
  base: DirectorProposal,
  participants: Participant[],
  snapshot: ZoomSessionSnapshot,
  state: ProductionState
): number {
  let confidence = base.confidence;

  const activeSpeakers = participants.filter((participant) => participant.isActiveSpeaker);
  const recovering = participants.filter(
    (participant) => participant.health === "recovering" || participant.health === "low-resolution"
  ).length;

  // Hysteresis toward the current program scene: if we already hold the proposed
  // scene, we are more sure (don't re-take). If the proposal differs, stay a bit
  // more conservative near the take threshold.
  if (base.recommendedSceneId === state.activeSceneId) {
    confidence += 3;
  } else {
    confidence -= 2;
  }

  // A single unambiguous active speaker raises confidence; zero or many
  // simultaneous "active" speakers (rapid flips / cross-talk) lowers it.
  if (base.ruleId === "panel-discussion" || base.ruleId === "focused-interview") {
    if (activeSpeakers.length === 1) {
      confidence += 2;
    } else if (activeSpeakers.length === 0) {
      confidence -= 4;
    } else if (activeSpeakers.length >= 3) {
      confidence -= 4;
    }
  }

  // Degraded feeds in the proposed layout reduce confidence proportionally.
  confidence -= Math.min(8, recovering * 2);

  // Near the 2<->3 participant boundary the panel/interview choice is the most
  // thrash-prone, so shave confidence so the gate prefers to queue (and the
  // hold window can absorb a transient join/leave).
  if (!isScreenShareActive(participants, snapshot) && (participants.length === 2 || participants.length === 3)) {
    confidence -= 2;
  }

  return Math.max(0, Math.min(100, Math.round(confidence)));
}
