import type {
  NativeMediaCoreAudioMixSession,
  NativeMediaCoreAudioRoutingBus,
  NativeMediaCoreAudioRoutingMatrix,
  NativeMediaCoreAudioRoutingSend,
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

export const NATIVE_AUDIO_ROUTING_BUSES: NativeMediaCoreAudioRoutingBus[] = ["pgm-l", "pgm-r", "iso-1", "iso-2", "mon", "stream"];

export const NATIVE_MIN_AUDIO_ROUTING_GAIN_DB = -60;
export const NATIVE_MAX_AUDIO_ROUTING_GAIN_DB = 10;

export const IDLE_NATIVE_AUDIO_ROUTING_MATRIX: NativeMediaCoreAudioRoutingMatrix = {
  status: "idle",
  routedSendCount: 0,
  routedSourceCount: 0,
  busSourceCounts: NATIVE_AUDIO_ROUTING_BUSES.map((busId) => ({ busId, sourceCount: 0 })),
  sends: [],
  summary: "Audio routing matrix idle.",
  warnings: []
};

function isNativeAudioRoutingBus(value: string): value is NativeMediaCoreAudioRoutingBus {
  return (NATIVE_AUDIO_ROUTING_BUSES as string[]).includes(value);
}

/**
 * Renderer-side mirror of the backend `AudioRoutingMatrixModel`. Validates the
 * incoming sends, clamps gain, and summarizes routed crosspoints so the in-memory
 * sync engine matches the real media core.
 */
export class NativeAudioRoutingMatrixSimulator {
  private sends: NativeMediaCoreAudioRoutingSend[] = [];
  private syncWarnings: string[] = [];
  private hasSynced = false;

  sync(sends: NativeMediaCoreAudioRoutingSend[]) {
    this.hasSynced = true;
    const warnings: string[] = [];
    const normalized: NativeMediaCoreAudioRoutingSend[] = [];
    const seen = new Set<string>();
    const routedSourceIds = new Set<string>();

    sends.forEach((send) => {
      const sourceId = typeof send.sourceId === "string" ? send.sourceId.trim() : "";
      if (!sourceId) {
        warnings.push("Audio routing send is missing a sourceId.");
        return;
      }

      if (!isNativeAudioRoutingBus(send.busId)) {
        warnings.push(`Audio routing send for ${sourceId} targets unknown bus ${String(send.busId)}.`);
        return;
      }

      const key = `${sourceId}:${send.busId}`;
      if (seen.has(key)) {
        warnings.push(`Audio routing send ${sourceId} → ${send.busId} is duplicated; keeping the first value.`);
        return;
      }
      seen.add(key);

      const rawGain = Number.isFinite(send.gainDb) ? send.gainDb : 0;
      if (rawGain < NATIVE_MIN_AUDIO_ROUTING_GAIN_DB || rawGain > NATIVE_MAX_AUDIO_ROUTING_GAIN_DB) {
        warnings.push(
          `Audio routing gain ${rawGain} dB for ${sourceId} → ${send.busId} is outside [${NATIVE_MIN_AUDIO_ROUTING_GAIN_DB}, ${NATIVE_MAX_AUDIO_ROUTING_GAIN_DB}] dB; clamped.`
        );
      }

      const gainDb = clamp(rawGain, NATIVE_MIN_AUDIO_ROUTING_GAIN_DB, NATIVE_MAX_AUDIO_ROUTING_GAIN_DB);
      routedSourceIds.add(sourceId);
      normalized.push({ sourceId, busId: send.busId, gainDb });
    });

    const requestedSourceIds = new Set(
      sends.map((send) => (typeof send.sourceId === "string" ? send.sourceId.trim() : "")).filter(Boolean)
    );
    requestedSourceIds.forEach((sourceId) => {
      if (!routedSourceIds.has(sourceId)) {
        warnings.push(`Audio routing source ${sourceId} is routed to no bus.`);
      }
    });

    this.sends = normalized;
    this.syncWarnings = [...new Set(warnings)];
    return this.snapshot();
  }

  snapshot(): NativeMediaCoreAudioRoutingMatrix {
    if (!this.hasSynced) {
      return {
        ...IDLE_NATIVE_AUDIO_ROUTING_MATRIX,
        busSourceCounts: IDLE_NATIVE_AUDIO_ROUTING_MATRIX.busSourceCounts.map((entry) => ({ ...entry })),
        sends: [],
        warnings: []
      };
    }

    const busSources = new Map<NativeMediaCoreAudioRoutingBus, Set<string>>(
      NATIVE_AUDIO_ROUTING_BUSES.map((busId) => [busId, new Set<string>()])
    );
    const routedSourceIds = new Set<string>();
    this.sends.forEach((send) => {
      busSources.get(send.busId)?.add(send.sourceId);
      routedSourceIds.add(send.sourceId);
    });

    const busSourceCounts = NATIVE_AUDIO_ROUTING_BUSES.map((busId) => ({
      busId,
      sourceCount: busSources.get(busId)?.size ?? 0
    }));

    const summary =
      this.sends.length === 0
        ? "No audio crosspoints routed."
        : `${this.sends.length} send${this.sends.length === 1 ? "" : "s"} from ${routedSourceIds.size} source${
            routedSourceIds.size === 1 ? "" : "s"
          } across ${busSourceCounts.filter((entry) => entry.sourceCount > 0).length} bus(es).`;

    return {
      status: this.syncWarnings.length > 0 ? "warning" : this.sends.length > 0 ? "live" : "idle",
      routedSendCount: this.sends.length,
      routedSourceCount: routedSourceIds.size,
      busSourceCounts,
      sends: this.sends.map((send) => ({ ...send })),
      summary,
      warnings: [...this.syncWarnings]
    };
  }
}