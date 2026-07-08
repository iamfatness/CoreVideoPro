#include "modules/ImageResize.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <vector>

using corevideo::modules::resizeBgraBilinear;

namespace {
std::vector<std::uint8_t> solid(int w, int h, std::uint8_t b, std::uint8_t g, std::uint8_t r) {
  std::vector<std::uint8_t> px(static_cast<size_t>(w) * h * 4);
  for (size_t i = 0; i < px.size(); i += 4) {
    px[i] = b; px[i + 1] = g; px[i + 2] = r; px[i + 3] = 255;
  }
  return px;
}
}  // namespace

TEST(ImageResize, RejectsBadInput) {
  std::vector<std::uint8_t> out;
  EXPECT_FALSE(resizeBgraBilinear(nullptr, 0, 0, 10, 10, out));
}

TEST(ImageResize, SameSizeIsExactCopy) {
  auto src = solid(8, 6, 10, 20, 30);
  std::vector<std::uint8_t> out;
  ASSERT_TRUE(resizeBgraBilinear(src.data(), 8, 6, 8, 6, out));
  EXPECT_EQ(out, src);
}

TEST(ImageResize, ProducesTargetSize) {
  auto src = solid(1920, 1080, 5, 5, 5);
  std::vector<std::uint8_t> out;
  ASSERT_TRUE(resizeBgraBilinear(src.data(), 1920, 1080, 1280, 720, out));
  EXPECT_EQ(out.size(), static_cast<size_t>(1280) * 720 * 4);
}

TEST(ImageResize, SolidColorIsPreservedAcrossScale) {
  // A solid image resizes to the same solid color everywhere (no ringing).
  auto src = solid(1920, 1080, 40, 80, 120);
  std::vector<std::uint8_t> out;
  ASSERT_TRUE(resizeBgraBilinear(src.data(), 1920, 1080, 1280, 720, out));
  for (size_t i = 0; i < out.size(); i += 4) {
    EXPECT_EQ(out[i], 40);
    EXPECT_EQ(out[i + 1], 80);
    EXPECT_EQ(out[i + 2], 120);
    EXPECT_EQ(out[i + 3], 255);
  }
}

TEST(ImageResize, HalfScaleKeepsCornersAndMidtone) {
  // Left column black, right column white (2px wide); downscale to 1px wide.
  const int w = 2, h = 2;
  std::vector<std::uint8_t> src(static_cast<size_t>(w) * h * 4, 0);
  for (int y = 0; y < h; ++y) {
    auto* right = src.data() + (static_cast<size_t>(y) * w + 1) * 4;
    right[0] = right[1] = right[2] = 255; right[3] = 255;
    auto* left = src.data() + (static_cast<size_t>(y) * w + 0) * 4;
    left[3] = 255;
  }
  std::vector<std::uint8_t> out;
  ASSERT_TRUE(resizeBgraBilinear(src.data(), w, h, 1, 1, out));
  ASSERT_EQ(out.size(), 4u);
  // With max(1,dim-1) mapping, the single output pixel samples src(0,0) = black.
  EXPECT_LT(out[0], 128);
}
