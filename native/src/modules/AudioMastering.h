#pragma once

// First-party mastering chain for the Master Bus (docs/mastering-chain-spec.md,
// owner-requested 2026-07-05). Phase M1: built-in insert, pure and unit-tested
// like every DSP kernel in AudioDsp.h. Chain:
//
//   loudness ride (slow LUFS-targeting gain) -> glue compressor -> peak limiter
//
// Design rules inherited from the week's session laws: all state persists
// across blocks; every control change is slewed (no gain steps on the data
// plane); the ride is a MINUTES-scale integrator (a mastering processor rides
// program loudness; it must never pump). B1 (master-vst-round2-spec §B1):
// the ceiling is a 4x-oversampled TRUE-PEAK limiter, glue dynamics are
// operator-exposed (defaults = the old fixed values, bit-exact), and an
// optional 3-band LR4 multiband glue mode exists (single-band remains the
// default). Every stage stays bit-identical bypass at neutral values.

#include "modules/AudioDsp.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace corevideo::modules {

struct MasteringParams {
  bool enabled = false;
  double targetLufs = -14.0;   // streaming default; -16 podcast, -23 EBU R128
  double ceilingDbfs = -1.3;   // TRUE-PEAK ceiling (B1: 4x oversampled ISP limiter)
  double glueAmount = 0.5;     // 0..1 scales the glue compressor engagement
  double maxRideDb = 12.0;     // loudness ride bound (+/-)
  // M2 rack stages (mastering-chain-spec, TG-style). 0/neutral = stage bypassed.
  double inputGainDb = 0.0;    // INPUT trim
  double highPassHz = 0.0;     // FILTER rumble cut (0 = off)
  double lowPassHz = 0.0;      // FILTER air cut (0 = off)
  double lowShelfDb = 0.0;     // TONE low shelf @ 120Hz
  double presenceDb = 0.0;     // TONE presence bell @ 3kHz
  double highShelfDb = 0.0;    // TONE high shelf @ 10kHz
  double stereoWidth = 1.0;    // SPREADER: 0 = mono, 1 = as-is, 2 = wide
  // B1 glue dynamics, exposed (master-vst-round2-spec §B1). The defaults ARE
  // the previous fixed constants, so a default params block is bit-identical
  // to pre-B1 behavior (back-compat invariant, pinned by unit test).
  double glueRatio = 2.0;      // compression ratio (>= 1; 1 = no compression)
  double glueAttackMs = 30.0;  // envelope attack
  double glueReleaseMs = 250.0;  // envelope release
  double glueMakeupDb = 0.0;   // post-glue makeup gain (0 = bypass, bit-exact)
  // B1 multiband glue: 3-band Linkwitz-Riley (LR4 @ ~200Hz / ~3kHz) with
  // per-band glue compressors sharing the character controls above plus
  // per-band trims. OFF = the exact single-band code path (owner keeps
  // single-band the default until his listening pass).
  bool glueMultiband = false;
  double glueBandLowDb = 0.0;  // per-band trim, +/-6 dB
  double glueBandMidDb = 0.0;
  double glueBandHighDb = 0.0;
};

// ---------------------------------------------------------------------------
// B1 true-peak ceiling: a 4x-oversampled inter-sample-peak limiter.
//
// The detector reuses the polyphase windowed-sinc shape of computeTruePeakDbfs
// (AudioDsp.h): between every pair of samples it evaluates 3 sub-sample points
// via a 32-tap Hann-windowed sinc, so the envelope sees the RECONSTRUCTED
// waveform's overshoot, not just the samples. The interpolator needs
// kTruePeakLookaheadFrames of future signal, so the audio path runs through a
// matching delay line (16 samples = 0.33ms @48k — inaudible, and it doubles as
// lookahead: the gain drop lands exactly on the peak, never after it).
//
// Gain computer mirrors applySmoothedPeakLimiter's persistent-state shape (the
// C7c/C7d block-boundary lesson): peak-hold detector (~10ms) so gain tracks
// syllables rather than waveform cycles, INSTANT attack at the ceiling (the
// lookahead makes this click-free: overshoot amounts are the ISP margin, ~1-2
// dB worst case, and the drop happens at the waveform peak), smooth release
// recovery. ALL state — gain, held peak, interpolator history, delay line —
// persists across 20ms ticks or it buzzes at block rate.
// ---------------------------------------------------------------------------

