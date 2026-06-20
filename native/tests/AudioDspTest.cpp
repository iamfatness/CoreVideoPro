#include "modules/AudioDsp.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace {

using namespace corevideo::modules;

// The vendored gtest is minimal (no EXPECT_NEAR); use absolute-difference
// checks for floating-point comparisons.
constexpr double kTightTol = 1e-4;

std::vector<float> makeSine(double freq, double amplitude, double sampleRate, size_t count) {
  std::vector<float> samples(count);
  for (size_t index = 0; index < count; ++index) {
    const double phase = 2.0 * kAudioPi * freq * static_cast<double>(index) / sampleRate;
    samples[index] = static_cast<float>(amplitude * std::sin(phase));
  }
  return samples;
}

}  // namespace

// --- RMS in dBFS --------------------------------------------------------------

TEST(AudioDsp, RmsSilenceReturnsFloor) {
  std::vector<float> silence(2048, 0.0f);
  EXPECT_TRUE(std::abs(computeRmsDbfs(silence.data(), silence.size()) - kAudioDbfsFloor) < kTightTol);
}

TEST(AudioDsp, RmsEmptyBufferReturnsFloor) {
  EXPECT_TRUE(std::abs(computeRmsDbfs(nullptr, 0) - kAudioDbfsFloor) < kTightTol);
  std::vector<float> data(8, 0.5f);
  EXPECT_TRUE(std::abs(computeRmsDbfs(data.data(), 0) - kAudioDbfsFloor) < kTightTol);
}

TEST(AudioDsp, RmsFullScaleSquareIsZeroDbfs) {
  // A constant full-scale buffer is an idealized full-scale square wave: 0 dBFS.
  std::vector<float> fullScale(4096, 1.0f);
  EXPECT_TRUE(std::abs(computeRmsDbfs(fullScale.data(), fullScale.size())) < kTightTol);
}

TEST(AudioDsp, RmsFullAmplitudeSineIsMinus3dbfs) {
  // A full-amplitude sine has RMS = 1/sqrt(2) -> -3.0103 dBFS.
  const auto sine = makeSine(1000.0, 1.0, 48000.0, 48000);
  const double rms = computeRmsDbfs(sine.data(), sine.size());
  EXPECT_TRUE(std::abs(rms - (-3.0103)) < 0.01);
}

TEST(AudioDsp, RmsHalfAmplitudeIsMinusSixDownFromFull) {
  const auto full = makeSine(1000.0, 1.0, 48000.0, 48000);
  const auto half = makeSine(1000.0, 0.5, 48000.0, 48000);
  const double delta = computeRmsDbfs(full.data(), full.size()) - computeRmsDbfs(half.data(), half.size());
  // Halving amplitude is -6.02 dB.
  EXPECT_TRUE(std::abs(delta - 6.0206) < 0.01);
}

// --- Sample peak in dBFS ------------------------------------------------------

TEST(AudioDsp, PeakSilenceReturnsFloor) {
  std::vector<float> silence(512, 0.0f);
  EXPECT_TRUE(std::abs(computeSamplePeakDbfs(silence.data(), silence.size()) - kAudioDbfsFloor) < kTightTol);
}

TEST(AudioDsp, PeakDetectsLargestMagnitude) {
  std::vector<float> data = {0.1f, -0.5f, 0.25f, -0.5f, 0.05f};
  // Largest |sample| is 0.5 -> 20*log10(0.5) = -6.0206 dBFS.
  EXPECT_TRUE(std::abs(computeSamplePeakDbfs(data.data(), data.size()) - (-6.0206)) < kTightTol);
}

TEST(AudioDsp, PeakFullScaleIsZeroDbfs) {
  std::vector<float> data = {0.0f, 0.2f, -1.0f, 0.3f};
  EXPECT_TRUE(std::abs(computeSamplePeakDbfs(data.data(), data.size())) < kTightTol);
}

