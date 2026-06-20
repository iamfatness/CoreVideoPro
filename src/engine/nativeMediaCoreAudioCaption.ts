import type {
  NativeMediaCoreAudioMixSession,
  NativeMediaCoreAudioRoutingBus,
  NativeMediaCoreAudioRoutingMatrix,
  NativeMediaCoreAudioRoutingSend,
  NativeMediaCoreCaptionTrack,
  NativeMediaCoreParticipantAudioChannel
} from "./nativeMediaCoreProtocol";
import {
  AUDIO_DBFS_FLOOR,
  computeMomentaryLufs,
  computeRmsDbfs,
  computeSamplePeakDbfs,
  computeShortTermLufs,
  dbfsToLinear,
  linearToDbfs,
  peakLimiterGainReductionDb
} from "./audioDspKernels";
import type { NativeMediaCoreAudioMixProgramMaster } from "./nativeMediaCoreProtocol";

const TARGET_LEVEL = 68;
const LIMITER_THRESHOLD = 88;

// Synthetic PCM characteristics used to MEASURE the audio-mix snapshot via the
// real DSP kernels. The signal is fully deterministic (fixed sample rate, fixed
// base frequency, per-participant phase) so snapshots stay stable across runs.
const SYNTH_SAMPLE_RATE = 48000;
// One short-term LUFS window (3 s) of mono samples per channel; short enough to
// stay cheap, long enough for the BS.1770 filters to settle.
const SYNTH_SAMPLE_COUNT = SYNTH_SAMPLE_RATE * 3;
const SYNTH_BASE_FREQUENCY_HZ = 220;
// dBFS the master limiter brickwalls the program mix at.
const PROGRAM_LIMITER_THRESHOLD_DBFS = -1;

// Map a 0-100 mixer level onto a peak amplitude in [0, 1] full scale. Level 0 ->
// digital silence, level 100 -> full scale; a sine at amplitude `a` has RMS
// a/sqrt(2), so the measured RMS dBFS scales smoothly with the level.
function levelToAmplitude(level: number): number {
  return clamp(level, 0, 100) / 100;
}

// Inverse of `levelToAmplitude` measured back from an RMS dBFS reading: recover
// the 0-100 level a sine of the measured RMS corresponds to, so `outputLevel`
// and `masterLevel` are computed FROM the synthesized/mixed signal rather than
// from the input number directly.
function rmsDbfsToLevel(rmsDbfs: number): number {
  // sine RMS = amplitude / sqrt(2); amplitude = level/100 -> level = amplitude*100.
  const amplitude = dbfsToLinear(rmsDbfs) * Math.SQRT2;
  return clamp(Math.round(amplitude * 100), 0, 100);
}

// Deterministic FNV-1a hash of the participant id, used to derive a per-source
// phase and frequency so summed mixes are reproducible AND effectively
// uncorrelated (distinct tones don't coherently cancel/reinforce, so program
// power tracks the sum of participant powers — physically realistic).
function participantHash(participantId: string): number {
  let hash = 2166136261;
  for (let index = 0; index < participantId.length; index += 1) {
    hash ^= participantId.charCodeAt(index);
    hash = Math.imul(hash, 16777619) >>> 0;
  }
  return hash >>> 0;
}

function participantPhase(participantId: string): number {
  return ((participantHash(participantId) % 360) / 360) * 2 * Math.PI;
}

// A small per-participant frequency offset (0..63 Hz above the base) keeps each
// synthesized tone distinct so the program sum is incoherent.
function participantFrequencyHz(participantId: string): number {
  return SYNTH_BASE_FREQUENCY_HZ + ((participantHash(participantId) >>> 8) % 64);
}

/**
 * Synthesize a deterministic mono PCM buffer for a participant: a sine whose
 * peak amplitude tracks `inputLevel` (muted -> silence), at a per-participant
 * phase and frequency. This is the synthetic signal the DSP kernels meter.
 */