inline constexpr int kTruePeakOversample = 4;
inline constexpr int kTruePeakHalfTaps = 16;                    // sinc half-width (matches the meter)
inline constexpr int kTruePeakTaps = 2 * kTruePeakHalfTaps;     // taps per polyphase branch
inline constexpr size_t kTruePeakLookaheadFrames = kTruePeakHalfTaps;
inline constexpr size_t kTruePeakRingSize = 64;                 // pow2 >= tap span + lookahead
inline constexpr size_t kTruePeakRingMask = kTruePeakRingSize - 1;

// Polyphase interpolation taps for sub-sample phases 1..3 (phase 0 is the raw
// sample). Same Hann-windowed sinc as computeTruePeakDbfs; built once.
inline const std::array<std::array<double, kTruePeakTaps>, kTruePeakOversample>& truePeakPolyphaseTaps() {
  static const std::array<std::array<double, kTruePeakTaps>, kTruePeakOversample> kTaps = [] {
    std::array<std::array<double, kTruePeakTaps>, kTruePeakOversample> taps{};
    for (int sub = 1; sub < kTruePeakOversample; ++sub) {
      const double fraction = static_cast<double>(sub) / static_cast<double>(kTruePeakOversample);
      for (int tap = -kTruePeakHalfTaps + 1; tap <= kTruePeakHalfTaps; ++tap) {
        const double x = static_cast<double>(tap) - fraction;
        double sinc;
        if (std::fabs(x) < 1e-9) {
          sinc = 1.0;
        } else {
          const double px = kAudioPi * x;
          sinc = std::sin(px) / px;
        }
        const double window = 0.5 + 0.5 * std::cos(kAudioPi * x / static_cast<double>(kTruePeakHalfTaps));
        taps[sub][tap + kTruePeakHalfTaps - 1] = sinc * window;
      }
    }
    return taps;
  }();
  return kTaps;
}

struct TruePeakLimiterState {
  double gain = 1.0;
  double heldPeak = 0.0;
  size_t holdRemaining = 0;
  // Interpolator history + lookahead delay line (stereo rings, zero pre-roll).
  std::array<float, kTruePeakRingSize> histL{};
  std::array<float, kTruePeakRingSize> histR{};
  uint64_t framesWritten = 0;
};

