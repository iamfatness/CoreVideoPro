/**
 * PCM DSP kernels — TypeScript port of the native `AudioDsp.h` F2 audio DSP core.
 *
 * Pure, deterministic functions that operate directly on PCM sample buffers in
 * the full-scale float range [-1, 1]. These mirror the C++ reference kernels
 * (RMS dBFS, sample-peak dBFS, ITU-R BS.1770 K-weighted momentary/short-term
 * LUFS, and a brickwall look-ahead peak limiter) so the renderer mock can report
 * MEASURED audio metrics from a synthetic signal instead of derived formulas.
 *
 * This file is shell-agnostic: no platform, browser, or node-only imports. It is
 * the renderer-side copy of the kernels; the Node native-core workspace keeps an
 * independent copy (the two workspaces do not share a package).
 */

export type PcmSamples = Float32Array | number[];

// pi as a local constant so we never depend on a platform definition.
export const AUDIO_PI = 3.14159265358979323846;

// Floor reported for digital silence, in dBFS. Real signals approach 0 dBFS at
// full scale; -120 dB is well below any measurable content and keeps the log
// math finite for empty/zero buffers.
export const AUDIO_DBFS_FLOOR = -120.0;

/**
 * Convert a non-negative linear amplitude ratio (relative to full scale 1.0) to
 * dBFS, clamped to the silence floor.
 */
export function linearToDbfs(linear: number): number {
  if (!Number.isFinite(linear) || linear <= 0.0) {
    return AUDIO_DBFS_FLOOR;
  }
  const db = 20.0 * Math.log10(linear);
  return db < AUDIO_DBFS_FLOOR ? AUDIO_DBFS_FLOOR : db;
}

/** dBFS -> linear amplitude ratio. */
export function dbfsToLinear(dbfs: number): number {
  return Math.pow(10.0, dbfs / 20.0);
}

/**
 * RMS level of a PCM buffer in dBFS. A full-scale square wave -> 0 dBFS, a
 * full-amplitude sine -> ~-3.01 dBFS, digital silence (or empty) ->
 * AUDIO_DBFS_FLOOR.
 */
export function computeRmsDbfs(samples: PcmSamples): number {
  const count = samples.length;
  if (count === 0) {
    return AUDIO_DBFS_FLOOR;
  }
  let sumSquares = 0.0;
  for (let index = 0; index < count; index += 1) {
    const sample = samples[index];
    sumSquares += sample * sample;
  }
  const meanSquare = sumSquares / count;
  return linearToDbfs(Math.sqrt(meanSquare));
}

/**
 * Peak |sample| of a PCM buffer in dBFS. Full-scale (|s| == 1) -> 0 dBFS,
 * silence (or empty) -> AUDIO_DBFS_FLOOR.
 */
export function computeSamplePeakDbfs(samples: PcmSamples): number {
  const count = samples.length;
  if (count === 0) {
    return AUDIO_DBFS_FLOOR;
  }
  let peak = 0.0;
  for (let index = 0; index < count; index += 1) {
    const magnitude = Math.abs(samples[index]);
    if (magnitude > peak) {
      peak = magnitude;
    }
  }
  return linearToDbfs(peak);
}

// ---------------------------------------------------------------------------
// ITU-R BS.1770 K-weighting + loudness.
//
// K-weighting is a two-stage biquad cascade: a stage-1 high-shelf "head" filter
// and a stage-2 RLB high-pass. The analog-prototype parameters below are the
// BS.1770 reference values; we bilinear-transform them at the actual sample
// rate so the weighting is correct away from 48 kHz, then take the mean square
// of the weighted signal over a sliding window.
//
// Momentary loudness uses a 400 ms window, short-term a 3 s window. Loudness in
// LUFS = -0.691 + 10*log10(sum_channels(weight * mean_square)); for stereo we
// use channel weights L = R = 1.0 per the reference spec.
// ---------------------------------------------------------------------------

export type AudioBiquadCoefficients = {
  b0: number;
  b1: number;
  b2: number;
  a1: number; // normalized so a0 == 1
  a2: number;
};

export type AudioBiquadState = {
  z1: number;
  z2: number;
};

/** Transposed-direct-form-II single-sample step. */
export function biquadProcessSample(coeff: AudioBiquadCoefficients, state: AudioBiquadState, input: number): number {
  const output = coeff.b0 * input + state.z1;
  state.z1 = coeff.b1 * input - coeff.a1 * output + state.z2;
  state.z2 = coeff.b2 * input - coeff.a2 * output;
  return output;
}

/** BS.1770 K-weighting stage 1: high-shelf "head" filter. */
export function bs1770Stage1(sampleRate: number): AudioBiquadCoefficients {
  // Reference analog parameters (BS.1770-4 / EBU R128 derivation).
  const f0 = 1681.974450955533;
  const gainDb = 3.999843853973347;
  const q = 0.7071752369554196;
  const k = Math.tan((AUDIO_PI * f0) / sampleRate);
  const vh = Math.pow(10.0, gainDb / 20.0);
  const vb = Math.pow(vh, 0.4996667741545416);
  const denom = 1.0 + k / q + k * k;

  return {
    b0: (vh + (vb * k) / q + k * k) / denom,
    b1: (2.0 * (k * k - vh)) / denom,
    b2: (vh - (vb * k) / q + k * k) / denom,
    a1: (2.0 * (k * k - 1.0)) / denom,
    a2: (1.0 - k / q + k * k) / denom
  };
}

