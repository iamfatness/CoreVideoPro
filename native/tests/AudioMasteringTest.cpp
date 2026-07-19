#include "modules/AudioMastering.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

using corevideo::modules::applySmoothedPeakLimiter;
using corevideo::modules::applyTruePeakCeiling;
using corevideo::modules::computeTruePeakDbfs;
using corevideo::modules::LimiterState;
using corevideo::modules::MasteringParams;
using corevideo::modules::MasteringState;
using corevideo::modules::processMasteringChain;
using corevideo::modules::TruePeakLimiterState;

namespace {

// Interleaved stereo sine at the given amplitude.
std::vector<float> makeSine(double freq, double amplitude, size_t frames, double rate,
                            size_t startSample = 0) {
  std::vector<float> pcm(frames * 2);
  for (size_t i = 0; i < frames; ++i) {
    const double phase = 2.0 * 3.14159265358979323846 * freq *
                         static_cast<double>(startSample + i) / rate;
    const float v = static_cast<float>(amplitude * std::sin(phase));
    pcm[i * 2] = v;
    pcm[i * 2 + 1] = v;
  }
  return pcm;
}

double blockRms(const std::vector<float>& pcm) {
  double sum = 0.0;
  for (const float v : pcm) sum += static_cast<double>(v) * v;
  return std::sqrt(sum / static_cast<double>(pcm.size()));
}

}  // namespace

TEST(AudioMastering, DisabledIsBitExactPassthrough) {
  MasteringState state;
  MasteringParams params;  // enabled=false
  auto pcm = makeSine(440.0, 0.25, 960, 48000.0);
  const auto original = pcm;
  processMasteringChain(state, params, pcm.data(), 960, 48000.0);
  EXPECT_EQ(pcm, original);
}

TEST(AudioMastering, QuietProgramRidesUpTowardTarget) {
  MasteringState state;
  MasteringParams params;
  params.enabled = true;
  params.targetLufs = -14.0;
  // A -30ish LUFS tone: amplitude 0.03.
  double firstRms = 0.0;
  double lastRms = 0.0;
  size_t start = 0;
  for (int block = 0; block < 3000; ++block) {  // 60s of 20ms blocks
    auto pcm = makeSine(440.0, 0.03, 960, 48000.0, start);
    start += 960;
    processMasteringChain(state, params, pcm.data(), 960, 48000.0);
    if (block == 0) firstRms = blockRms(pcm);
    lastRms = blockRms(pcm);
  }
  // The ride must have lifted the level substantially (bounded by maxRideDb).
  EXPECT_GT(lastRms, firstRms * 1.8);
  EXPECT_GT(state.rideGainDb, 4.0);
  EXPECT_LE(state.rideGainDb, params.maxRideDb + 1e-9);
}

TEST(AudioMastering, RideNeverStepsBetweenSamples) {
  MasteringState state;
  MasteringParams params;
  params.enabled = true;
  params.targetLufs = -14.0;
  params.glueAmount = 0.0;  // isolate the ride
  size_t start = 0;
  float prev = 0.0f;
  bool first = true;
  for (int block = 0; block < 500; ++block) {
    auto pcm = makeSine(440.0, 0.03, 960, 48000.0, start);
    start += 960;
    processMasteringChain(state, params, pcm.data(), 960, 48000.0);
    for (size_t i = 0; i < pcm.size(); i += 2) {
      if (!first) {
        // 440Hz at <=0.25 amp: adjacent-sample tone delta < 0.015; the ride
        // slew must not add visible steps on top.
        EXPECT_LT(std::abs(pcm[i] - prev), 0.05f)
            << "gain step at block " << block << " sample " << i;
      }
      prev = pcm[i];
      first = false;
    }
  }
}