// In-place true-peak ceiling on an interleaved-stereo block. Returns the max
// gain reduction applied in dB (telemetry parity with applySmoothedPeakLimiter).
inline double applyTruePeakCeiling(float* interleaved, size_t frames, double ceilingDbfs,
                                   double releaseMs, double sampleRate,
                                   TruePeakLimiterState& state) {
  if (interleaved == nullptr || frames == 0 || sampleRate <= 0.0) {
    return 0.0;
  }
  const double threshold = dbfsToLinear(ceilingDbfs);
  const double releaseCoeff =
      releaseMs <= 0.0 ? 0.0 : std::exp(-1.0 / (releaseMs / 1000.0 * sampleRate));
  const size_t holdFrames = static_cast<size_t>(0.010 * sampleRate);
  if (!(state.gain > 0.0) || state.gain > 1.0) {
    state.gain = 1.0;  // heal an uninitialized/corrupt state
  }
  const auto& taps = truePeakPolyphaseTaps();
  // Sample at absolute frame index n (0 before the stream started).
  const auto ringAt = [](const std::array<float, kTruePeakRingSize>& ring, int64_t n) -> double {
    return n < 0 ? 0.0 : static_cast<double>(ring[static_cast<size_t>(n) & kTruePeakRingMask]);
  };
  double maxReductionDb = 0.0;
  for (size_t frame = 0; frame < frames; ++frame) {
    const size_t writeIdx = static_cast<size_t>(state.framesWritten) & kTruePeakRingMask;
    state.histL[writeIdx] = interleaved[frame * 2];
    state.histR[writeIdx] = interleaved[frame * 2 + 1];
    ++state.framesWritten;
    // The frame leaving the delay line; its inter-sample interval [base,base+1]
    // needs taps up to base + kTruePeakHalfTaps == the newest input — that is
    // exactly why the lookahead equals the sinc half-width.
    const int64_t base = static_cast<int64_t>(state.framesWritten) - 1 -
                         static_cast<int64_t>(kTruePeakLookaheadFrames);
    double magnitude = std::max(std::fabs(ringAt(state.histL, base)),
                                std::fabs(ringAt(state.histR, base)));
    for (int sub = 1; sub < kTruePeakOversample; ++sub) {
      double accumL = 0.0;
      double accumR = 0.0;
      const auto& branch = taps[sub];
      for (int tap = -kTruePeakHalfTaps + 1; tap <= kTruePeakHalfTaps; ++tap) {
        const double coefficient = branch[tap + kTruePeakHalfTaps - 1];
        const int64_t n = base + tap;
        accumL += ringAt(state.histL, n) * coefficient;
        accumR += ringAt(state.histR, n) * coefficient;
      }
      magnitude = std::max(magnitude, std::max(std::fabs(accumL), std::fabs(accumR)));
    }
    // Peak-hold envelope (the applySmoothedPeakLimiter detector shape).
    if (magnitude >= state.heldPeak) {
      state.heldPeak = magnitude;
      state.holdRemaining = holdFrames;
    } else if (state.holdRemaining > 0) {
      --state.holdRemaining;
    } else {
      state.heldPeak = releaseCoeff * state.heldPeak + (1.0 - releaseCoeff) * magnitude;
    }
    const double required = state.heldPeak > threshold ? threshold / state.heldPeak : 1.0;
    if (required < state.gain) {
      state.gain = required;  // instant attack — the lookahead lands it on the peak
    } else {
      state.gain = releaseCoeff * state.gain + (1.0 - releaseCoeff) * required;
    }
    // Emit the delayed frame under the computed gain. No hard clamp: the true-
    // peak envelope >= the sample peak (phase 0 is included), so the gain
    // already holds the samples — clamping would re-introduce the ISP overs.
    interleaved[frame * 2] = static_cast<float>(ringAt(state.histL, base) * state.gain);
    interleaved[frame * 2 + 1] = static_cast<float>(ringAt(state.histR, base) * state.gain);
    const double reductionDb = -linearToDbfs(state.gain);
    if (reductionDb > maxReductionDb) {
      maxReductionDb = reductionDb;
    }
  }
  return maxReductionDb;
}

// Per-band dynamics state for the multiband glue mode.
struct MasteringGlueBandState {
  double envelope = 0.0;
  double gainDb = 0.0;
};

struct MasteringState {
  // Program loudness measurement: momentary blocks folded into a slow average.
  double loudnessAvgLufs = kAudioDbfsFloor;
  bool loudnessPrimed = false;
  // The ride: slow gain steering toward the target.
  double rideGainDb = 0.0;
  double slewedRideLinear = 1.0;
  // Downstream dynamics state (persist across blocks - the limiter lesson).
  double glueEnvelope = 0.0;
  double glueGainDb = 0.0;
  TruePeakLimiterState truePeak;
  // M2 rack filter/EQ states (per channel, persist across blocks).
  AudioBiquadState hpL, hpR, lpL, lpR;
  AudioBiquadState lowShelfL, lowShelfR, presenceL, presenceR, highShelfL, highShelfR;
  // B1 multiband glue: LR4 crossover + per-band compressor state, per band per
  // channel, PERSISTENT across ticks (spec §3 — the block-boundary lesson).
  // [stage][channel] for the cascaded Butterworth pairs that form each LR4.
  AudioBiquadState mbLowLp[2][2];    // f1 low-pass cascade
  AudioBiquadState mbRestHp[2][2];   // f1 high-pass cascade (mid+high path)
  AudioBiquadState mbMidLp[2][2];    // f2 low-pass cascade on the rest path
  AudioBiquadState mbHighHp[2][2];   // f2 high-pass cascade on the rest path
  AudioBiquadState mbLowAp[2];       // f2 allpass phase-align on the low band
  MasteringGlueBandState glueBands[3];  // low / mid / high
  // Scratch band buffers (kept in state so the 50Hz worker doesn't realloc).
  std::vector<float> mbScratchLow, mbScratchMid, mbScratchHigh;
};