function synthesizeParticipantPcm(channel: NativeAudioMixChannelInput): Float32Array {
  const samples = new Float32Array(SYNTH_SAMPLE_COUNT);
  if (channel.muted) {
    return samples;
  }
  const amplitude = levelToAmplitude(channel.inputLevel);
  if (amplitude <= 0) {
    return samples;
  }
  const phase = participantPhase(channel.participantId);
  const angular = (2 * Math.PI * participantFrequencyHz(channel.participantId)) / SYNTH_SAMPLE_RATE;
  for (let index = 0; index < SYNTH_SAMPLE_COUNT; index += 1) {
    samples[index] = amplitude * Math.sin(angular * index + phase);
  }
  return samples;
}

// Linear gain to apply to a synthesized signal given the smart/manual gain in dB.
function gainDbToLinear(gainDb: number): number {
  return dbfsToLinear(gainDb);
}

// F2 idle master metrics: no program PCM has flowed yet.
const IDLE_NATIVE_PROGRAM_MASTER: NativeMediaCoreAudioMixProgramMaster = {
  programTapPresent: false,
  programTapSampleCount: 0,
  truePeakDbfs: AUDIO_DBFS_FLOOR,
  programRmsDbfs: AUDIO_DBFS_FLOOR,
  momentaryLufs: AUDIO_DBFS_FLOOR,
  shortTermLufs: AUDIO_DBFS_FLOOR,
  integratedLufs: AUDIO_DBFS_FLOOR,
  gainReductionDb: 0
};