TEST(AudioDsp, PeakIsAtLeastRms) {
  const auto sine = makeSine(440.0, 0.8, 48000.0, 48000);
  EXPECT_TRUE(computeSamplePeakDbfs(sine.data(), sine.size()) >= computeRmsDbfs(sine.data(), sine.size()));
}

// --- BS.1770 K-weighted loudness ----------------------------------------------

TEST(AudioDsp, LufsSilenceReturnsFloor) {
  std::vector<float> left(48000, 0.0f);
  std::vector<float> right(48000, 0.0f);
  EXPECT_TRUE(std::abs(computeMomentaryLufs(left.data(), right.data(), left.size(), 48000.0) - kAudioDbfsFloor) < kTightTol);
  EXPECT_TRUE(std::abs(computeShortTermLufs(left.data(), right.data(), left.size(), 48000.0) - kAudioDbfsFloor) < kTightTol);
}

TEST(AudioDsp, LufsEmptyReturnsFloor) {
  EXPECT_TRUE(std::abs(computeMomentaryLufs(nullptr, nullptr, 0, 48000.0) - kAudioDbfsFloor) < kTightTol);
}

TEST(AudioDsp, MomentaryLufsForKnownToneWithinTolerance) {
  // A -20 dBFS 1 kHz stereo tone (L = R) is the meter's calibration point: with
  // channel weights L = R = 1.0 it reads ~ -20 LUFS. K-weighting near 1 kHz is
  // essentially unity, so allow a 1.0 LU tolerance.
  const double sampleRate = 48000.0;
  const double amplitude = std::pow(10.0, -20.0 / 20.0);
  const size_t count = static_cast<size_t>(sampleRate);  // 1 s, covers 400 ms window
  const auto left = makeSine(1000.0, amplitude, sampleRate, count);
  const auto right = makeSine(1000.0, amplitude, sampleRate, count);
  const double lufs = computeMomentaryLufs(left.data(), right.data(), count, sampleRate);
  EXPECT_TRUE(std::abs(lufs - (-20.0)) < 1.0);
}

TEST(AudioDsp, ShortTermLufsForKnownToneWithinTolerance) {
  const double sampleRate = 48000.0;
  const double amplitude = std::pow(10.0, -20.0 / 20.0);
  const size_t count = static_cast<size_t>(sampleRate * 4);  // 4 s, covers 3 s window
  const auto left = makeSine(1000.0, amplitude, sampleRate, count);
  const auto right = makeSine(1000.0, amplitude, sampleRate, count);
  const double lufs = computeShortTermLufs(left.data(), right.data(), count, sampleRate);
  EXPECT_TRUE(std::abs(lufs - (-20.0)) < 1.0);
}

TEST(AudioDsp, LufsLouderToneReadsHigher) {
  const double sampleRate = 48000.0;
  const size_t count = static_cast<size_t>(sampleRate);
  const auto quietL = makeSine(1000.0, 0.05, sampleRate, count);
  const auto quietR = makeSine(1000.0, 0.05, sampleRate, count);
  const auto loudL = makeSine(1000.0, 0.5, sampleRate, count);
  const auto loudR = makeSine(1000.0, 0.5, sampleRate, count);
  const double quiet = computeMomentaryLufs(quietL.data(), quietR.data(), count, sampleRate);
  const double loud = computeMomentaryLufs(loudL.data(), loudR.data(), count, sampleRate);
  // A 20 dB amplitude difference -> ~20 LU louder.
  EXPECT_TRUE(loud > quiet);
  EXPECT_TRUE(std::abs((loud - quiet) - 20.0) < 1.0);
}

// --- Peak limiter -------------------------------------------------------------