// RBJ cookbook shelves (peaking already lives in AudioDsp.h as eqPeaking).
inline AudioBiquadCoefficients masteringLowShelf(double sampleRate, double frequencyHz, double gainDb) {
  const double a = std::pow(10.0, gainDb / 40.0);
  const double w0 = 2.0 * kAudioPi * frequencyHz / sampleRate;
  const double cosW0 = std::cos(w0);
  const double alpha = std::sin(w0) / 2.0 * std::sqrt(2.0);
  const double twoSqrtAAlpha = 2.0 * std::sqrt(a) * alpha;
  const double a0 = (a + 1.0) + (a - 1.0) * cosW0 + twoSqrtAAlpha;
  AudioBiquadCoefficients c;
  c.b0 = a * ((a + 1.0) - (a - 1.0) * cosW0 + twoSqrtAAlpha) / a0;
  c.b1 = 2.0 * a * ((a - 1.0) - (a + 1.0) * cosW0) / a0;
  c.b2 = a * ((a + 1.0) - (a - 1.0) * cosW0 - twoSqrtAAlpha) / a0;
  c.a1 = -2.0 * ((a - 1.0) + (a + 1.0) * cosW0) / a0;
  c.a2 = ((a + 1.0) + (a - 1.0) * cosW0 - twoSqrtAAlpha) / a0;
  return c;
}

inline AudioBiquadCoefficients masteringHighShelf(double sampleRate, double frequencyHz, double gainDb) {
  const double a = std::pow(10.0, gainDb / 40.0);
  const double w0 = 2.0 * kAudioPi * frequencyHz / sampleRate;
  const double cosW0 = std::cos(w0);
  const double alpha = std::sin(w0) / 2.0 * std::sqrt(2.0);
  const double twoSqrtAAlpha = 2.0 * std::sqrt(a) * alpha;
  const double a0 = (a + 1.0) - (a - 1.0) * cosW0 + twoSqrtAAlpha;
  AudioBiquadCoefficients c;
  c.b0 = a * ((a + 1.0) + (a - 1.0) * cosW0 + twoSqrtAAlpha) / a0;
  c.b1 = -2.0 * a * ((a - 1.0) + (a + 1.0) * cosW0) / a0;
  c.b2 = a * ((a + 1.0) + (a - 1.0) * cosW0 - twoSqrtAAlpha) / a0;
  c.a1 = 2.0 * ((a - 1.0) - (a + 1.0) * cosW0) / a0;
  c.a2 = ((a + 1.0) - (a - 1.0) * cosW0 - twoSqrtAAlpha) / a0;
  return c;
}

inline AudioBiquadCoefficients masteringHighPass(double sampleRate, double frequencyHz, double q = 0.707) {
  const double w0 = 2.0 * kAudioPi * frequencyHz / sampleRate;
  const double cosW0 = std::cos(w0);
  const double alpha = std::sin(w0) / (2.0 * q);
  const double a0 = 1.0 + alpha;
  AudioBiquadCoefficients c;
  c.b0 = (1.0 + cosW0) / 2.0 / a0;
  c.b1 = -(1.0 + cosW0) / a0;
  c.b2 = (1.0 + cosW0) / 2.0 / a0;
  c.a1 = -2.0 * cosW0 / a0;
  c.a2 = (1.0 - alpha) / a0;
  return c;
}

