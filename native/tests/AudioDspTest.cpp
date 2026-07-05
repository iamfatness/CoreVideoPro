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

// --- Noise gate ---------------------------------------------------------------

namespace {

double rmsLinear(const std::vector<float>& samples, size_t from, size_t to) {
  double sumSquares = 0.0;
  const size_t span = to - from;
  for (size_t index = from; index < to; ++index) {
    sumSquares += static_cast<double>(samples[index]) * static_cast<double>(samples[index]);
  }
  return span == 0 ? 0.0 : std::sqrt(sumSquares / static_cast<double>(span));
}

}  // namespace

TEST(AudioDsp, GateClosesOnSilence) {
  std::vector<float> silence(4800, 0.0f);
  const double gatedFraction = applyNoiseGate(silence.data(), silence.size(), -30.0, 5.0, 50.0, 48000.0);
  // Silence is always below threshold: the gate stays closed for the whole buffer.
  EXPECT_TRUE(std::abs(gatedFraction - 1.0) < kTightTol);
  EXPECT_TRUE(std::abs(rmsLinear(silence, 0, silence.size())) < kTightTol);
}

TEST(AudioDsp, GateHeavilyAttenuatesQuietSignal) {
  // A -50 dBFS sine sits well below a -30 dBFS threshold and must be crushed.
  const double quietAmp = std::pow(10.0, -50.0 / 20.0);
  auto quiet = makeSine(1000.0, quietAmp, 48000.0, 9600);
  const double inRms = rmsLinear(quiet, 0, quiet.size());
  const double gatedFraction = applyNoiseGate(quiet.data(), quiet.size(), -30.0, 5.0, 50.0, 48000.0);
  const double outRms = rmsLinear(quiet, 0, quiet.size());
  EXPECT_TRUE(gatedFraction > 0.99);
  // Output RMS is a tiny fraction of the input (well below -40 dB of it).
  EXPECT_TRUE(outRms < inRms * 0.01);
}

TEST(AudioDsp, GatePassesLoudSignalNearlyUnchanged) {
  // A -6 dBFS sine is far above a -30 dBFS threshold: the gate opens and the
  // steady-state tail passes through at unity gain.
  auto loud = makeSine(1000.0, 0.5, 48000.0, 9600);
  const auto reference = loud;
  const double gatedFraction = applyNoiseGate(loud.data(), loud.size(), -30.0, 5.0, 50.0, 48000.0);
  // Almost nothing gated (only the brief attack ramp at the very start).
  EXPECT_TRUE(gatedFraction < 0.1);
  // The trailing half (past the attack transient) is unchanged at unity gain.
  const double inTail = rmsLinear(reference, reference.size() / 2, reference.size());
  const double outTail = rmsLinear(loud, loud.size() / 2, loud.size());
  EXPECT_TRUE(std::abs(outTail - inTail) < 1e-3);
}

