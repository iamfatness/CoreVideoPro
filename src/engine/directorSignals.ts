import type { FeedHealth, Participant } from "../domain/production";
import type { DirectorSignals } from "./directorStrategy";

/**
 * Richer, model-facing signal inputs for the AI director (Item 10 scaffold).
 *
 * This module is DECISION-FREE: it only defines the typed signal shapes a future
 * intelligence provider would consume and a *pure* constructor that derives them
 * deterministically from the data the renderer already has ({@link DirectorSignals}
 * / the Zoom snapshot). No model, no network, no provider lives here — those are
 * deferred pending the local-vs-cloud / meeting-content privacy decision (see
 * `docs/native-production-completion-plan.md` Item 10 and §6).
 *
 * These signals are transport-neutral, JSON-serializable plain data so they can
 * cross a `services/` boundary unchanged if/when a backend director is built. The
 * deterministic heuristic never needs them; they exist purely so a provider could
 * reason over more context than the raw snapshot, while the stabilizer in
 * `autoProductionDirector.ts` still gates whatever the provider proposes.
 */

/** Coarse per-feed health roll-up so a provider can weigh layout risk. */
export type FeedHealthSummary = {
  /** Live (non `video-off`) participant feeds. */
  liveCount: number;
  /** Feeds that are degraded (`recovering` or `low-resolution`). */
  degradedCount: number;
  /** Feeds reporting fully healthy (`live`). */
  healthyCount: number;
  /** Breakdown by raw {@link FeedHealth} value, for explainability/telemetry. */
  byHealth: Record<FeedHealth, number>;
};

/**
 * A single speaker-turn observation. A provider can use a window of these to read
 * conversational dynamics (interview back-and-forth vs. one dominant host vs.
 * cross-talk). Today we can only synthesize the *current* turn from the snapshot;
 * a real turn history would be supplied by the capture/transcription layer.
 */
export type DirectorSpeakerTurn = {
  participantId: string;
  participantName: string;
  /** Monotonic ms at which this turn was observed. */
  atMs: number;
  /** Whether the participant was muted while flagged active (stale-flag hint). */
  muted: boolean;
};

/** Roll-up of who is talking right now and how contested the floor is. */
export type SpeakerTurnSummary = {
  /** Participants currently flagged as active speakers. */
  activeSpeakerIds: string[];
  /** The single dominant speaker id, if exactly one is active. */
  dominantSpeakerId?: string;
  /** True when 2+ feeds are simultaneously "active" (cross-talk). */
  crossTalk: boolean;
  /** The most recent observed turns (newest last). */
  recentTurns: DirectorSpeakerTurn[];
};

/**
 * Presence-only description of an active screen share. Item 10's spec mentions
 * "content of screen share", but reading that content is exactly the privacy-
 * sensitive capability that is BLOCKED pending the product decision. So this
 * scaffold carries presence and the sharer's identity only — never decoded
 * content — and a future provider may enrich it once a posture is chosen.
 */
export type ScreenShareContentSignal = {
  active: boolean;
  /** Participant id sharing, if known. */
  sharerId?: string;
  sharerName?: string;
  /**
   * Deliberately absent: any classification/OCR of the shared surface. Left as an
   * explicit, documented gap so no caller assumes content is available.
   */
  contentClassification?: never;
};

/** Lightweight engagement/room-temperature proxy derived from the snapshot. */
export type EngagementSignal = {
  /** Live participants / total participants present (0-1). */
  liveRatio: number;
  /** Unmuted-or-talking live participants (plausible contributors). */
  activeContributorCount: number;
  /** Mean audio level across live participants (0-1), a coarse "energy" proxy. */
  meanAudioLevel: number;
};

/**
 * The full richer-signal bundle handed to an intelligence provider. It embeds the
 * existing deterministic {@link DirectorSignals} (so a provider gets the same view
 * the heuristic does) plus the derived summaries above. Everything is plain data.
 */