export const IDLE_NATIVE_AUDIO_MIX_SESSION: NativeMediaCoreAudioMixSession = {
  status: "idle",
  masterLevel: 0,
  loudnessLufs: -60,
  limiterActive: false,
  mixedFrameCount: 0,
  participants: [],
  summary: "Audio mix idle.",
  warnings: [],
  programMaster: IDLE_NATIVE_PROGRAM_MASTER
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

    const measured = this.channels.map(measureParticipantChannel);
    const participants = measured.map((entry) => entry.channel);
    const audible = measured.filter((entry) => !entry.channel.muted);

    // Sum the post-gain participant signals into a deterministic stereo program
    // mix, then MEASURE the master metrics from that mixed PCM via the kernels.
    const programLeft = new Float32Array(SYNTH_SAMPLE_COUNT);
    const programRight = new Float32Array(SYNTH_SAMPLE_COUNT);
    audible.forEach((entry, sourceIndex) => {
      // Equal-power pan spread so left/right differ deterministically per source.
      const pan = audible.length > 1 ? (sourceIndex / (audible.length - 1)) * 2 - 1 : 0;
      const leftGain = Math.cos(((pan + 1) / 2) * (Math.PI / 2));
      const rightGain = Math.sin(((pan + 1) / 2) * (Math.PI / 2));
      const pcm = entry.pcm;
      for (let index = 0; index < SYNTH_SAMPLE_COUNT; index += 1) {
        programLeft[index] += pcm[index] * leftGain;
        programRight[index] += pcm[index] * rightGain;
      }
    });

    // Master level is measured from the program RMS dBFS: combine the L and R
    // channel power (the louder-summing measure, not a -6 dB mono fold-down) so a
    // hard-panned full-scale pair still reads full.
    const masterLevel = audible.length > 0 ? rmsDbfsToLevel(stereoRmsDbfs(programLeft, programRight)) : 0;
    const loudnessLufs =
      audible.length > 0 ? round1(computeShortTermLufs(programLeft, programRight, SYNTH_SAMPLE_RATE)) : -60;
    // Run the brickwall limiter against whichever channel is hottest so the
    // master limiter activity reflects a real program-peak overshoot.
    const programGainReductionDb = Math.max(
      peakLimiterGainReductionDb(programLeft, PROGRAM_LIMITER_THRESHOLD_DBFS),
      peakLimiterGainReductionDb(programRight, PROGRAM_LIMITER_THRESHOLD_DBFS)
    );
    const limiterActive = participants.some((channel) => channel.limiterActive) || programGainReductionDb > 0;

    // F2 master metrics MEASURED from the summed program stereo bus.
    const programPresent = audible.length > 0;
    const shortTermLufs = programPresent
      ? round1(computeShortTermLufs(programLeft, programRight, SYNTH_SAMPLE_RATE))
      : AUDIO_DBFS_FLOOR;
    const momentaryLufs = programPresent
      ? round1(computeMomentaryLufs(programLeft, programRight, SYNTH_SAMPLE_RATE))
      : AUDIO_DBFS_FLOOR;
    const integratedLufs = programPresent && shortTermLufs > -70 ? shortTermLufs : AUDIO_DBFS_FLOOR;
    const programMaster: NativeMediaCoreAudioMixProgramMaster = {
      programTapPresent: programPresent,
      programTapSampleCount: programPresent ? SYNTH_SAMPLE_COUNT : 0,
      truePeakDbfs: programPresent
        ? round1(Math.max(computeSamplePeakDbfs(programLeft), computeSamplePeakDbfs(programRight)))
        : AUDIO_DBFS_FLOOR,
      programRmsDbfs: programPresent ? round1(stereoRmsDbfs(programLeft, programRight)) : AUDIO_DBFS_FLOOR,
      momentaryLufs,
      shortTermLufs,
      integratedLufs,
      gainReductionDb: round1(programGainReductionDb)
    };

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
      loudnessLufs,
      limiterActive,
      mixedFrameCount: this.mixedFrameCount,
      participants,
      summary: buildSummary(boostingCount, duckingCount, mutedCount, manualCount),
      warnings,
      programMaster
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

type MeasuredParticipantChannel = {
  channel: NativeMediaCoreParticipantAudioChannel;
  /** Post-gain synthesized PCM, summed into the program mix. */
  pcm: Float32Array;
};

// Build a participant channel whose `outputLevel` and `limiterActive` are
// MEASURED from a synthesized signal: synthesize the input PCM, apply the smart
// + manual gain, then meter the post-gain buffer's RMS (outputLevel) and run the
// brickwall limiter to see whether it would reduce the channel (limiterActive).
function measureParticipantChannel(channel: NativeAudioMixChannelInput): MeasuredParticipantChannel {
  const smartGainDb = calculateGain(channel.inputLevel);
  const gainDb = channel.muted ? -60 : clamp(smartGainDb + (channel.manualGainDb ?? 0), -12, 12);
  const noiseSuppression = channel.noiseSuppression || channel.inputLevel < 35;

  const input = synthesizeParticipantPcm(channel);
  const gainLinear = channel.muted ? 0 : gainDbToLinear(gainDb);
  const postGain = new Float32Array(input.length);
  for (let index = 0; index < input.length; index += 1) {
    postGain[index] = input[index] * gainLinear;
  }

  const outputLevel = channel.muted ? 0 : rmsDbfsToLevel(computeRmsDbfs(postGain));
  // The channel limiter would engage if the synthesized post-gain peak exceeds
  // the channel threshold; reuse the level-based threshold the protocol uses.
  const limiterActive = !channel.muted && (peakLimiterGainReductionDb(postGain, channelLimiterThresholdDbfs()) > 0 || outputLevel >= LIMITER_THRESHOLD);

  return {
    channel: {
      participantId: channel.participantId,
      inputLevel: channel.inputLevel,
      outputLevel,
      gainDb,
      manualGainDb: channel.manualGainDb,
      noiseSuppression,
      limiterActive,
      muted: channel.muted,
      status: getStatus(channel.muted, gainDb)
    },
    pcm: postGain
  };
}

// The channel-level brickwall threshold expressed in dBFS: the 0-100 LIMITER
// threshold maps to the amplitude a sine of that level would reach.
function channelLimiterThresholdDbfs(): number {
  const amplitude = LIMITER_THRESHOLD / 100;
  // amplitude is the sine peak; convert to dBFS of that peak.
  return 20 * Math.log10(amplitude);
}

function round1(value: number): number {
  return Math.round(value * 10) / 10;
}

// Combined RMS dBFS of a stereo pair: the RMS of the average channel power, so
// equal-power-panned sources contribute their full level instead of being
// halved by a mono fold-down.
function stereoRmsDbfs(left: Float32Array, right: Float32Array): number {
  const leftDbfs = computeRmsDbfs(left);
  const rightDbfs = computeRmsDbfs(right);
  const leftPower = dbfsToLinear(leftDbfs) ** 2;
  const rightPower = dbfsToLinear(rightDbfs) ** 2;
  const meanPower = (leftPower + rightPower) / 2;
  return linearToDbfs(Math.sqrt(meanPower));
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