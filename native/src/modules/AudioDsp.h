#pragma once

#include "modules/Interfaces.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <deque>
#include <map>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

namespace corevideo::modules {

// ---------------------------------------------------------------------------
// PCM DSP kernels (F2 audio DSP core).
//
// Pure, deterministic functions that operate directly on PCM sample buffers
// (full-scale float range [-1, 1]). These are the reusable core the live audio
// path will meter and mix with; everything here is header-only, allocation-
// light, and free of platform/dev-gated code so it builds and is unit-tested in
// the default stub build. The metadata-only telemetry helpers (derived from
// AudioFrame fields) live further down and are unchanged.
// ---------------------------------------------------------------------------

// pi as a local constant so we never depend on M_PI being defined.
inline constexpr double kAudioPi = 3.14159265358979323846;

// Floor reported for digital silence, in dBFS. Real signals approach 0 dBFS at
// full scale; -120 dB is well below any measurable content and keeps the log
// math finite for empty/zero buffers.
inline constexpr double kAudioDbfsFloor = -120.0;

// Convert a non-negative linear amplitude ratio (relative to full scale 1.0) to
// dBFS, clamped to the silence floor.
inline double linearToDbfs(double linear) {
  if (!std::isfinite(linear) || linear <= 0.0) {
    return kAudioDbfsFloor;
  }
  const double db = 20.0 * std::log10(linear);
  return db < kAudioDbfsFloor ? kAudioDbfsFloor : db;
}

// dBFS -> linear amplitude ratio.
inline double dbfsToLinear(double dbfs) {
  return std::pow(10.0, dbfs / 20.0);
}

// RMS level of a PCM buffer in dBFS. A full-scale square wave -> 0 dBFS, a
// full-amplitude sine -> ~-3.01 dBFS, digital silence (or count == 0) ->
// kAudioDbfsFloor.
inline double computeRmsDbfs(const float* samples, size_t count) {
  if (samples == nullptr || count == 0) {
    return kAudioDbfsFloor;
  }
  double sumSquares = 0.0;
  for (size_t index = 0; index < count; ++index) {
    const double sample = static_cast<double>(samples[index]);
    sumSquares += sample * sample;
  }
  const double meanSquare = sumSquares / static_cast<double>(count);
  return linearToDbfs(std::sqrt(meanSquare));
}

// Peak |sample| of a PCM buffer in dBFS. Full-scale (|s| == 1) -> 0 dBFS,
// silence (or count == 0) -> kAudioDbfsFloor.
inline double computeSamplePeakDbfs(const float* samples, size_t count) {
  if (samples == nullptr || count == 0) {
    return kAudioDbfsFloor;
  }
  double peak = 0.0;
  for (size_t index = 0; index < count; ++index) {
    const double magnitude = std::fabs(static_cast<double>(samples[index]));
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
// use channel weights L = R = 1.0 per the task spec.
// ---------------------------------------------------------------------------

struct AudioBiquadCoefficients {
  double b0 = 1.0;
  double b1 = 0.0;
  double b2 = 0.0;
  double a1 = 0.0;  // normalized so a0 == 1
  double a2 = 0.0;
};

struct AudioBiquadState {
  double z1 = 0.0;
  double z2 = 0.0;
};

// Transposed-direct-form-II single-sample step.
inline double biquadProcessSample(const AudioBiquadCoefficients& coeff, AudioBiquadState& state, double input) {
  const double output = coeff.b0 * input + state.z1;
  state.z1 = coeff.b1 * input - coeff.a1 * output + state.z2;
  state.z2 = coeff.b2 * input - coeff.a2 * output;
  return output;
}

// BS.1770 K-weighting stage 1: high-shelf "head" filter.
inline AudioBiquadCoefficients bs1770Stage1(double sampleRate) {
  // Reference analog parameters (BS.1770-4 / EBU R128 derivation).
  const double f0 = 1681.974450955533;
  const double gainDb = 3.999843853973347;
  const double q = 0.7071752369554196;
  const double k = std::tan(kAudioPi * f0 / sampleRate);
  const double vh = std::pow(10.0, gainDb / 20.0);
  const double vb = std::pow(vh, 0.4996667741545416);
  const double denom = 1.0 + k / q + k * k;

  AudioBiquadCoefficients coeff;
  coeff.b0 = (vh + vb * k / q + k * k) / denom;
  coeff.b1 = 2.0 * (k * k - vh) / denom;
  coeff.b2 = (vh - vb * k / q + k * k) / denom;
  coeff.a1 = 2.0 * (k * k - 1.0) / denom;
  coeff.a2 = (1.0 - k / q + k * k) / denom;
  return coeff;
}

// BS.1770 K-weighting stage 2: RLB high-pass filter.
inline AudioBiquadCoefficients bs1770Stage2(double sampleRate) {
  const double f0 = 38.13547087602444;
  const double q = 0.5003270373238773;
  const double k = std::tan(kAudioPi * f0 / sampleRate);
  const double denom = 1.0 + k / q + k * k;

  AudioBiquadCoefficients coeff;
  coeff.b0 = 1.0;
  coeff.b1 = -2.0;
  coeff.b2 = 1.0;
  coeff.a1 = 2.0 * (k * k - 1.0) / denom;
  coeff.a2 = (1.0 - k / q + k * k) / denom;
  return coeff;
}

// Mean square of a single channel after K-weighting, over the trailing
// `windowSamples` samples (or the whole buffer when shorter / 0).
inline double kWeightedMeanSquare(const float* samples, size_t count, double sampleRate, size_t windowSamples) {
  if (samples == nullptr || count == 0 || sampleRate <= 0.0) {
    return 0.0;
  }
  const AudioBiquadCoefficients stage1 = bs1770Stage1(sampleRate);
  const AudioBiquadCoefficients stage2 = bs1770Stage2(sampleRate);
  AudioBiquadState state1;
  AudioBiquadState state2;

  const size_t window = (windowSamples == 0 || windowSamples > count) ? count : windowSamples;
  const size_t windowStart = count - window;
  double sumSquares = 0.0;
  for (size_t index = 0; index < count; ++index) {
    const double pre = biquadProcessSample(stage1, state1, static_cast<double>(samples[index]));
    const double weighted = biquadProcessSample(stage2, state2, pre);
    if (index >= windowStart) {
      sumSquares += weighted * weighted;
    }
  }
  return sumSquares / static_cast<double>(window);
}

// Loudness (LUFS) of a stereo PCM signal over a trailing window, per BS.1770.
// `left`/`right` are deinterleaved channel buffers of `count` samples each;
// channel weights are L = R = 1.0. Returns kAudioDbfsFloor for empty/zero input.
inline double computeStereoLoudnessLufs(const float* left, const float* right, size_t count, double sampleRate, double windowSeconds) {
  if (count == 0 || sampleRate <= 0.0) {
    return kAudioDbfsFloor;
  }
  const size_t windowSamples = static_cast<size_t>(std::llround(windowSeconds * sampleRate));
  const double leftMeanSquare = kWeightedMeanSquare(left, count, sampleRate, windowSamples);
  const double rightMeanSquare = kWeightedMeanSquare(right, count, sampleRate, windowSamples);
  const double weightedSum = leftMeanSquare + rightMeanSquare;  // L = R = 1.0
  if (weightedSum <= 0.0) {
    return kAudioDbfsFloor;
  }
  const double lufs = -0.691 + 10.0 * std::log10(weightedSum);
  return lufs < kAudioDbfsFloor ? kAudioDbfsFloor : lufs;
}

// Momentary (400 ms) loudness in LUFS for a stereo signal.
inline double computeMomentaryLufs(const float* left, const float* right, size_t count, double sampleRate) {
  return computeStereoLoudnessLufs(left, right, count, sampleRate, 0.400);
}

// ---------------------------------------------------------------------------
// Incremental ITU-R BS.1770-4 integrated loudness meter (stereo).
//
// Feed PCM over time; the K-weighting filters run continuously across feeds so
// there is no per-chunk discontinuity. Loudness is integrated over 400 ms gating
// blocks with 100 ms hops (75% overlap), with the two-stage gating from the
// spec: an absolute gate at -70 LUFS, then a relative gate at -10 LU below the
// mean loudness of the absolute-gated blocks. `integratedLufs()` returns the
// gated mean over all blocks observed so far (kAudioDbfsFloor when none pass the
// gate). Deterministic. Block history is capped at ~1 hour to stay bounded.
// ---------------------------------------------------------------------------
class Bs1770IntegratedMeter {
 public:
  explicit Bs1770IntegratedMeter(double sampleRate = 48000.0) { reset(sampleRate); }

  void reset(double sampleRate) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    stage1_ = bs1770Stage1(sampleRate_);
    stage2_ = bs1770Stage2(sampleRate_);
    stateL1_ = stateL2_ = stateR1_ = stateR2_ = AudioBiquadState{};
    blockSamples_ = std::max<size_t>(1, static_cast<size_t>(std::llround(0.400 * sampleRate_)));
    hopSamples_ = std::max<size_t>(1, static_cast<size_t>(std::llround(0.100 * sampleRate_)));
    ring_.assign(blockSamples_, 0.0);
    ringPos_ = 0;
    ringFilled_ = 0;
    sinceHop_ = 0;
    runningSum_ = 0.0;
    blocks_.clear();
  }

  [[nodiscard]] double sampleRateHz() const { return sampleRate_; }

  // Feed `count` sample-frames of deinterleaved stereo (mono callers pass the
  // same pointer for left and right).
  void process(const float* left, const float* right, size_t count) {
    for (size_t index = 0; index < count; ++index) {
      const double weightedL = biquadProcessSample(stage2_, stateL2_, biquadProcessSample(stage1_, stateL1_, left ? static_cast<double>(left[index]) : 0.0));
      const double weightedR = biquadProcessSample(stage2_, stateR2_, biquadProcessSample(stage1_, stateR1_, right ? static_cast<double>(right[index]) : 0.0));
      const double energy = weightedL * weightedL + weightedR * weightedR;
      // Sliding sum over the trailing 400 ms ring.
      runningSum_ += energy - ring_[ringPos_];
      ring_[ringPos_] = energy;
      ringPos_ = (ringPos_ + 1) % blockSamples_;
      if (ringFilled_ < blockSamples_) {
        ++ringFilled_;
      }
      if (++sinceHop_ >= hopSamples_) {
        sinceHop_ = 0;
        if (ringFilled_ >= blockSamples_) {
          const double meanSquare = runningSum_ / static_cast<double>(blockSamples_);
          blocks_.push_back(meanSquare > 0.0 ? meanSquare : 0.0);
          if (blocks_.size() > kMaxBlocks) {
            blocks_.pop_front();
          }
        }
      }
    }
  }

  [[nodiscard]] double integratedLufs() const {
    if (blocks_.empty()) {
      return kAudioDbfsFloor;
    }
    const auto blockLoudness = [](double meanSquare) {
      return meanSquare > 0.0 ? -0.691 + 10.0 * std::log10(meanSquare) : kAudioDbfsFloor;
    };
    // Absolute gate at -70 LUFS.
    double absSum = 0.0;
    size_t absCount = 0;
    for (double meanSquare : blocks_) {
      if (blockLoudness(meanSquare) >= -70.0) {
        absSum += meanSquare;
        ++absCount;
      }
    }
    if (absCount == 0) {
      return kAudioDbfsFloor;
    }
    // Relative gate: -10 LU below the mean loudness of the absolute-gated blocks.
    const double relativeThreshold = -0.691 + 10.0 * std::log10(absSum / static_cast<double>(absCount)) - 10.0;
    double relSum = 0.0;
    size_t relCount = 0;
    for (double meanSquare : blocks_) {
      const double loudness = blockLoudness(meanSquare);
      if (loudness >= -70.0 && loudness >= relativeThreshold) {
        relSum += meanSquare;
        ++relCount;
      }
    }
    if (relCount == 0) {
      return kAudioDbfsFloor;
    }
    return -0.691 + 10.0 * std::log10(relSum / static_cast<double>(relCount));
  }

 private:
  static constexpr size_t kMaxBlocks = 36000;  // ~1 hour of 100 ms hops
  double sampleRate_ = 48000.0;
  AudioBiquadCoefficients stage1_;
  AudioBiquadCoefficients stage2_;
  AudioBiquadState stateL1_;
  AudioBiquadState stateL2_;
  AudioBiquadState stateR1_;
  AudioBiquadState stateR2_;
  size_t blockSamples_ = 1;
  size_t hopSamples_ = 1;
  std::vector<double> ring_;
  size_t ringPos_ = 0;
  size_t ringFilled_ = 0;
  size_t sinceHop_ = 0;
  double runningSum_ = 0.0;
  std::deque<double> blocks_;
};

// Short-term (3 s) loudness in LUFS for a stereo signal.
inline double computeShortTermLufs(const float* left, const float* right, size_t count, double sampleRate) {
  return computeStereoLoudnessLufs(left, right, count, sampleRate, 3.000);
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
//
// This is the pure kernel the live master chain will later wrap with
// attack/release smoothing; the per-block look-ahead form keeps the unit test
// able to prove output peak <= threshold.
// ---------------------------------------------------------------------------
inline double applyPeakLimiter(float* samples, size_t count, double thresholdDbfs) {
  if (samples == nullptr || count == 0) {
    return 0.0;
  }
  const double threshold = dbfsToLinear(thresholdDbfs);
  double peak = 0.0;
  for (size_t index = 0; index < count; ++index) {
    const double magnitude = std::fabs(static_cast<double>(samples[index]));
    if (magnitude > peak) {
      peak = magnitude;
    }
  }
  if (peak <= threshold || peak <= 0.0) {
    return 0.0;
  }
  const double gain = threshold / peak;
  for (size_t index = 0; index < count; ++index) {
    double value = static_cast<double>(samples[index]) * gain;
    // Guard against floating-point overshoot so the brickwall holds exactly.
    value = std::max(-threshold, std::min(threshold, value));
    samples[index] = static_cast<float>(value);
  }
  return -linearToDbfs(gain);  // gain < 1 -> negative dB; report the magnitude (>0)
}

// ---------------------------------------------------------------------------
// Static downward compressor (no attack/release smoothing).
//
// Samples whose magnitude exceeds `thresholdDbfs` are reduced toward the
// threshold by `ratio` (e.g. 4.0 == 4:1): the dB amount over threshold is
// divided by the ratio. Applied in place, deterministic, and interleave-safe
// (each sample is shaped on its own magnitude). Returns the largest gain
// reduction applied in dB (>= 0; 0 when nothing crossed the threshold). This is
// a built-in dynamics stand-in for the eventual VST insert host.
// ---------------------------------------------------------------------------
inline double applyCompressor(float* samples, size_t count, double thresholdDbfs, double ratio) {
  if (samples == nullptr || count == 0 || ratio <= 1.0) {
    return 0.0;
  }
  const double threshold = dbfsToLinear(thresholdDbfs);
  double maxReductionDb = 0.0;
  for (size_t index = 0; index < count; ++index) {
    const double value = static_cast<double>(samples[index]);
    const double magnitude = std::fabs(value);
    if (magnitude <= threshold || magnitude <= 0.0) {
      continue;
    }
    const double overDb = linearToDbfs(magnitude) - thresholdDbfs;  // > 0
    const double compressedDb = thresholdDbfs + overDb / ratio;
    const double gain = dbfsToLinear(compressedDb) / magnitude;  // < 1
    samples[index] = static_cast<float>(value * gain);
    const double reductionDb = -linearToDbfs(gain);
    if (reductionDb > maxReductionDb) {
      maxReductionDb = reductionDb;
    }
  }
  return maxReductionDb;
}

// ---------------------------------------------------------------------------
// Bus mix.
//
// Sums N source PCM buffers, each scaled by a per-source linear gain, into a
// destination buffer of `count` samples, then soft-clips (or hard-clamps) the
// sum into [-1, 1]. Sources shorter than `count` contribute only their
// available samples. Deterministic and allocation-free (writes into the
// caller's destination buffer).
// ---------------------------------------------------------------------------
struct AudioBusSource {
  const float* samples = nullptr;
  size_t count = 0;
  double gain = 1.0;  // linear
};

// tanh-based soft clip normalized so an input of magnitude 1 maps to magnitude
// 1: linear near zero, smoothly saturating toward +/-1 so a hot sum never
// exceeds full scale and never introduces a hard discontinuity.
inline double softClipSample(double value) {
  if (value > 1.0) {
    value = 1.0;
  } else if (value < -1.0) {
    value = -1.0;
  }
  return std::tanh(value) / std::tanh(1.0);
}

inline void mixAudioBus(float* destination, size_t count, const std::vector<AudioBusSource>& sources, bool softClip = true) {
  if (destination == nullptr || count == 0) {
    return;
  }
  for (size_t index = 0; index < count; ++index) {
    double sum = 0.0;
    for (const auto& source : sources) {
      if (source.samples != nullptr && index < source.count) {
        sum += static_cast<double>(source.samples[index]) * source.gain;
      }
    }
    const double mixed = softClip ? softClipSample(sum) : std::max(-1.0, std::min(1.0, sum));
    destination[index] = static_cast<float>(mixed);
  }
}

// ---------------------------------------------------------------------------
// Routing-matrix bus mix (program / ISO / aux taps).
//
// Mixes participant sources into named stereo buses through the operator's
// routing-matrix crosspoints, producing real PCM the outputs (program encode,
// ISO record) consume. Each source runs its channel strip first — fader gain,
// stereo pan/balance, mute, and solo — then every crosspoint sends the panned
// source into a destination bus at the crosspoint's gain. Buses are stereo
// (interleaved L/R); mono sources are duplicated to both channels. Solo on any
// active source restricts the whole mix to soloed sources; muted sources
// contribute nothing. Each bus is brickwall-limited to -1 dBFS so summed
// crosspoints never clip. Pure and deterministic: same inputs -> same buses.
// ---------------------------------------------------------------------------
struct RoutedAudioSource {
  std::string sourceId;
  const std::vector<float>* pcm = nullptr;  // interleaved, not owned
  int channels = 1;
  double gainLinear = 1.0;  // channel-strip fader gain (linear)
  double pan = 0.0;         // -1 hard left .. 0 center .. +1 hard right
  bool muted = false;
  bool solo = false;
};

struct RoutedAudioCrosspoint {
  std::string sourceId;
  std::string busId;
  double gainLinear = 1.0;  // per-crosspoint send gain (linear)
};

inline std::map<std::string, std::vector<float>> mixRoutedBuses(
    const std::vector<RoutedAudioSource>& sources, const std::vector<RoutedAudioCrosspoint>& crosspoints) {
  bool soloActive = false;
  for (const auto& source : sources) {
    if (source.solo && !source.muted) {
      soloActive = true;
      break;
    }
  }

  // Resolve each active source to its panned stereo signal once, keyed by id.
  std::map<std::string, std::vector<float>> sourceStereo;
  for (const auto& source : sources) {
    if (source.pcm == nullptr || source.channels <= 0 || source.muted) {
      continue;
    }
    if (soloActive && !source.solo) {
      continue;
    }
    const size_t frames = source.pcm->size() / static_cast<size_t>(source.channels);
    if (frames == 0) {
      continue;
    }
    // Linear balance: panning toward one side attenuates the opposite channel.
    const double leftWeight = source.pan <= 0.0 ? 1.0 : (1.0 - source.pan);
    const double rightWeight = source.pan >= 0.0 ? 1.0 : (1.0 + source.pan);
    std::vector<float> stereo(frames * 2, 0.0f);
    for (size_t index = 0; index < frames; ++index) {
      const float left = (*source.pcm)[index * static_cast<size_t>(source.channels)];
      const float right =
          source.channels == 1 ? left : (*source.pcm)[index * static_cast<size_t>(source.channels) + 1];
      stereo[index * 2] = static_cast<float>(left * source.gainLinear * leftWeight);
      stereo[index * 2 + 1] = static_cast<float>(right * source.gainLinear * rightWeight);
    }
    sourceStereo[source.sourceId] = std::move(stereo);
  }

  std::map<std::string, std::vector<float>> buses;
  for (const auto& crosspoint : crosspoints) {
    const auto found = sourceStereo.find(crosspoint.sourceId);
    if (found == sourceStereo.end()) {
      continue;  // source missing, muted, or excluded by solo
    }
    const auto& panned = found->second;
    auto& bus = buses[crosspoint.busId];
    if (bus.size() < panned.size()) {
      bus.resize(panned.size(), 0.0f);
    }
    for (size_t index = 0; index < panned.size(); ++index) {
      bus[index] += static_cast<float>(panned[index] * crosspoint.gainLinear);
    }
  }

  for (auto& [busId, bus] : buses) {
    (void)busId;
    applyPeakLimiter(bus.data(), bus.size(), -1.0);
  }
  return buses;
}

// Apply a bus's named insert chain to its interleaved PCM, in order. Recognized
// built-in dynamics inserts process the audio (compressor, limiter); other
// inserts (EQ, gate, third-party plugins) are acknowledged pass-throughs until a
// real VST/AU insert host lands. Returns the number of inserts that actually
// processed audio. In place, deterministic.
inline int applyBusInsertChain(float* samples, size_t count, double sampleRate, const std::vector<std::string>& inserts) {
  (void)sampleRate;
  if (samples == nullptr || count == 0) {
    return 0;
  }
  int applied = 0;
  for (const auto& insert : inserts) {
    std::string lowered;
    lowered.reserve(insert.size());
    for (const char character : insert) {
      lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    }
    if (lowered.find("limiter") != std::string::npos) {
      applyPeakLimiter(samples, count, -1.0);
      ++applied;
    } else if (lowered.find("compressor") != std::string::npos) {
      applyCompressor(samples, count, -18.0, 4.0);
      ++applied;
    }
    // EQ / gate / third-party inserts: pass-through until the plugin host lands.
  }
  return applied;
}

// ---------------------------------------------------------------------------
// Noise gate (envelope follower + attack/release smoothing).
//
// A downward expander/gate: a one-pole peak envelope follower tracks the signal
// level; when the envelope sits below `thresholdDbfs` the gate closes (gain ->
// 0) and the signal is attenuated, when it rises above threshold the gate opens
// (gain -> 1) and the signal passes ~unchanged. Both the envelope and the gate
// gain are smoothed with per-sample one-pole coefficients derived from the
// attack/release times at the given sample rate, so transitions are click-free
// and the result is deterministic (same input -> same output).
//
// Applied in place. Returns the fraction of samples [0, 1] whose applied gain
// was below 0.5 (i.e. effectively gated) — 0 means the gate stayed open for the
// whole buffer, ~1 means it was closed throughout. Empty/zero-count buffers are
// safe and return 0.
// ---------------------------------------------------------------------------
inline double applyNoiseGate(float* samples, size_t count, double thresholdDbfs, double attackMs, double releaseMs, double sampleRate) {
  if (samples == nullptr || count == 0 || sampleRate <= 0.0) {
    return 0.0;
  }
  const double threshold = dbfsToLinear(thresholdDbfs);
  // One-pole smoothing coefficient for a time constant t (ms): the per-sample
  // factor that reaches ~63% of a step in t. 0 ms -> instantaneous (coeff 0).
  const auto timeConstantCoeff = [sampleRate](double timeMs) -> double {
    if (timeMs <= 0.0) {
      return 0.0;
    }
    const double samplesForTc = (timeMs / 1000.0) * sampleRate;
    if (samplesForTc <= 0.0) {
      return 0.0;
    }
    return std::exp(-1.0 / samplesForTc);
  };
  const double attackCoeff = timeConstantCoeff(attackMs);
  const double releaseCoeff = timeConstantCoeff(releaseMs);
  // Envelope follower tracks magnitude: fast attack to rising peaks, slower
  // release as the signal decays, using the same attack/release times.
  double envelope = 0.0;
  double gain = 0.0;  // start closed; opens as soon as signal exceeds threshold
  size_t gatedSamples = 0;
  for (size_t index = 0; index < count; ++index) {
    const double magnitude = std::fabs(static_cast<double>(samples[index]));
    const double envCoeff = magnitude > envelope ? attackCoeff : releaseCoeff;
    envelope = envCoeff * envelope + (1.0 - envCoeff) * magnitude;
    // Target gain: open (1) when the envelope is at/above threshold, else closed.
    const double targetGain = envelope >= threshold ? 1.0 : 0.0;
    // Smooth toward the target: attack when opening, release when closing.
    const double gainCoeff = targetGain > gain ? attackCoeff : releaseCoeff;
    gain = gainCoeff * gain + (1.0 - gainCoeff) * targetGain;
    if (gain < 0.5) {
      ++gatedSamples;
    }
    samples[index] = static_cast<float>(static_cast<double>(samples[index]) * gain);
  }
  return static_cast<double>(gatedSamples) / static_cast<double>(count);
}

// ---------------------------------------------------------------------------
// True-peak (inter-sample peak) estimator.
//
// The sample-peak meter only sees the discrete samples; the reconstructed analog
// waveform can overshoot between them (inter-sample peaks). We estimate the true
// peak by band-limited polyphase oversampling: each output sub-sample is a
// windowed-sinc (Hann-windowed) interpolation of the surrounding input samples,
// evaluated `oversampleFactor` times per input sample. Sinc reconstruction
// genuinely overshoots near steep transitions, so a signal whose samples sit
// below full scale can still report a higher true peak — exactly the inter-
// sample overshoot a sample-peak meter misses.
//
// The interpolation phase 0 reproduces the original sample exactly (sinc(0)=1,
// all other taps land on zeros of the sinc), so the original sample magnitudes
// are always included and the result is guaranteed >= the sample peak. Returns
// dBFS; silence/empty/zero-count -> kAudioDbfsFloor.
// ---------------------------------------------------------------------------
inline double computeTruePeakDbfs(const float* samples, size_t count, int oversampleFactor = 4) {
  if (samples == nullptr || count == 0) {
    return kAudioDbfsFloor;
  }
  const int factor = oversampleFactor < 1 ? 1 : oversampleFactor;
  // Half-width of the windowed-sinc kernel (taps on each side). Wider -> closer
  // to ideal reconstruction; 16 is plenty for a deterministic ISP estimate.
  const int half = 16;

  double truePeak = 0.0;
  for (size_t index = 0; index < count; ++index) {
    for (int sub = 0; sub < factor; ++sub) {
      const double fraction = static_cast<double>(sub) / static_cast<double>(factor);
      double value;
      if (sub == 0) {
        // Phase 0 is the original sample exactly; no need to convolve.
        value = static_cast<double>(samples[index]);
      } else {
        double accum = 0.0;
        for (int tap = -half + 1; tap <= half; ++tap) {
          // Clamp (replicate) at the buffer edges so a flat signal near a
          // boundary doesn't ring against zero padding and over-report.
          long long sourceIndex = static_cast<long long>(index) + tap;
          if (sourceIndex < 0) {
            sourceIndex = 0;
          } else if (sourceIndex >= static_cast<long long>(count)) {
            sourceIndex = static_cast<long long>(count) - 1;
          }
          const double x = static_cast<double>(tap) - fraction;
          double sinc;
          if (std::fabs(x) < 1e-9) {
            sinc = 1.0;
          } else {
            const double px = kAudioPi * x;
            sinc = std::sin(px) / px;
          }
          // Hann window over the kernel support to tame ringing.
          const double window = 0.5 + 0.5 * std::cos(kAudioPi * x / static_cast<double>(half));
          accum += static_cast<double>(samples[sourceIndex]) * sinc * window;
        }
        value = accum;
      }
      const double magnitude = std::fabs(value);
      if (magnitude > truePeak) {
        truePeak = magnitude;
      }
    }
  }
  return linearToDbfs(truePeak);
}

inline int clampAudioInt(int value, int minValue, int maxValue) {
  return std::max(minValue, std::min(maxValue, value));
}

inline double clampAudioDouble(double value, double minValue, double maxValue) {
  if (!std::isfinite(value)) {
    return minValue;
  }
  return std::max(minValue, std::min(maxValue, value));
}

inline int64_t clampAudioInt64(int64_t value, int64_t minValue, int64_t maxValue) {
  return std::max(minValue, std::min(maxValue, value));
}

struct AudioDspTimingReference {
  bool hasPreviousTimestamp = false;
  int64_t previousTimestampMs = 0;
  bool hasMixReferenceTimestamp = false;
  int64_t mixReferenceTimestampMs = 0;
};

struct BoundedAudioFrame {
  std::string participantId;
  int sampleRate = 48000;
  int channels = 1;
  int64_t timestampMs = 0;
  int sampleCount = 960;
  double rmsLevel = 0.0;
  double peakLevel = 0.0;
  double noiseFloorDb = -60.0;
  bool voiceActive = true;
  bool invalidShape = false;
};

inline BoundedAudioFrame boundAudioFrame(const AudioFrame& frame) {
  BoundedAudioFrame bounded;
  bounded.participantId = frame.participantId.empty() ? "unknown-audio-source" : frame.participantId;
  bounded.sampleRate = clampAudioInt(frame.sampleRate, 8000, 192000);
  bounded.channels = clampAudioInt(frame.channels, 1, 8);
  bounded.timestampMs = clampAudioInt64(frame.timestampMs, 0, 24LL * 60 * 60 * 1000);
  bounded.sampleCount = clampAudioInt(frame.sampleCount, 0, bounded.sampleRate * bounded.channels);
  bounded.rmsLevel = clampAudioDouble(frame.rmsLevel, 0.0, 1.0);
  bounded.peakLevel = clampAudioDouble(frame.peakLevel, 0.0, 1.0);
  bounded.noiseFloorDb = frame.noiseFloorDb < 0.0 ? clampAudioDouble(frame.noiseFloorDb, -96.0, -18.0) : -60.0;
  bounded.voiceActive = frame.voiceActive;
  bounded.invalidShape = frame.sampleRate != bounded.sampleRate || frame.channels != bounded.channels || frame.sampleCount != bounded.sampleCount || frame.timestampMs != bounded.timestampMs;
  return bounded;
}

inline std::uint32_t audioDspHash(const AudioFrame& frame) {
  std::uint32_t hash = 2166136261u;
  const auto mixByte = [&](std::uint8_t value) {
    hash ^= value;
    hash *= 16777619u;
  };
  for (const char value : frame.participantId) {
    mixByte(static_cast<std::uint8_t>(value));
  }
  const auto mixNumber = [&](std::int64_t value) {
    for (int index = 0; index < 8; ++index) {
      mixByte(static_cast<std::uint8_t>((value >> (index * 8)) & 0xff));
    }
  };
  mixNumber(frame.timestampMs);
  mixNumber(frame.sampleRate);
  mixNumber(frame.channels);
  mixNumber(frame.sampleCount);
  return hash;
}

inline double deterministicRmsLevel(const AudioFrame& frame, std::uint32_t hash) {
  if (frame.rmsLevel > 0.0) {
    return clampAudioDouble(frame.rmsLevel, 0.0, 1.0);
  }
  const int sampleRateBias = frame.sampleRate >= 48000 ? 5 : 0;
  const int channelBias = frame.channels > 1 ? 4 : 0;
  const int bucket = static_cast<int>(hash % 58u);
  return clampAudioDouble((18 + bucket + sampleRateBias + channelBias) / 100.0, 0.0, 0.96);
}

// When a frame carries real PCM, measure linear RMS/peak amplitude from the
// samples and fold them into the frame's level fields so the existing telemetry
// path treats them exactly like producer-supplied levels. When `pcm` is empty
// this returns the frame untouched, preserving the original metadata behavior.
inline AudioFrame meterAudioFrameFromPcm(const AudioFrame& frame) {
  if (frame.pcm.empty()) {
    return frame;
  }
  AudioFrame metered = frame;
  const double rmsDbfs = computeRmsDbfs(metered.pcm.data(), metered.pcm.size());
  const double peakDbfs = computeSamplePeakDbfs(metered.pcm.data(), metered.pcm.size());
  // Downstream level fields are linear amplitude ratios in [0, 1]; convert.
  metered.rmsLevel = clampAudioDouble(dbfsToLinear(rmsDbfs), 0.0, 1.0);
  metered.peakLevel = clampAudioDouble(dbfsToLinear(peakDbfs), 0.0, 1.0);
  return metered;
}

inline AudioParticipantMixMetrics analyzeAudioParticipantFrame(const AudioFrame& rawFrame, const AudioDspTimingReference& timing = {}) {
  const AudioFrame frame = meterAudioFrameFromPcm(rawFrame);
  const BoundedAudioFrame bounded = boundAudioFrame(frame);
  const std::uint32_t hash = audioDspHash(frame);
  const double rmsLevel = frame.rmsLevel > 0.0 ? bounded.rmsLevel : deterministicRmsLevel(frame, hash);
  const double peakLevel = frame.peakLevel > 0.0 ? clampAudioDouble(bounded.peakLevel, rmsLevel, 1.0) : clampAudioDouble(rmsLevel + 0.12 + ((hash >> 8) % 18u) / 100.0, 0.0, 1.0);
  const double noiseFloorDb = frame.noiseFloorDb < 0.0 ? bounded.noiseFloorDb : -72.0 + static_cast<double>((hash >> 16) % 22u);
  const int inputLevel = frame.voiceActive ? clampAudioInt(static_cast<int>(std::lround(rmsLevel * 100.0)), 0, 100) : 0;
  const int nominalSamplesPerPacket = std::max(1, bounded.sampleRate / 50);
  const bool silenceDetected = !bounded.voiceActive || (frame.rmsLevel > 0.0 && rmsLevel <= 0.005) || (frame.peakLevel > 0.0 && peakLevel <= 0.01);
  const bool clippingDetected = frame.peakLevel > 1.0 || peakLevel >= 0.98 || frame.rmsLevel > 1.0;
  int64_t timingDriftMs = 0;
  if (timing.hasPreviousTimestamp) {
    timingDriftMs = clampAudioInt64(bounded.timestampMs - (timing.previousTimestampMs + 20), -500, 500);
  }
  const int64_t avSyncOffsetMs = timing.hasMixReferenceTimestamp ? clampAudioInt64(bounded.timestampMs - timing.mixReferenceTimestampMs, -500, 500) : 0;
  const bool underrunDetected = bounded.invalidShape || bounded.sampleCount < nominalSamplesPerPacket / 2 || timingDriftMs > 40;

  int gainDb = 0;
  const int targetDelta = 68 - inputLevel;
  if (!frame.voiceActive) {
    gainDb = -60;
  } else if (targetDelta > 28) {
    gainDb = 6;
  } else if (targetDelta > 14) {
    gainDb = 3;
  } else if (targetDelta < -12) {
    gainDb = -4;
  } else if (targetDelta < -4) {
    gainDb = -2;
  }

  const bool noiseSuppression = frame.voiceActive && (noiseFloorDb > -48.0 || inputLevel < 32);
  int outputLevel = frame.voiceActive ? clampAudioInt(inputLevel + gainDb * 4 - (noiseSuppression ? 2 : 0), 0, 100) : 0;
  const bool limiterActive = frame.voiceActive && (peakLevel >= 0.92 || outputLevel >= 88);
  if (limiterActive) {
    outputLevel = std::min(outputLevel, 88);
  }

  AudioParticipantMixMetrics metrics;
  metrics.participantId = frame.participantId.empty() ? "unknown-audio-source" : frame.participantId;
  metrics.inputLevel = inputLevel;
  metrics.outputLevel = outputLevel;
  metrics.gainDb = gainDb;
  metrics.rmsLevel = rmsLevel;
  metrics.peakLevel = peakLevel;
  metrics.noiseFloorDb = noiseFloorDb;
  metrics.noiseSuppressionActive = noiseSuppression;
  metrics.limiterActive = limiterActive;
  metrics.underrunDetected = underrunDetected;
  metrics.clippingDetected = clippingDetected;
  metrics.silenceDetected = silenceDetected;
  metrics.muted = !frame.voiceActive;
  metrics.avSyncOffsetMs = avSyncOffsetMs;
  metrics.timingDriftMs = timingDriftMs;
  metrics.framesMixed = 1;
  if (metrics.muted) {
    metrics.status = "muted";
  } else if (limiterActive) {
    metrics.status = "limited";
  } else if (noiseSuppression) {
    metrics.status = "cleaning";
  } else if (gainDb > 0) {
    metrics.status = "boosting";
  } else if (gainDb < 0) {
    metrics.status = "ducking";
  } else {
    metrics.status = "balanced";
  }
  return metrics;
}

inline AudioMixMetrics summarizeAudioMixMetrics(std::vector<AudioParticipantMixMetrics> participants, int64_t mixedFrameCount) {
  AudioMixMetrics session;
  session.mixedFrameCount = mixedFrameCount;
  session.participantCount = static_cast<int>(participants.size());
  session.participants = std::move(participants);
  if (session.participants.empty()) {
    return session;
  }

  int audibleCount = 0;
  int outputTotal = 0;
  int boostedCount = 0;
  int duckedCount = 0;
  int cleanedCount = 0;
  int limitedCount = 0;
  int mutedCount = 0;
  for (const auto& participant : session.participants) {
    session.limiterActive = session.limiterActive || participant.limiterActive;
    if (participant.underrunDetected) ++session.underrunCount;
    if (participant.clippingDetected) ++session.clippingCount;
    if (participant.silenceDetected) ++session.silenceCount;
    session.maxAbsAvSyncOffsetMs =
        std::max<std::int64_t>(session.maxAbsAvSyncOffsetMs, std::llabs(participant.avSyncOffsetMs));
    if (participant.muted) {
      ++mutedCount;
      continue;
    }
    outputTotal += participant.outputLevel;
    ++audibleCount;
    if (participant.gainDb > 0) ++boostedCount;
    if (participant.gainDb < 0) ++duckedCount;
    if (participant.noiseSuppressionActive) ++cleanedCount;
    if (participant.limiterActive) ++limitedCount;
  }

  session.masterLevel = audibleCount > 0 ? clampAudioInt((outputTotal / audibleCount) + 8, 0, 100) : 0;
  session.limiterActive = session.limiterActive || session.masterLevel >= 88;
  session.loudnessLufs = audibleCount > 0 ? clampAudioDouble(-24.0 + session.masterLevel * 0.12, -60.0, -12.0) : -60.0;

  if (cleanedCount > 0) {
    session.warnings.emplace_back("Noise suppression active on one or more participant sources.");
  }
  if (limitedCount > 0) {
    session.warnings.emplace_back("Limiter engaged on one or more participant sources.");
  }
  if (session.underrunCount > 0) {
    session.warnings.emplace_back("Audio packet underrun or timing gap detected in native DSP mix.");
  }
  if (session.clippingCount > 0) {
    session.warnings.emplace_back("Audio clipping detected before native DSP limiting.");
  }
  if (session.silenceCount == session.participantCount && session.participantCount > 0) {
    session.warnings.emplace_back("All participant audio sources are silent.");
  }

  std::ostringstream summary;
  if (boostedCount > 0) summary << boostedCount << " boosted";
  if (duckedCount > 0) summary << (summary.tellp() > 0 ? ", " : "") << duckedCount << " ducked";
  if (cleanedCount > 0) summary << (summary.tellp() > 0 ? ", " : "") << cleanedCount << " cleaned";
  if (limitedCount > 0) summary << (summary.tellp() > 0 ? ", " : "") << limitedCount << " limited";
  if (mutedCount > 0) summary << (summary.tellp() > 0 ? ", " : "") << mutedCount << " muted";
  session.summary = summary.tellp() > 0 ? summary.str() + " in native DSP mix" : "Native DSP mix balanced";
  session.status = session.warnings.empty() ? "live" : "warning";
  return session;
}

}  // namespace corevideo::modules
