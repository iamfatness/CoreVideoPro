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
