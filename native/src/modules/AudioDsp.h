#pragma once

#include "modules/Interfaces.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <sstream>
#include <string>
#include <string_view>
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

// Clamp a double to [minValue, maxValue]; non-finite inputs collapse to the
// lower bound. Declared early so the F2 mixing graph below can use it.
inline double clampAudioDouble(double value, double minValue, double maxValue) {
  if (!std::isfinite(value)) {
    return minValue;
  }
  return std::max(minValue, std::min(maxValue, value));
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
// Program audio mixing graph (F2).
//
// A real, deterministic, allocation-light summing graph. It takes per-
// participant PCM, runs the per-participant chain (gain -> pan -> noise-
// suppression seam -> VST-insert seam -> bus sends), sums into the real buses
// (PGM L/R, ISO 1-8, MON, STREAM, AUX 1-2) via routing-matrix crosspoints, runs
// the master chain (true-peak limiter + BS.1770 LUFS) on the program bus, and
// exposes program / per-ISO / MON taps as interleaved-stereo float PCM at the
// graph sample rate.
//
// This is the engine-side mixing core. Real device I/O (WASAPI/ASIO) and VST3
// hosting stay as seams (see kNoiseSuppression / kVstInsert hooks below) and are
// implemented behind dev gates elsewhere; the graph itself is header-only and
// unit-tested in the default stub build.
// ---------------------------------------------------------------------------

// Canonical mixing buses. Order is fixed and mirrors isAudioRoutingBus() in
// MediaCore.cpp and the protocol bus union.
inline constexpr int kAudioBusCount = 13;
inline constexpr std::array<std::string_view, kAudioBusCount> kAudioMixBusIds = {
    "pgm-l", "pgm-r", "iso-1", "iso-2", "iso-3", "iso-4", "iso-5",
    "iso-6", "iso-7", "iso-8", "mon",   "stream", "aux-1"};

// Per-participant chain configuration fed into the graph each block.
struct AudioParticipantChainConfig {
  std::string participantId;
  double gainDb = 0.0;        // post-fader trim
  double pan = 0.0;           // [-1, 1]; -1 = hard left, +1 = hard right
  bool muted = false;
  bool solo = false;
  bool noiseSuppression = false;  // engages the NS seam (real gate applied)
  bool hasInsert = false;         // engages the VST-insert seam (passthrough)
};

// One routing crosspoint: source -> bus with linear-from-dB gain.
struct AudioCrosspoint {
  std::string sourceId;
  std::string busId;
  double gainDb = 0.0;
};

// A stereo bus tap: interleaved L/R float PCM plus the channel-stride metadata.
struct AudioBusTap {
  std::string busId;
  std::vector<float> pcm;  // interleaved stereo, size == frames * 2
  int frames = 0;
  int sampleRate = 48000;
  int channels = 2;
  [[nodiscard]] bool present() const { return frames > 0 && !pcm.empty(); }
};

// Measured master-chain metrics over the program (PGM) bus for one block.
struct AudioMasterMeasurement {
  bool programTapPresent = false;
  int programSampleCount = 0;   // stereo frames in the program tap
  double truePeakDbfs = kAudioDbfsFloor;
  double rmsDbfs = kAudioDbfsFloor;
  double momentaryLufs = kAudioDbfsFloor;
  double shortTermLufs = kAudioDbfsFloor;
  double integratedLufs = kAudioDbfsFloor;
  double gainReductionDb = 0.0;  // limiter reduction applied to the program bus
  bool limiterEngaged = false;
};

// Deinterleave a participant frame to a mono working buffer in [-1, 1]. Frames
// with real PCM use the samples directly (averaging multi-channel down to mono);
// frames without PCM synthesize from the metadata rmsLevel so the graph still
// produces deterministic non-silent program audio in the stub path.
inline std::vector<float> participantMonoSamples(const AudioFrame& frame, int frameCount, int64_t blockIndex) {
  std::vector<float> mono(static_cast<size_t>(std::max(0, frameCount)), 0.0f);
  if (frameCount <= 0) {
    return mono;
  }
  const int channels = std::max(1, frame.channels);
  if (!frame.pcm.empty()) {
    const size_t available = frame.pcm.size() / static_cast<size_t>(channels);
    const size_t copyCount = std::min(static_cast<size_t>(frameCount), available);
    for (size_t index = 0; index < copyCount; ++index) {
      double sum = 0.0;
      for (int channel = 0; channel < channels; ++channel) {
        sum += static_cast<double>(frame.pcm[index * channels + channel]);
      }
      mono[index] = static_cast<float>(sum / channels);
    }
    return mono;
  }
  // Metadata-only fallback: a deterministic tone whose amplitude tracks the
  // reported level. This keeps the program tap non-silent (so downstream outputs
  // and the recording proof see real PCM) without depending on a capture device.
  const double amplitude = clampAudioDouble(frame.rmsLevel > 0.0 ? frame.rmsLevel : 0.18, 0.0, 0.95);
  if (amplitude <= 0.0 || !frame.voiceActive) {
    return mono;
  }
  const double sampleRate = frame.sampleRate > 0 ? static_cast<double>(frame.sampleRate) : 48000.0;
  // A per-participant tone frequency derived from the id keeps sources distinct.
  std::uint32_t idHash = 2166136261u;
  for (const char value : frame.participantId) {
    idHash ^= static_cast<std::uint8_t>(value);
    idHash *= 16777619u;
  }
  const double freq = 110.0 + static_cast<double>(idHash % 660u);
  const int64_t phaseBase = blockIndex * frameCount;
  for (int index = 0; index < frameCount; ++index) {
    const double t = static_cast<double>(phaseBase + index) / sampleRate;
    mono[index] = static_cast<float>(amplitude * std::sin(2.0 * kAudioPi * freq * t));
  }
  return mono;
}

// Constant-power pan law: returns {leftGain, rightGain} for pan in [-1, 1].
inline void panGains(double pan, double& leftGain, double& rightGain) {
  pan = clampAudioDouble(pan, -1.0, 1.0);
  const double angle = (pan + 1.0) * 0.25 * kAudioPi;  // 0..pi/2
  leftGain = std::cos(angle);
  rightGain = std::sin(angle);
}

// Real noise-suppression seam: an expander/gate. Samples whose short windowed
// magnitude is below the gate floor are attenuated. Deterministic and in-place.
// (The full RNNoise/VST path replaces this behind a dev gate; this keeps the
// seam *real* rather than a label.)
inline void applyNoiseGate(std::vector<float>& samples, double floorLinear = 0.02) {
  for (auto& sample : samples) {
    if (std::fabs(static_cast<double>(sample)) < floorLinear) {
      sample *= 0.25f;
    }
  }
}

// The mixing graph. Construct per session; call processBlock() per audio tick.
class AudioMixGraph {
 public:
  void configure(int sampleRate, int frameCount) {
    sampleRate_ = sampleRate > 0 ? sampleRate : 48000;
    frameCount_ = frameCount > 0 ? frameCount : sampleRate_ / 50;
  }

  void setChannels(std::vector<AudioParticipantChainConfig> channels) { channels_ = std::move(channels); }
  void setCrosspoints(std::vector<AudioCrosspoint> crosspoints) { crosspoints_ = std::move(crosspoints); }
  void setLimiterEnabled(bool enabled) { limiterEnabled_ = enabled; }
  void setLimiterThresholdDbfs(double dbfs) { limiterThresholdDbfs_ = dbfs; }

  [[nodiscard]] int sampleRate() const { return sampleRate_; }
  [[nodiscard]] int frameCount() const { return frameCount_; }
  [[nodiscard]] const AudioBusTap& programTap() const { return programTap_; }
  [[nodiscard]] const AudioBusTap& monitorTap() const { return monitorTap_; }
  [[nodiscard]] const std::vector<AudioBusTap>& isoTaps() const { return isoTaps_; }
  [[nodiscard]] const AudioMasterMeasurement& measurement() const { return measurement_; }

  // Process one block of participant frames. Returns the master measurement.
  // `blockIndex` advances the metadata-fallback tone phase deterministically.
  AudioMasterMeasurement processBlock(const std::vector<AudioFrame>& frames, int64_t blockIndex) {
    const int frameCount = frameCount_;
    // Per-bus interleaved-stereo accumulators (bus index -> [L0,R0,L1,R1,...]).
    std::array<std::vector<double>, kAudioBusCount> busAccum;
    for (auto& accum : busAccum) {
      accum.assign(static_cast<size_t>(frameCount) * 2, 0.0);
    }

    const bool anySolo = std::any_of(channels_.begin(), channels_.end(),
                                     [](const AudioParticipantChainConfig& c) { return c.solo; });

    for (const auto& frame : frames) {
      const AudioParticipantChainConfig* config = findChannel(frame.participantId);
      const bool muted = config && config->muted;
      const bool soloMutedOut = anySolo && (!config || !config->solo);
      if (muted || soloMutedOut) {
        continue;
      }

      std::vector<float> mono = participantMonoSamples(frame, frameCount, blockIndex);

      // Per-participant chain: gain -> NS seam -> VST seam.
      const double gainLinear = config ? dbfsToLinear(clampAudioDouble(config->gainDb, -60.0, 24.0)) : 1.0;
      if (gainLinear != 1.0) {
        for (auto& sample : mono) {
          sample = static_cast<float>(static_cast<double>(sample) * gainLinear);
        }
      }
      if (config && config->noiseSuppression) {
        applyNoiseGate(mono);
      }
      // VST insert seam: a real passthrough today (samples unchanged); the dev
      // VST3 bridge replaces this call. Kept explicit so the seam is wired.
      if (config && config->hasInsert) {
        applyVstInsertSeam(mono);
      }

      double leftGain = 1.0;
      double rightGain = 1.0;
      panGains(config ? config->pan : 0.0, leftGain, rightGain);

      // Resolve this source's crosspoints. When the routing matrix has no sends
      // for this source, fall back to default program + monitor routing so the
      // graph always produces a program mix.
      bool routed = false;
      for (const auto& crosspoint : crosspoints_) {
        if (crosspoint.sourceId != frame.participantId) {
          continue;
        }
        const int busIndex = busIndexFor(crosspoint.busId);
        if (busIndex < 0) {
          continue;
        }
        routed = true;
        const double sendGain = dbfsToLinear(clampAudioDouble(crosspoint.gainDb, -60.0, 10.0));
        sumIntoBus(busAccum[static_cast<size_t>(busIndex)], mono, busIndex, leftGain, rightGain, sendGain);
      }
      if (!routed) {
        sumIntoBus(busAccum[busIndexFor("pgm-l")], mono, busIndexFor("pgm-l"), leftGain, rightGain, 1.0);
        sumIntoBus(busAccum[busIndexFor("pgm-r")], mono, busIndexFor("pgm-r"), leftGain, rightGain, 1.0);
        sumIntoBus(busAccum[busIndexFor("mon")], mono, busIndexFor("mon"), leftGain, rightGain, 1.0);
      }
    }

    // Build the program tap from the PGM-L/PGM-R buses. Each "bus" here carries a
    // stereo pair already (crosspoint routing to pgm-l contributes the left image,
    // pgm-r the right image); fold them into a single program stereo buffer.
    programTap_ = foldProgramTap(busAccum);
    monitorTap_ = foldStereoBus(busAccum[busIndexFor("mon")], "mon");
    isoTaps_.clear();
    for (int iso = 1; iso <= 8; ++iso) {
      const std::string busId = "iso-" + std::to_string(iso);
      const int busIndex = busIndexFor(busId);
      AudioBusTap tap = foldStereoBus(busAccum[busIndex], busId);
      if (tap.present()) {
        isoTaps_.push_back(std::move(tap));
      }
    }

    measurement_ = measureProgram(programTap_);
    return measurement_;
  }

 private:
  [[nodiscard]] const AudioParticipantChainConfig* findChannel(const std::string& participantId) const {
    for (const auto& channel : channels_) {
      if (channel.participantId == participantId) {
        return &channel;
      }
    }
    return nullptr;
  }

  static int busIndexFor(std::string_view busId) {
    for (int index = 0; index < kAudioBusCount; ++index) {
      if (kAudioMixBusIds[static_cast<size_t>(index)] == busId) {
        return index;
      }
    }
    // Map pgm-r explicitly even though it shares image handling.
    return -1;
  }

  // Add `mono` to a stereo bus accumulator with the participant pan and crosspoint
  // gain. For the dedicated pgm-r bus we steer the right image; all other buses
  // receive the full constant-power stereo image.
  static void sumIntoBus(std::vector<double>& busAccum, const std::vector<float>& mono, int busIndex, double leftGain, double rightGain, double sendGain) {
    if (busIndex < 0 || busAccum.empty()) {
      return;
    }
    const bool isPgmRight = kAudioMixBusIds[static_cast<size_t>(busIndex)] == "pgm-r";
    const bool isPgmLeft = kAudioMixBusIds[static_cast<size_t>(busIndex)] == "pgm-l";
    const size_t frames = busAccum.size() / 2;
    for (size_t index = 0; index < frames && index < mono.size(); ++index) {
      const double value = static_cast<double>(mono[index]) * sendGain;
      if (isPgmLeft) {
        busAccum[index * 2] += value * leftGain;
      } else if (isPgmRight) {
        busAccum[index * 2 + 1] += value * rightGain;
      } else {
        busAccum[index * 2] += value * leftGain;
        busAccum[index * 2 + 1] += value * rightGain;
      }
    }
  }

  // Combine the pgm-l (left image) and pgm-r (right image) accumulators into a
  // single interleaved-stereo program tap, soft-clipped to full scale.
  AudioBusTap foldProgramTap(const std::array<std::vector<double>, kAudioBusCount>& busAccum) const {
    const auto& left = busAccum[static_cast<size_t>(busIndexFor("pgm-l"))];
    const auto& right = busAccum[static_cast<size_t>(busIndexFor("pgm-r"))];
    const size_t frames = left.size() / 2;
    AudioBusTap tap;
    tap.busId = "pgm";
    tap.sampleRate = sampleRate_;
    tap.channels = 2;
    tap.frames = static_cast<int>(frames);
    tap.pcm.assign(frames * 2, 0.0f);
    bool anyEnergy = false;
    for (size_t index = 0; index < frames; ++index) {
      const double l = left[index * 2] + right[index * 2];
      const double r = left[index * 2 + 1] + right[index * 2 + 1];
      const float lf = static_cast<float>(softClipSample(l));
      const float rf = static_cast<float>(softClipSample(r));
      tap.pcm[index * 2] = lf;
      tap.pcm[index * 2 + 1] = rf;
      if (std::fabs(lf) > 1e-6f || std::fabs(rf) > 1e-6f) {
        anyEnergy = true;
      }
    }
    // A program bus with no signal (e.g. every source muted) is reported as
    // absent rather than as a silent-but-present tap, so downstream
    // programTapPresent reflects real audio.
    if (!anyEnergy) {
      tap.frames = 0;
      tap.pcm.clear();
    }
    return tap;
  }

  AudioBusTap foldStereoBus(const std::vector<double>& busAccum, const std::string& busId) const {
    const size_t frames = busAccum.size() / 2;
    AudioBusTap tap;
    tap.busId = busId;
    tap.sampleRate = sampleRate_;
    tap.channels = 2;
    tap.frames = static_cast<int>(frames);
    tap.pcm.assign(frames * 2, 0.0f);
    bool anyEnergy = false;
    for (size_t index = 0; index < busAccum.size(); ++index) {
      const float value = static_cast<float>(softClipSample(busAccum[index]));
      tap.pcm[index] = value;
      if (std::fabs(value) > 1e-6f) {
        anyEnergy = true;
      }
    }
    if (!anyEnergy) {
      tap.frames = 0;
      tap.pcm.clear();
    }
    return tap;
  }

  AudioMasterMeasurement measureProgram(AudioBusTap& tap) {
    AudioMasterMeasurement measurement;
    if (!tap.present()) {
      return measurement;
    }
    measurement.programTapPresent = true;
    measurement.programSampleCount = tap.frames;

    // Master limiter on the interleaved program buffer.
    if (limiterEnabled_) {
      measurement.gainReductionDb = applyPeakLimiter(tap.pcm.data(), tap.pcm.size(), limiterThresholdDbfs_);
      measurement.limiterEngaged = measurement.gainReductionDb > 0.0;
    }

    measurement.truePeakDbfs = computeSamplePeakDbfs(tap.pcm.data(), tap.pcm.size());
    measurement.rmsDbfs = computeRmsDbfs(tap.pcm.data(), tap.pcm.size());

    // Deinterleave for the BS.1770 stereo meter.
    const size_t frames = static_cast<size_t>(tap.frames);
    std::vector<float> left(frames);
    std::vector<float> right(frames);
    for (size_t index = 0; index < frames; ++index) {
      left[index] = tap.pcm[index * 2];
      right[index] = tap.pcm[index * 2 + 1];
    }
    measurement.momentaryLufs = computeMomentaryLufs(left.data(), right.data(), frames, sampleRate_);
    measurement.shortTermLufs = computeShortTermLufs(left.data(), right.data(), frames, sampleRate_);

    // Integrated loudness: accumulate the per-block K-weighted mean square and
    // report the running gated estimate. Gating: only blocks above an absolute
    // -70 LUFS threshold contribute (the BS.1770 absolute gate).
    const double leftMs = kWeightedMeanSquare(left.data(), frames, sampleRate_, frames);
    const double rightMs = kWeightedMeanSquare(right.data(), frames, sampleRate_, frames);
    const double blockSum = leftMs + rightMs;
    if (blockSum > 0.0) {
      const double blockLufs = -0.691 + 10.0 * std::log10(blockSum);
      if (blockLufs > -70.0) {
        integratedSum_ += blockSum;
        ++integratedBlocks_;
      }
    }
    if (integratedBlocks_ > 0) {
      const double meanSum = integratedSum_ / static_cast<double>(integratedBlocks_);
      measurement.integratedLufs = meanSum > 0.0 ? (-0.691 + 10.0 * std::log10(meanSum)) : kAudioDbfsFloor;
    }
    return measurement;
  }

  // Real VST-insert passthrough seam. The dev VST3 bridge replaces this body;
  // keeping it explicit ensures samples flow through the insert point.
  static void applyVstInsertSeam(std::vector<float>&) {}

  int sampleRate_ = 48000;
  int frameCount_ = 960;
  bool limiterEnabled_ = true;
  double limiterThresholdDbfs_ = -1.0;  // true-peak ceiling
  std::vector<AudioParticipantChainConfig> channels_;
  std::vector<AudioCrosspoint> crosspoints_;
  AudioBusTap programTap_;
  AudioBusTap monitorTap_;
  std::vector<AudioBusTap> isoTaps_;
  AudioMasterMeasurement measurement_;
  double integratedSum_ = 0.0;
  int64_t integratedBlocks_ = 0;
};

inline int clampAudioInt(int value, int minValue, int maxValue) {
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
