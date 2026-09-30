#include "modules/H264CbrFiller.h"

#include <gtest/gtest.h>

#include <array>
#include <vector>

using namespace corevideo::modules;

TEST(H264CbrFiller, LowComplexityAccessUnitsReachConfiguredSixMbpsByEncodedTime) {
  H264CbrFiller filler(6000, 60);
  const std::array<uint8_t, 6> encoded{0, 0, 0, 1, 0x65, 0x88};
  GpuEncodedChunk chunk;
  chunk.data = encoded.data();
  chunk.size = encoded.size();
  chunk.timingValid = true;
  chunk.duration100ns = 10'000'000 / 60;
  std::vector<uint8_t> padded;
  uint64_t publishedBytes = 0;
  for (int frame = 0; frame < 600; ++frame) {
    chunk.dts100ns = chunk.pts100ns = static_cast<int64_t>(frame) * 10'000'000 / 60;
    ASSERT_TRUE(filler.pad(chunk, padded));
    ASSERT_GE(padded.size(), chunk.size + 6);
    EXPECT_EQ(padded[4] & 0x1f, 6);  // user_data_unregistered SEI
    EXPECT_EQ(padded[5], 5);
    EXPECT_EQ(padded[padded.size() - chunk.size - 1], 0x80);  // rbsp_trailing_bits
    EXPECT_TRUE(std::equal(encoded.begin(), encoded.end(), padded.end() - encoded.size()));
    publishedBytes += padded.size();
  }
  EXPECT_EQ(publishedBytes, filler.emittedBytes());
  const uint64_t expected = 6000ull * 1000 * 10 / 8;
  EXPECT_NEAR(static_cast<double>(publishedBytes), static_cast<double>(expected), 16.0);
}

TEST(H264CbrFiller, InvalidOrUntimedAccessUnitIsNeverAltered) {
  H264CbrFiller filler(6000, 60);
  const std::array<uint8_t, 6> encoded{0, 0, 0, 1, 0x65, 0x88};
  GpuEncodedChunk chunk;
  chunk.data = encoded.data();
  chunk.size = encoded.size();
  std::vector<uint8_t> padded;
  EXPECT_FALSE(filler.pad(chunk, padded));
  EXPECT_TRUE(padded.empty());
  chunk.timingValid = true;
  chunk.data = encoded.data() + 4;
  chunk.size = 2;
  EXPECT_FALSE(filler.pad(chunk, padded));
  EXPECT_TRUE(padded.empty());
}