TEST(AudioDsp, GateIsDeterministic) {
  auto a = makeSine(800.0, 0.4, 48000.0, 4800);
  auto b = a;
  const double ra = applyNoiseGate(a.data(), a.size(), -30.0, 5.0, 50.0, 48000.0);
  const double rb = applyNoiseGate(b.data(), b.size(), -30.0, 5.0, 50.0, 48000.0);
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

TEST(AudioDsp, GateHandlesEmptyBuffer) {
  EXPECT_TRUE(std::abs(applyNoiseGate(nullptr, 0, -30.0, 5.0, 50.0, 48000.0)) < kTightTol);
  std::vector<float> data(8, 0.5f);
  EXPECT_TRUE(std::abs(applyNoiseGate(data.data(), 0, -30.0, 5.0, 50.0, 48000.0)) < kTightTol);
}

// --- True-peak (inter-sample peak) --------------------------------------------

namespace {

// A 12 kHz (fs/4) tone sampled half a sample off-phase: the discrete samples sit
// at +/-amp/sqrt(2), so the sample peak is ~3 dB below the real tone amplitude.
// A true-peak meter must recover the higher inter-sample peak.
std::vector<float> makeInterSampleOvershoot(double amplitude, double sampleRate, size_t count) {
  std::vector<float> samples(count);
  for (size_t index = 0; index < count; ++index) {
    const double phase = 2.0 * kAudioPi * (sampleRate / 4.0) * (static_cast<double>(index) + 0.5) / sampleRate;
    samples[index] = static_cast<float>(amplitude * std::sin(phase));
  }
  return samples;
}

}  // namespace

TEST(AudioDsp, TruePeakSilenceReturnsFloor) {
  std::vector<float> silence(1024, 0.0f);
  EXPECT_TRUE(std::abs(computeTruePeakDbfs(silence.data(), silence.size()) - kAudioDbfsFloor) < kTightTol);
}

TEST(AudioDsp, TruePeakEmptyBufferReturnsFloor) {
  EXPECT_TRUE(std::abs(computeTruePeakDbfs(nullptr, 0) - kAudioDbfsFloor) < kTightTol);
  std::vector<float> data(8, 0.5f);
  EXPECT_TRUE(std::abs(computeTruePeakDbfs(data.data(), 0) - kAudioDbfsFloor) < kTightTol);
}

TEST(AudioDsp, TruePeakIsAtLeastSamplePeak) {
  const auto sine = makeSine(997.0, 0.7, 48000.0, 4800);
  const double samplePeak = computeSamplePeakDbfs(sine.data(), sine.size());
  const double truePeak = computeTruePeakDbfs(sine.data(), sine.size());
  EXPECT_TRUE(truePeak >= samplePeak - 1e-6);
}

TEST(AudioDsp, TruePeakNeverBelowSamplePeakForDc) {
  // A constant full-scale block has true peak == sample peak == 0 dBFS; edge
  // handling must not invent overshoot for a flat signal.
  std::vector<float> dc(2048, 1.0f);
  const double truePeak = computeTruePeakDbfs(dc.data(), dc.size());
  EXPECT_TRUE(truePeak >= computeSamplePeakDbfs(dc.data(), dc.size()) - 1e-6);
  EXPECT_TRUE(std::abs(truePeak) < 0.05);  // ~0 dBFS, no false overshoot
}

TEST(AudioDsp, TruePeakCatchesInterSampleOvershoot) {
  // Samples sit ~3 dB below the 0.9 tone amplitude; the true peak must climb back
  // toward the real amplitude and clearly exceed the sample peak.
  const auto signal = makeInterSampleOvershoot(0.9, 48000.0, 2048);
  const double samplePeak = computeSamplePeakDbfs(signal.data(), signal.size());
  const double truePeak = computeTruePeakDbfs(signal.data(), signal.size(), 4);
  EXPECT_TRUE(truePeak > samplePeak);
  // The inter-sample overshoot here is multiple dB above the sample peak.
  EXPECT_TRUE(truePeak - samplePeak > 2.0);
  // And the true peak approaches the real tone amplitude (0.9 -> ~-0.92 dBFS).
  EXPECT_TRUE(truePeak > -1.5);
}

TEST(AudioDsp, TruePeakIsDeterministic) {
  const auto a = makeInterSampleOvershoot(0.8, 48000.0, 2048);
  const double first = computeTruePeakDbfs(a.data(), a.size(), 4);
  const double second = computeTruePeakDbfs(a.data(), a.size(), 4);
  EXPECT_TRUE(std::abs(first - second) < kTightTol);
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

TEST(AudioDsp, AnalyzeMissingPcmAndMetadataReportsSilence) {
  AudioFrame frame;
  frame.participantId = "empty-source";
  frame.sampleRate = 48000;
  frame.channels = 2;
  frame.voiceActive = true;
  frame.sampleCount = 960;

  const AudioParticipantMixMetrics metrics = analyzeAudioParticipantFrame(frame);

  EXPECT_TRUE(metrics.silenceDetected);
  EXPECT_EQ(metrics.status, "silent");
  EXPECT_EQ(metrics.inputLevel, 0);
  EXPECT_EQ(metrics.outputLevel, 0);
  EXPECT_TRUE(std::abs(metrics.rmsLevel) < kTightTol);
  EXPECT_TRUE(std::abs(metrics.peakLevel) < kTightTol);
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

// --- Routing-matrix bus mix --------------------------------------------------

TEST(AudioDsp, RoutedBusMixSumsSourcesWithCrosspointGain) {
  std::vector<float> a(8, 0.25f);  // mono, 8 sample-frames
  std::vector<float> b(8, 0.10f);
  const std::vector<RoutedAudioSource> sources = {
      {"a", &a, 1, 1.0, 0.0, false, false},
      {"b", &b, 1, 1.0, 0.0, false, false},
  };
  const std::vector<RoutedAudioCrosspoint> crosspoints = {
      {"a", "master", 1.0},
      {"b", "master", 1.0},
      {"a", "iso-1", 1.0},
  };
  const auto buses = mixRoutedBuses(sources, crosspoints);

  const auto master = buses.find("master");
  const auto iso = buses.find("iso-1");
  EXPECT_TRUE(master != buses.end());
  EXPECT_TRUE(iso != buses.end());
  EXPECT_TRUE(master->second.size() == 16u);            // 8 frames * 2 channels
  EXPECT_TRUE(std::abs(master->second[0] - 0.35f) < kTightTol);  // 0.25 + 0.10
  EXPECT_TRUE(std::abs(iso->second[0] - 0.25f) < kTightTol);     // a only
}

TEST(AudioDsp, RoutedBusMixHalvesWithMinusSixDbCrosspoint) {
  std::vector<float> a(4, 0.8f);
  const std::vector<RoutedAudioSource> sources = {{"a", &a, 1, 1.0, 0.0, false, false}};
  const std::vector<RoutedAudioCrosspoint> crosspoints = {{"a", "master", dbfsToLinear(-6.0206)}};
  const auto buses = mixRoutedBuses(sources, crosspoints);
  const auto master = buses.find("master");
  EXPECT_TRUE(master != buses.end());
  EXPECT_TRUE(std::abs(master->second[0] - 0.4f) < 1e-3);  // -6 dB halves 0.8
}

TEST(AudioDsp, RoutedBusMixSoloExcludesNonSoloedSources) {
  std::vector<float> a(4, 0.5f);
  std::vector<float> b(4, 0.5f);
  const std::vector<RoutedAudioSource> sources = {
      {"a", &a, 1, 1.0, 0.0, false, true},   // solo
      {"b", &b, 1, 1.0, 0.0, false, false},
  };
  const std::vector<RoutedAudioCrosspoint> crosspoints = {{"a", "master", 1.0}, {"b", "master", 1.0}};
  const auto buses = mixRoutedBuses(sources, crosspoints);
  const auto master = buses.find("master");
  EXPECT_TRUE(master != buses.end());
  EXPECT_TRUE(std::abs(master->second[0] - 0.5f) < kTightTol);  // only the soloed source
}

TEST(AudioDsp, RoutedBusMixMutedSourceContributesNothing) {
  std::vector<float> a(4, 0.8f);
  std::vector<float> b(4, 0.2f);
  const std::vector<RoutedAudioSource> sources = {
      {"a", &a, 1, 1.0, 0.0, true, false},   // muted
      {"b", &b, 1, 1.0, 0.0, false, false},
  };
  const std::vector<RoutedAudioCrosspoint> crosspoints = {{"a", "master", 1.0}, {"b", "master", 1.0}};
  const auto buses = mixRoutedBuses(sources, crosspoints);
  const auto master = buses.find("master");
  EXPECT_TRUE(master != buses.end());
  EXPECT_TRUE(std::abs(master->second[0] - 0.2f) < kTightTol);  // muted source dropped
}

TEST(AudioDsp, RoutedBusMixPanBiasesStereoBalance) {
  std::vector<float> a(4, 0.5f);
  const std::vector<RoutedAudioSource> sources = {{"a", &a, 1, 1.0, 1.0, false, false}};  // hard right
  const std::vector<RoutedAudioCrosspoint> crosspoints = {{"a", "master", 1.0}};
  const auto buses = mixRoutedBuses(sources, crosspoints);
  const auto master = buses.find("master");
  EXPECT_TRUE(master != buses.end());
  EXPECT_TRUE(std::abs(master->second[0] - 0.0f) < kTightTol);  // left muted by pan
  EXPECT_TRUE(std::abs(master->second[1] - 0.5f) < kTightTol);  // right full
}

TEST(AudioDsp, RoutedBusMixWithoutCrosspointsProducesNoBuses) {
  std::vector<float> a(4, 0.5f);
  const std::vector<RoutedAudioSource> sources = {{"a", &a, 1, 1.0, 0.0, false, false}};
  const auto buses = mixRoutedBuses(sources, {});
  EXPECT_TRUE(buses.empty());
}

// --- BS.1770 integrated loudness meter ---------------------------------------

TEST(AudioDsp, IntegratedLoudnessSilenceStaysBelowGate) {
  const double rate = 48000.0;
  std::vector<float> silence(static_cast<size_t>(rate * 1.5), 0.0f);
  Bs1770IntegratedMeter meter(rate);
  meter.process(silence.data(), silence.data(), silence.size());
  // Every block falls below the -70 LUFS absolute gate -> floor.
  EXPECT_TRUE(meter.integratedLufs() <= -70.0);
}

TEST(AudioDsp, IntegratedLoudnessMatchesSteadyToneShortTerm) {
  const double rate = 48000.0;
  const auto tone = makeSine(1000.0, 0.5, rate, static_cast<size_t>(rate * 2));  // 2 s steady
  Bs1770IntegratedMeter meter(rate);
  meter.process(tone.data(), tone.data(), tone.size());
  // A steady tone: every gated block has the same loudness, so the integrated
  // value matches the short-term loudness of the same signal.
  const double integrated = meter.integratedLufs();
  const double shortTerm = computeShortTermLufs(tone.data(), tone.data(), tone.size(), rate);
  EXPECT_TRUE(std::abs(integrated - shortTerm) < 1.0);
}

TEST(AudioDsp, IntegratedLoudnessAbsoluteGateDropsSilentSection) {
  const double rate = 48000.0;
  const auto loud = makeSine(1000.0, 0.5, rate, static_cast<size_t>(rate * 1));
  std::vector<float> silence(static_cast<size_t>(rate * 1), 0.0f);

  Bs1770IntegratedMeter gated(rate);
  gated.process(loud.data(), loud.data(), loud.size());
  gated.process(silence.data(), silence.data(), silence.size());

  Bs1770IntegratedMeter loudOnly(rate);
  loudOnly.process(loud.data(), loud.data(), loud.size());

  // The silent second is gated out, so the integrated loudness tracks the loud
  // section rather than being dragged toward silence.
  EXPECT_TRUE(std::abs(gated.integratedLufs() - loudOnly.integratedLufs()) < 1.0);
}

// --- Bus insert chain (built-in dynamics) ------------------------------------

TEST(AudioDsp, CompressorReducesHotSignalPeak) {
  std::vector<float> samples(256, 0.9f);  // ~-0.9 dBFS, well above the threshold
  const double reduction = applyCompressor(samples.data(), samples.size(), -18.0, 4.0);
  EXPECT_TRUE(reduction > 0.0);
  // 4:1 above -18 dBFS pulls a -0.9 dBFS signal down well below -10 dBFS.
  EXPECT_TRUE(computeSamplePeakDbfs(samples.data(), samples.size()) < -10.0);
}

TEST(AudioDsp, CompressorLeavesQuietSignalUntouched) {
  std::vector<float> samples(256, 0.05f);  // ~-26 dBFS, below the -18 threshold
  const double reduction = applyCompressor(samples.data(), samples.size(), -18.0, 4.0);
  EXPECT_TRUE(std::abs(reduction) < kTightTol);
  EXPECT_TRUE(std::abs(samples[0] - 0.05f) < kTightTol);
}

TEST(AudioDsp, BusInsertChainAppliesLimiterAndPassesThroughOthers) {
  std::vector<float> samples(256, 0.95f);
  const int applied = applyBusInsertChain(samples.data(), samples.size(), 48000.0, {"Built-in EQ", "Limiter"});
  EXPECT_EQ(applied, 1);  // limiter processes audio; EQ is a pass-through for now
  // Limiter brickwalls to -1 dBFS, so the 0.95 peak is pulled down to ~0.891.
  EXPECT_TRUE(computeSamplePeakDbfs(samples.data(), samples.size()) <= -0.9);
}

TEST(AudioDsp, BusInsertChainWithNoKnownInsertsIsNoOp) {
  std::vector<float> samples(64, 0.4f);
  const int applied = applyBusInsertChain(samples.data(), samples.size(), 48000.0, {"Built-in EQ", "Reverb"});
  EXPECT_EQ(applied, 0);
  EXPECT_TRUE(std::abs(samples[0] - 0.4f) < kTightTol);
}

// --- Per-source PCM frame coalescing (audio overhaul spec R3) -------------------

TEST(AudioDsp, CoalescePcmFramesConcatenatesSameSourceInArrivalOrder) {
  AudioFrame first;
  first.participantId = "a";
  first.sampleRate = 48000;
  first.channels = 1;
  first.pcm = {0.1f, 0.2f};
  AudioFrame other;
  other.participantId = "b";
  other.sampleRate = 48000;
  other.channels = 1;
  other.pcm = {0.9f};
  AudioFrame second = first;
  second.pcm = {0.3f, 0.4f};

  const auto coalesced = coalescePcmAudioFramesBySource({first, other, second});
  ASSERT_TRUE(coalesced.size() == 2u);
  EXPECT_EQ(coalesced[0].participantId, "a");
  ASSERT_TRUE(coalesced[0].pcm.size() == 4u);
  EXPECT_TRUE(std::abs(coalesced[0].pcm[2] - 0.3f) < kTightTol);
  EXPECT_EQ(coalesced[0].sampleCount, 4);
  EXPECT_EQ(coalesced[1].participantId, "b");
  EXPECT_EQ(coalesced[1].pcm.size(), 1u);
}

TEST(AudioDsp, CoalescePcmFramesPassesThroughMetadataAndSplitsFormatChanges) {
  AudioFrame metadata;
  metadata.participantId = "meta";
  metadata.sampleRate = 48000;
  metadata.channels = 1;  // no pcm — placeholder frame

  AudioFrame mono;
  mono.participantId = "x";
  mono.sampleRate = 48000;
  mono.channels = 1;
  mono.pcm = {0.5f};
  AudioFrame stereo = mono;
  stereo.channels = 2;
  stereo.pcm = {0.6f, 0.7f};

  const auto coalesced = coalescePcmAudioFramesBySource({metadata, mono, stereo});
  ASSERT_TRUE(coalesced.size() == 3u);
  EXPECT_EQ(coalesced[0].participantId, "meta");
  EXPECT_TRUE(coalesced[0].pcm.empty());
  EXPECT_EQ(coalesced[1].channels, 1);
  EXPECT_EQ(coalesced[1].pcm.size(), 1u);
  EXPECT_EQ(coalesced[2].channels, 2);
  EXPECT_EQ(coalesced[2].sampleCount, 1);
}

// ---- Spec 4.4: channel insert chain actually processes -----------------------

TEST(AudioDsp, ChannelGateSilencesNoiseFloorAndPassesSpeechLevels) {
  const double sampleRate = 48000.0;
  // 200ms of quiet hiss at ~-60 dBFS: the gate must close on it.
  std::vector<float> hiss(19200, 0.001f);
  const double gatedQuiet = corevideo::modules::applyStereoLinkedGate(
      hiss.data(), hiss.size(), -48.0, 5.0, 120.0, sampleRate);
  EXPECT_TRUE(gatedQuiet > 0.9);
  double peakAfter = 0.0;
  for (size_t i = hiss.size() / 2; i < hiss.size(); ++i) {
    peakAfter = std::max(peakAfter, std::fabs(static_cast<double>(hiss[i])));
  }
  EXPECT_TRUE(peakAfter < 0.0005);  // tail is attenuated, not passed

  // Speech-level signal (~-12 dBFS) passes essentially unchanged once open.
  std::vector<float> speech(19200, 0.25f);
  const double gatedLoud = corevideo::modules::applyStereoLinkedGate(
      speech.data(), speech.size(), -48.0, 5.0, 120.0, sampleRate);
  EXPECT_TRUE(gatedLoud < 0.05);
  EXPECT_TRUE(std::fabs(static_cast<double>(speech[speech.size() - 2]) - 0.25) < 0.01);
}

TEST(AudioDsp, HighpassEqAttenuatesRumbleAndPassesMidband) {
  const double sampleRate = 48000.0;
  const auto sineEnergyAfterHpf = [sampleRate](double frequencyHz) {
    // 0.5s interleaved-stereo sine so the filter settles.
    const size_t frames = 24000;
    std::vector<float> stereo(frames * 2);
    for (size_t frame = 0; frame < frames; ++frame) {
      const float sample = static_cast<float>(
          0.5 * std::sin(2.0 * 3.14159265358979323846 * frequencyHz * static_cast<double>(frame) / sampleRate));
      stereo[frame * 2] = sample;
      stereo[frame * 2 + 1] = sample;
    }
    corevideo::modules::applyStereoEqCascade(
        stereo.data(), stereo.size(), {corevideo::modules::eqHighpass(sampleRate, 90.0)});
    double peak = 0.0;
    for (size_t i = stereo.size() / 2; i < stereo.size(); ++i) {
      peak = std::max(peak, std::fabs(static_cast<double>(stereo[i])));
    }
    return peak;
  };

  const double rumble = sineEnergyAfterHpf(30.0);    // well below 90 Hz cutoff
  const double midband = sineEnergyAfterHpf(1000.0);  // voice band
  EXPECT_TRUE(rumble < 0.1);    // >14 dB down
  EXPECT_TRUE(midband > 0.45);  // essentially unity
}

TEST(AudioDsp, ChannelInsertChainRunsInsideTheRoutedBusMix) {
  // A source with a "Noise Gate" insert carrying only hiss must arrive at its
  // bus gated to (near) silence; the same source without the insert passes.
  std::vector<float> hiss(9600, 0.001f);  // mono, ~200ms at 48k
  corevideo::modules::RoutedAudioSource gatedSource;
  gatedSource.sourceId = "s1";
  gatedSource.pcm = &hiss;
  gatedSource.channels = 1;
  std::vector<std::string> inserts{"Noise Gate"};
  gatedSource.inserts = &inserts;

  corevideo::modules::RoutedAudioSource plainSource = gatedSource;
  plainSource.inserts = nullptr;

  const std::vector<corevideo::modules::RoutedAudioCrosspoint> sends{{"s1", "master", 1.0}};

  const auto gatedBuses = corevideo::modules::mixRoutedBuses({gatedSource}, sends);
  const auto plainBuses = corevideo::modules::mixRoutedBuses({plainSource}, sends);

  const auto& gatedBus = gatedBuses.at("master");
  const auto& plainBus = plainBuses.at("master");
  double gatedPeakTail = 0.0;
  double plainPeakTail = 0.0;
  for (size_t i = gatedBus.size() / 2; i < gatedBus.size(); ++i) {
    gatedPeakTail = std::max(gatedPeakTail, std::fabs(static_cast<double>(gatedBus[i])));
    plainPeakTail = std::max(plainPeakTail, std::fabs(static_cast<double>(plainBus[i])));
  }
  EXPECT_TRUE(gatedPeakTail < 0.0005);
  EXPECT_TRUE(plainPeakTail > 0.0009);  // ungated hiss reaches the bus
}

TEST(AudioDsp, MasterLimiterToggleOffSkipsLimiterButStillClamps) {
  // Two full-scale sources summed at unity would hit 2.0; with the limiter the
  // bus stays at/below -1 dBFS (~0.891); with it OFF the hard clamp holds 1.0.
  std::vector<float> full(9600, 1.0f);
  corevideo::modules::RoutedAudioSource a;
  a.sourceId = "a";
  a.pcm = &full;
  a.channels = 1;
  corevideo::modules::RoutedAudioSource b = a;
  b.sourceId = "b";
  const std::vector<corevideo::modules::RoutedAudioCrosspoint> sends{
      {"a", "master", 1.0}, {"b", "master", 1.0}};

  const auto limited = corevideo::modules::mixRoutedBuses({a, b}, sends, true);
  const auto clamped = corevideo::modules::mixRoutedBuses({a, b}, sends, false);
  double limitedPeak = 0.0;
  double clampedPeak = 0.0;
  for (const float sample : limited.at("master")) {
    limitedPeak = std::max(limitedPeak, std::fabs(static_cast<double>(sample)));
  }
  for (const float sample : clamped.at("master")) {
    clampedPeak = std::max(clampedPeak, std::fabs(static_cast<double>(sample)));
  }
  EXPECT_TRUE(limitedPeak < 0.95);
  EXPECT_TRUE(std::fabs(clampedPeak - 1.0) < 1e-6);
}

TEST(AudioDsp, ChannelInsertSettingsOverrideTheDefaults) {
  // C5b: hiss at ~-34 dBFS. The DEFAULT gate threshold (-48) lets it pass;
  // an operator override to -20 dBFS gates it to silence.
  const double sampleRate = 48000.0;
  const auto tailPeak = [sampleRate](const corevideo::modules::ChannelInsertSettings* settings) {
    std::vector<float> hiss(19200, 0.02f);  // interleaved stereo, 200ms
    const std::vector<std::string> inserts{"Noise Gate"};
    corevideo::modules::applyChannelInsertChain(hiss.data(), hiss.size(), sampleRate, inserts, false, settings);
    double peak = 0.0;
    for (size_t i = hiss.size() / 2; i < hiss.size(); ++i) {
      peak = std::max(peak, std::fabs(static_cast<double>(hiss[i])));
    }
    return peak;
  };

  EXPECT_TRUE(tailPeak(nullptr) > 0.015);  // default -48 threshold: passes

  corevideo::modules::ChannelInsertSettings settings;
  settings["Noise Gate"]["thresholdDb"] = -20.0;
  EXPECT_TRUE(tailPeak(&settings) < 0.001);  // operator threshold: gated
}

TEST(AudioDsp, EightBandEqBoostsItsBand) {
  // C6b: +12 dB on band 8 (8 kHz) must boost an 8 kHz tone while leaving a
  // 500 Hz tone essentially unchanged (band 4 flat).
  const double sampleRate = 48000.0;
  const auto tailPeakAt = [sampleRate](double toneHz, const corevideo::modules::ChannelInsertSettings* settings) {
    const size_t frames = 24000;
    std::vector<float> stereo(frames * 2);
    for (size_t frame = 0; frame < frames; ++frame) {
      const float sample = static_cast<float>(
          0.1 * std::sin(2.0 * 3.14159265358979323846 * toneHz * static_cast<double>(frame) / sampleRate));
      stereo[frame * 2] = sample;
      stereo[frame * 2 + 1] = sample;
    }
    const std::vector<std::string> inserts{"Built-in EQ"};
    corevideo::modules::applyChannelInsertChain(stereo.data(), stereo.size(), sampleRate, inserts, false, settings);
    double peak = 0.0;
    for (size_t i = stereo.size() / 2; i < stereo.size(); ++i) {
      peak = std::max(peak, std::fabs(static_cast<double>(stereo[i])));
    }
    return peak;
  };

  corevideo::modules::ChannelInsertSettings settings;
  settings["Built-in EQ"]["band8Db"] = 12.0;

  const double boosted8k = tailPeakAt(8000.0, &settings);
  const double flat8k = tailPeakAt(8000.0, nullptr);
  EXPECT_TRUE(boosted8k > flat8k * 2.5);  // +12 dB ~= x3.98, allow filter slop

  const double mid500 = tailPeakAt(500.0, &settings);
  const double flat500 = tailPeakAt(500.0, nullptr);
  EXPECT_TRUE(std::fabs(mid500 - flat500) < 0.02);  // distant band untouched
}

// ---- C7: competitive dynamics ------------------------------------------------

TEST(AudioDsp, StereoCompressorAttacksReleasesAndReportsGainReduction) {
  const double sampleRate = 48000.0;
  // 0.5s of a loud constant signal (~-2.5 dBFS) against threshold -20, ratio 4.
  std::vector<float> stereo(48000, 0.75f);
  const double grDb = corevideo::modules::applyStereoCompressor(
      stereo.data(), stereo.size(), sampleRate, -20.0, 4.0, 10.0, 120.0, 0.0, 0.0);

  // Static math: level -2.5dB, over = 17.5dB, GR = (1 - 1/4)*17.5 ~= 13.1dB.
  EXPECT_TRUE(grDb > 11.0);
  EXPECT_TRUE(grDb < 15.0);

  // ATTACK: the first millisecond is barely compressed; the tail is fully
  // compressed (output ~= 0.75 * 10^(-13.1/20) ~= 0.166).
  EXPECT_TRUE(stereo[2] > 0.6f);                      // ~40µs in: gain still near 1
  EXPECT_TRUE(std::fabs(stereo[stereo.size() - 2] - 0.166f) < 0.02f);
}

TEST(AudioDsp, StereoCompressorMakeupRestoresLevel) {
  const double sampleRate = 48000.0;
  std::vector<float> stereo(48000, 0.75f);
  corevideo::modules::applyStereoCompressor(
      stereo.data(), stereo.size(), sampleRate, -20.0, 4.0, 1.0, 120.0, 0.0, 13.0);
  // GR ~13.1dB + makeup 13dB ~= unity at the tail.
  EXPECT_TRUE(std::fabs(stereo[stereo.size() - 2] - 0.74f) < 0.05f);
}

TEST(AudioDsp, GateRangeDucksToFloorInsteadOfSilenceAndHoldStaysOpen) {
  const double sampleRate = 48000.0;
  // Layout: 100ms loud burst, then 400ms quiet hiss.
  const size_t frames = 24000;
  std::vector<float> stereo(frames * 2);
  for (size_t frame = 0; frame < frames; ++frame) {
    const float sample = frame < 4800 ? 0.5f : 0.01f;
    stereo[frame * 2] = sample;
    stereo[frame * 2 + 1] = sample;
  }

  // rangeDb -20 (audible floor), holdMs 100.
  corevideo::modules::applyStereoLinkedGate(stereo.data(), stereo.size(), -30.0, 2.0, 30.0, sampleRate,
                                            -20.0, 100.0);

  // HOLD: ~50ms after the burst ends (inside the 100ms hold) the hiss passes.
  const size_t holdIdx = (4800 + 2400) * 2;
  EXPECT_TRUE(std::fabs(stereo[holdIdx] - 0.01f) < 0.003f);

  // RANGE: deep in the gated tail the hiss sits near the -20dB floor
  // (0.01 * 0.1 = 0.001), NOT at zero.
  const size_t tailIdx = (frames - 100) * 2;
  EXPECT_TRUE(stereo[tailIdx] > 0.0005f);
  EXPECT_TRUE(stereo[tailIdx] < 0.002f);
}

TEST(AudioDsp, PersistentStateMakesBlockProcessingContinuous) {
  // C7c: the owner-reported mic distortion. Streaming DSP processes 20ms
  // blocks; WITHOUT carried state every biquad/envelope restarts at each block
  // boundary (a 50Hz buzz). With ChannelDspState, two half-buffer calls must
  // produce EXACTLY the same output as one full-buffer call.
  const double sampleRate = 48000.0;
  const size_t frames = 1920;  // two 20ms stereo blocks
  std::vector<float> reference(frames * 2);
  for (size_t frame = 0; frame < frames; ++frame) {
    const float sample = static_cast<float>(0.4 * std::sin(2.0 * 3.14159265358979323846 * 220.0 * frame / sampleRate));
    reference[frame * 2] = sample;
    reference[frame * 2 + 1] = sample;
  }
  const std::vector<std::string> inserts{"Built-in EQ", "Compressor"};
  corevideo::modules::ChannelInsertSettings settings;
  settings["Built-in EQ"]["band3Db"] = 6.0;
  settings["Compressor"]["thresholdDb"] = -24.0;

  // One continuous call (stateless is fine for a single buffer).
  std::vector<float> oneShot = reference;
  corevideo::modules::applyChannelInsertChain(oneShot.data(), oneShot.size(), sampleRate, inserts, false,
                                              &settings);

  // Two block calls WITH persistent state: must match the one-shot exactly.
  std::vector<float> blocks = reference;
  corevideo::modules::ChannelDspState state;
  const size_t half = frames;  // half the interleaved buffer
  corevideo::modules::applyChannelInsertChain(blocks.data(), half, sampleRate, inserts, false, &settings,
                                              nullptr, &state);
  corevideo::modules::applyChannelInsertChain(blocks.data() + half, half, sampleRate, inserts, false,
                                              &settings, nullptr, &state);
  double maxDelta = 0.0;
  for (size_t index = 0; index < blocks.size(); ++index) {
    maxDelta = std::max(maxDelta, std::fabs(static_cast<double>(blocks[index] - oneShot[index])));
  }
  EXPECT_TRUE(maxDelta < 1e-6);

  // And WITHOUT state the second block restarts its filters — visibly
  // different (the bug this fixes).
  std::vector<float> stateless = reference;
  corevideo::modules::applyChannelInsertChain(stateless.data(), half, sampleRate, inserts, false, &settings);
  corevideo::modules::applyChannelInsertChain(stateless.data() + half, half, sampleRate, inserts, false, &settings);
  double statelessDelta = 0.0;
  for (size_t index = half; index < stateless.size(); ++index) {
    statelessDelta = std::max(statelessDelta, std::fabs(static_cast<double>(stateless[index] - oneShot[index])));
  }
  EXPECT_TRUE(statelessDelta > 1e-4);
}

TEST(AudioDsp, SmoothedLimiterIsContinuousAcrossBlocks) {
  // C7d: the block limiter jumped its gain at every 20ms boundary on hot
  // input (zipper distortion). The smoothed limiter with carried state must
  // produce the same output block-by-block as one continuous call.
  const double sampleRate = 48000.0;
  const size_t frames = 1920;
  std::vector<float> reference(frames * 2);
  for (size_t frame = 0; frame < frames; ++frame) {
    const float sample = static_cast<float>(1.4 * std::sin(2.0 * 3.14159265358979323846 * 180.0 * frame / sampleRate));
    reference[frame * 2] = sample;
    reference[frame * 2 + 1] = sample;
  }

  std::vector<float> oneShot = reference;
  double gainA = 1.0;
  corevideo::modules::applySmoothedPeakLimiter(oneShot.data(), oneShot.size(), 2, -1.0, 60.0, sampleRate, &gainA);

  std::vector<float> blocks = reference;
  double gainB = 1.0;
  const size_t half = frames;
  corevideo::modules::applySmoothedPeakLimiter(blocks.data(), half, 2, -1.0, 60.0, sampleRate, &gainB);
  corevideo::modules::applySmoothedPeakLimiter(blocks.data() + half, half, 2, -1.0, 60.0, sampleRate, &gainB);

  double maxDelta = 0.0;
  double peak = 0.0;
  for (size_t index = 0; index < blocks.size(); ++index) {
    maxDelta = std::max(maxDelta, std::fabs(static_cast<double>(blocks[index] - oneShot[index])));
    peak = std::max(peak, std::fabs(static_cast<double>(blocks[index])));
  }
  EXPECT_TRUE(maxDelta < 1e-6);   // continuous across the boundary
  EXPECT_TRUE(peak <= corevideo::modules::dbfsToLinear(-1.0) + 1e-6);  // brickwall holds
}
