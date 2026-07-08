#include "modules/VirtualCameraCompose.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <vector>

using corevideo::modules::composeVirtualCameraNv12;
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

TEST(VirtualCameraCompose, NoProgramServesSlate) {
  std::vector<std::uint8_t> out;
  auto r = composeVirtualCameraNv12(nullptr, 0, 0, 1280, 720, false, out);
  EXPECT_TRUE(r.isSlate);
  EXPECT_EQ(r.width, 1280);
  EXPECT_EQ(r.height, 720);
  EXPECT_EQ(out.size(), nv12FrameSize(1280, 720));
}

TEST(VirtualCameraCompose, ValidProgramConverts) {
  auto px = solidBgra(64, 36, 40, 80, 120);
  std::vector<std::uint8_t> out;
  auto r = composeVirtualCameraNv12(px.data(), 64, 36, 1280, 720, false, out);
  EXPECT_FALSE(r.isSlate);
  EXPECT_EQ(r.width, 64);
  EXPECT_EQ(r.height, 36);
  EXPECT_EQ(out.size(), nv12FrameSize(64, 36));
}

TEST(VirtualCameraCompose, MirrorDiffersFromUnmirrored) {
  // Left half red, right half blue -> mirror must swap them (Y differs).
  const int w = 8, h = 4;
  std::vector<std::uint8_t> px(static_cast<size_t>(w) * h * 4, 0);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      auto* p = px.data() + (static_cast<size_t>(y) * w + x) * 4;
      const bool leftHalf = x < w / 2;
      p[2] = leftHalf ? 255 : 0;    // R on the left
      p[0] = leftHalf ? 0 : 255;    // B on the right
      p[3] = 255;
    }
  }
  std::vector<std::uint8_t> plain, mirrored;
  composeVirtualCameraNv12(px.data(), w, h, w, h, false, plain);
  composeVirtualCameraNv12(px.data(), w, h, w, h, true, mirrored);
  EXPECT_NE(plain, mirrored);
  // Mirroring twice returns the original.
  std::vector<std::uint8_t> twice;
  composeVirtualCameraNv12(px.data(), w, h, w, h, true, twice);
  corevideo::modules::mirrorNv12InPlace(twice.data(), w, h);
  EXPECT_EQ(twice, plain);
}

TEST(VirtualCameraCompose, OddProgramDimsFallBackToSlate) {
  // 3x3 (odd, and <2 after mask on one axis is fine but width&~1=2, height&~1=2
  // still >=2) -> actually converts at 2x2. Use a 1-pixel-tall program to force
  // the invalid path.
  auto px = solidBgra(4, 1, 10, 20, 30);
  std::vector<std::uint8_t> out;
  auto r = composeVirtualCameraNv12(px.data(), 4, 1, 640, 360, false, out);
  EXPECT_TRUE(r.isSlate);   // height 1 < 2 -> no program -> slate
  EXPECT_EQ(r.width, 640);
}
