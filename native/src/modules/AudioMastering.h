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
  double maxRideDb = 8.0;      // loudness ride bound (+/-)
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
};

// Process one interleaved-stereo block in place. Returns the current ride gain
// in dB (telemetry; never echo it into settings - law 5).
inline double processMasteringChain(MasteringState& state, const MasteringParams& params,
                                    float* interleaved, size_t frames, double sampleRate) {
  if (!params.enabled || interleaved == nullptr || frames == 0 || sampleRate <= 0.0) {
    return state.rideGainDb;
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

  // 2) Ride: steer toward the target over ~30s of sustained error, bounded.
  if (state.loudnessPrimed && !gated) {
    const double errorDb = params.targetLufs - state.loudnessAvgLufs;
    const double blockSeconds = static_cast<double>(frames) / sampleRate;
    const double step = errorDb * std::min(1.0, blockSeconds / 30.0);
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

  // 5) Ceiling: the shipped smooth peak limiter (persistent state).
  applySmoothedPeakLimiter(interleaved, frames * 2, 2, params.ceilingDbfs, 80.0, sampleRate,
                           &state.limiter);
  return state.rideGainDb;
}

}  // namespace corevideo::modules