TEST(AudioMastering, CeilingHolds) {
  MasteringState state;
  MasteringParams params;
  params.enabled = true;
  params.targetLufs = -6.0;  // push the ride upward hard
  params.ceilingDbfs = -1.3;
  const float ceilingLinear = static_cast<float>(std::pow(10.0, -1.3 / 20.0));
  size_t start = 0;
  float peak = 0.0f;
  for (int block = 0; block < 2000; ++block) {
    auto pcm = makeSine(440.0, 0.5, 960, 48000.0, start);
    start += 960;
    processMasteringChain(state, params, pcm.data(), 960, 48000.0);
    for (const float v : pcm) peak = std::max(peak, std::abs(v));
  }
  EXPECT_LE(peak, ceilingLinear * 1.02f);  // limiter's sub-ms soft clamp margin
}

TEST(AudioMastering, LoudProgramRidesDownTowardTarget) {
  MasteringState state;
  MasteringParams params;
  params.enabled = true;
  params.targetLufs = -20.0;
  size_t start = 0;
  for (int block = 0; block < 3000; ++block) {
    auto pcm = makeSine(440.0, 0.4, 960, 48000.0, start);  // hot program
    start += 960;
    processMasteringChain(state, params, pcm.data(), 960, 48000.0);
  }
  EXPECT_LT(state.rideGainDb, -3.0);
}

// --- M2 rack stages ---

// Steady-state RMS of a mono tone after running enough blocks for filter
// transients to settle.
static double steadyToneRms(const MasteringParams& params, double freq, double amp) {
  MasteringState state;
  double rms = 0.0;
  size_t start = 0;
  for (int block = 0; block < 40; ++block) {
    auto pcm = makeSine(freq, amp, 960, 48000.0, start);
    start += 960;
    processMasteringChain(state, params, pcm.data(), 960, 48000.0);
    rms = blockRms(pcm);  // last block = settled
  }
  return rms;
}

TEST(AudioMasteringRack, HighPassAttenuatesLowTonePassesHigh) {
  MasteringParams params;
  params.enabled = true;
  params.targetLufs = 0.0;  // ride effectively off at this level range
  params.maxRideDb = 0.0;   // freeze the ride so we measure the filter only
  params.glueAmount = 0.0;
  params.ceilingDbfs = -0.01;
  params.highPassHz = 200.0;

  const double lowRms = steadyToneRms(params, 40.0, 0.1);   // an octave+ below cutoff
  const double highRms = steadyToneRms(params, 2000.0, 0.1); // well above cutoff
  EXPECT_LT(lowRms, highRms * 0.5);  // low tone strongly attenuated
  EXPECT_GT(highRms, 0.05);          // high tone largely preserved
}

TEST(AudioMasteringRack, LowPassAttenuatesHighTone) {
  MasteringParams params;
  params.enabled = true;
  params.maxRideDb = 0.0;
  params.glueAmount = 0.0;
  params.ceilingDbfs = -0.01;
  params.lowPassHz = 1000.0;

  const double lowRms = steadyToneRms(params, 200.0, 0.1);
  const double highRms = steadyToneRms(params, 8000.0, 0.1);
  EXPECT_LT(highRms, lowRms * 0.5);
}

TEST(AudioMasteringRack, LowShelfBoostRaisesLowTone) {
  MasteringParams base;
  base.enabled = true;
  base.maxRideDb = 0.0;
  base.glueAmount = 0.0;
  base.ceilingDbfs = -0.01;

  MasteringParams boosted = base;
  boosted.lowShelfDb = 6.0;

  const double flat = steadyToneRms(base, 60.0, 0.05);
  const double lifted = steadyToneRms(boosted, 60.0, 0.05);
  EXPECT_GT(lifted, flat * 1.5);  // ~+6dB shelf ~= x2
}

