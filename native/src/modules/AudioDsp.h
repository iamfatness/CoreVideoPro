#pragma once

#include "modules/Interfaces.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <numeric>
#include <set>
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

// Coalesces multiple PCM-carrying frames from the SAME source into one frame of
// contiguous samples (arrival order preserved). The bus mixers align every
// frame at offset 0 and size buses to the LONGEST frame, so two 10ms packets
// from one source in one tick would otherwise overlap-sum into one packet's
// worth of output — corrupting the waveform AND under-producing real time
// (audio overhaul spec R3). Metadata-only frames pass through unchanged; a
// format change mid-tick starts a fresh output frame at the new layout.
inline std::vector<AudioFrame> coalescePcmAudioFramesBySource(std::vector<AudioFrame> frames) {
  std::vector<AudioFrame> coalesced;
  coalesced.reserve(frames.size());
  std::map<std::string, std::size_t> pcmIndexBySource;
  for (auto& frame : frames) {
    if (frame.pcm.empty() || frame.channels <= 0) {
      coalesced.push_back(std::move(frame));
      continue;
    }
    const auto found = pcmIndexBySource.find(frame.participantId);
    if (found != pcmIndexBySource.end()) {
      auto& target = coalesced[found->second];
      if (target.sampleRate == frame.sampleRate && target.channels == frame.channels) {
        target.requiresSteadyFeedPriming =
            target.requiresSteadyFeedPriming || frame.requiresSteadyFeedPriming;
        target.pcm.insert(target.pcm.end(), frame.pcm.begin(), frame.pcm.end());
        target.sampleCount = static_cast<int>(target.pcm.size() / static_cast<std::size_t>(target.channels));
        continue;
      }
    }
    pcmIndexBySource[frame.participantId] = coalesced.size();
    frame.sampleCount = static_cast<int>(frame.pcm.size() / static_cast<std::size_t>(frame.channels));
    coalesced.push_back(std::move(frame));
  }
  return coalesced;
}

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

// Full limiter state: the GAIN and the PEAK-HOLD DETECTOR both persist across
// blocks (a per-call detector failed the block-continuity test - the same
// stateless-across-blocks defect class this file keeps re-learning).
struct LimiterState {
  double gain = 1.0;
  double heldPeak = 0.0;
  size_t holdRemaining = 0;
};