export type DirectorIntelligenceSignals = {
  /** Schema version so a provider/service can negotiate compatibility. */
  schemaVersion: 1;
  /** The monotonic clock the rest of the bundle was sampled at. */
  elapsedMs: number;
  meetingState: string;
  speakerTurns: SpeakerTurnSummary;
  screenShare: ScreenShareContentSignal;
  engagement: EngagementSignal;
  feedHealth: FeedHealthSummary;
};

const EMPTY_HEALTH_BREAKDOWN: Record<FeedHealth, number> = {
  live: 0,
  "low-resolution": 0,
  recovering: 0,
  "video-off": 0
};

function summarizeFeedHealth(allParticipants: Participant[], liveParticipants: Participant[]): FeedHealthSummary {
  const byHealth: Record<FeedHealth, number> = { ...EMPTY_HEALTH_BREAKDOWN };
  for (const participant of allParticipants) {
    byHealth[participant.health] += 1;
  }
  const degradedCount = byHealth.recovering + byHealth["low-resolution"];
  return {
    liveCount: liveParticipants.length,
    degradedCount,
    healthyCount: byHealth.live,
    byHealth
  };
}

function summarizeSpeakerTurns(liveParticipants: Participant[], elapsedMs: number): SpeakerTurnSummary {
  const active = liveParticipants.filter((participant) => participant.isActiveSpeaker);
  const recentTurns: DirectorSpeakerTurn[] = active.map((participant) => ({
    participantId: participant.id,
    participantName: participant.name,
    atMs: elapsedMs,
    muted: participant.isMuted
  }));
  return {
    activeSpeakerIds: active.map((participant) => participant.id),
    dominantSpeakerId: active.length === 1 ? active[0].id : undefined,
    crossTalk: active.length >= 2,
    recentTurns
  };
}

function summarizeScreenShare(liveParticipants: Participant[], snapshotScreenShareActive: boolean): ScreenShareContentSignal {
  const sharer = liveParticipants.find((participant) => participant.isScreenSharing);
  return {
    active: snapshotScreenShareActive || Boolean(sharer),
    sharerId: sharer?.id,
    sharerName: sharer?.name
  };
}

function summarizeEngagement(allParticipants: Participant[], liveParticipants: Participant[]): EngagementSignal {
  const totalPresent = allParticipants.length;
  const activeContributorCount = liveParticipants.filter(
    (participant) => !participant.isMuted || participant.isActiveSpeaker
  ).length;
  const meanAudioLevel =
    liveParticipants.length === 0
      ? 0
      : liveParticipants.reduce((sum, participant) => sum + participant.audioLevel, 0) / liveParticipants.length;
  return {
    liveRatio: totalPresent === 0 ? 0 : liveParticipants.length / totalPresent,
    activeContributorCount,
    meanAudioLevel: Number(meanAudioLevel.toFixed(4))
  };
}

/**
 * Pure, deterministic constructor for the richer director-signal bundle.
 *
 * It derives everything from the existing {@link DirectorSignals} the heuristic
 * already receives — no I/O, no model, no new data source — so it is safe to build
 * unconditionally and hand to a (future) provider. Construction only: nothing here
 * makes a scene decision.
 */
export function buildDirectorIntelligenceSignals(signals: DirectorSignals): DirectorIntelligenceSignals {
  const { snapshot, liveParticipants, elapsedMs } = signals;
  const allParticipants = snapshot.participants;
  return {
    schemaVersion: 1,
    elapsedMs,
    meetingState: snapshot.meetingState,
    speakerTurns: summarizeSpeakerTurns(liveParticipants, elapsedMs),
    screenShare: summarizeScreenShare(liveParticipants, snapshot.screenShareActive),
    engagement: summarizeEngagement(allParticipants, liveParticipants),
    feedHealth: summarizeFeedHealth(allParticipants, liveParticipants)
  };
}