TEST(AudioMasteringRack, StereoWidthZeroCollapsesToMono) {
  MasteringState state;
  MasteringParams params;
  params.enabled = true;
  params.maxRideDb = 0.0;
  params.glueAmount = 0.0;
  params.ceilingDbfs = -0.01;
  params.stereoWidth = 0.0;

  // Hard-panned stereo: L tone, R silent.
  std::vector<float> pcm(960 * 2);
  for (size_t i = 0; i < 960; ++i) {
    pcm[i * 2] = static_cast<float>(0.2 * std::sin(2.0 * 3.14159265 * 440.0 * i / 48000.0));
    pcm[i * 2 + 1] = 0.0f;
  }
  processMasteringChain(state, params, pcm.data(), 960, 48000.0);
  // width 0 => L == R at every sample (pure mid).
  for (size_t i = 0; i < 960; ++i) {
    EXPECT_LT(std::abs(pcm[i * 2] - pcm[i * 2 + 1]), 1e-5f);
  }
}

// --- B1: true-peak ceiling / exposed glue dynamics / multiband glue ---

namespace {

// fs/4 sine sampled 45 degrees off-grid: every SAMPLE has magnitude
// amplitude/sqrt(2) (~-3dB below amplitude) but the band-limited
// reconstruction peaks at `amplitude` BETWEEN the samples — the classic
// inter-sample-over vector a sample-peak ceiling cannot see.
std::vector<float> makeIspSignal(double amplitude, size_t frames, size_t startSample) {
  std::vector<float> pcm(frames * 2);
  for (size_t i = 0; i < frames; ++i) {
    const double phase = 3.14159265358979323846 * 0.5 * static_cast<double>(startSample + i) +
                         3.14159265358979323846 * 0.25;
    const float v = static_cast<float>(amplitude * std::sin(phase));
    pcm[i * 2] = v;
    pcm[i * 2 + 1] = v;
  }
  return pcm;
}

std::vector<float> leftChannel(const std::vector<float>& interleaved) {
  std::vector<float> left(interleaved.size() / 2);
  for (size_t i = 0; i < left.size(); ++i) {
    left[i] = interleaved[i * 2];
  }
  return left;
}

// computeTruePeakDbfs clamp-replicates at buffer edges; against an fs/4
// alternating pattern that replication is a step discontinuity whose sinc
// reconstruction RINGS (~+0.4dB over-report measured) — an artifact of
// metering a finite slice, not a real inter-sample over. A short raised-cosine
// fade at both ends removes the edge discontinuity so the meter reads the
// steady-state interior.
double fadedTruePeakDbfs(std::vector<float> mono) {
  const size_t fade = 64;
  for (size_t i = 0; i < fade && i < mono.size(); ++i) {
    const double w = 0.5 - 0.5 * std::cos(3.14159265358979323846 * static_cast<double>(i) /
                                          static_cast<double>(fade));
    mono[i] = static_cast<float>(mono[i] * w);
    mono[mono.size() - 1 - i] = static_cast<float>(mono[mono.size() - 1 - i] * w);
  }
  return computeTruePeakDbfs(mono.data(), mono.size(), 4);
}

}  // namespace

TEST(AudioMasteringTruePeak, HoldsIntersampleOversTheSamplePeakCeilingMissed) {
  const double ceiling = -1.3;
  // Pre-B1 shape: the sample-peak limiter sees only ~-3dBFS samples, does
  // nothing, and the reconstructed waveform still peaks near 0dBFS.
  {
    LimiterState oldState;
    auto pcm = makeIspSignal(1.0, 960, 0);
    applySmoothedPeakLimiter(pcm.data(), pcm.size(), 2, ceiling, 80.0, 48000.0, &oldState);
    const double oldTruePeak = fadedTruePeakDbfs(leftChannel(pcm));
    EXPECT_GT(oldTruePeak, ceiling + 0.5);  // the ISP over sails through (~0dBFS)
  }
  // B1: the full chain's true-peak ceiling holds the reconstruction under.
  MasteringState state;
  MasteringParams params;
  params.enabled = true;
  params.maxRideDb = 0.0;   // freeze the ride: isolate the ceiling stage
  params.glueAmount = 0.0;  // no glue
  params.ceilingDbfs = ceiling;
  size_t start = 0;
  std::vector<float> steadyOut;  // blocks 2..5 concatenated (post warm-up)
  for (int block = 0; block < 6; ++block) {
    auto pcm = makeIspSignal(1.0, 960, start);
    start += 960;
    processMasteringChain(state, params, pcm.data(), 960, 48000.0);
    if (block >= 2) {
      const auto left = leftChannel(pcm);
      steadyOut.insert(steadyOut.end(), left.begin(), left.end());
    }
  }
  const double newTruePeak = fadedTruePeakDbfs(steadyOut);
  EXPECT_LT(newTruePeak, ceiling + 0.15);
}

