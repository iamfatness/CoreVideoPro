import type { MediaCoreAudioMixSession, MediaCoreParticipantAudioChannel } from "./protocol.js";

const TARGET_LEVEL = 68;
const LIMITER_THRESHOLD = 88;

export type AudioMixChannelInput = {
  participantId: string;
  inputLevel: number;
  muted: boolean;
  noiseSuppression: boolean;
  manualGainDb?: number;
  pan?: number;
  solo?: boolean;
  pluginInserts?: string[];
};

const IDLE_SESSION: MediaCoreAudioMixSession = {
  status: "idle",
  masterLevel: 0,
  loudnessLufs: -60,
  limiterEnabled: true,
  limiterActive: false,
  mixedFrameCount: 0,
  participants: [],
  summary: "Audio mix idle.",
  warnings: []
};

export class AudioMixSessionModel {
  private channels: AudioMixChannelInput[] = [];
  private mixedFrameCount = 0;
  private hasSyncedRawAudio = false;
  private limiterEnabled = true;
  private syncWarnings: string[] = [];

  sync(channels: AudioMixChannelInput[], limiterEnabled = true) {
    this.hasSyncedRawAudio = true;
    this.limiterEnabled = limiterEnabled;
    const { channels: normalizedChannels, warnings } = normalizeChannels(channels);
    this.channels = normalizedChannels;
    this.syncWarnings = warnings;
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
      const warnings = this.hasSyncedRawAudio ? ["Raw participant audio is missing from the mix session.", ...this.syncWarnings] : [];
      return {
        ...IDLE_SESSION,
        status: warnings.length > 0 ? "warning" : IDLE_SESSION.status,
        limiterEnabled: this.limiterEnabled,
        mixedFrameCount: this.mixedFrameCount,
        warnings
      };
    }

    const participants = this.channels.map(buildParticipantChannel);
    const audible = participants.filter((channel) => !channel.muted);
    const masterLevel =
      audible.length > 0
        ? Math.min(100, Math.round(audible.reduce((total, channel) => total + channel.outputLevel, 0) / audible.length + 8))
        : 0;
    const limiterWouldReduce = participants.some((channel) => channel.limiterActive) || masterLevel >= LIMITER_THRESHOLD;
    const limiterActive = this.limiterEnabled && limiterWouldReduce;
    const boostingCount = participants.filter((channel) => channel.status === "boosting").length;
    const duckingCount = participants.filter((channel) => channel.status === "ducking").length;
    const mutedCount = participants.filter((channel) => channel.muted).length;
    const manualCount = participants.filter((channel) => channel.manualGainDb !== undefined && channel.manualGainDb !== 0).length;
    const warnings = [...this.syncWarnings];
    if (participants.some((channel) => channel.noiseSuppression && channel.inputLevel < 35)) {
      warnings.push("Noise suppression active on low-level sources.");
    }
    if (audible.length === 0) {
      warnings.push("All synced participant audio channels are muted.");
    }
    if (limiterActive) {
      warnings.push("Limiter active in participant audio mix.");
    }
    if (participants.some((channel) => (channel.pluginInserts?.length ?? 0) > 0)) {
      warnings.push("VST inserts are configured but live third-party plugin processing requires the dev VST bridge.");
    }

    return {
      status: warnings.length > 0 ? "warning" : "live",
      masterLevel,
      loudnessLufs: limiterActive ? -14 : -16,
      limiterEnabled: this.limiterEnabled,
      limiterActive,
      mixedFrameCount: this.mixedFrameCount,
      participants: participants.map((participant) => ({
        ...participant,
        limiterActive: this.limiterEnabled && participant.limiterActive
      })),
      summary: buildSummary(boostingCount, duckingCount, mutedCount, manualCount),
      warnings
    };
  }
}

function normalizeChannels(channels: AudioMixChannelInput[]) {
  const normalizedChannels: AudioMixChannelInput[] = [];
  const warnings: string[] = [];
  const seenParticipantIds = new Set<string>();
  let boundedValueCount = 0;

  channels.forEach((channel, index) => {
    const participantId = channel.participantId.trim() || `unknown-audio-source-${index + 1}`;
    if (seenParticipantIds.has(participantId)) {
      warnings.push(`Participant ${participantId} has duplicated isolated audio channels; using the first channel for deterministic mix.`);
      return;
    }
    seenParticipantIds.add(participantId);

    const inputLevel = clampFinite(channel.inputLevel, 0, 100);
    const manualGainDb = channel.manualGainDb === undefined ? undefined : clampFinite(channel.manualGainDb, -12, 12);
    if (inputLevel !== channel.inputLevel || manualGainDb !== channel.manualGainDb) {
      boundedValueCount += 1;
    }

    normalizedChannels.push({
      participantId,
      inputLevel,
      muted: channel.muted,
      noiseSuppression: channel.noiseSuppression,
      manualGainDb,
      pan: clampFinite(channel.pan ?? 0, -1, 1),
      solo: channel.solo === true,
      pluginInserts: Array.isArray(channel.pluginInserts) ? channel.pluginInserts.filter((insert) => typeof insert === "string") : []
    });
  });

  if (boundedValueCount > 0) {
    warnings.push("Audio mix channel levels were bounded to DSP readiness limits.");
  }

  return { channels: normalizedChannels, warnings };
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
    pan: channel.pan,
    solo: channel.solo,
    pluginInserts: channel.pluginInserts?.map((insert) => ({
      name: insert,
      format: insert.startsWith("VST") ? "vst3" : "builtin",
      status: insert.startsWith("VST") ? "scan-only" : "available",
      processingEnabled: false
    })),
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

function clampFinite(value: number, min: number, max: number) {
  return Number.isFinite(value) ? clamp(value, min, max) : min;
}
