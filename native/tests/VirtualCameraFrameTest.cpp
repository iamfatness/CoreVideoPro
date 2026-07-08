#include "modules/VirtualCameraFrame.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <vector>

using corevideo::modules::convertBgraToNv12;
using corevideo::modules::nv12FrameSize;

namespace {

std::vector<std::uint8_t> solidBgra(int w, int h, std::uint8_t b, std::uint8_t g, std::uint8_t r) {
  std::vector<std::uint8_t> px(static_cast<size_t>(w) * h * 4);
  for (size_t i = 0; i < px.size(); i += 4) {
    px[i] = b;
    px[i + 1] = g;
    px[i + 2] = r;
    px[i + 3] = 255;
  }
  return px;
}

}  // namespace

TEST(VirtualCameraFrame, Nv12SizeIsYPlusHalfUv) {
  EXPECT_EQ(nv12FrameSize(1280, 720), static_cast<size_t>(1280 * 720 * 3 / 2));
  EXPECT_EQ(nv12FrameSize(2, 2), static_cast<size_t>(6));  // 4 Y + 2 UV
  EXPECT_EQ(nv12FrameSize(0, 0), static_cast<size_t>(0));
}

TEST(VirtualCameraFrame, RejectsOddDimensions) {
  auto px = solidBgra(3, 2, 0, 0, 0);
  std::vector<std::uint8_t> out;
  EXPECT_FALSE(convertBgraToNv12(px.data(), 3, 2, out));  // odd width
  EXPECT_TRUE(out.empty());
}

TEST(VirtualCameraFrame, BlackMapsToY16Uv128) {
  auto px = solidBgra(4, 4, 0, 0, 0);
  std::vector<std::uint8_t> out;
  ASSERT_TRUE(convertBgraToNv12(px.data(), 4, 4, out));
  ASSERT_EQ(out.size(), nv12FrameSize(4, 4));
  // Y plane = studio-black 16.
  for (int i = 0; i < 4 * 4; ++i) EXPECT_EQ(out[i], 16) << "Y at " << i;
  // UV plane = neutral 128.
  for (size_t i = 4 * 4; i < out.size(); ++i) EXPECT_EQ(out[i], 128) << "UV at " << i;
}

TEST(VirtualCameraFrame, WhiteMapsToHighLumaNeutralChroma) {
  auto px = solidBgra(4, 4, 255, 255, 255);
  std::vector<std::uint8_t> out;
  ASSERT_TRUE(convertBgraToNv12(px.data(), 4, 4, out));
  // White luma ~ 235 (studio white); chroma stays ~128 (achromatic).
  EXPECT_GT(out[0], 230);
  for (size_t i = 4 * 4; i < out.size(); ++i) {
    EXPECT_LT(std::abs(static_cast<int>(out[i]) - 128), 3) << "UV at " << i;
  }
}

TEST(VirtualCameraFrame, PureRedAndBlueHaveOppositeChroma) {
  std::vector<std::uint8_t> red, blue;
  auto rp = solidBgra(4, 4, 0, 0, 255);
  auto bp = solidBgra(4, 4, 255, 0, 0);
  ASSERT_TRUE(convertBgraToNv12(rp.data(), 4, 4, red));
  ASSERT_TRUE(convertBgraToNv12(bp.data(), 4, 4, blue));
  const size_t uvStart = 4 * 4;  // first U (Cb), then V (Cr)
  // Red: high Cr (V), low-ish Cb. Blue: high Cb (U), low Cr. They must differ.
  EXPECT_GT(static_cast<int>(red[uvStart + 1]), static_cast<int>(blue[uvStart + 1]));   // V: red > blue
  EXPECT_GT(static_cast<int>(blue[uvStart]), static_cast<int>(red[uvStart]));           // U: blue > red
}

TEST(VirtualCameraFrame, MirrorFlipsLumaRowsAndChromaPairs) {
  using corevideo::modules::mirrorNv12InPlace;
  // 4x2 NV12: Y = 8 bytes (2 rows of 4), UV = 4 bytes (1 row of 2 pairs).
  // Y row0 = 1 2 3 4 -> 4 3 2 1; row1 = 5 6 7 8 -> 8 7 6 5.
  // UV row = (U0 V0)(U1 V1) = 10 11 20 21 -> (U1 V1)(U0 V0) = 20 21 10 11.
  std::vector<std::uint8_t> f = {1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 20, 21};
  mirrorNv12InPlace(f.data(), 4, 2);
  const std::vector<std::uint8_t> want = {4, 3, 2, 1, 8, 7, 6, 5, 20, 21, 10, 11};
  EXPECT_EQ(f, want);
}

TEST(VirtualCameraFrame, MirrorTwiceIsIdentity) {
  using corevideo::modules::mirrorNv12InPlace;
  auto px = solidBgra(8, 4, 30, 60, 90);
  std::vector<std::uint8_t> nv12;
  ASSERT_TRUE(convertBgraToNv12(px.data(), 8, 4, nv12));
  const auto original = nv12;
  mirrorNv12InPlace(nv12.data(), 8, 4);
  mirrorNv12InPlace(nv12.data(), 8, 4);
  EXPECT_EQ(nv12, original);
}
