import type { AutoProductionSignals, ZoomSessionSnapshot } from "./contracts";
import type { ProductionState } from "../domain/production";
import {
  sanitizeDirectorProposal,
  type DirectorProposal,
  type DirectorSignals
} from "./directorStrategy";

/**
 * Optional intelligence-service seam (Item 10).
 *
 * This is the typed boundary between the renderer's `AiProductionEngine` and an
 * out-of-process **intelligence service** (hosted in `services/ai-director/`)
 * that consumes richer signals (speaker turns, sentiment, screen-share content,
 * engagement) and proposes a scene/transition. The service calls the latest
 * Claude model; the renderer never embeds an Anthropic/HTTP client — it talks to
 * this interface only.
 *
 * Crucially, a proposal from this service is **only a proposal**. The renderer
 * wraps it in a `StaticDirectorStrategy` and feeds it through the always-on
 * heuristic stabilizer (`planAutoProduction`), so the deterministic anti-thrash
 * holds + confidence gate still override anything unsafe, and any
 * timeout/error/invalid proposal degrades to the pure heuristic.
 */

/** The request the renderer sends to the intelligence service. */
export type AutoProductionProposalRequest = {
  /** Production state the proposal must be valid against (scene ids, mode). */
  state: ProductionState;
  /** Current live Zoom snapshot. */
  snapshot: ZoomSessionSnapshot;
  /** Richer, best-effort signals for the model to reason over. */
  signals?: AutoProductionSignals;
};

/** What the intelligence service returns: a director proposal, or nothing. */
export type AutoProductionProposalResponse = {
  proposal?: DirectorProposal;
  /** Optional model/provider id for telemetry (e.g. "claude-opus-4-8"). */
  modelId?: string;
};

/**
 * Transport-neutral handle to the intelligence service. Implemented by
 * {@link HttpDirectorIntelligenceService} (calls the `services/ai-director`
 * worker) and by test doubles. Always resolve within `timeoutMs`; on any
 * failure the caller falls back to the heuristic, so implementations SHOULD
 * surface failures as a rejected promise or an empty proposal rather than hang.
 */
export interface DirectorIntelligenceService {
  /** Human-readable id for telemetry (proposal-vs-taken logging). */
  readonly id: string;
  propose(request: AutoProductionProposalRequest): Promise<AutoProductionProposalResponse>;
}

/**
 * Config for the HTTP intelligence service. The URL + API key come from runtime
 * config (env / native host bridge), NEVER committed. When `serviceUrl` is
 * absent the engine simply runs the always-on heuristic.
 */
export type AutoProductionDirectorServiceConfig = {
  /** Base URL of the `services/ai-director` worker. */
  serviceUrl?: string;
  /** Bearer key for the service (optional; the service may be open in dev). */
  apiKey?: string;
  /** Per-call timeout before falling back to the heuristic. Default 1500ms. */
  timeoutMs?: number;
  /**
   * Privacy posture. When `false` (the default), meeting *content* signals
   * (sentiment, screen-share content) are stripped before leaving the machine;
   * only structural signals (counts, speaker-turn timing) are sent. Set to
   * `true` only when the operator has consented to cloud inference over content.
   */
  allowContentSignals?: boolean;
};

export const DEFAULT_PROPOSAL_TIMEOUT_MS = 1500;

/**
 * Strip content-bearing signals unless the operator opted in. Structural signals
 * (turn timing, engagement score) are retained; sentiment and the screen-share
 * *content classification* are dropped, since they derive from meeting content.
 */
export function redactSignalsForPrivacy(
  signals: AutoProductionSignals | undefined,
  allowContentSignals: boolean
): AutoProductionSignals | undefined {
  if (!signals) {
    return undefined;
  }
  if (allowContentSignals) {
    return signals;
  }
  const { sentiment: _sentiment, screenShareContent: _content, ...structural } = signals;
  return structural;
}

/**
 * HTTP-backed intelligence service. Calls the `services/ai-director` worker with
 * a bounded timeout and validates whatever comes back. Any network error,
 * timeout, non-2xx, or unparseable/invalid proposal resolves to an empty
 * proposal so the caller degrades to the heuristic — this client never throws.
 */
export class HttpDirectorIntelligenceService implements DirectorIntelligenceService {
  readonly id = "claude-intelligence-service";
  private readonly config: AutoProductionDirectorServiceConfig;
  private readonly fetchImpl: typeof fetch;

  constructor(config: AutoProductionDirectorServiceConfig, fetchImpl: typeof fetch = fetch) {
    this.config = config;
    this.fetchImpl = fetchImpl;
  }

  async propose(request: AutoProductionProposalRequest): Promise<AutoProductionProposalResponse> {
    const serviceUrl = this.config.serviceUrl;
    if (!serviceUrl) {
      return {};
    }

    const timeoutMs = this.config.timeoutMs ?? DEFAULT_PROPOSAL_TIMEOUT_MS;
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), timeoutMs);

    try {
      const body = {
        // Send only what the service needs to reason + validate, never the whole
        // production graph: valid scene ids, mode, the live snapshot, and the
        // privacy-redacted signal bundle.
        sceneIds: request.state.scenes.map((scene) => scene.id),
        activeSceneId: request.state.activeSceneId,
        mode: request.state.mode,
        snapshot: serializeSnapshot(request.snapshot),
        signals: redactSignalsForPrivacy(request.signals, this.config.allowContentSignals ?? false)
      };

      const headers: Record<string, string> = { "content-type": "application/json" };
      if (this.config.apiKey) {
        headers.authorization = `Bearer ${this.config.apiKey}`;
      }

      const response = await this.fetchImpl(`${serviceUrl.replace(/\/$/, "")}/v1/propose`, {
        method: "POST",
        headers,
        body: JSON.stringify(body),
        signal: controller.signal
      });

      if (!response.ok) {
        return {};
      }

      const payload = (await response.json()) as { proposal?: unknown; modelId?: unknown };
      const proposal = sanitizeDirectorProposal(payload?.proposal);
      return {
        proposal,
        modelId: typeof payload?.modelId === "string" ? payload.modelId : undefined
      };
    } catch {
      // Network error, abort/timeout, or bad JSON — degrade to the heuristic.
      return {};
    } finally {
      clearTimeout(timer);
    }
  }
}

/** Compact, content-light snapshot the service reasons over. */
function serializeSnapshot(snapshot: ZoomSessionSnapshot) {
  return {
    meetingState: snapshot.meetingState,
    screenShareActive: snapshot.screenShareActive,
    elapsedSeconds: snapshot.elapsedSeconds,
    participants: snapshot.participants.map((participant) => ({
      id: participant.id,
      isActiveSpeaker: participant.isActiveSpeaker,
      isMuted: participant.isMuted,
      isScreenSharing: participant.isScreenSharing,
      health: participant.health
    }))
  };
}

/**
 * Build {@link DirectorSignals}-shaped input is the renderer's job; this helper
 * keeps the service request narrow. Exposed for tests/back-compat.
 */
export function toProposalRequest(
  signals: DirectorSignals,
  richer?: AutoProductionSignals
): AutoProductionProposalRequest {
  return { state: signals.state, snapshot: signals.snapshot, signals: richer };
}
