#include "modules/LumaRangeProbe.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using corevideo::modules::probeLumaRange;

namespace {

std::vector<std::uint8_t> ramp(std::uint32_t width, std::uint32_t height,
                               std::uint8_t low, std::uint8_t high) {
  std::vector<std::uint8_t> plane(static_cast<std::size_t>(width) * height, low);
  for (std::uint32_t y = 0; y < height; ++y) {
    const auto span = height > 1 ? height - 1 : 1;
    const auto value = low + (high - low) * static_cast<int>(y) / static_cast<int>(span);
    for (std::uint32_t x = 0; x < width; ++x) {
      plane[static_cast<std::size_t>(y) * width + x] = static_cast<std::uint8_t>(value);
    }
  }
  return plane;
}

}  // namespace

TEST(LumaRangeProbe, ReportsStudioSwingWithoutInventingExtremes) {
  const auto plane = ramp(64, 64, 16, 235);
  const auto result = probeLumaRange(plane.data(), 64, 64, 64, 1);
  EXPECT_EQ(result.minimum, 16);
  EXPECT_EQ(result.maximum, 235);
  EXPECT_EQ(result.below16, 0u);
  EXPECT_EQ(result.above235, 0u);
}

TEST(LumaRangeProbe, ReportsFullRangeEvidence) {
  const auto plane = ramp(64, 64, 0, 255);
  const auto result = probeLumaRange(plane.data(), 64, 64, 64, 1);
  EXPECT_EQ(result.minimum, 0);
  EXPECT_EQ(result.maximum, 255);
  EXPECT_GT(result.below16, 0u);
  EXPECT_GT(result.above235, 0u);
}

TEST(LumaRangeProbe, DoesNotSampleStridePadding) {
  constexpr std::uint32_t width = 8;
  constexpr std::uint32_t height = 4;
  constexpr std::uint32_t stride = 16;
  std::vector<std::uint8_t> plane(static_cast<std::size_t>(stride) * height, 255);
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      plane[static_cast<std::size_t>(y) * stride + x] = 100;
    }
  }
  const auto result = probeLumaRange(plane.data(), width, height, stride, 1);
  EXPECT_EQ(result.minimum, 100);
  EXPECT_EQ(result.maximum, 100);
}

TEST(LumaRangeProbe, HandlesDegenerateInput) {
  const auto result = probeLumaRange(nullptr, 0, 0, 0, 1);
  EXPECT_EQ(result.sampled, 0u);
}