TEST(AudioMasteringTruePeak, StateContinuityAcrossBlockSplits) {
  // The house continuity test: one 1920-frame call must be bit-identical to
  // two 960-frame calls through the same persistent state (the C7c/C7d
  // block-boundary lesson — per-call state buzzes at tick rate).
  auto whole = makeIspSignal(1.0, 1920, 0);
  auto split = whole;
  TruePeakLimiterState stateWhole;
  TruePeakLimiterState stateSplit;
  applyTruePeakCeiling(whole.data(), 1920, -1.3, 80.0, 48000.0, stateWhole);
  applyTruePeakCeiling(split.data(), 960, -1.3, 80.0, 48000.0, stateSplit);
  applyTruePeakCeiling(split.data() + 960 * 2, 960, -1.3, 80.0, 48000.0, stateSplit);
  for (size_t i = 0; i < whole.size(); ++i) {
    ASSERT_EQ(whole[i], split[i]) << "sample " << i;
  }
}

TEST(AudioMasteringTruePeak, TransparentBelowCeilingAfterDelayPreRoll) {
  // A quiet signal must pass gain-untouched (gain pinned at 1.0): the output
  // is exactly the input delayed by the 16-frame lookahead.
  auto pcm = makeSine(440.0, 0.1, 960, 48000.0);
  const auto original = pcm;
  TruePeakLimiterState state;
  applyTruePeakCeiling(pcm.data(), 960, -1.3, 80.0, 48000.0, state);
  const size_t delay = corevideo::modules::kTruePeakLookaheadFrames;
  for (size_t i = delay; i < 960; ++i) {
    ASSERT_EQ(pcm[i * 2], original[(i - delay) * 2]) << "frame " << i;
    ASSERT_EQ(pcm[i * 2 + 1], original[(i - delay) * 2 + 1]) << "frame " << i;
  }
}

TEST(AudioMasteringGlue, ExposedParamDefaultsAreBitExactLegacyBehavior) {
  // The exposed glue controls default to the pre-B1 fixed constants (2:1,
  // 30ms/250ms, no makeup, single-band). Defaults vs explicit legacy values
  // must be bit-identical — the back-compat invariant for the whole PR.
  MasteringParams defaults;
  defaults.enabled = true;
  MasteringParams explicitLegacy = defaults;
  explicitLegacy.glueRatio = 2.0;
  explicitLegacy.glueAttackMs = 30.0;
  explicitLegacy.glueReleaseMs = 250.0;
  explicitLegacy.glueMakeupDb = 0.0;
  explicitLegacy.glueMultiband = false;
  explicitLegacy.glueBandLowDb = 0.0;
  explicitLegacy.glueBandMidDb = 0.0;
  explicitLegacy.glueBandHighDb = 0.0;
  MasteringState stateA;
  MasteringState stateB;
  size_t start = 0;
  for (int block = 0; block < 50; ++block) {
    auto a = makeSine(440.0, 0.5, 960, 48000.0, start);
    auto b = a;
    start += 960;
    processMasteringChain(stateA, defaults, a.data(), 960, 48000.0);
    processMasteringChain(stateB, explicitLegacy, b.data(), 960, 48000.0);
    ASSERT_EQ(a, b) << "block " << block;
  }
}

