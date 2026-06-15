import type {
  NativeMediaCoreAudioMixSession,
  NativeMediaCoreCaptionTrack,
  NativeMediaCoreParticipantAudioChannel
} from "./nativeMediaCoreProtocol";

const TARGET_LEVEL = 68;
const LIMITER_THRESHOLD = 88;

export const IDLE_NATIVE_AUDIO_MIX_SESSION: NativeMediaCoreAudioMixSession = {
  status: "idle",
  masterLevel: 0,
  loudnessLufs: -60,
  limiterActive: false,
  mixedFrameCount: 0,
  participants: [],
  summary: "Audio mix idle.",
  warnings: []
};

export const IDLE_NATIVE_CAPTION_TRACK: NativeMediaCoreCaptionTrack = {
  enabled: true,
  status: "idle",
  latencyMs: 180,
  warnings: []
};

export type NativeAudioMixChannelInput = {
  participantId: string;
  inputLevel: number;
  muted: boolean;
  noiseSuppression: boolean;
  manualGainDb?: number;
};

export class NativeAudioMixSessionSimulator {
  private channels: NativeAudioMixChannelInput[] = [];
  private mixedFrameCount = 0;

  sync(channels: NativeAudioMixChannelInput[]) {
    this.channels = channels.map((channel) => ({ ...channel }));
    return this.snapshot();
  }

  mix(frameCount = 1) {
    if (this.channels.length > 0) {
      this.mixedFrameCount += frameCount;
    }
    return this.snapshot();
  }

  snapshot(): NativeMediaCoreAudioMixSession {
    if (this.channels.length === 0) {
      return { ...IDLE_NATIVE_AUDIO_MIX_SESSION, mixedFrameCount: this.mixedFrameCount };
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

export class NativeCaptionTrackSimulator {
  private enabled = true;
  private currentCue?: NativeMediaCoreCaptionTrack["currentCue"];

  setEnabled(enabled: boolean) {
    this.enabled = enabled;
    return this.snapshot();
  }

  pushCue(text: string, atMs: number, speaker?: string) {
    const trimmed = text.trim();
    if (!trimmed) {
      return this.snapshot(["Caption cue ignored because text was empty."]);
    }

    this.currentCue = {
      text: trimmed,
      speaker,
      atMs,
      confidence: Math.max(82, 97 - Math.floor(trimmed.length / 28))
    };
    return this.snapshot();
  }

  snapshot(extraWarnings: string[] = []): NativeMediaCoreCaptionTrack {
    const warnings = [...extraWarnings];
    if (!this.enabled) {
      return {
        enabled: false,
        status: "idle",
        currentCue: this.currentCue,
        latencyMs: 0,
        warnings: ["Caption track disabled."]
      };
    }

    if (!this.currentCue) {
      return {
        ...IDLE_NATIVE_CAPTION_TRACK,
        enabled: true,
        warnings
      };
    }

    if (this.currentCue.text.length > 96) {
      warnings.push("Caption line is long; compact mode enabled.");
    }

    return {
      enabled: true,
      status: warnings.length > 0 ? "warning" : "live",
      currentCue: this.currentCue,
      latencyMs: 180,
      warnings
    };
  }
}

function buildParticipantChannel(channel: NativeAudioMixChannelInput): NativeMediaCoreParticipantAudioChannel {
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

function getStatus(muted: boolean, gainDb: number): NativeMediaCoreParticipantAudioChannel["status"] {
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