/** BS.1770 K-weighting stage 2: RLB high-pass filter. */
export function bs1770Stage2(sampleRate: number): AudioBiquadCoefficients {
  const f0 = 38.13547087602444;
  const q = 0.5003270373238773;
  const k = Math.tan((AUDIO_PI * f0) / sampleRate);
  const denom = 1.0 + k / q + k * k;

  return {
    b0: 1.0,
    b1: -2.0,
    b2: 1.0,
    a1: (2.0 * (k * k - 1.0)) / denom,
    a2: (1.0 - k / q + k * k) / denom
  };
}

/**
 * Mean square of a single channel after K-weighting, over the trailing
 * `windowSamples` samples (or the whole buffer when shorter / 0).
 */
export function kWeightedMeanSquare(samples: PcmSamples, sampleRate: number, windowSamples: number): number {
  const count = samples.length;
  if (count === 0 || sampleRate <= 0.0) {
    return 0.0;
  }
  const stage1 = bs1770Stage1(sampleRate);
  const stage2 = bs1770Stage2(sampleRate);
  const state1: AudioBiquadState = { z1: 0.0, z2: 0.0 };
  const state2: AudioBiquadState = { z1: 0.0, z2: 0.0 };

  const window = windowSamples === 0 || windowSamples > count ? count : windowSamples;
  const windowStart = count - window;
  let sumSquares = 0.0;
  for (let index = 0; index < count; index += 1) {
    const pre = biquadProcessSample(stage1, state1, samples[index]);
    const weighted = biquadProcessSample(stage2, state2, pre);
    if (index >= windowStart) {
      sumSquares += weighted * weighted;
    }
  }
  return sumSquares / window;
}

/**
 * Loudness (LUFS) of a stereo PCM signal over a trailing window, per BS.1770.
 * `left`/`right` are deinterleaved channel buffers of equal length; channel
 * weights are L = R = 1.0. Returns AUDIO_DBFS_FLOOR for empty/zero input.
 */
export function computeStereoLoudnessLufs(
  left: PcmSamples,
  right: PcmSamples,
  sampleRate: number,
  windowSeconds: number
): number {
  const count = Math.min(left.length, right.length);
  if (count === 0 || sampleRate <= 0.0) {
    return AUDIO_DBFS_FLOOR;
  }
  const windowSamples = Math.round(windowSeconds * sampleRate);
  const leftMeanSquare = kWeightedMeanSquare(left, sampleRate, windowSamples);
  const rightMeanSquare = kWeightedMeanSquare(right, sampleRate, windowSamples);
  const weightedSum = leftMeanSquare + rightMeanSquare; // L = R = 1.0
  if (weightedSum <= 0.0) {
    return AUDIO_DBFS_FLOOR;
  }
  const lufs = -0.691 + 10.0 * Math.log10(weightedSum);
  return lufs < AUDIO_DBFS_FLOOR ? AUDIO_DBFS_FLOOR : lufs;
}

/** Momentary (400 ms) loudness in LUFS for a stereo signal. */
export function computeMomentaryLufs(left: PcmSamples, right: PcmSamples, sampleRate: number): number {
  return computeStereoLoudnessLufs(left, right, sampleRate, 0.4);
}

/** Short-term (3 s) loudness in LUFS for a stereo signal. */
export function computeShortTermLufs(left: PcmSamples, right: PcmSamples, sampleRate: number): number {
  return computeStereoLoudnessLufs(left, right, sampleRate, 3.0);
}

// ---------------------------------------------------------------------------
// Peak limiter (brickwall / look-ahead).
//
// Scans the whole buffer (look-ahead over the block), computes a single static
// gain that guarantees the output sample peak does not exceed `thresholdDbfs`,
// and applies it in place. A final clamp catches floating-point overshoot so
// the brickwall holds exactly. Deterministic: same input -> same output and the
// same reported gain reduction. Returns the gain reduction applied in dB
// (>= 0; 0 when the signal was already at or under threshold).
// ---------------------------------------------------------------------------

/**
 * Apply the brickwall peak limiter in place, returning the gain reduction in dB
 * (>= 0). Operates on a `Float32Array` so the mutation is observable.
 */
export function applyPeakLimiter(samples: Float32Array, thresholdDbfs: number): number {
  const count = samples.length;
  if (count === 0) {
    return 0.0;
  }
  const threshold = dbfsToLinear(thresholdDbfs);
  let peak = 0.0;
  for (let index = 0; index < count; index += 1) {
    const magnitude = Math.abs(samples[index]);
    if (magnitude > peak) {
      peak = magnitude;
    }
  }
  if (peak <= threshold || peak <= 0.0) {
    return 0.0;
  }
  const gain = threshold / peak;
  for (let index = 0; index < count; index += 1) {
    let value = samples[index] * gain;
    // Guard against floating-point overshoot so the brickwall holds exactly.
    value = Math.max(-threshold, Math.min(threshold, value));
    samples[index] = value;
  }
  return -linearToDbfs(gain); // gain < 1 -> negative dB; report the magnitude (>0)
}

/**
 * Compute the gain reduction (dB, >= 0) the brickwall peak limiter would apply
 * to a buffer at `thresholdDbfs`, without mutating the caller's samples.
 */
export function peakLimiterGainReductionDb(samples: PcmSamples, thresholdDbfs: number): number {
  const count = samples.length;
  if (count === 0) {
    return 0.0;
  }
  const threshold = dbfsToLinear(thresholdDbfs);
  let peak = 0.0;
  for (let index = 0; index < count; index += 1) {
    const magnitude = Math.abs(samples[index]);
    if (magnitude > peak) {
      peak = magnitude;
    }
  }
  if (peak <= threshold || peak <= 0.0) {
    return 0.0;
  }
  const gain = threshold / peak;
  return -linearToDbfs(gain);
}
