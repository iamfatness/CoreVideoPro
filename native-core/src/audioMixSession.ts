import type { MediaCoreAudioMixSession, MediaCoreParticipantAudioChannel } from "./protocol.js";
import {
  computeRmsDbfs,
  computeShortTermLufs,
  dbfsToLinear,
  linearToDbfs,
  peakLimiterGainReductionDb
} from "./audioDspKernels.js";

const TARGET_LEVEL = 68;
const LIMITER_THRESHOLD = 88;

// Synthetic PCM characteristics used to MEASURE the audio-mix snapshot via the
// real DSP kernels. The signal is fully deterministic (fixed sample rate, fixed
// base frequency, per-participant phase/frequency) so snapshots stay stable.
const SYNTH_SAMPLE_RATE = 48000;
const SYNTH_SAMPLE_COUNT = SYNTH_SAMPLE_RATE * 3; // one 3 s short-term LUFS window
const SYNTH_BASE_FREQUENCY_HZ = 220;
const PROGRAM_LIMITER_THRESHOLD_DBFS = -1;

// Deterministic FNV-1a hash of the participant id -> per-source phase/frequency
// so summed mixes are reproducible AND effectively uncorrelated.
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

function participantFrequencyHz(participantId: string): number {
  return SYNTH_BASE_FREQUENCY_HZ + ((participantHash(participantId) >>> 8) % 64);
}

// Map a 0-100 mixer level onto a peak amplitude in [0, 1] full scale.
function levelToAmplitude(level: number): number {
  return clamp(level, 0, 100) / 100;
}

// Recover the 0-100 level that a sine of the measured RMS dBFS corresponds to.
function rmsDbfsToLevel(rmsDbfs: number): number {
  const amplitude = dbfsToLinear(rmsDbfs) * Math.SQRT2;
  return clamp(Math.round(amplitude * 100), 0, 100);
}

function gainDbToLinear(gainDb: number): number {
  return dbfsToLinear(gainDb);
}

// Synthesize a deterministic mono PCM buffer: a sine whose peak amplitude tracks
// `inputLevel` (muted -> silence), at a per-participant phase and frequency.
function synthesizeParticipantPcm(channel: AudioMixChannelInput): Float32Array {
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

// The channel-level brickwall threshold expressed in dBFS (the 0-100 LIMITER
// threshold mapped to the amplitude a sine of that level would reach).
function channelLimiterThresholdDbfs(): number {
  return 20 * Math.log10(LIMITER_THRESHOLD / 100);
}

function round1(value: number): number {
  return Math.round(value * 10) / 10;
}

// Combined RMS dBFS of a stereo pair via the mean of the channel powers.
function stereoRmsDbfs(left: Float32Array, right: Float32Array): number {
  const leftPower = dbfsToLinear(computeRmsDbfs(left)) ** 2;
  const rightPower = dbfsToLinear(computeRmsDbfs(right)) ** 2;
  return linearToDbfs(Math.sqrt((leftPower + rightPower) / 2));
}

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

    const measured = this.channels.map(measureParticipantChannel);
    const participants = measured.map((entry) => entry.channel);
    const audible = measured.filter((entry) => !entry.channel.muted);

    // Sum the post-gain participant signals into a deterministic stereo program
    // mix, then MEASURE the master metrics from that mixed PCM via the kernels.
    const programLeft = new Float32Array(SYNTH_SAMPLE_COUNT);
    const programRight = new Float32Array(SYNTH_SAMPLE_COUNT);
    audible.forEach((entry, sourceIndex) => {
      const pan = audible.length > 1 ? (sourceIndex / (audible.length - 1)) * 2 - 1 : 0;
      const leftGain = Math.cos(((pan + 1) / 2) * (Math.PI / 2));
      const rightGain = Math.sin(((pan + 1) / 2) * (Math.PI / 2));
      const pcm = entry.pcm;
      for (let index = 0; index < SYNTH_SAMPLE_COUNT; index += 1) {
        programLeft[index] += pcm[index] * leftGain;
        programRight[index] += pcm[index] * rightGain;
      }
    });

    const masterLevel = audible.length > 0 ? rmsDbfsToLevel(stereoRmsDbfs(programLeft, programRight)) : 0;
    const loudnessLufs =
      audible.length > 0 ? round1(computeShortTermLufs(programLeft, programRight, SYNTH_SAMPLE_RATE)) : -60;
    const programGainReductionDb = Math.max(
      peakLimiterGainReductionDb(programLeft, PROGRAM_LIMITER_THRESHOLD_DBFS),
      peakLimiterGainReductionDb(programRight, PROGRAM_LIMITER_THRESHOLD_DBFS)
    );
    const limiterWouldReduce = participants.some((channel) => channel.limiterActive) || programGainReductionDb > 0;
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
      loudnessLufs,
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

type MeasuredParticipantChannel = {
  channel: MediaCoreParticipantAudioChannel;
  /** Post-gain synthesized PCM, summed into the program mix. */
  pcm: Float32Array;
};

// Build a participant channel whose `outputLevel` and `limiterActive` are
// MEASURED from a synthesized signal: synthesize the input PCM, apply the smart
// + manual gain, then meter the post-gain buffer's RMS (outputLevel) and run the
// brickwall limiter to see whether it would reduce the channel (limiterActive).
function measureParticipantChannel(channel: AudioMixChannelInput): MeasuredParticipantChannel {
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
  const limiterActive =
    !channel.muted && (peakLimiterGainReductionDb(postGain, channelLimiterThresholdDbfs()) > 0 || outputLevel >= LIMITER_THRESHOLD);

  return {
    channel: {
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
    },
    pcm: postGain
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