// RBJ 2nd-order allpass. Used to phase-align the multiband low band with the
// second crossover: the mid+high recombination equals AP2(f2) applied to the
// rest path, so the low band must pass the same allpass or the band sum combs
// around f2 instead of summing flat.
inline AudioBiquadCoefficients masteringAllpass(double sampleRate, double frequencyHz, double q) {
  const double w0 = 2.0 * kAudioPi * frequencyHz / sampleRate;
  const double cosW0 = std::cos(w0);
  const double alpha = std::sin(w0) / (2.0 * q);
  const double a0 = 1.0 + alpha;
  AudioBiquadCoefficients c;
  c.b0 = (1.0 - alpha) / a0;
  c.b1 = -2.0 * cosW0 / a0;
  c.b2 = (1.0 + alpha) / a0;
  c.a1 = -2.0 * cosW0 / a0;
  c.a2 = (1.0 - alpha) / a0;
  return c;
}

inline AudioBiquadCoefficients masteringLowPass(double sampleRate, double frequencyHz, double q = 0.707) {
  const double w0 = 2.0 * kAudioPi * frequencyHz / sampleRate;
  const double cosW0 = std::cos(w0);
  const double alpha = std::sin(w0) / (2.0 * q);
  const double a0 = 1.0 + alpha;
  AudioBiquadCoefficients c;
  c.b0 = (1.0 - cosW0) / 2.0 / a0;
  c.b1 = (1.0 - cosW0) / a0;
  c.b2 = (1.0 - cosW0) / 2.0 / a0;
  c.a1 = -2.0 * cosW0 / a0;
  c.a2 = (1.0 - alpha) / a0;
  return c;
}

// Exact Butterworth Q for the LR4 crossover sections (cascading two of these
// forms each 4th-order Linkwitz-Riley slope; LP4 + HP4 then sums to AP2, i.e.
// flat magnitude).
inline constexpr double kMasteringLr4Q = 0.70710678118654752440;

// One glue compressor pass over an interleaved-stereo buffer. This is the
// pre-B1 fixed glue loop verbatim with ratio/attack/release lifted to
// parameters — at ratio=2, 30ms/250ms it is arithmetically IDENTICAL to the
// old constants (bit-exact back-compat, pinned by unit test). Envelope and
// smoothed gain live in the caller's state so blocks stay continuous.
inline void masteringGlueCompress(float* interleaved, size_t frames, double thresholdDb,
                                  double ratio, double amount, double attackCoeff,
                                  double releaseCoeff, double& envelope, double& gainDb) {
  for (size_t i = 0; i < frames; ++i) {
    const double sampleAbs = std::max(std::abs(static_cast<double>(interleaved[i * 2])),
                                      std::abs(static_cast<double>(interleaved[i * 2 + 1])));
    const double coeff = sampleAbs > envelope ? attackCoeff : releaseCoeff;
    envelope = coeff * envelope + (1.0 - coeff) * sampleAbs;
    const double envDb = envelope > 1e-9 ? 20.0 * std::log10(envelope) : -180.0;
    double reductionDb = 0.0;
    if (envDb > thresholdDb) {
      reductionDb = (envDb - thresholdDb) * (1.0 - 1.0 / ratio) * amount;
    }
    // Smooth the gain trajectory itself (release side already smooth via env).
    gainDb += 0.001 * (-reductionDb - gainDb);
    const double glueLinear = std::pow(10.0, gainDb / 20.0);
    interleaved[i * 2] = static_cast<float>(interleaved[i * 2] * glueLinear);
    interleaved[i * 2 + 1] = static_cast<float>(interleaved[i * 2 + 1] * glueLinear);
  }
}

