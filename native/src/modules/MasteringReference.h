#pragma once

// Reference-matched mastering (docs/mastering-chain-spec.md M2 remainder,
// matchering-inspired). Analyze a reference track offline -> a spectral+loudness
// PROFILE -> derive mastering params that push our program toward it
// ("make my show sound like X"). Pure and unit-tested; runs off the audio
// worker (offline analysis of a loaded file), never on the live path. The
// SOUND is the operator's judgment; the analysis and derivation are
// deterministic and verifiable.

#include "modules/AudioDsp.h"
#include "modules/AudioMastering.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace corevideo::modules {

// A reference track's character: per-band level (dBFS RMS) in three bands plus
// integrated loudness. Bands: low < 250 Hz, mid 250-4000 Hz, high > 4000 Hz.
struct MasteringReferenceProfile {
  double lowDb = kAudioDbfsFloor;
  double midDb = kAudioDbfsFloor;
  double highDb = kAudioDbfsFloor;
  double integratedLufs = kAudioDbfsFloor;
  bool valid = false;
};

namespace detail {

inline double rmsToDbfs(double meanSquare) {
  if (meanSquare <= 1e-12) {
    return kAudioDbfsFloor;
  }
  const double db = 10.0 * std::log10(meanSquare);
  return db < kAudioDbfsFloor ? kAudioDbfsFloor : db;
}

// Mean square of a mono signal after a cascade of biquads (fresh state).
inline double filteredMeanSquare(const std::vector<float>& mono, double sampleRate,
                                 const std::vector<AudioBiquadCoefficients>& cascade) {
  if (mono.empty()) {
    return 0.0;
  }
  std::vector<AudioBiquadState> states(cascade.size());
  double sum = 0.0;
  for (float sample : mono) {
    double x = sample;
    for (std::size_t i = 0; i < cascade.size(); ++i) {
      x = biquadProcessSample(cascade[i], states[i], x);
    }
    sum += x * x;
  }
  return sum / static_cast<double>(mono.size());
}

}  // namespace detail

// Analyze an interleaved-stereo reference buffer into a profile.
inline MasteringReferenceProfile analyzeMasteringReference(const float* interleaved, std::size_t frames,
                                                           double sampleRate) {
  MasteringReferenceProfile profile;
  if (interleaved == nullptr || frames == 0 || sampleRate <= 0.0) {
    return profile;
  }
  // Deinterleave to L/R for loudness, and a mono sum for the band split.
  std::vector<float> left(frames);
  std::vector<float> right(frames);
  std::vector<float> mono(frames);
  for (std::size_t i = 0; i < frames; ++i) {
    left[i] = interleaved[i * 2];
    right[i] = interleaved[i * 2 + 1];
    mono[i] = 0.5f * (interleaved[i * 2] + interleaved[i * 2 + 1]);
  }

  // Band split via the mastering RBJ filters. Low = LP250; high = HP4000;
  // mid = HP250 -> LP4000 (band-pass).
  const std::vector<AudioBiquadCoefficients> lowBand{masteringLowPass(sampleRate, 250.0)};
  const std::vector<AudioBiquadCoefficients> highBand{masteringHighPass(sampleRate, 4000.0)};
  const std::vector<AudioBiquadCoefficients> midBand{masteringHighPass(sampleRate, 250.0),
                                                     masteringLowPass(sampleRate, 4000.0)};

  profile.lowDb = detail::rmsToDbfs(detail::filteredMeanSquare(mono, sampleRate, lowBand));
  profile.midDb = detail::rmsToDbfs(detail::filteredMeanSquare(mono, sampleRate, midBand));
  profile.highDb = detail::rmsToDbfs(detail::filteredMeanSquare(mono, sampleRate, highBand));
  profile.integratedLufs = computeStereoLoudnessLufs(left.data(), right.data(), frames, sampleRate,
                                                     static_cast<double>(frames) / sampleRate);
  profile.valid = true;
  return profile;
}

// Derive mastering params from a reference profile: match its spectral tilt
// (shelves toward the reference's low/high emphasis relative to mid, capped
// +/-6 dB) and its loudness (targetLufs, clamped to a sane broadcast range).
// Presence is left neutral (the bell is an operator taste control). Filters
// and dynamics stay at the caller's existing values.
inline MasteringParams deriveMasteringParamsFromReference(const MasteringReferenceProfile& ref,
                                                          const MasteringParams& base) {
  MasteringParams out = base;
  if (!ref.valid) {
    return out;
  }
  auto cap6 = [](double v) { return std::clamp(v, -6.0, 6.0); };
  out.lowShelfDb = cap6(ref.lowDb - ref.midDb);
  out.highShelfDb = cap6(ref.highDb - ref.midDb);
  if (ref.integratedLufs > kAudioDbfsFloor + 1.0) {
    out.targetLufs = std::clamp(ref.integratedLufs, -30.0, -9.0);
  }
  out.enabled = true;
  return out;
}

// How the current program compares to a reference, for operator feedback
// ("your show is brighter / quieter than the reference"). Positive = program
// has MORE than the reference in that band / is louder.
struct MasteringMatchDelta {
  double lowDb = 0.0;
  double midDb = 0.0;
  double highDb = 0.0;
  double loudnessLu = 0.0;  // program LUFS - reference LUFS
  bool valid = false;
};

inline MasteringMatchDelta compareMasteringToReference(const MasteringReferenceProfile& program,
                                                       const MasteringReferenceProfile& reference) {
  MasteringMatchDelta delta;
  if (!program.valid || !reference.valid) {
    return delta;
  }
  // Tilt is what matters for "brighter/darker", so compare each band RELATIVE
  // to its own mid (removes overall level, isolating spectral balance).
  delta.lowDb = (program.lowDb - program.midDb) - (reference.lowDb - reference.midDb);
  delta.midDb = 0.0;  // mid is the reference point for tilt
  delta.highDb = (program.highDb - program.midDb) - (reference.highDb - reference.midDb);
  delta.loudnessLu = program.integratedLufs - reference.integratedLufs;
  delta.valid = true;
  return delta;
}

// One-word brightness verdict from a match delta (for a status readout).
// "brighter" when the program's high-vs-low tilt exceeds the reference by a
// perceptible margin (~1.5 dB), "darker" below, else "matched".
inline const char* masteringBrightnessVerdict(const MasteringMatchDelta& delta) {
  if (!delta.valid) {
    return "unknown";
  }
  const double tilt = delta.highDb - delta.lowDb;  // + = program relatively brighter
  if (tilt > 1.5) {
    return "brighter";
  }
  if (tilt < -1.5) {
    return "darker";
  }
  return "matched";
}

}  // namespace corevideo::modules
