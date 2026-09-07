#include "core/ProgramAudioDelay.h"
#include <gtest/gtest.h>
using corevideo::core::ProgramAudioDelay;

TEST(ProgramAudioDelay, ThreeFramesDelaysStereoImpulseAcrossTwentyMillisecondBlocks) {
  ProgramAudioDelay delay;
  std::vector<float> block(960 * 2, 0.f); block[0] = 1.f; block[1] = -1.f;
  auto first = delay.process(block, 2, 48000, 3);
  EXPECT_EQ(first[0], 0.f);
  block[0] = block[1] = 0.f;
  auto second = delay.process(block, 2, 48000, 3);
  EXPECT_EQ(second[0], 0.f);
  const auto third = delay.process(block, 2, 48000, 3);
  EXPECT_EQ(third[480 * 2], 1.f);
  EXPECT_EQ(third[480 * 2 + 1], -1.f);
  EXPECT_EQ(third.size(), block.size());
}
TEST(ProgramAudioDelay, TwoFramesPreservesRampAndExactSampleCount) {
  ProgramAudioDelay delay;
  std::vector<float> result;
  for (int offset = 0; offset < 4800; offset += 960) {
    std::vector<float> block(960);
    for (int i = 0; i < 960; ++i) block[i] = static_cast<float>(offset + i + 1);
    const auto& output = delay.process(block, 1, 48000, 2);
    result.insert(result.end(), output.begin(), output.end());
  }
  EXPECT_EQ(result.size(), 4800U);
  EXPECT_EQ(result[1599], 0.f);
  EXPECT_EQ(result[1600], 1.f);
  EXPECT_EQ(result[4799], 3200.f);
}
TEST(ProgramAudioDelay, EmptyMutedBlocksAdvanceRatherThanReplayStaleSamplesLater) {
  ProgramAudioDelay delay;
  std::vector<float> signal(960, 1.f);
  delay.process(signal, 1, 48000, 2);
  delay.process({}, 1, 48000, 2, 960);
  delay.process({}, 1, 48000, 2, 960);
  const auto output = delay.process({}, 1, 48000, 2, 960);
  EXPECT_EQ(output.size(), 960U);
  for (float sample : output) EXPECT_EQ(sample, 0.f);
}
TEST(ProgramAudioDelay, UnsupportedBackendBypassesAndReactivationResetsHistory) {
  ProgramAudioDelay delay;
  const std::vector<float> signal{1.f, -1.f};
  EXPECT_TRUE(delay.process(signal, 2, 48000, 0) == signal);
  delay.process(signal, 2, 48000, 3);
  EXPECT_TRUE(delay.process(signal, 2, 48000, 0) == signal);
  const auto output = delay.process(signal, 2, 48000, 3);
  EXPECT_EQ(output[0], 0.f);
  EXPECT_EQ(output[1], 0.f);
}
TEST(ProgramAudioDelay, ChannelLayoutChangeClearsPriorChannelHistory) {
  ProgramAudioDelay delay;
  delay.process(std::vector<float>(2400, 1.f), 1, 48000, 3);
  const auto output = delay.process(std::vector<float>(960 * 2, 0.f), 2, 48000, 3);
  for (float sample : output) EXPECT_EQ(sample, 0.f);
}