// Process one interleaved-stereo block in place. Returns the current ride gain
// in dB (telemetry; never echo it into settings - law 5).
inline double processMasteringChain(MasteringState& state, const MasteringParams& params,
                                    float* interleaved, size_t frames, double sampleRate) {
  if (!params.enabled || interleaved == nullptr || frames == 0 || sampleRate <= 0.0) {
    return state.rideGainDb;
  }

  // M2 rack, front of chain (TG order: INPUT -> FILTER -> TONE, all before the
  // dynamics). Each stage is bypassed at its neutral value so "off" is exactly
  // bit-identical to no stage. Filter/EQ biquad state persists across blocks.
  if (params.inputGainDb != 0.0) {
    const double g = std::pow(10.0, params.inputGainDb / 20.0);
    for (size_t i = 0; i < frames * 2; ++i) {
      interleaved[i] = static_cast<float>(interleaved[i] * g);
    }
  }
  auto applyStereoBiquad = [&](const AudioBiquadCoefficients& c, AudioBiquadState& sL, AudioBiquadState& sR) {
    for (size_t i = 0; i < frames; ++i) {
      interleaved[i * 2] = static_cast<float>(biquadProcessSample(c, sL, interleaved[i * 2]));
      interleaved[i * 2 + 1] = static_cast<float>(biquadProcessSample(c, sR, interleaved[i * 2 + 1]));
    }
  };
  if (params.highPassHz > 0.0) {
    applyStereoBiquad(masteringHighPass(sampleRate, params.highPassHz), state.hpL, state.hpR);
  }
  if (params.lowPassHz > 0.0 && params.lowPassHz < sampleRate / 2.0) {
    applyStereoBiquad(masteringLowPass(sampleRate, params.lowPassHz), state.lpL, state.lpR);
  }
  if (params.lowShelfDb != 0.0) {
    applyStereoBiquad(masteringLowShelf(sampleRate, 120.0, params.lowShelfDb), state.lowShelfL, state.lowShelfR);
  }
  if (params.presenceDb != 0.0) {
    applyStereoBiquad(eqPeaking(sampleRate, 3000.0, params.presenceDb, 1.0), state.presenceL, state.presenceR);
  }
  if (params.highShelfDb != 0.0) {
    applyStereoBiquad(masteringHighShelf(sampleRate, 10000.0, params.highShelfDb), state.highShelfL, state.highShelfR);
  }

  // 1) Measure: momentary loudness of this block folded into a ~20s average
  //    (one-pole). Gate near-silence so the ride never chases room tone upward
  //    at full speed.
  std::vector<float> left(frames);
  std::vector<float> right(frames);
  for (size_t i = 0; i < frames; ++i) {
    left[i] = interleaved[i * 2];
    right[i] = interleaved[i * 2 + 1];
  }
  const double momentary = computeMomentaryLufs(left.data(), right.data(), frames, sampleRate);
  const bool gated = momentary < -45.0;
  if (!gated) {
    if (!state.loudnessPrimed) {
      state.loudnessAvgLufs = momentary;
      state.loudnessPrimed = true;
    } else {
      const double blockSeconds = static_cast<double>(frames) / sampleRate;
      const double alpha = std::min(1.0, blockSeconds / 20.0);
      state.loudnessAvgLufs += alpha * (momentary - state.loudnessAvgLufs);
    }
  }

  // 2) Ride: asymmetric steering - DOWN (too loud) in ~3s, UP in ~8s. Down
  // is protective and must act fast; up is a boost and can be gentler. The
  // 200ms gain slew below keeps both inaudible as changes.
  if (state.loudnessPrimed && !gated) {
    // Steer on OUTPUT loudness (input + current ride) - steering on input
    // never sees its own correction and integrates to the clamp (windup).
    const double errorDb = params.targetLufs - (state.loudnessAvgLufs + state.rideGainDb);
    const double blockSeconds = static_cast<double>(frames) / sampleRate;
    const double timeConstant = errorDb < 0.0 ? 3.0 : 8.0;
    const double step = errorDb * std::min(1.0, blockSeconds / timeConstant);
    state.rideGainDb = std::clamp(state.rideGainDb + step, -params.maxRideDb, params.maxRideDb);
  }

  // 3) Apply the ride with a per-sample slew (~200ms full-scale) - a mastering
  //    gain change must be inaudible as a change.
  const double targetLinear = std::pow(10.0, state.rideGainDb / 20.0);
  const double slewSamples = 0.2 * sampleRate;
  const double slewStep = slewSamples > 0.0 ? std::abs(targetLinear - state.slewedRideLinear) /
                                                  std::max(1.0, slewSamples)
                                            : 0.0;
  double gain = state.slewedRideLinear;
  for (size_t i = 0; i < frames; ++i) {
    if (gain < targetLinear) {
      gain = std::min(targetLinear, gain + slewStep);
    } else if (gain > targetLinear) {
      gain = std::max(targetLinear, gain - slewStep);
    }
    interleaved[i * 2] = static_cast<float>(interleaved[i * 2] * gain);
    interleaved[i * 2 + 1] = static_cast<float>(interleaved[i * 2 + 1] * gain);
  }
  state.slewedRideLinear = gain;

  // 4) Glue compressor: program-relative threshold, character controls exposed
  //    (B1) with the old fixed values as defaults (2:1, 30ms/250ms, no makeup).
  //    Single-band by default; optional 3-band LR4 multiband mode (owner keeps
  //    single-band default until his listening pass).
  if (params.glueAmount > 0.0) {
    const double thresholdDb = params.targetLufs + 6.0;  // catches peaks of program riding at target
    const double ratio = std::max(1.0, params.glueRatio);
    const double attackSeconds = std::max(0.1, params.glueAttackMs) / 1000.0;
    const double releaseSeconds = std::max(1.0, params.glueReleaseMs) / 1000.0;
    const double attackCoeff = std::exp(-1.0 / (attackSeconds * sampleRate));
    const double releaseCoeff = std::exp(-1.0 / (releaseSeconds * sampleRate));
    if (!params.glueMultiband) {
      masteringGlueCompress(interleaved, frames, thresholdDb, ratio, params.glueAmount,
                            attackCoeff, releaseCoeff, state.glueEnvelope, state.glueGainDb);
    } else {
      // 3-band split: LR4 @ ~200Hz / ~3kHz. Tree: input -> [LP4(f1) -> AP2(f2)]
      // = low, input -> HP4(f1) = rest, rest -> LP4(f2) = mid, rest -> HP4(f2)
      // = high. The low-band allpass matches the phase rotation the mid+high
      // recombination picks up at f2, so with neutral compressors the band sum
      // reconstructs the input flat (magnitude; the overall AP2(f1)*AP2(f2)
      // phase is the standard LR trade). Crossover state is per-band
      // per-channel PERSISTENT — a per-call filter state buzzes at block rate.
      const double f1 = 200.0;
      const double f2 = 3000.0;
      const AudioBiquadCoefficients lp1 = masteringLowPass(sampleRate, f1, kMasteringLr4Q);
      const AudioBiquadCoefficients hp1 = masteringHighPass(sampleRate, f1, kMasteringLr4Q);
      const AudioBiquadCoefficients lp2 = masteringLowPass(sampleRate, f2, kMasteringLr4Q);
      const AudioBiquadCoefficients hp2 = masteringHighPass(sampleRate, f2, kMasteringLr4Q);
      const AudioBiquadCoefficients ap2 = masteringAllpass(sampleRate, f2, kMasteringLr4Q);
      auto& low = state.mbScratchLow;
      auto& mid = state.mbScratchMid;
      auto& high = state.mbScratchHigh;
      low.resize(frames * 2);
      mid.resize(frames * 2);
      high.resize(frames * 2);
      for (size_t i = 0; i < frames; ++i) {
        for (int ch = 0; ch < 2; ++ch) {
          const double x = static_cast<double>(interleaved[i * 2 + ch]);
          double lowV = biquadProcessSample(lp1, state.mbLowLp[0][ch], x);
          lowV = biquadProcessSample(lp1, state.mbLowLp[1][ch], lowV);
          lowV = biquadProcessSample(ap2, state.mbLowAp[ch], lowV);
          double rest = biquadProcessSample(hp1, state.mbRestHp[0][ch], x);
          rest = biquadProcessSample(hp1, state.mbRestHp[1][ch], rest);
          double midV = biquadProcessSample(lp2, state.mbMidLp[0][ch], rest);
          midV = biquadProcessSample(lp2, state.mbMidLp[1][ch], midV);
          double highV = biquadProcessSample(hp2, state.mbHighHp[0][ch], rest);
          highV = biquadProcessSample(hp2, state.mbHighHp[1][ch], highV);
          low[i * 2 + ch] = static_cast<float>(lowV);
          mid[i * 2 + ch] = static_cast<float>(midV);
          high[i * 2 + ch] = static_cast<float>(highV);
        }
      }
      // Per-band glue sharing the character controls; per-band trims (+/-6dB)
      // ride the recombination. Trim 0 multiplies by exactly 1.0 (bit-exact).
      masteringGlueCompress(low.data(), frames, thresholdDb, ratio, params.glueAmount,
                            attackCoeff, releaseCoeff, state.glueBands[0].envelope,
                            state.glueBands[0].gainDb);
      masteringGlueCompress(mid.data(), frames, thresholdDb, ratio, params.glueAmount,
                            attackCoeff, releaseCoeff, state.glueBands[1].envelope,
                            state.glueBands[1].gainDb);
      masteringGlueCompress(high.data(), frames, thresholdDb, ratio, params.glueAmount,
                            attackCoeff, releaseCoeff, state.glueBands[2].envelope,
                            state.glueBands[2].gainDb);
      const double trimLow = std::pow(10.0, std::clamp(params.glueBandLowDb, -6.0, 6.0) / 20.0);
      const double trimMid = std::pow(10.0, std::clamp(params.glueBandMidDb, -6.0, 6.0) / 20.0);
      const double trimHigh = std::pow(10.0, std::clamp(params.glueBandHighDb, -6.0, 6.0) / 20.0);
      for (size_t i = 0; i < frames * 2; ++i) {
        interleaved[i] = static_cast<float>(static_cast<double>(low[i]) * trimLow +
                                            static_cast<double>(mid[i]) * trimMid +
                                            static_cast<double>(high[i]) * trimHigh);
      }
    }
    if (params.glueMakeupDb != 0.0) {
      const double makeup = std::pow(10.0, params.glueMakeupDb / 20.0);
      for (size_t i = 0; i < frames * 2; ++i) {
        interleaved[i] = static_cast<float>(interleaved[i] * makeup);
      }
    }
  }

  // 5) SPREADER: mid/side stereo width (TG spreader). width 1.0 = identity,
  //    0 = mono, 2 = double the side signal. Applied before the ceiling so the
  //    limiter still guarantees the true peak after widening.
  if (params.stereoWidth != 1.0) {
    const double w = std::clamp(params.stereoWidth, 0.0, 2.0);
    for (size_t i = 0; i < frames; ++i) {
      const double l = interleaved[i * 2];
      const double r = interleaved[i * 2 + 1];
      const double mid = (l + r) * 0.5;
      const double side = (l - r) * 0.5 * w;
      interleaved[i * 2] = static_cast<float>(mid + side);
      interleaved[i * 2 + 1] = static_cast<float>(mid - side);
    }
  }

  // 6) Ceiling: TRUE-PEAK limiter (B1) — 4x oversampled inter-sample peak
  //    detection so the reconstructed waveform, not just the samples, stays
  //    under ceilingDbfs. Adds kTruePeakLookaheadFrames (16 samples, 0.33ms)
  //    of latency as detector lookahead; all state persists across ticks.
  applyTruePeakCeiling(interleaved, frames, params.ceilingDbfs, 80.0, sampleRate,
                       state.truePeak);
  return state.rideGainDb;
}

}  // namespace corevideo::modules