TEST(AudioDsp, LimiterCapsHotSignalAndReportsReduction) {
  // A hot signal peaking at 0.9 (~-0.9 dBFS) limited to -6 dBFS must come down.
  auto samples = makeSine(1000.0, 0.9, 48000.0, 4800);
  const double thresholdDbfs = -6.0;
  const double reductionDb = applyPeakLimiter(samples.data(), samples.size(), thresholdDbfs);

  EXPECT_TRUE(reductionDb > 0.0);
  const double outputPeakDbfs = computeSamplePeakDbfs(samples.data(), samples.size());
  // Output peak must not exceed threshold (allow a tiny epsilon for rounding).
  EXPECT_TRUE(outputPeakDbfs <= thresholdDbfs + 1e-3);
  // Reduction roughly equals the difference between input peak and threshold.
  const double inputPeakDbfs = -0.9151;  // 20*log10(0.9)
  EXPECT_TRUE(std::abs(reductionDb - (inputPeakDbfs - thresholdDbfs)) < 0.05);
}

TEST(AudioDsp, LimiterIsNoOpBelowThreshold) {
  auto samples = makeSine(1000.0, 0.25, 48000.0, 4800);  // -12 dBFS peak
  const double reductionDb = applyPeakLimiter(samples.data(), samples.size(), -6.0);
  EXPECT_TRUE(std::abs(reductionDb) < kTightTol);
  // Untouched: peak still ~-12 dBFS.
  EXPECT_TRUE(std::abs(computeSamplePeakDbfs(samples.data(), samples.size()) - (-12.0412)) < 0.01);
}

TEST(AudioDsp, LimiterHandlesEmptyBuffer) {
  EXPECT_TRUE(std::abs(applyPeakLimiter(nullptr, 0, -6.0)) < kTightTol);
}

TEST(AudioDsp, LimiterIsDeterministic) {
  auto a = makeSine(1000.0, 0.95, 48000.0, 2400);
  auto b = a;
  const double ra = applyPeakLimiter(a.data(), a.size(), -3.0);
  const double rb = applyPeakLimiter(b.data(), b.size(), -3.0);
  EXPECT_TRUE(std::abs(ra - rb) < kTightTol);
  bool identical = true;
  for (size_t index = 0; index < a.size(); ++index) {
    if (a[index] != b[index]) {
      identical = false;
      break;
    }
  }
  EXPECT_TRUE(identical);
}

// --- Bus mix ------------------------------------------------------------------

TEST(AudioDsp, BusMixSumsScaledSources) {
  std::vector<float> a = {0.10f, 0.20f, 0.30f, 0.40f};
  std::vector<float> b = {0.05f, 0.05f, 0.05f, 0.05f};
  std::vector<float> dst(4, 0.0f);

  std::vector<AudioBusSource> sources = {
      {a.data(), a.size(), 0.5},
      {b.data(), b.size(), 2.0},
  };
  // Sum stays well under 1.0, so soft-clip is effectively linear here; disable
  // it to check the exact arithmetic: 0.5*a + 2.0*b.
  mixAudioBus(dst.data(), dst.size(), sources, /*softClip=*/false);

  EXPECT_TRUE(std::abs(dst[0] - (0.10f * 0.5f + 0.05f * 2.0f)) < kTightTol);
  EXPECT_TRUE(std::abs(dst[1] - (0.20f * 0.5f + 0.05f * 2.0f)) < kTightTol);
  EXPECT_TRUE(std::abs(dst[2] - (0.30f * 0.5f + 0.05f * 2.0f)) < kTightTol);
  EXPECT_TRUE(std::abs(dst[3] - (0.40f * 0.5f + 0.05f * 2.0f)) < kTightTol);
}

TEST(AudioDsp, BusMixHandlesShorterSources) {
  std::vector<float> shortSrc = {1.0f, 1.0f};  // only 2 samples
  std::vector<float> longSrc = {0.1f, 0.1f, 0.1f, 0.1f};
  std::vector<float> dst(4, 99.0f);
  std::vector<AudioBusSource> sources = {
      {shortSrc.data(), shortSrc.size(), 0.2},
      {longSrc.data(), longSrc.size(), 1.0},
  };
  mixAudioBus(dst.data(), dst.size(), sources, /*softClip=*/false);
  EXPECT_TRUE(std::abs(dst[0] - (1.0f * 0.2f + 0.1f)) < kTightTol);
  EXPECT_TRUE(std::abs(dst[1] - (1.0f * 0.2f + 0.1f)) < kTightTol);
  // Past the short source: only the long source contributes.
  EXPECT_TRUE(std::abs(dst[2] - 0.1f) < kTightTol);
  EXPECT_TRUE(std::abs(dst[3] - 0.1f) < kTightTol);
}

