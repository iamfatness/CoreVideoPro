#include "core/StreamAudioAlignmentPolicy.h"

#include <gtest/gtest.h>

using corevideo::core::gpuStreamAudioStartupSkipSamples;
using corevideo::core::holdGpuStreamAudio;

TEST(StreamAudioAlignmentPolicy, GpuAudioWaitsForFirstEncodedVideo) {
  EXPECT_TRUE(holdGpuStreamAudio(true, false));
  EXPECT_FALSE(holdGpuStreamAudio(true, true));
  EXPECT_FALSE(holdGpuStreamAudio(false, false));
}

TEST(StreamAudioAlignmentPolicy, CancelsOnlyGpuStreamProgramDelay) {
  EXPECT_EQ(gpuStreamAudioStartupSkipSamples(true, 2, 48000, 2), 3200u);
  EXPECT_EQ(gpuStreamAudioStartupSkipSamples(true, 3, 48000, 2), 4800u);
  EXPECT_EQ(gpuStreamAudioStartupSkipSamples(false, 3, 48000, 2), 0u);
  EXPECT_EQ(gpuStreamAudioStartupSkipSamples(true, 0, 48000, 2), 0u);
  EXPECT_EQ(gpuStreamAudioStartupSkipSamples(true, 3, 0, 2), 0u);
}
