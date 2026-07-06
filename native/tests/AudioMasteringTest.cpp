#include "modules/AudioMastering.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using corevideo::modules::MasteringParams;
using corevideo::modules::MasteringState;
using corevideo::modules::processMasteringChain;

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