TEST(AudioDsp, BusMixSoftClipKeepsWithinFullScale) {
  // Three loud sources sum well over 1.0; soft-clip must keep |out| <= 1.0.
  std::vector<float> a(256, 0.8f);
  std::vector<float> b(256, 0.8f);
  std::vector<float> c(256, 0.8f);
  std::vector<float> dst(256, 0.0f);
  std::vector<AudioBusSource> sources = {
      {a.data(), a.size(), 1.0},
      {b.data(), b.size(), 1.0},
      {c.data(), c.size(), 1.0},
  };
  mixAudioBus(dst.data(), dst.size(), sources, /*softClip=*/true);
  bool withinRange = true;
  for (float value : dst) {
    if (value > 1.0f || value < -1.0f) {
      withinRange = false;
      break;
    }
  }
  EXPECT_TRUE(withinRange);
  // The sum (2.4) is well into saturation, so output is near +1.0.
  EXPECT_TRUE(dst[0] > 0.9f);
}

TEST(AudioDsp, BusMixEmptyDestinationIsSafe) {
  std::vector<AudioBusSource> sources;
  mixAudioBus(nullptr, 0, sources);
  std::vector<float> dst;
  mixAudioBus(dst.data(), 0, sources);
  EXPECT_TRUE(true);
}

// --- Optional AudioFrame PCM metering (backward-compatible) -------------------

TEST(AudioDsp, AnalyzeUsesPcmRmsPeakWhenPresent) {
  // A frame with real PCM should report measured linear levels rather than the
  // synthetic deterministic fallback.
  AudioFrame frame;
  frame.participantId = "pcm-source";
  frame.sampleRate = 48000;
  frame.channels = 1;
  frame.voiceActive = true;
  frame.pcm = makeSine(1000.0, 0.5, 48000.0, 4800);  // -6 dBFS sine
  frame.sampleCount = static_cast<int>(frame.pcm.size());

  const AudioParticipantMixMetrics metrics = analyzeAudioParticipantFrame(frame);
  // Peak of a 0.5 sine is ~0.5 linear; RMS ~0.3536 linear.
  EXPECT_TRUE(std::abs(metrics.peakLevel - 0.5) < 0.01);
  EXPECT_TRUE(std::abs(metrics.rmsLevel - 0.3536) < 0.01);
  EXPECT_TRUE(metrics.peakLevel >= metrics.rmsLevel);
}

TEST(AudioDsp, AnalyzeFallsBackToMetadataWhenNoPcm) {
  // No PCM: behavior is unchanged from the metadata path. A provided rmsLevel is
  // used verbatim (clamped).
  AudioFrame frame;
  frame.participantId = "metadata-source";
  frame.rmsLevel = 0.42;
  frame.peakLevel = 0.6;
  const AudioParticipantMixMetrics metrics = analyzeAudioParticipantFrame(frame);
  EXPECT_TRUE(std::abs(metrics.rmsLevel - 0.42) < kTightTol);
  EXPECT_TRUE(std::abs(metrics.peakLevel - 0.6) < kTightTol);
}

TEST(AudioDsp, AnalyzeFlagsSilentPcmAsSilence) {
  AudioFrame frame;
  frame.participantId = "silent-source";
  frame.sampleRate = 48000;
  frame.channels = 1;
  frame.voiceActive = true;
  frame.pcm.assign(4800, 0.0f);
  frame.sampleCount = static_cast<int>(frame.pcm.size());
  const AudioParticipantMixMetrics metrics = analyzeAudioParticipantFrame(frame);
  EXPECT_TRUE(metrics.silenceDetected);
}
