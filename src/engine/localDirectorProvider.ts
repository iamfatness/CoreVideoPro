import type { DirectorIntelligenceSignals } from "./directorSignals";
import type { AiDirectorProvider, DirectorProposal } from "./directorStrategy";

/**
 * Local, on-device AI director provider (Item 10, first cut).
 *
 * Product decision: AI inference runs ENTIRELY on the production machine — meeting
 * content never leaves the box — so the intelligence provider is pure local
 * compute with no network, no cloud client, and no `services/` backend. This is
 * the async {@link AiDirectorProvider} the engine already knows how to run under a
 * timeout with a heuristic fallback, and whose proposals the deterministic
 * stabilizer in `autoProductionDirector.ts` still gates. A heavier local model
 * can later slot behind this exact seam without touching the gate.
 *
 * Unlike the count-only heuristic, this provider reasons over the richer
 * {@link DirectorIntelligenceSignals}: conversational dynamics (dominant speaker,
 * cross-talk), engagement (active contributors, audio energy), and feed health.
 * It agrees with the heuristic on the unambiguous cases (screen share, lone
 * speaker, two-up interview, multi-speaker panel) and only diverges where the
 * extra signal genuinely helps — e.g. a single dominant speaker carrying a large
 * room reads cleaner as host-focus than a sparse grid.
 */

const clampConfidence = (value: number): number => Math.max(0, Math.min(100, Math.round(value)));

/**
 * Deterministic, pure local scene selection over the richer signal bundle.
 * Exported for direct unit testing; the provider just awaits this.
 */
export function selectLocalProposal(signals: DirectorIntelligenceSignals): DirectorProposal {
  const { screenShare, speakerTurns, engagement, feedHealth } = signals;
  const liveCount = feedHealth.liveCount;
  const contributors = engagement.activeContributorCount;
  const energy = engagement.meanAudioLevel; // 0-1 coarse room energy
  const degradePenalty = Math.min(8, feedHealth.degradedCount * 2);
  const hasDominant = speakerTurns.dominantSpeakerId !== undefined;

  // Active screen share is the safest, highest-priority live layout.
  if (screenShare.active) {
    return {
      ruleId: "screen-share-priority",
      recommendedSceneId: "speaker-slides",
      confidence: clampConfidence(96 + (screenShare.sharerId ? 1 : -2) - degradePenalty),
      rationale: screenShare.sharerName
        ? `${screenShare.sharerName} is sharing; lead with the shared surface.`
        : "Active screen share is the safest live layout."
    };
  }

  // At most one live feed that can carry program -> host-focus framing.
  if (liveCount <= 1) {
    return {
      ruleId: "single-speaker",
      recommendedSceneId: "intro",
      confidence: clampConfidence(90 + energy * 6 - degradePenalty),
      rationale: "Single live participant; host-focus framing."
    };
  }

  // A lone contributor carrying several live feeds (everyone else muted/quiet)
  // reads cleaner as host-focus than a sparse grid — a call the count-only
  // heuristic only makes at 3+, but the contributor signal lets us make it
  // wherever it holds.
  if (hasDominant && contributors <= 1) {
    return {
      ruleId: "single-speaker",
      recommendedSceneId: "intro",
      confidence: clampConfidence(89 + energy * 6 - degradePenalty),
      rationale: "A single dominant speaker is carrying the room; host-focus is cleaner than a grid."
    };
  }

  // Two live contributors -> two-up interview. Cross-talk shaves certainty.
  if (liveCount === 2) {
    return {
      ruleId: "focused-interview",
      recommendedSceneId: "interview",
      confidence: clampConfidence((hasDominant ? 92 : 89) - (speakerTurns.crossTalk ? 4 : 0) - degradePenalty),
      rationale: "Two live contributors; keep the conversation two-up."
    };
  }

  // Three or more live contributors without slides -> panel. An energetic,
  // contested floor (multiple simultaneous speakers) reinforces keeping everyone
  // visible rather than chasing a single talker.
  const panelConfidence =
    90 + (contributors >= 3 ? 3 : 0) + (energy > 0.4 ? 2 : 0) - (speakerTurns.crossTalk ? 2 : 0) - degradePenalty;
  return {
    ruleId: "panel-discussion",
    recommendedSceneId: "panel",
    confidence: clampConfidence(panelConfidence),
    rationale: "Multiple live contributors without slides; keep everyone visible."
  };
}

export class LocalDirectorProvider implements AiDirectorProvider {
  readonly id = "local-director-v1";

  async propose(signals: DirectorIntelligenceSignals): Promise<DirectorProposal> {
    return selectLocalProposal(signals);
  }
}

export const localDirectorProvider = new LocalDirectorProvider();
