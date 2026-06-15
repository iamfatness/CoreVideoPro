import type { MediaCoreAudioMixSession, MediaCoreParticipantAudioChannel } from "./protocol.js";

const TARGET_LEVEL = 68;
const LIMITER_THRESHOLD = 88;

export type AudioMixChannelInput = {
  participantId: string;
  inputLevel: number;
  muted: boolean;
  noiseSuppression: boolean;
  manualGainDb?: number;
};

const IDLE_SESSION: MediaCoreAudioMixSession = {
  status: "idle",
  masterLevel: 0,
  loudnessLufs: -60,
  limiterActive: false,
  mixedFrameCount: 0,
  participants: [],
  summary: "Audio mix idle.",
  warnings: []
};

export class AudioMixSessionModel {
  private channels: AudioMixChannelInput[] = [];
  private mixedFrameCount = 0;

  sync(channels: AudioMixChannelInput[]) {
    this.channels = channels.map((channel) => ({ ...channel }));
    return this.snapshot();
  }

  mix(frameCount = 1) {
    if (this.channels.length > 0) {
      this.mixedFrameCount += frameCount;
    }
    return this.snapshot();
  }

  snapshot(): MediaCoreAudioMixSession {
    if (this.channels.length === 0) {
      return { ...IDLE_SESSION, mixedFrameCount: this.mixedFrameCount };
    }

    const participants = this.channels.map(buildParticipantChannel);
    const audible = participants.filter((channel) => !channel.muted);
    const masterLevel = Math.min(
      100,
      Math.round(audible.reduce((total, channel) => total + channel.outputLevel, 0) / Math.max(1, audible.length) + 8)
    );
    const limiterActive = participants.some((channel) => channel.limiterActive) || masterLevel >= LIMITER_THRESHOLD;
    const boostingCount = participants.filter((channel) => channel.status === "boosting").length;
    const duckingCount = participants.filter((channel) => channel.status === "ducking").length;
    const mutedCount = participants.filter((channel) => channel.muted).length;
    const manualCount = participants.filter((channel) => channel.manualGainDb !== undefined && channel.manualGainDb !== 0).length;
    const warnings: string[] = [];
    if (participants.some((channel) => channel.noiseSuppression && channel.inputLevel < 35)) {
      warnings.push("Noise suppression active on low-level sources.");
    }

    return {
      status: warnings.length > 0 ? "warning" : "live",
      masterLevel,
      loudnessLufs: limiterActive ? -14 : -16,
      limiterActive,
      mixedFrameCount: this.mixedFrameCount,
      participants,
      summary: buildSummary(boostingCount, duckingCount, mutedCount, manualCount),
      warnings
    };
  }
}

function buildParticipantChannel(channel: AudioMixChannelInput): MediaCoreParticipantAudioChannel {
  const smartGainDb = calculateGain(channel.inputLevel);
  const gainDb = channel.muted ? -60 : clamp(smartGainDb + (channel.manualGainDb ?? 0), -12, 12);
  const noiseSuppression = channel.noiseSuppression || channel.inputLevel < 35;
  const outputLevel = channel.muted ? 0 : clamp(Math.round(channel.inputLevel + gainDb * 4), 0, 100);
  const limiterActive = outputLevel >= LIMITER_THRESHOLD;

  return {
    participantId: channel.participantId,
    inputLevel: channel.inputLevel,
    outputLevel,
    gainDb,
    manualGainDb: channel.manualGainDb,
    noiseSuppression,
    limiterActive,
    muted: channel.muted,
    status: getStatus(channel.muted, gainDb)
  };
}

function calculateGain(level: number) {
  const delta = TARGET_LEVEL - level;
  if (delta > 28) return 6;
  if (delta > 14) return 3;
  if (delta < -12) return -4;
  if (delta < -4) return -2;
  return 0;
}

function getStatus(muted: boolean, gainDb: number): MediaCoreParticipantAudioChannel["status"] {
  if (muted) return "muted";
  if (gainDb > 0) return "boosting";
  if (gainDb < 0) return "ducking";
  return "balanced";
}

function buildSummary(boostingCount: number, duckingCount: number, mutedCount: number, manualCount: number) {
  const actions = [
    boostingCount > 0 ? `${boostingCount} boosted` : "",
    duckingCount > 0 ? `${duckingCount} ducked` : "",
    mutedCount > 0 ? `${mutedCount} muted` : "",
    manualCount > 0 ? `${manualCount} manual` : ""
  ].filter(Boolean);
  return actions.length > 0 ? `${actions.join(", ")} in program mix` : "Program mix balanced";
}

function clamp(value: number, min: number, max: number) {
  return Math.max(min, Math.min(max, value));
}