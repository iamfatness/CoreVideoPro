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
// program loudness; it must never pump). EQ / reference-matched curves and
// 4x-oversampled true-peak detection are M1.5 follow-ups (the limiter ceiling
// keeps a safety margin until true-peak lands).

#include "modules/AudioDsp.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace corevideo::modules {

struct MasteringParams {
  bool enabled = false;
  double targetLufs = -14.0;   // streaming default; -16 podcast, -23 EBU R128
  double ceilingDbfs = -1.3;   // sample-peak ceiling with margin until true-peak lands
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
  LimiterState limiter;
  // M2 rack filter/EQ states (per channel, persist across blocks).
  AudioBiquadState hpL, hpR, lpL, lpR;
  AudioBiquadState lowShelfL, lowShelfR, presenceL, presenceR, highShelfL, highShelfR;
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

  // 4) Glue compressor: gentle 2:1 above a program-relative threshold, slow
  //    attack (30ms), program release (250ms), engagement scaled by glueAmount.
  if (params.glueAmount > 0.0) {
    const double thresholdDb = params.targetLufs + 6.0;  // catches peaks of program riding at target
    const double ratio = 2.0;
    const double attackCoeff = std::exp(-1.0 / (0.030 * sampleRate));
    const double releaseCoeff = std::exp(-1.0 / (0.250 * sampleRate));
    for (size_t i = 0; i < frames; ++i) {
      const double sampleAbs = std::max(std::abs(static_cast<double>(interleaved[i * 2])),
                                        std::abs(static_cast<double>(interleaved[i * 2 + 1])));
      const double coeff = sampleAbs > state.glueEnvelope ? attackCoeff : releaseCoeff;
      state.glueEnvelope = coeff * state.glueEnvelope + (1.0 - coeff) * sampleAbs;
      const double envDb = state.glueEnvelope > 1e-9 ? 20.0 * std::log10(state.glueEnvelope) : -180.0;
      double reductionDb = 0.0;
      if (envDb > thresholdDb) {
        reductionDb = (envDb - thresholdDb) * (1.0 - 1.0 / ratio) * params.glueAmount;
      }
      // Smooth the gain trajectory itself (release side already smooth via env).
      state.glueGainDb += 0.001 * (-reductionDb - state.glueGainDb);
      const double glueLinear = std::pow(10.0, state.glueGainDb / 20.0);
      interleaved[i * 2] = static_cast<float>(interleaved[i * 2] * glueLinear);
      interleaved[i * 2 + 1] = static_cast<float>(interleaved[i * 2 + 1] * glueLinear);
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

  // 6) Ceiling: the shipped smooth peak limiter (persistent state).
  applySmoothedPeakLimiter(interleaved, frames * 2, 2, params.ceilingDbfs, 80.0, sampleRate,
                           &state.limiter);
  return state.rideGainDb;
}

}  // namespace corevideo::modules
