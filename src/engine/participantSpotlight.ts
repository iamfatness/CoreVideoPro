/**
 * Participant spotlight engine.
 * Scores and selects the best participant to feature in a single-speaker
 * or two-up layout, balancing active speech, role priority, camera quality,
 * and recency to avoid unfair exclusion of quieter participants.
 */

export type SpotlightReason =
  | "active-speaker"
  | "role-priority"
  | "recent-speaker"
  | "only-video"
  | "manual";

export type ParticipantSpotlightScore = {
  participantId: string;
  name: string;
  score: number;
  reasons: SpotlightReason[];
};

export type SpotlightDecision = {
  primaryId: string | null;
  secondaryId: string | null;
  scores: ParticipantSpotlightScore[];
  /** Whether the decision was made manually or by the engine */
  mode: "auto" | "manual";
};

export type SpotlightCandidate = {
  id: string;
  name: string;
  /** Whether this participant is the active/dominant speaker right now */
  isActiveSpeaker: boolean;
  /** Whether the participant's video is usable */
  hasVideo: boolean;
  /** Role weight: higher = higher priority (Host > Presenter > Panelist > Guest) */
  roleWeight: number;
  /** Seconds since this participant last spoke (lower = more recent) */
  secondsSinceLastSpoke: number;
  /** Total speaking seconds in the session */
  totalSpeakingSeconds: number;
};

export type SpotlightOptions = {
  /** Maximum seconds since last spoke before a participant is considered idle */
  idleThresholdSeconds?: number;
  /** Whether to pick a second (secondary) spotlight candidate */
  pickSecondary?: boolean;
  /** Manually pinned primary participant id (overrides scoring) */
  pinnedPrimaryId?: string | null;
};

const IDLE_THRESHOLD_DEFAULT = 60;

/**
 * Score a single candidate. Returns 0 for participants without usable video.
 */
export function scoreCandidate(
  candidate: SpotlightCandidate,
  idleThresholdSeconds: number = IDLE_THRESHOLD_DEFAULT
): ParticipantSpotlightScore {
  if (!candidate.hasVideo) {
    return { participantId: candidate.id, name: candidate.name, score: 0, reasons: [] };
  }

  const reasons: SpotlightReason[] = [];
  let score = 0;

  // Active speaker is the strongest signal
  if (candidate.isActiveSpeaker) {
    score += 60;
    reasons.push("active-speaker");
  }

  // Role weight contributes up to 20 points (Host=20, Presenter=15, Panelist=10, Guest=5)
  const rolePoints = Math.min(20, candidate.roleWeight * 5);
  if (rolePoints > 0) {
    score += rolePoints;
    reasons.push("role-priority");
  }

  // Recency: speaking in last idleThresholdSeconds earns up to 15 points
  if (candidate.secondsSinceLastSpoke <= idleThresholdSeconds) {
    const recencyScore = Math.round(
      15 * (1 - candidate.secondsSinceLastSpoke / idleThresholdSeconds)
    );
    score += recencyScore;
    if (recencyScore > 0) reasons.push("recent-speaker");
  }

  // Sole video source earns a base score so they're always shown
  score += 5;
  if (reasons.length === 0) reasons.push("only-video");

  return { participantId: candidate.id, name: candidate.name, score, reasons };
}

/**
 * Rank all candidates and select the primary (and optionally secondary) spotlight.
 */
export function selectSpotlight(
  candidates: SpotlightCandidate[],
  options: SpotlightOptions = {}
): SpotlightDecision {
  const {
    idleThresholdSeconds = IDLE_THRESHOLD_DEFAULT,
    pickSecondary = true,
    pinnedPrimaryId = null,
  } = options;

  const scores = candidates
    .map((c) => scoreCandidate(c, idleThresholdSeconds))
    .sort((a, b) => b.score - a.score);

  // Manual pin overrides engine scoring
  if (pinnedPrimaryId) {
    const pinnedScore = scores.find((s) => s.participantId === pinnedPrimaryId);
    if (pinnedScore) {
      const rest = scores.filter((s) => s.participantId !== pinnedPrimaryId);
      const secondary = pickSecondary && rest[0]?.score > 0 ? rest[0].participantId : null;
      return { primaryId: pinnedPrimaryId, secondaryId: secondary, scores, mode: "manual" };
    }
  }

  const eligible = scores.filter((s) => s.score > 0);
  const primary = eligible[0]?.participantId ?? null;
  const secondary =
    pickSecondary && eligible.length > 1 ? eligible[1].participantId : null;

  return { primaryId: primary, secondaryId: secondary, scores, mode: "auto" };
}

export function spotlightSummary(decision: SpotlightDecision): string {
  if (!decision.primaryId) return "No spotlight candidate available";
  const primary = decision.scores.find((s) => s.participantId === decision.primaryId);
  const mode = decision.mode === "manual" ? "Pinned" : "Auto";
  const secondary = decision.secondaryId
    ? ` + ${decision.scores.find((s) => s.participantId === decision.secondaryId)?.name}`
    : "";
  return `${mode}: ${primary?.name ?? decision.primaryId}${secondary}`;
}

/** Describe why a participant was chosen for the spotlight */
export function explainSpotlight(score: ParticipantSpotlightScore): string {
  if (score.score === 0) return "Not eligible (no video)";
  const parts: string[] = [];
  if (score.reasons.includes("active-speaker")) parts.push("speaking now");
  if (score.reasons.includes("role-priority")) parts.push("priority role");
  if (score.reasons.includes("recent-speaker")) parts.push("recently spoke");
  if (score.reasons.includes("only-video")) parts.push("only video source");
  return parts.join(", ") || "eligible";
}