// C7d: SMOOTHED brickwall limiter for streaming blocks. The block limiter
// above derives ONE gain from the whole block's peak — across 20ms ticks that
// gain JUMPS at every boundary (50Hz zipper distortion on a hot mix; owner-
// reported). Timeline-analysis lesson (10-min tape: click bursts EXACTLY when
// speech peaks crossed the -1dBFS ceiling): the original INSTANT attack
// dropped the gain arbitrarily far in ONE sample — a waveform kink = a click
// per engagement on a hot mic. The attack is now FAST BUT SMOOTH (~0.5ms);
// during that half-millisecond the per-sample clamp below is the true
// brickwall (sub-ms soft clipping is far less audible than gain steps). Gain
// persists across blocks through `gainState` (nullptr = stateless).
inline double applySmoothedPeakLimiter(float* samples, size_t count, int channels,
                                       double thresholdDbfs, double releaseMs, double sampleRate,
                                       LimiterState* state = nullptr) {
  if (samples == nullptr || count == 0 || channels <= 0 || sampleRate <= 0.0) {
    return 0.0;
  }
  const double threshold = dbfsToLinear(thresholdDbfs);
  const double releaseCoeff = releaseMs <= 0.0 ? 0.0 : std::exp(-1.0 / (releaseMs / 1000.0 * sampleRate));
  const double attackCoeff = std::exp(-1.0 / (0.0005 * sampleRate));  // ~0.5ms
  // Owner: "buzzing when speaking" — with the raw waveform as detector, the
  // gain recovers between glottal pulses and re-attacks on each one: gain
  // ripple at the voice's pitch rate = harmonic buzz. PEAK-HOLD detector:
  // the envelope holds the recent peak ~10ms so gain tracks syllables, never
  // individual waveform cycles.
  const size_t holdFrames = static_cast<size_t>(0.010 * sampleRate);
  LimiterState localState;
  LimiterState& st = state != nullptr ? *state : localState;
  if (!(st.gain > 0.0) || st.gain > 1.0) {
    st.gain = 1.0;  // heal an uninitialized/corrupt state
  }
  double& gain = st.gain;
  double& heldPeak = st.heldPeak;
  size_t& holdRemaining = st.holdRemaining;
  double maxReductionDb = 0.0;
  const size_t frames = count / static_cast<size_t>(channels);
  for (size_t frame = 0; frame < frames; ++frame) {
    double magnitude = 0.0;
    for (int channel = 0; channel < channels; ++channel) {
      magnitude = std::max(magnitude, std::fabs(static_cast<double>(samples[frame * channels + channel])));
    }
    if (magnitude >= heldPeak) {
      heldPeak = magnitude;
      holdRemaining = holdFrames;
    } else if (holdRemaining > 0) {
      --holdRemaining;
    } else {
      heldPeak = releaseCoeff * heldPeak + (1.0 - releaseCoeff) * magnitude;
    }
    const double required = heldPeak > threshold ? threshold / heldPeak : 1.0;
    if (required < gain) {
      gain = attackCoeff * gain + (1.0 - attackCoeff) * required;  // fast smooth attack
    } else {
      gain = releaseCoeff * gain + (1.0 - releaseCoeff) * required;  // smooth recovery
    }
    for (int channel = 0; channel < channels; ++channel) {
      double value = static_cast<double>(samples[frame * channels + channel]) * gain;
      value = std::max(-threshold, std::min(threshold, value));
      samples[frame * channels + channel] = static_cast<float>(value);
    }
    const double reductionDb = -linearToDbfs(gain);
    if (reductionDb > maxReductionDb) {
      maxReductionDb = reductionDb;
    }
  }
  return maxReductionDb;
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
// Per-insert parameter overrides (C5b): {insertName -> {param -> value}}.
using ChannelInsertSettings = std::map<std::string, std::map<std::string, double>>;

// Spec 4.2 (deferred piece, now owner-forcing): per-source sample-steady feed.
// Capture delivers ~10ms packets on the DEVICE clock while the worker consumes
// on ITS 50Hz clock — some ticks catch one packet, some two, so downstream
// blocks vary 480/960/1440 samples and every short tick is an audible hole in
// the signal (raw-path distortion with zero underruns on either side). This
// FIFO absorbs the phase jitter: audio accumulates per source, nothing is
// emitted until PRIME (2 ticks) is buffered, then every tick emits EXACTLY
// one tick of samples. Steady-state latency cost: ~20-40ms on these sources.
struct AudioFeedState {
  std::vector<float> fifo;
  int sampleRate = 0;
  int channels = 0;
  // Z2a (Ardour: every discontinuity gets a ramp): when flow RESUMES after a
  // silent/absent tick, a ~5ms linear fade-in declicks the onset (Zoom gates
  // ISO streams server-side between talk bursts - resumptions were steps).
  size_t lastEmitted = 0;
  bool hasEverEmitted = false;  // fade RESUMPTIONS only - first onsets stay bit-exact
  size_t fadeInRemaining = 0;   // frames left in the current fade
  size_t fadeInTotal = 0;
  // Loud-failure telemetry: real-time samples SHED at the FIFO cap (a worker
  // that under-ticks loses audio here — it shortened recordings' audio track
  // 3.1% vs video before the pacer catch-up fix, silently).
  size_t shedSamples = 0;
  size_t shedEvents = 0;
  // Zoom's SDK packet clock is 10 ms while the program worker is 20 ms. Prime
  // to TWO complete worker ticks before the first emission, then retain the
  // remaining tick as the same permanent cushion used by the video frame sync.
  // Non-Zoom producers do not opt in and remain pass-through.
  bool primed = false;
  bool primingRequired = false;
  size_t primeEvents = 0;
  size_t partialBlocksPrevented = 0;
  // The Zoom callback clock and the program worker clock are both nominally
  // 48 kHz, but their scheduling phase is independent.  A strict 960-frame
  // dequeue lets the one-tick reserve random-walk to zero when polls alternate
  // between 480 and 1440 frames.  We therefore consume a few frames either
  // side of nominal and resample that tiny window back to one exact output
  // tick.  This is the same elastic-jitter-buffer principle used by realtime
  // playout engines: preserve cadence without inserting periodic 20-40 ms
  // holes.  The correction is bounded to 2.5% per tick.
  size_t clockCorrectionEvents = 0;
  size_t clockCorrectionInputFrames = 0;
  size_t emptyInputTicks = 0;
};

inline std::vector<float> resampleInterleavedBlockToFrames(
    const float* input, size_t inputFrames, size_t outputFrames, size_t channels) {
  std::vector<float> output;
  if (input == nullptr || inputFrames == 0 || outputFrames == 0 || channels == 0) {
    return output;
  }
  output.resize(outputFrames * channels);
  if (inputFrames == outputFrames) {
    std::copy(input, input + inputFrames * channels, output.begin());
    return output;
  }
  if (inputFrames == 1 || outputFrames == 1) {
    for (size_t frame = 0; frame < outputFrames; ++frame) {
      for (size_t channel = 0; channel < channels; ++channel) {
        output[frame * channels + channel] = input[channel];
      }
    }
    return output;
  }

  // Map the complete input block onto the complete output block.  Adjacent
  // blocks remain ordered: after this call the FIFO erases exactly inputFrames
  // and the next block begins at the following source frame.
  const double scale = static_cast<double>(inputFrames - 1) /
                       static_cast<double>(outputFrames - 1);
  for (size_t frame = 0; frame < outputFrames; ++frame) {
    const double position = static_cast<double>(frame) * scale;
    const size_t base = static_cast<size_t>(position);
    const size_t next = (std::min)(base + 1, inputFrames - 1);
    const float fraction = static_cast<float>(position - static_cast<double>(base));
    for (size_t channel = 0; channel < channels; ++channel) {
      const float a = input[base * channels + channel];
      const float b = input[next * channels + channel];
      output[frame * channels + channel] = a + (b - a) * fraction;
    }
  }
  return output;
}

inline void applyResumeFadeIn(AudioFeedState& state, float* interleaved, size_t samples, size_t channels) {
  if (state.fadeInRemaining == 0 || channels == 0 || samples == 0) {
    return;
  }
  const size_t frames = samples / channels;
  const size_t fadeFrames = frames < state.fadeInRemaining ? frames : state.fadeInRemaining;
  const auto total = static_cast<double>(state.fadeInTotal);
  for (size_t frame = 0; frame < fadeFrames; ++frame) {
    const double progressed = static_cast<double>(state.fadeInTotal - state.fadeInRemaining + frame);
    const auto gain = static_cast<float>(progressed / total);
    for (size_t channel = 0; channel < channels; ++channel) {
      interleaved[frame * channels + channel] *= gain;
    }
  }
  state.fadeInRemaining -= fadeFrames;
}

// Block reshaper. Ordinary sources retain zero-added-latency pass-through.
// Zoom sources opt into a permanent one-tick reserve: wait until two ticks are
// buffered, emit one exact tick, and leave one in reserve. This converts the
// observed 480/1440/480 arrival jitter into full 960-frame blocks without a
// 10 ms hole reaching the bus. A real starvation disarms the source so its next
// talk spurt re-primes instead of leaking another partial block.
inline void steadyAudioFrameFeed(std::vector<AudioFrame>& frames,
                                 std::map<std::string, AudioFeedState>& states,
                                 double ticksPerSecond = 50.0) {
  std::vector<std::string> seenSources;
  for (auto& frame : frames) {
    if (frame.channels <= 0 || frame.sampleRate <= 0) {
      continue;  // metadata-only frames pass through untouched
    }
    seenSources.push_back(frame.participantId);
    auto& state = states[frame.participantId];
    if (state.sampleRate != frame.sampleRate || state.channels != frame.channels) {
      state.fifo.clear();  // format change: restart at the new layout
      state.sampleRate = frame.sampleRate;
      state.channels = frame.channels;
      state.primed = false;
    }
    state.primingRequired = state.primingRequired || frame.requiresSteadyFeedPriming;
    const bool hadInputPcm = !frame.pcm.empty();
    if (hadInputPcm) {
      state.fifo.insert(state.fifo.end(), frame.pcm.begin(), frame.pcm.end());
    }
    if (state.primingRequired) {
      state.emptyInputTicks = hadInputPcm ? 0 : state.emptyInputTicks + 1;
    }

    const size_t tickSamples =
        static_cast<size_t>(frame.sampleRate / ticksPerSecond) * static_cast<size_t>(frame.channels);
    size_t emit = 0;
    size_t consume = 0;
    if (!state.primingRequired) {
      emit = std::min(state.fifo.size(), tickSamples);
    } else {
      if (!state.primed && state.fifo.size() >= tickSamples * 2) {
        state.primed = true;
        ++state.primeEvents;
        if (state.primeEvents == 1 || state.primeEvents % 100 == 0) {
          std::fprintf(stderr,
                       "[audio] steady feed primed %s at %zu frames (%zu event%s)\n",
                       frame.participantId.c_str(),
                       state.fifo.size() / static_cast<size_t>(frame.channels),
                       state.primeEvents, state.primeEvents == 1 ? "" : "s");
        }
      }
      if (state.primed && state.fifo.size() >= tickSamples) {
        emit = tickSamples;
        const size_t channels = static_cast<size_t>(frame.channels);
        const size_t tickFrames = tickSamples / channels;
        const size_t availableFrames = state.fifo.size() / channels;
        const size_t reserveFrames = tickFrames;
        const size_t desiredFrames = availableFrames > reserveFrames
                                         ? availableFrames - reserveFrames
                                         : 0;
        const size_t maxCorrectionFrames = (std::max)(size_t{1}, tickFrames / 40);
        const size_t minConsumeFrames = tickFrames > maxCorrectionFrames
                                            ? tickFrames - maxCorrectionFrames
                                            : 1;
        const size_t maxConsumeFrames = tickFrames + maxCorrectionFrames;
        const size_t consumeFrames = (std::min)(
            availableFrames,
            (std::max)(minConsumeFrames, (std::min)(desiredFrames, maxConsumeFrames)));
        consume = consumeFrames * channels;
      } else if (!state.fifo.empty()) {
        ++state.partialBlocksPrevented;
      }
    }
    if (!state.primingRequired) {
      consume = emit;
    }
    if (emit > 0 && consume != emit) {
      frame.pcm = resampleInterleavedBlockToFrames(
          state.fifo.data(), consume / static_cast<size_t>(frame.channels),
          emit / static_cast<size_t>(frame.channels), static_cast<size_t>(frame.channels));
      ++state.clockCorrectionEvents;
      state.clockCorrectionInputFrames += consume / static_cast<size_t>(frame.channels);
    } else {
      frame.pcm.assign(state.fifo.begin(), state.fifo.begin() + static_cast<std::ptrdiff_t>(emit));
    }
    frame.sampleCount = static_cast<int>(emit / static_cast<size_t>(frame.channels));
    state.fifo.erase(state.fifo.begin(), state.fifo.begin() + static_cast<std::ptrdiff_t>(consume));
    if (emit > 0 && state.lastEmitted == 0 && state.hasEverEmitted) {
      state.fadeInTotal = static_cast<size_t>(0.005 * frame.sampleRate);
      state.fadeInRemaining = state.fadeInTotal;
    }
    applyResumeFadeIn(state, frame.pcm.data(), emit, static_cast<size_t>(frame.channels));
    if (emit > 0) {
      state.hasEverEmitted = true;
    }
    state.lastEmitted = emit;
    if (state.primingRequired && emit == 0 && state.emptyInputTicks >= 2) {
      state.fifo.clear();
      state.primed = false;
    }

    // Cap runaway accumulation (device clock slightly fast): keep at most
    // 6 ticks buffered by dropping the OLDEST audio. The drop MUST be frame-
    // aligned: an odd-count erase on interleaved stereo flips L into R for
    // every sample thereafter (owner-heard: left/right out of sync + comb
    // warble on the bursty Zoom streams that hit this cap).
    const size_t cap = tickSamples * 6;
    if (state.fifo.size() > cap) {
      size_t drop = state.fifo.size() - cap;
      drop -= drop % static_cast<size_t>(frame.channels);
      state.fifo.erase(state.fifo.begin(), state.fifo.begin() + static_cast<std::ptrdiff_t>(drop));
      // LOUD (rate-capped 1st + every 100th event per source): this shed is
      // real-time audio permanently lost to every consumer (recording mux,
      // stream, monitor). Persistent sheds mean the worker is under-ticking.
      state.shedSamples += drop;
      if (state.shedEvents++ % 100 == 0) {
        std::fprintf(stderr, "[audio] feed FIFO shed %zu samples for %s (total %zu over %zu events)\n",
                     drop, frame.participantId.c_str(), state.shedSamples, state.shedEvents);
      }
    }
  }

  // Second pass: a tick where a source delivered NO packet is exactly the
  // hole the surplus exists to fill — drain up to one tick from its FIFO as
  // a synthesized frame.
  for (auto& [sourceId, state] : states) {
    if (state.channels <= 0 || state.sampleRate <= 0) {
      continue;
    }
    bool seen = false;
    for (const auto& id : seenSources) {
      if (id == sourceId) {
        seen = true;
        break;
      }
    }
    if (seen) {
      continue;
    }
    if (state.primingRequired) {
      ++state.emptyInputTicks;
    }
    if (state.fifo.empty()) {
      state.lastEmitted = 0;  // dry: the NEXT flow onset gets a declick fade
      if (state.primingRequired && state.emptyInputTicks >= 2) {
        state.primed = false;
      }
      continue;
    }
    const size_t tickSamples =
        static_cast<size_t>(state.sampleRate / ticksPerSecond) * static_cast<size_t>(state.channels);
    size_t emit = 0;
    size_t consume = 0;
    if (!state.primingRequired) {
      emit = std::min(state.fifo.size(), tickSamples);
    } else {
      if (!state.primed && state.fifo.size() >= tickSamples * 2) {
        state.primed = true;
        ++state.primeEvents;
      }
      if (state.primed && state.fifo.size() >= tickSamples) {
        emit = tickSamples;
        const size_t channels = static_cast<size_t>(state.channels);
        const size_t tickFrames = tickSamples / channels;
        const size_t availableFrames = state.fifo.size() / channels;
        const size_t reserveFrames = tickFrames;
        const size_t desiredFrames = availableFrames > reserveFrames
                                         ? availableFrames - reserveFrames
                                         : 0;
        const size_t maxCorrectionFrames = (std::max)(size_t{1}, tickFrames / 40);
        const size_t minConsumeFrames = tickFrames > maxCorrectionFrames
                                            ? tickFrames - maxCorrectionFrames
                                            : 1;
        const size_t maxConsumeFrames = tickFrames + maxCorrectionFrames;
        const size_t consumeFrames = (std::min)(
            availableFrames,
            (std::max)(minConsumeFrames, (std::min)(desiredFrames, maxConsumeFrames)));
        consume = consumeFrames * channels;
      } else {
        ++state.partialBlocksPrevented;
      }
    }
    if (emit == 0) {
      state.lastEmitted = 0;
      if (state.primingRequired && state.emptyInputTicks >= 2) {
        state.fifo.clear();
        state.primed = false;
      }
      continue;
    }
    AudioFrame fill;
    fill.participantId = sourceId;
    fill.sampleRate = state.sampleRate;
    fill.channels = state.channels;
    if (!state.primingRequired) {
      consume = emit;
    }
    if (consume != emit) {
      fill.pcm = resampleInterleavedBlockToFrames(
          state.fifo.data(), consume / static_cast<size_t>(state.channels),
          emit / static_cast<size_t>(state.channels), static_cast<size_t>(state.channels));
      ++state.clockCorrectionEvents;
      state.clockCorrectionInputFrames += consume / static_cast<size_t>(state.channels);
    } else {
      fill.pcm.assign(state.fifo.begin(), state.fifo.begin() + static_cast<std::ptrdiff_t>(emit));
    }
    fill.sampleCount = static_cast<int>(emit / static_cast<size_t>(state.channels));
    state.fifo.erase(state.fifo.begin(), state.fifo.begin() + static_cast<std::ptrdiff_t>(consume));
    if (emit > 0 && state.lastEmitted == 0 && state.hasEverEmitted) {
      state.fadeInTotal = static_cast<size_t>(0.005 * state.sampleRate);
      state.fadeInRemaining = state.fadeInTotal;
    }
    applyResumeFadeIn(state, fill.pcm.data(), emit, static_cast<size_t>(state.channels));
    if (emit > 0) {
      state.hasEverEmitted = true;
    }
    state.lastEmitted = emit;
    frames.push_back(std::move(fill));
  }
}

// Zoom-source lesson (owner: same buzz on a Zoom source): PCM enters the mix
// at the SOURCE devices rate (Zoom SDK commonly 32k mono) and the 48k bus
// summed it raw - wrong speed + a shortfall tail every tick (50Hz buzz). All
// PCM is resampled to the bus rate at mix ingest with PERSISTENT fractional
// phase + one-frame history so block boundaries stay seamless.
struct LinearResampleState {
  double phase = 0.0;          // fractional position inside the history frame
  std::vector<float> tail;     // last source frame (per channel) from the previous block
  bool primed = false;
  int fromRate = 0;            // reset state if the source rate changes
};

inline void resampleLinearTo(std::vector<float>& pcm, int channels, int fromRate, int toRate,
                             LinearResampleState& state) {
  if (pcm.empty() || channels <= 0 || fromRate <= 0 || toRate <= 0 || fromRate == toRate) {
    return;
  }
  if (state.fromRate != fromRate) {
    state.tail.clear();
    state.primed = false;
    state.phase = 0.0;
    state.fromRate = fromRate;
  }
  const auto ch = static_cast<size_t>(channels);
  std::vector<float> source;
  source.reserve(pcm.size() + ch);
  if (state.primed) {
    source.insert(source.end(), state.tail.begin(), state.tail.end());
  }
  source.insert(source.end(), pcm.begin(), pcm.end());
  const size_t sourceFrames = source.size() / ch;
  if (sourceFrames < 2) {
    state.tail.assign(source.end() - static_cast<std::ptrdiff_t>(ch), source.end());
    state.primed = true;
    pcm.clear();
    return;
  }
  const double step = static_cast<double>(fromRate) / static_cast<double>(toRate);
  std::vector<float> out;
  out.reserve((static_cast<size_t>(static_cast<double>(sourceFrames) / step) + 2) * ch);
  double pos = state.phase;
  while (pos <= static_cast<double>(sourceFrames - 2) + 1e-9) {
    const auto base = static_cast<size_t>(pos);
    const double frac = pos - static_cast<double>(base);
    for (size_t c = 0; c < ch; ++c) {
      const double a = source[base * ch + c];
      const double b = source[(base + 1) * ch + c];
      out.push_back(static_cast<float>(a + (b - a) * frac));
    }
    pos += step;
  }
  // Keep history from the first UNPRODUCED position onward so the carried
  // phase is always in [0,1) - a negative phase under a single-frame tail
  // cast to huge unsigned indices (the first stitched sample read garbage).
  const auto keepFrom = static_cast<size_t>(pos);
  const size_t keepClamped = keepFrom < sourceFrames ? keepFrom : sourceFrames - 1;
  state.tail.assign(source.begin() + static_cast<std::ptrdiff_t>(keepClamped * ch), source.end());
  state.primed = true;
  state.phase = pos - static_cast<double>(keepClamped);
  pcm = std::move(out);
}

// ---------------------------------------------------------------------------
// A3 (VST latency compensation): a persistent compensating delay line for
// interleaved STEREO blocks. Paths that did NOT pass through a
// latency-reporting plugin are delayed by the plugin's reported latency so
// they stay time-aligned with the path that did.
//
// Delay changes ONLY on plugin selection / latency change (rare, operator
// action) and are DECLICKED: for ~5ms the output crossfades from the
// old-delay read tap to the new-delay read tap over the same history ring —
// no gap, no duplicated hard splice. The ring is allocated once at the
// capacity cap (a delay line is a fixed cost, not per-tick churn) and starts
// zero-filled, so a newly delayed path fades in from silence exactly like a
// real delay would.
// ---------------------------------------------------------------------------
inline constexpr size_t kMaxCompensationDelayFrames = 48000;  // 1s @ 48k — beyond any sane plugin

struct CompensatingDelayState {
  std::vector<float> ring;      // interleaved stereo history, fixed capacity
  size_t writeFrame = 0;        // next frame slot to write (modulo capacity)
  size_t delayFrames = 0;       // currently applied delay
  size_t previousDelayFrames = 0;
  size_t fadeRemaining = 0;     // crossfade frames left after a delay change
  size_t fadeTotal = 0;
};

inline void applyCompensatingDelay(float* stereo, size_t frames, CompensatingDelayState& state,
                                   size_t targetDelayFrames, double sampleRate) {
  if (stereo == nullptr || frames == 0) {
    return;
  }
  if (targetDelayFrames > kMaxCompensationDelayFrames) {
    targetDelayFrames = kMaxCompensationDelayFrames;
  }
  if (targetDelayFrames == 0 && state.delayFrames == 0 && state.fadeRemaining == 0 &&
      state.ring.empty()) {
    return;  // never-delayed fast path: bit-identical pass-through
  }
  const size_t capacity = kMaxCompensationDelayFrames + 1;
  if (state.ring.size() != capacity * 2) {
    state.ring.assign(capacity * 2, 0.0f);  // first activation only
    state.writeFrame = 0;
  }
  if (targetDelayFrames != state.delayFrames) {
    state.previousDelayFrames = state.delayFrames;
    state.delayFrames = targetDelayFrames;
    state.fadeTotal = static_cast<size_t>(0.005 * (sampleRate > 0.0 ? sampleRate : 48000.0));
    state.fadeRemaining = state.fadeTotal;
  }
  for (size_t frame = 0; frame < frames; ++frame) {
    const size_t writeIndex = state.writeFrame * 2;
    state.ring[writeIndex] = stereo[frame * 2];
    state.ring[writeIndex + 1] = stereo[frame * 2 + 1];
    const size_t readNew = (state.writeFrame + capacity - state.delayFrames) % capacity;
    float left = state.ring[readNew * 2];
    float right = state.ring[readNew * 2 + 1];
    if (state.fadeRemaining > 0 && state.fadeTotal > 0) {
      const size_t readOld = (state.writeFrame + capacity - state.previousDelayFrames) % capacity;
      const auto progress = static_cast<float>(
          static_cast<double>(state.fadeTotal - state.fadeRemaining) / static_cast<double>(state.fadeTotal));
      left = state.ring[readOld * 2] * (1.0f - progress) + left * progress;
      right = state.ring[readOld * 2 + 1] * (1.0f - progress) + right * progress;
      --state.fadeRemaining;
    }
    stereo[frame * 2] = left;
    stereo[frame * 2 + 1] = right;
    state.writeFrame = (state.writeFrame + 1) % capacity;
  }
}

// C7c: PERSISTENT per-source DSP state. The chain runs on 20ms blocks; without
// state carried across blocks every biquad and envelope restarts at each block
// boundary - audible as a 50Hz buzz/distortion layered on the signal (owner-
// reported on the mic). One of these lives per source for the source.s
// lifetime; the pure single-block behavior (state = nullptr) is preserved for
// deterministic tests.
struct ChannelDspState {
  // EQ cascade: one L/R state pair per section index (resized on demand;
  // reset if the cascade shape changes between blocks).
  std::vector<std::pair<AudioBiquadState, AudioBiquadState>> eqSections;
  size_t eqShapeSignature = 0;
  // Gate follower.
  double gateEnvelope = 0.0;
  double gateGain = 0.0;
  size_t gateHoldRemaining = 0;
  // Compressor smoothed gain reduction (dB).
  double compGrDb = 0.0;
  // C7d: channel limiter full state (gain + peak-hold detector).
  LimiterState limiter;
  // P2 (pull-model spec): every parameter change is a RAMP, never a step -
  // slewed values per insert/param key (~60ms time constant per block).
  std::map<std::string, double> paramSlew;
  // Fader/pan slew (mixRoutedBuses) - control changes must not zipper.
  double gainSlew = -1.0;  // <0 = uninitialized (snap to target)
  double panSlew = -2.0;   // <-1 = uninitialized
  // A3: compensating delay so this channel stays aligned with a sibling
  // channel whose chain hosts a latency-reporting plugin.
  CompensatingDelayState alignDelay;
};

// P2 helper: slew a control value toward its target across blocks. Stateless
// callers (tests) get the raw target - deterministic single-block behavior.
inline double slewParam(ChannelDspState* state, const std::string& key, double target,
                        double blockSeconds) {
  if (state == nullptr) {
    return target;
  }
  auto [it, inserted] = state->paramSlew.try_emplace(key, target);
  if (inserted) {
    return target;  // first sighting: no ramp-in
  }
  const double coeff = std::exp(-blockSeconds / 0.060);
  it->second = coeff * it->second + (1.0 - coeff) * target;
  if (std::fabs(it->second - target) < 1e-4) {
    it->second = target;  // snap when converged so steady state is exact
  }
  return it->second;
}

// Defined below (spec 4.4): per-channel insert processing over panned stereo.
inline int applyChannelInsertChain(float* stereo, size_t count, double sampleRate,
                                   const std::vector<std::string>& inserts, bool noiseSuppression,
                                   const ChannelInsertSettings* settings = nullptr,
                                   double* compGainReductionDb = nullptr,
                                   ChannelDspState* state = nullptr);

struct RoutedAudioSource {
  std::string sourceId;
  const std::vector<float>* pcm = nullptr;  // interleaved, not owned
  int channels = 1;
  double gainLinear = 1.0;  // channel-strip fader gain (linear)
  double pan = 0.0;         // -1 hard left .. 0 center .. +1 hard right
  bool muted = false;
  bool solo = false;
  // Spec 4.4: channel strip processing, applied to the panned stereo BEFORE
  // the crosspoint sends. Not owned; may be null (no inserts).
  const std::vector<std::string>* inserts = nullptr;
  bool noiseSuppression = false;
  double sampleRate = 48000.0;
  // C5b: per-insert parameter overrides; null = all defaults.
  const ChannelInsertSettings* insertSettings = nullptr;
  // C7c: persistent DSP state for this source (owned by the caller, one per
  // source lifetime). Null = stateless single-block processing.
  ChannelDspState* dspState = nullptr;
  // A3: frames of compensating delay for THIS source's panned stereo (0 = no
  // delay). The caller sets it to the active plugin latency on sources whose
  // chain does NOT host the plugin, so plugin-hosting channels aren't late vs
  // their siblings. Requires dspState (the delay is persistent by nature).
  size_t alignDelayFrames = 0;
};

struct RoutedAudioCrosspoint {
  std::string sourceId;
  std::string busId;
  double gainLinear = 1.0;  // per-crosspoint send gain (linear)
};

// Optional out-of-process insert hook. Return true when the insert name is
// owned by the external host, including fail-open bypass; false leaves it for
// CoreVideo's built-in DSP matcher. The hook runs on the already-panned stereo
// source block so a channel VST processes before its crosspoint sends.
using ExternalAudioInsertProcessor =
    std::function<bool(const std::string& insertName, std::vector<float>& stereo, double sampleRate)>;

inline std::map<std::string, std::vector<float>> mixRoutedBuses(
    const std::vector<RoutedAudioSource>& sources, const std::vector<RoutedAudioCrosspoint>& crosspoints,
    bool masterLimiter = true,
    std::map<std::string, double>* outCompGainReductionDbBySource = nullptr,
    std::map<std::string, LimiterState>* busLimiterGainStates = nullptr,
    ExternalAudioInsertProcessor externalInsertProcessor = {}) {
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
    // P2: fader and pan RAMP (Ardour: every control change is a ramp) - raw
    // per-tick values stepped audibly (zipper) when the operator moved a
    // control during speech. Slew ~60ms via the per-source DSP state.
    double gainNow = source.gainLinear;
    double panNow = source.pan;
    if (source.dspState != nullptr) {
      const double blockSec = static_cast<double>(frames) / (source.sampleRate > 0.0 ? source.sampleRate : 48000.0);
      const double coeff = std::exp(-blockSec / 0.060);
      auto& st = *source.dspState;
      if (st.gainSlew < 0.0) st.gainSlew = gainNow;
      if (st.panSlew < -1.0) st.panSlew = panNow;
      st.gainSlew = coeff * st.gainSlew + (1.0 - coeff) * gainNow;
      st.panSlew = coeff * st.panSlew + (1.0 - coeff) * panNow;
      if (std::fabs(st.gainSlew - gainNow) < 1e-4) st.gainSlew = gainNow;
      if (std::fabs(st.panSlew - panNow) < 1e-4) st.panSlew = panNow;
      gainNow = st.gainSlew;
      panNow = st.panSlew;
    }
    // Linear balance: panning toward one side attenuates the opposite channel.
    const double leftWeight = panNow <= 0.0 ? 1.0 : (1.0 - panNow);
    const double rightWeight = panNow >= 0.0 ? 1.0 : (1.0 + panNow);
    std::vector<float> stereo(frames * 2, 0.0f);
    for (size_t index = 0; index < frames; ++index) {
      const float left = (*source.pcm)[index * static_cast<size_t>(source.channels)];
      const float right =
          source.channels == 1 ? left : (*source.pcm)[index * static_cast<size_t>(source.channels) + 1];
      stereo[index * 2] = static_cast<float>(left * gainNow * leftWeight);
      stereo[index * 2 + 1] = static_cast<float>(right * gainNow * rightWeight);
    }
    // Spec 4.4: the channel's insert chain (gate/EQ/compressor/limiter) and the
    // noise-suppression toggle process the strip output before any send.
    if ((source.inserts != nullptr && !source.inserts->empty()) || source.noiseSuppression) {
      static const std::vector<std::string> kNoInserts;
      double compGrDb = 0.0;
      if (externalInsertProcessor && source.inserts != nullptr) {
        // Preserve configured chain order across native and third-party
        // processors. Noise suppression is a strip control, so it runs once
        // at the front before the ordered named inserts.
        if (source.noiseSuppression) {
          applyChannelInsertChain(stereo.data(), stereo.size(), source.sampleRate,
                                  kNoInserts, true, source.insertSettings, nullptr,
                                  source.dspState);
        }
        for (const auto& insert : *source.inserts) {
          if (externalInsertProcessor(insert, stereo, source.sampleRate)) {
            continue;
          }
          const std::vector<std::string> oneInsert{insert};
          applyChannelInsertChain(stereo.data(), stereo.size(), source.sampleRate,
                                  oneInsert, false, source.insertSettings, &compGrDb,
                                  source.dspState);
        }
      } else {
        applyChannelInsertChain(stereo.data(), stereo.size(), source.sampleRate,
                                source.inserts != nullptr ? *source.inserts : kNoInserts,
                                source.noiseSuppression, source.insertSettings, &compGrDb,
                                source.dspState);
      }
      // C7b: per-source compressor gain reduction, captured for the GR meter.
      if (outCompGainReductionDbBySource != nullptr && compGrDb > 0.0) {
        (*outCompGainReductionDbBySource)[source.sourceId] = compGrDb;
      }
    }
    // A3: compensating delay AFTER the full channel chain, BEFORE the
    // crosspoint sends — this source stays time-aligned with a sibling whose
    // chain hosts a latency-reporting plugin. The call itself is stateful and
    // declicked; a persistent 0 target with an idle state is a no-op.
    if (source.dspState != nullptr &&
        (source.alignDelayFrames > 0 || !source.dspState->alignDelay.ring.empty())) {
      applyCompensatingDelay(stereo.data(), stereo.size() / 2, source.dspState->alignDelay,
                             source.alignDelayFrames, source.sampleRate);
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
    if (masterLimiter) {
      // C7d: smoothed + stateful per bus - the old block limiter jumped its
      // gain at every 20ms boundary (zipper distortion on a hot mix).
      LimiterState* gainState = busLimiterGainStates != nullptr ? &(*busLimiterGainStates)[busId] : nullptr;
      applySmoothedPeakLimiter(bus.data(), bus.size(), 2, -1.0, 60.0, 48000.0, gainState);
    } else {
      // Spec 4.4: the limiter toggle previously gated REPORTING only — buses
      // were always limited. Off now means off, with a hard clamp as the
      // digital-full-scale safety net.
      for (auto& sample : bus) {
        sample = std::max(-1.0f, std::min(1.0f, sample));
      }
    }
  }
  return buses;
}

// Bus OUTPUT routing (mixer topology): sum one bus's mix into another, like a
// subgroup/aux feeding the master on a real desk. Aux buses were previously
// metered dead ends — routable INTO but feeding nothing (owner 2026-07-12).
struct AudioBusSend {
  std::string fromBusId;
  std::string toBusId;
  double gainLinear = 1.0;
};

// Applies the given sends in place over the mixed bus map. A missing/empty
// source bus contributes nothing; a missing TARGET bus is created zero-filled
// (a send is an explicit route — it must produce output even if nothing routes
// to the target directly). Self-sends are ignored. Returns the ids of targets
// that actually received signal (callers re-limit those — summing can exceed
// the per-bus ceiling applied in mixRoutedBuses).
inline std::set<std::string> applyBusSends(std::map<std::string, std::vector<float>>& buses,
                                           const std::vector<AudioBusSend>& sends) {
  std::set<std::string> touched;
  for (const auto& send : sends) {
    if (send.fromBusId == send.toBusId) {
      continue;
    }
    const auto from = buses.find(send.fromBusId);
    if (from == buses.end() || from->second.empty()) {
      continue;
    }
    // NOTE: take a reference AFTER potential map insertion below — inserting
    // the target can invalidate nothing for std::map, but from->second stays
    // valid regardless (map iterators/references survive insertions).
    auto& target = buses[send.toBusId];
    if (target.size() < from->second.size()) {
      target.resize(from->second.size(), 0.0f);
    }
    const auto& sourcePcm = buses[send.fromBusId];
    for (size_t index = 0; index < sourcePcm.size(); ++index) {
      target[index] += static_cast<float>(sourcePcm[index] * send.gainLinear);
    }
    touched.insert(send.toBusId);
  }
  return touched;
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
    // VST names are consumed by the ordered external-host hook before this
    // built-in-only matcher is called.
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
// Parametric EQ biquads (RBJ audio-EQ-cookbook forms) + channel insert chain
// (spec 4.4). The BS.1770 biquads above are fixed-coefficient; these builders
// produce arbitrary high-pass / peaking / shelf sections that the channel
// insert chain cascades over interleaved stereo with independent per-channel
// filter state.
// ---------------------------------------------------------------------------
inline AudioBiquadCoefficients eqHighpass(double sampleRate, double frequencyHz, double q = 0.7071) {
  const double w0 = 2.0 * 3.14159265358979323846 * frequencyHz / sampleRate;
  const double cosW0 = std::cos(w0);
  const double alpha = std::sin(w0) / (2.0 * q);
  const double a0 = 1.0 + alpha;
  AudioBiquadCoefficients coeff;
  coeff.b0 = ((1.0 + cosW0) / 2.0) / a0;
  coeff.b1 = (-(1.0 + cosW0)) / a0;
  coeff.b2 = ((1.0 + cosW0) / 2.0) / a0;
  coeff.a1 = (-2.0 * cosW0) / a0;
  coeff.a2 = (1.0 - alpha) / a0;
  return coeff;
}

inline AudioBiquadCoefficients eqPeaking(double sampleRate, double frequencyHz, double gainDb, double q = 1.0) {
  const double amplitude = std::pow(10.0, gainDb / 40.0);
  const double w0 = 2.0 * 3.14159265358979323846 * frequencyHz / sampleRate;
  const double cosW0 = std::cos(w0);
  const double alpha = std::sin(w0) / (2.0 * q);
  const double a0 = 1.0 + alpha / amplitude;
  AudioBiquadCoefficients coeff;
  coeff.b0 = (1.0 + alpha * amplitude) / a0;
  coeff.b1 = (-2.0 * cosW0) / a0;
  coeff.b2 = (1.0 - alpha * amplitude) / a0;
  coeff.a1 = (-2.0 * cosW0) / a0;
  coeff.a2 = (1.0 - alpha / amplitude) / a0;
  return coeff;
}

// Cascade biquad sections over INTERLEAVED stereo, independent state per
// channel. C7c: pass `state` to persist filter memory across blocks (the
// stateless form restarts every call - fine for one-shot buffers, buzzy for
// 20ms streaming ticks).
inline void applyStereoEqCascade(float* stereo, size_t count, const std::vector<AudioBiquadCoefficients>& sections,
                                 ChannelDspState* state = nullptr) {
  if (stereo == nullptr || count < 2 || sections.empty()) {
    return;
  }
  if (state != nullptr) {
    state->eqSections.resize(sections.size());
  }
  for (size_t sectionIndex = 0; sectionIndex < sections.size(); ++sectionIndex) {
    const auto& section = sections[sectionIndex];
    AudioBiquadState localLeft;
    AudioBiquadState localRight;
    AudioBiquadState& left = state != nullptr ? state->eqSections[sectionIndex].first : localLeft;
    AudioBiquadState& right = state != nullptr ? state->eqSections[sectionIndex].second : localRight;
    for (size_t index = 0; index + 1 < count; index += 2) {
      stereo[index] = static_cast<float>(biquadProcessSample(section, left, static_cast<double>(stereo[index])));
      stereo[index + 1] = static_cast<float>(biquadProcessSample(section, right, static_cast<double>(stereo[index + 1])));
    }
  }
}

// Channel-linked stereo noise gate: one envelope over max(|L|,|R|) per frame,
// the SAME smoothed gain applied to both channels (an unlinked gate would let
// the image wander as channels open independently). Same envelope/coefficient
// model as applyNoiseGate. Returns the gated-sample fraction [0,1].
// C7 additions: `rangeDb` is the closed-gate FLOOR (competitive gates duck to
// a range, not to digital silence — far more natural on speech), `holdMs`
// keeps the gate open after the signal drops below threshold (prevents
// inter-word chatter). Defaults preserve the original hard-gate behavior.
inline double applyStereoLinkedGate(float* stereo, size_t count, double thresholdDbfs, double attackMs,
                                    double releaseMs, double sampleRate,
                                    double rangeDb = -80.0, double holdMs = 0.0,
                                    ChannelDspState* state = nullptr) {
  if (stereo == nullptr || count < 2 || sampleRate <= 0.0) {
    return 0.0;
  }
  const double threshold = dbfsToLinear(thresholdDbfs);
  const double floorGain = dbfsToLinear(rangeDb);
  const auto timeConstantCoeff = [sampleRate](double timeMs) -> double {
    if (timeMs <= 0.0) {
      return 0.0;
    }
    const double samplesForTc = (timeMs / 1000.0) * sampleRate;
    return samplesForTc <= 0.0 ? 0.0 : std::exp(-1.0 / samplesForTc);
  };
  const double attackCoeff = timeConstantCoeff(attackMs);
  const double releaseCoeff = timeConstantCoeff(releaseMs);
  const size_t holdFrames = holdMs <= 0.0 ? 0 : static_cast<size_t>(holdMs / 1000.0 * sampleRate);
  // C7c: envelope/gain/hold persist across blocks when state is provided.
  double localEnvelope = 0.0;
  double localGain = floorGain;
  size_t localHold = 0;
  double& envelope = state != nullptr ? state->gateEnvelope : localEnvelope;
  double& gain = state != nullptr ? state->gateGain : localGain;
  size_t& holdRemaining = state != nullptr ? state->gateHoldRemaining : localHold;
  size_t gatedFrames = 0;
  const size_t frames = count / 2;
  for (size_t frame = 0; frame < frames; ++frame) {
    const double magnitude = std::max(std::fabs(static_cast<double>(stereo[frame * 2])),
                                      std::fabs(static_cast<double>(stereo[frame * 2 + 1])));
    const double envCoeff = magnitude > envelope ? attackCoeff : releaseCoeff;
    envelope = envCoeff * envelope + (1.0 - envCoeff) * magnitude;

    double targetGain;
    if (envelope >= threshold) {
      targetGain = 1.0;
      holdRemaining = holdFrames;
    } else if (holdRemaining > 0) {
      targetGain = 1.0;  // hold window: stay open between words
      --holdRemaining;
    } else {
      targetGain = floorGain;
    }

    const double gainCoeff = targetGain > gain ? attackCoeff : releaseCoeff;
    gain = gainCoeff * gain + (1.0 - gainCoeff) * targetGain;
    if (gain < 0.5) {
      ++gatedFrames;
    }
    stereo[frame * 2] = static_cast<float>(static_cast<double>(stereo[frame * 2]) * gain);
    stereo[frame * 2 + 1] = static_cast<float>(static_cast<double>(stereo[frame * 2 + 1]) * gain);
  }
  return static_cast<double>(gatedFrames) / static_cast<double>(frames);
}

// Channel-linked stereo compressor (C7: the competitive version — the old
// applyCompressor is an envelope-free waveshaper). Feed-forward design:
// peak detector over max(|L|,|R|) per frame, STATIC soft-knee gain computer,
// attack/release smoothing on the gain-reduction signal (attack when GR
// rises, release when it falls), same gain to both channels, then makeup.
// Returns the maximum gain reduction in dB (>= 0) for GR metering.
inline double applyStereoCompressor(float* stereo, size_t count, double sampleRate,
                                    double thresholdDb, double ratio, double attackMs,
                                    double releaseMs, double kneeDb, double makeupDb,
                                    ChannelDspState* state = nullptr) {
  if (stereo == nullptr || count < 2 || sampleRate <= 0.0 || ratio < 1.0) {
    return 0.0;
  }
  const auto timeCoeff = [sampleRate](double timeMs) -> double {
    if (timeMs <= 0.0) {
      return 0.0;
    }
    return std::exp(-1.0 / (timeMs / 1000.0 * sampleRate));
  };
  const double attackCoeff = timeCoeff(attackMs);
  const double releaseCoeff = timeCoeff(releaseMs);
  const double makeup = dbfsToLinear(makeupDb);
  const double knee = std::max(0.0, kneeDb);
  const double slope = 1.0 - 1.0 / ratio;

  // C7c: the smoothed GR persists across blocks when state is provided.
  double localGr = 0.0;
  double& smoothedGrDb = state != nullptr ? state->compGrDb : localGr;
  double maxGrDb = 0.0;
  const size_t frames = count / 2;
  for (size_t frame = 0; frame < frames; ++frame) {
    const double magnitude = std::max(std::fabs(static_cast<double>(stereo[frame * 2])),
                                      std::fabs(static_cast<double>(stereo[frame * 2 + 1])));
    const double levelDb = linearToDbfs(magnitude);
    const double over = levelDb - thresholdDb;

    double targetGrDb = 0.0;
    if (knee > 0.0 && over > -knee / 2.0 && over < knee / 2.0) {
      const double x = over + knee / 2.0;
      targetGrDb = slope * x * x / (2.0 * knee);  // quadratic knee interpolation
    } else if (over >= knee / 2.0) {
      targetGrDb = slope * over;
    }

    const double smoothing = targetGrDb > smoothedGrDb ? attackCoeff : releaseCoeff;
    smoothedGrDb = smoothing * smoothedGrDb + (1.0 - smoothing) * targetGrDb;
    if (smoothedGrDb > maxGrDb) {
      maxGrDb = smoothedGrDb;
    }

    const double gain = dbfsToLinear(-smoothedGrDb) * makeup;
    stereo[frame * 2] = static_cast<float>(stereo[frame * 2] * gain);
    stereo[frame * 2 + 1] = static_cast<float>(stereo[frame * 2 + 1] * gain);
  }
  return maxGrDb;
}

// Per-CHANNEL insert chain over the source's panned stereo, run inside
// mixRoutedBuses before the crosspoint sends (spec 4.4 — these were stored and
// exported but never processed). Deterministic name matching, mirroring
// applyBusInsertChain:
//   gate / noise            -> channel-linked stereo gate (-48 dBFS, 5/120 ms)
//   high-pass / low-cut/hpf -> 90 Hz high-pass
//   eq / voice              -> broadcast voice curve (90 Hz HPF + +2 dB @ 3 kHz presence)
//   compressor              -> existing program compressor (-18 dBFS, 4:1)
//   limiter                 -> existing -1 dBFS brickwall
// `noiseSuppression` (the strip toggle) gates even with no named insert.
// Returns the number of inserts that actually processed audio.
inline int applyChannelInsertChain(float* stereo, size_t count, double sampleRate,
                                   const std::vector<std::string>& inserts, bool noiseSuppression,
                                   const ChannelInsertSettings* settings,
                                   double* compGainReductionDb,
                                   ChannelDspState* state) {
  if (stereo == nullptr || count < 2 || sampleRate <= 0.0) {
    return 0;
  }

  // C5b: per-insert parameter with a default — looked up by the insert's exact
  // name (the shell sends the same names it stores on the chain).
  const auto rawParam = [settings](const std::string& insertName, const char* key, double fallback) -> double {
    if (settings == nullptr) {
      return fallback;
    }
    const auto insertIt = settings->find(insertName);
    if (insertIt == settings->end()) {
      return fallback;
    }
    const auto valueIt = insertIt->second.find(key);
    return valueIt != insertIt->second.end() ? valueIt->second : fallback;
  };
  // P2 (owner: clicking when adjusting the plugins): parameter reads are
  // SLEWED (~60ms) so slider drags ramp the DSP instead of stepping its
  // coefficients between blocks. Stateless callers get raw values.
  const double blockSeconds = sampleRate > 0.0 ? (static_cast<double>(count) / 2.0) / sampleRate : 0.02;
  const auto param = [&rawParam, state, blockSeconds](const std::string& insertName, const char* key,
                                                      double fallback) -> double {
    return slewParam(state, insertName + "/" + key, rawParam(insertName, key, fallback), blockSeconds);
  };

  int applied = 0;
  bool gated = false;
  for (const auto& insert : inserts) {
    std::string lowered;
    lowered.reserve(insert.size());
    for (const char character : insert) {
      lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    }
    if (lowered.find("gate") != std::string::npos || lowered.find("noise") != std::string::npos) {
      applyStereoLinkedGate(stereo, count,
                            std::clamp(param(insert, "thresholdDb", -48.0), -80.0, -12.0),
                            std::clamp(param(insert, "attackMs", 5.0), 0.5, 50.0),
                            std::clamp(param(insert, "releaseMs", 120.0), 20.0, 500.0),
                            sampleRate,
                            std::clamp(param(insert, "rangeDb", -60.0), -80.0, -6.0),
                            std::clamp(param(insert, "holdMs", 50.0), 0.0, 500.0),
                            state);
      gated = true;
      ++applied;
    } else if (lowered.find("high-pass") != std::string::npos || lowered.find("highpass") != std::string::npos ||
               lowered.find("low-cut") != std::string::npos || lowered.find("hpf") != std::string::npos) {
      applyStereoEqCascade(stereo, count,
                           {eqHighpass(sampleRate, std::clamp(param(insert, "highpassHz", 90.0), 20.0, 400.0))},
                           state);
      ++applied;
    } else if (lowered.find("eq") != std::string::npos || lowered.find("voice") != std::string::npos) {
      // C6b: 8-BAND parametric EQ (owner-directed). Bands sit at the standard
      // octave centers; each band's gain comes from insertSettings
      // (band1Db..band8Db, default 0 = flat). Flat bands are skipped — an
      // untouched EQ is just the low-cut.
      static constexpr double kEqBandHz[8] = {63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0};
      std::vector<AudioBiquadCoefficients> cascade;
      cascade.push_back(eqHighpass(sampleRate, std::clamp(param(insert, "highpassHz", 90.0), 20.0, 400.0)));
      for (int band = 0; band < 8; ++band) {
        const std::string key = "band" + std::to_string(band + 1) + "Db";
        const double gainDb = std::clamp(param(insert, key.c_str(), 0.0), -12.0, 12.0);
        // P2: with persistent state the topology stays CONSTANT (all 8 bands
        // always in the cascade) so a band leaving flat mid-drag cannot shift
        // section indices and orphan filter memory - a flat peaking biquad is
        // an exact pass-through, so always-on costs nothing audible.
        if (state != nullptr || std::fabs(gainDb) >= 0.05) {
          cascade.push_back(eqPeaking(sampleRate, kEqBandHz[band], gainDb, 1.4));
        }
      }
      applyStereoEqCascade(stereo, count, cascade, state);
      ++applied;
    } else if (lowered.find("compressor") != std::string::npos) {
      // C7: the real compressor (envelope + soft knee + makeup). GR of the
      // LAST compressor in the chain is reported for metering.
      const double grDb = applyStereoCompressor(
          stereo, count, sampleRate,
          std::clamp(param(insert, "thresholdDb", -18.0), -40.0, -6.0),
          std::clamp(param(insert, "ratio", 4.0), 1.0, 20.0),
          std::clamp(param(insert, "attackMs", 10.0), 0.1, 100.0),
          std::clamp(param(insert, "releaseMs", 120.0), 10.0, 1000.0),
          std::clamp(param(insert, "kneeDb", 6.0), 0.0, 24.0),
          std::clamp(param(insert, "makeupDb", 0.0), 0.0, 24.0),
          state);
      if (compGainReductionDb != nullptr) {
        *compGainReductionDb = grDb;
      }
      ++applied;
    } else if (lowered.find("limiter") != std::string::npos) {
      applySmoothedPeakLimiter(stereo, count, 2,
                               std::clamp(param(insert, "ceilingDb", -1.0), -12.0, -0.1),
                               60.0, sampleRate,
                               state != nullptr ? &state->limiter : nullptr);
      ++applied;
    }
    // Unrecognized names remain pass-through. VST names are consumed by the
    // ordered external-host hook before this built-in matcher is called.
  }
  if (noiseSuppression && !gated) {
    applyStereoLinkedGate(stereo, count, -48.0, 5.0, 120.0, sampleRate);
    ++applied;
  }
  return applied;
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
  (void)hash;
  return 0.0;
}

// When a frame carries real PCM, measure linear RMS/peak amplitude from the
// samples and fold them into the frame's level fields so the existing telemetry
// path treats them exactly like producer-supplied levels. When `pcm` is empty,
// only explicit producer metadata is trusted; missing metadata stays silent.
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
  const bool hasPcm = !frame.pcm.empty();
  const bool hasExplicitRms = frame.rmsLevel > 0.0;
  const bool hasExplicitPeak = frame.peakLevel > 0.0;
  // HONEST METERS ONLY (2026-08-09). This used to fall back to
  // deterministicRmsLevel(frame, hash) — a level SYNTHESIZED from a hash — when
  // a frame carried no PCM and no producer metadata. In a live meeting that
  // painted every participant strip with a steady fake level: seven strips
  // pulsing together while the ISO stems on disk proved six of them were
  // DIGITAL SILENCE. A meter may only show measured PCM or explicit producer
  // levels; no evidence = silence. (The synthetic path was a leftover from the
  // pre-real-audio simulated core.)
  const double rmsLevel = frame.rmsLevel > 0.0 ? bounded.rmsLevel : 0.0;
  const double peakLevel = frame.peakLevel > 0.0 ? clampAudioDouble(bounded.peakLevel, rmsLevel, 1.0) : rmsLevel;
  const double noiseFloorDb = frame.noiseFloorDb < 0.0 ? bounded.noiseFloorDb : -72.0;
  const int inputLevel = frame.voiceActive ? clampAudioInt(static_cast<int>(std::lround(rmsLevel * 100.0)), 0, 100) : 0;
  const int nominalSamplesPerPacket = std::max(1, bounded.sampleRate / 50);
  // MEASURED-SILENT PCM IS SILENT. A silent stem measures rms ~0, but
  // voiceActive defaults true and the old checks only caught rms in (0, 0.005]
  // — exactly 0.0 slipped through, so the AGC below "boosted" digital silence
  // by +6dB and reported outputLevel 24 for EVERY quiet guest: seven identical
  // meter columns fabricated from silence (live meeting, 2026-08-09, the third
  // and final meter fabricator). PCM present with no measurable level = silent.
  const bool silenceDetected = !bounded.voiceActive ||
                               (!hasPcm && !hasExplicitRms && !hasExplicitPeak) ||
                               (hasPcm && rmsLevel <= 0.005 && peakLevel <= 0.01) ||
                               (frame.rmsLevel > 0.0 && rmsLevel <= 0.005) ||
                               (frame.peakLevel > 0.0 && peakLevel <= 0.01);
  const bool clippingDetected = frame.peakLevel > 1.0 || peakLevel >= 0.98 || frame.rmsLevel > 1.0;
  int64_t timingDriftMs = 0;
  if (timing.hasPreviousTimestamp) {
    timingDriftMs = clampAudioInt64(bounded.timestampMs - (timing.previousTimestampMs + 20), -500, 500);
  }
  const int64_t avSyncOffsetMs = timing.hasMixReferenceTimestamp ? clampAudioInt64(bounded.timestampMs - timing.mixReferenceTimestampMs, -500, 500) : 0;
  const bool underrunDetected = bounded.invalidShape || bounded.sampleCount < nominalSamplesPerPacket / 2 || timingDriftMs > 40;

  int gainDb = 0;
  const int targetDelta = 68 - inputLevel;
  if (!frame.voiceActive || silenceDetected) {
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

  const bool noiseSuppression = frame.voiceActive && !silenceDetected && (noiseFloorDb > -48.0 || inputLevel < 32);
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
  } else if (silenceDetected) {
    metrics.status = "silent";
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
