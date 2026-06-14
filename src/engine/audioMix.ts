import type { AudioMixState, Participant, ParticipantAudioMix } from "../domain/production";

const TARGET_LEVEL = 68;
const LIMITER_THRESHOLD = 88;

export class SimulatedAudioMixEngine {
  private mutedOverrides = new Map<string, boolean>();
  private gainOverrides = new Map<string, number>();

  async buildMix(participants: Participant[]): Promise<AudioMixState> {
    return buildSmartAudioMix(participants, this.mutedOverrides, this.gainOverrides);
  }

  async setParticipantMuted(participantId: string, muted: boolean, participants: Participant[]): Promise<AudioMixState> {
    this.mutedOverrides.set(participantId, muted);
    return buildSmartAudioMix(participants, this.mutedOverrides, this.gainOverrides);
  }

  async setParticipantGain(participantId: string, gainDb: number, participants: Participant[]): Promise<AudioMixState> {
    this.gainOverrides.set(participantId, clamp(gainDb, -12, 12));
    return buildSmartAudioMix(participants, this.mutedOverrides, this.gainOverrides);
  }
}

export function buildSmartAudioMix(
  participants: Participant[],
  mutedOverrides: ReadonlyMap<string, boolean> = new Map(),
  gainOverrides: ReadonlyMap<string, number> = new Map()
): AudioMixState {
  const mixes = participants.map((participant) =>
    buildParticipantMix(participant, mutedOverrides.get(participant.id), gainOverrides.get(participant.id))
  );
  const audibleMixes = mixes.filter((mix) => !mix.muted);
  const masterLevel = Math.min(
    100,
    Math.round(audibleMixes.reduce((total, mix) => total + mix.outputLevel, 0) / Math.max(1, audibleMixes.length) + 8)
  );
  const limiterActive = mixes.some((mix) => mix.limiterActive) || masterLevel >= LIMITER_THRESHOLD;
  const boostingCount = mixes.filter((mix) => mix.status === "boosting").length;
  const duckingCount = mixes.filter((mix) => mix.status === "ducking").length;
  const manualCount = mixes.filter((mix) => mix.manualGainDb !== undefined && mix.manualGainDb !== 0).length;

  return Object.freeze({
    participants: mixes,
    masterLevel,
    loudnessLufs: limiterActive ? -14 : -16,
    limiterActive,
    summary: buildSummary(boostingCount, duckingCount, mixes.filter((mix) => mix.muted).length, manualCount)
  });
}

function buildParticipantMix(
  participant: Participant,
  mutedOverride: boolean | undefined,
  manualGainDb: number | undefined
): ParticipantAudioMix {
  const muted = mutedOverride ?? participant.isMuted;
  const smartGainDb = calculateGain(participant.audioLevel);
  const gainDb = muted ? -60 : clamp(smartGainDb + (manualGainDb ?? 0), -12, 12);
  const outputLevel = muted ? 0 : clamp(Math.round(participant.audioLevel + gainDb * 4), 0, 100);
  const limiterActive = outputLevel >= LIMITER_THRESHOLD;

  return Object.freeze({
    participantId: participant.id,
    inputLevel: participant.audioLevel,
    outputLevel,
    gainDb,
    manualGainDb,
    noiseSuppression: participant.noiseSuppression || participant.audioLevel < 35,
    limiterActive,
    muted,
    status: getStatus(muted, gainDb)
  });
}

function calculateGain(level: number) {
  const delta = TARGET_LEVEL - level;

  if (delta > 28) {
    return 6;
  }

  if (delta > 14) {
    return 3;
  }

  if (delta < -12) {
    return -4;
  }

  if (delta < -4) {
    return -2;
  }

  return 0;
}

function getStatus(muted: boolean, gainDb: number): ParticipantAudioMix["status"] {
  if (muted) {
    return "muted";
  }

  if (gainDb > 0) {
    return "boosting";
  }

  if (gainDb < 0) {
    return "ducking";
  }

  return "balanced";
}

function buildSummary(boostingCount: number, duckingCount: number, mutedCount: number, manualCount: number) {
  const actions = [
    boostingCount > 0 ? `${boostingCount} boosted` : "",
    duckingCount > 0 ? `${duckingCount} ducked` : "",
    mutedCount > 0 ? `${mutedCount} muted` : "",
    manualCount > 0 ? `${manualCount} manual` : ""
  ].filter(Boolean);

  return actions.length > 0 ? `Smart mix: ${actions.join(", ")}` : "Smart mix balanced";
}

function clamp(value: number, min: number, max: number) {
  return Math.min(max, Math.max(min, value));
}