namespace {

// Last-block RMS of a hot tone through the chain with the given glue params
// (ride frozen, ceiling out of the way unless stated).
double glueSteadyRms(double ratio, double makeupDb, double amplitude) {
  MasteringParams params;
  params.enabled = true;
  params.maxRideDb = 0.0;
  params.glueAmount = 1.0;
  params.glueRatio = ratio;
  params.glueMakeupDb = makeupDb;
  params.ceilingDbfs = -0.01;
  MasteringState state;
  double rms = 0.0;
  size_t start = 0;
  for (int block = 0; block < 100; ++block) {
    auto pcm = makeSine(440.0, amplitude, 960, 48000.0, start);
    start += 960;
    processMasteringChain(state, params, pcm.data(), 960, 48000.0);
    rms = blockRms(pcm);
  }
  return rms;
}

}  // namespace

TEST(AudioMasteringGlue, HigherRatioCompressesHarder) {
  // amp 0.9 rides ~7dB over the program-relative threshold (targetLufs+6):
  // 2:1 trims ~3.5dB, 8:1 ~6.1dB — clearly ordered.
  const double rms2 = glueSteadyRms(2.0, 0.0, 0.9);
  const double rms8 = glueSteadyRms(8.0, 0.0, 0.9);
  EXPECT_LT(rms8, rms2 * 0.85);
}

TEST(AudioMasteringGlue, MakeupGainRaisesLevel) {
  // Below threshold the compressor is transparent, so +6dB makeup is x2.
  const double flat = glueSteadyRms(2.0, 0.0, 0.05);
  const double madeUp = glueSteadyRms(2.0, 6.0, 0.05);
  EXPECT_LT(std::abs(madeUp / flat - 2.0), 0.1);
}

TEST(AudioMasteringMultiband, OffIsBitIdenticalToSingleBandPath) {
  // With glueMultiband=false the band trims must be dead controls: the
  // single-band code path runs, bit-identical, regardless of their values.
  MasteringParams plain;
  plain.enabled = true;
  MasteringParams withTrims = plain;
  withTrims.glueBandLowDb = 6.0;
  withTrims.glueBandMidDb = -6.0;
  withTrims.glueBandHighDb = 3.0;
  MasteringState stateA;
  MasteringState stateB;
  size_t start = 0;
  for (int block = 0; block < 25; ++block) {
    auto a = makeSine(440.0, 0.5, 960, 48000.0, start);
    auto b = a;
    start += 960;
    processMasteringChain(stateA, plain, a.data(), 960, 48000.0);
    processMasteringChain(stateB, withTrims, b.data(), 960, 48000.0);
    ASSERT_EQ(a, b) << "block " << block;
  }
}

TEST(AudioMasteringMultiband, NeutralRecombinationIsFlat) {
  // THE multiband correctness test: with the compressors engaged but neutral
  // (signal far below threshold) and trims at 0, the 3-band LR4 split must
  // recombine flat — the band sum is allpass (AP2(f1)*AP2(f2) phase, flat
  // magnitude), so steady-state RMS is preserved at every frequency,
  // including AT the 200Hz / 3kHz crossovers. Frequencies are multiples of
  // 50Hz so each 20ms block holds an integer cycle count (phase-independent
  // RMS window despite the limiter's 16-sample delay).
  const double freqs[] = {100.0, 200.0, 750.0, 3000.0, 9000.0};
  for (const double freq : freqs) {
    MasteringParams params;
    params.enabled = true;
    params.maxRideDb = 0.0;
    params.glueAmount = 0.5;  // engaged, but transparent at this level
    params.glueMultiband = true;
    params.ceilingDbfs = -0.01;
    MasteringState state;
    double inRms = 0.0;
    double outRms = 0.0;
    size_t start = 0;
    for (int block = 0; block < 40; ++block) {
      auto pcm = makeSine(freq, 0.02, 960, 48000.0, start);
      start += 960;
      inRms = blockRms(pcm);
      processMasteringChain(state, params, pcm.data(), 960, 48000.0);
      outRms = blockRms(pcm);
    }
    EXPECT_LT(std::abs(outRms / inRms - 1.0), 0.03)
        << "band sum not flat at " << freq << " Hz (in " << inRms << " out " << outRms << ")";
  }
}

