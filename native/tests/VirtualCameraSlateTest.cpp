#include "modules/VirtualCameraSlate.h"

#include <gtest/gtest.h>

#include <vector>

using corevideo::modules::fillVirtualCameraStandbySlate;
using corevideo::modules::nv12FrameSize;

TEST(VirtualCameraSlate, RejectsOddDimensions) {
  std::vector<std::uint8_t> out;
  EXPECT_FALSE(fillVirtualCameraStandbySlate(3, 4, out));
  EXPECT_TRUE(out.empty());
}

TEST(VirtualCameraSlate, ProducesCorrectSizeDarkFieldNeutralChroma) {
  std::vector<std::uint8_t> out;
  ASSERT_TRUE(fillVirtualCameraStandbySlate(48, 24, out));
  EXPECT_EQ(out.size(), nv12FrameSize(48, 24));

  // Top-left corner is the dark field (not the centered bar).
  EXPECT_EQ(out[0], 24);
  // Chroma plane is all neutral 128.
  const size_t uvStart = 48 * 24;
  for (size_t i = uvStart; i < out.size(); ++i) {
    EXPECT_EQ(out[i], 128) << "UV at " << i;
  }
}

TEST(VirtualCameraSlate, HasABrighterCenteredBar) {
  const int w = 48, h = 24;
  std::vector<std::uint8_t> out;
  ASSERT_TRUE(fillVirtualCameraStandbySlate(w, h, out));
  const int centerRow = h / 2;
  const int edgeRow = 0;
  // The center row is inside the bar (brighter) than an edge row.
  EXPECT_GT(out[static_cast<size_t>(centerRow) * w], out[static_cast<size_t>(edgeRow) * w]);
  EXPECT_EQ(out[static_cast<size_t>(centerRow) * w], 130);
}

TEST(VirtualCameraSlate, NeverAllBlack) {
  // Law 2: the slate must never be a dead/black frame.
  std::vector<std::uint8_t> out;
  ASSERT_TRUE(fillVirtualCameraStandbySlate(64, 36, out));
  bool anyNonZero = false;
  for (size_t i = 0; i < static_cast<size_t>(64 * 36); ++i) {
    if (out[i] > 16) {
      anyNonZero = true;
      break;
    }
  }
  EXPECT_TRUE(anyNonZero);
}