TEST(AudioMasteringMultiband, ChainContinuityAcrossBlockSplits) {
  // Full-chain continuity with multiband glue + trims + makeup + the true-peak
  // ceiling all engaged: one 1920-frame call == two 960-frame calls through
  // the same states, bit-exact (ride frozen so the block-size-dependent
  // loudness integration cannot touch the audio).
  MasteringParams params;
  params.enabled = true;
  params.maxRideDb = 0.0;
  params.glueAmount = 1.0;
  params.glueRatio = 4.0;
  params.glueMultiband = true;
  params.glueBandLowDb = 2.0;
  params.glueBandMidDb = -1.0;
  params.glueBandHighDb = 1.0;
  params.glueMakeupDb = 1.5;
  params.ceilingDbfs = -1.3;
  auto whole = makeSine(440.0, 0.8, 1920, 48000.0);
  auto split = whole;
  MasteringState stateWhole;
  MasteringState stateSplit;
  processMasteringChain(stateWhole, params, whole.data(), 1920, 48000.0);
  processMasteringChain(stateSplit, params, split.data(), 960, 48000.0);
  processMasteringChain(stateSplit, params, split.data() + 960 * 2, 960, 48000.0);
  for (size_t i = 0; i < whole.size(); ++i) {
    ASSERT_EQ(whole[i], split[i]) << "sample " << i;
  }
}

TEST(AudioMasteringPerf, TruePeakCeilingWorkerBudget) {
  // Worker-budget guard: the 4x-oversampled detector must stay far under the
  // 20ms tick at 48k stereo. Prints the measured per-block cost (PR evidence)
  // and fails only if it blows an order-of-magnitude margin.
  const int blocks = 500;
  auto makeBlock = [](size_t start) { return makeIspSignal(0.95, 960, start); };
  LimiterState oldState;
  TruePeakLimiterState newState;
  size_t start = 0;
  const auto t0 = std::chrono::steady_clock::now();
  for (int block = 0; block < blocks; ++block) {
    auto pcm = makeBlock(start);
    applySmoothedPeakLimiter(pcm.data(), pcm.size(), 2, -1.3, 80.0, 48000.0, &oldState);
    start += 960;
  }
  const auto t1 = std::chrono::steady_clock::now();
  start = 0;
  for (int block = 0; block < blocks; ++block) {
    auto pcm = makeBlock(start);
    applyTruePeakCeiling(pcm.data(), 960, -1.3, 80.0, 48000.0, newState);
    start += 960;
  }
  const auto t2 = std::chrono::steady_clock::now();
  const double oldMsPerBlock =
      std::chrono::duration<double, std::milli>(t1 - t0).count() / blocks;
  const double newMsPerBlock =
      std::chrono::duration<double, std::milli>(t2 - t1).count() / blocks;
  std::printf("[perf] ceiling per 20ms block: sample-peak %.4f ms, true-peak %.4f ms (delta %.4f ms)\n",
              oldMsPerBlock, newMsPerBlock, newMsPerBlock - oldMsPerBlock);
  EXPECT_LT(newMsPerBlock, 2.0);
}

TEST(AudioMasteringRack, InputGainScales) {
  MasteringParams params;
  params.enabled = true;
  params.maxRideDb = 0.0;
  params.glueAmount = 0.0;
  params.ceilingDbfs = -0.01;
  params.inputGainDb = -6.0;

  const double full = steadyToneRms([] { MasteringParams p; p.enabled = true; p.maxRideDb = 0.0; p.glueAmount = 0.0; p.ceilingDbfs = -0.01; return p; }(), 440.0, 0.1);
  const double trimmed = steadyToneRms(params, 440.0, 0.1);
  EXPECT_LT(trimmed, full * 0.6);  // -6dB ~= x0.5
}
