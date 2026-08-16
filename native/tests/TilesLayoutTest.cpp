#include "compositor/TilesLayout.h"

#include <gtest/gtest.h>

#include <cmath>

namespace {

using corevideo::compositor::LayerRect;
using corevideo::compositor::resolveTileAspectRatio;
using corevideo::compositor::solveTilesLayout;

constexpr double kTol = 1e-9;

std::vector<LayerRect> solve(int n) {
  return solveTilesLayout(n, 16.0 / 9.0, "16:9", 16.0 / 9.0, 0.741, 0.741);
}

TEST(TilesLayout, ZeroTilesProducesNoRects) {
  EXPECT_TRUE(solve(0).empty());
  EXPECT_TRUE(solve(-3).empty());
}

TEST(TilesLayout, OneTileIsCentered) {
  const auto rects = solve(1);
  ASSERT_EQ(rects.size(), 1u);
  EXPECT_NEAR(rects[0].x + rects[0].width / 2.0, 0.5, 1e-6);
  EXPECT_NEAR(rects[0].y + rects[0].height / 2.0, 0.5, 1e-6);
}

TEST(TilesLayout, EveryTileIsTheSameSize) {
  for (int n = 1; n <= 16; ++n) {
    const auto rects = solve(n);
    ASSERT_EQ(rects.size(), static_cast<size_t>(n)) << "n=" << n;
    for (const auto& rect : rects) {
      EXPECT_NEAR(rect.width, rects[0].width, kTol) << "n=" << n;
      EXPECT_NEAR(rect.height, rects[0].height, kTol) << "n=" << n;
    }
  }
}

TEST(TilesLayout, NoTileEscapesTheCanvas) {
  for (int n = 1; n <= 16; ++n) {
    for (const auto& rect : solve(n)) {
      EXPECT_GE(rect.x, -kTol) << "n=" << n;
      EXPECT_GE(rect.y, -kTol) << "n=" << n;
      EXPECT_LE(rect.x + rect.width, 1.0 + kTol) << "n=" << n;
      EXPECT_LE(rect.y + rect.height, 1.0 + kTol) << "n=" << n;
    }
  }
}

TEST(TilesLayout, NoTwoTilesOverlap) {
  for (int n = 2; n <= 12; ++n) {
    const auto rects = solve(n);
    for (size_t a = 0; a < rects.size(); ++a) {
      for (size_t b = a + 1; b < rects.size(); ++b) {
        const bool disjoint =
            rects[a].x + rects[a].width <= rects[b].x + kTol ||
            rects[b].x + rects[b].width <= rects[a].x + kTol ||
            rects[a].y + rects[a].height <= rects[b].y + kTol ||
            rects[b].y + rects[b].height <= rects[a].y + kTol;
        EXPECT_TRUE(disjoint) << "n=" << n << " a=" << a << " b=" << b;
      }
    }
  }
}

// A short last row is centered, not left-aligned — five tiles is 3+2 with the
// pair centered under the trio.
TEST(TilesLayout, ShortLastRowIsCentered) {
  const auto rects = solve(5);
  ASSERT_EQ(rects.size(), 5u);
  const double topRowCenter = (rects[0].x + rects[2].x + rects[2].width) / 2.0;
  const double lastRowCenter = (rects[3].x + rects[4].x + rects[4].width) / 2.0;
  EXPECT_NEAR(topRowCenter, lastRowCenter, 1e-6);
  EXPECT_NEAR(lastRowCenter, 0.5, 1e-6);
}

TEST(TilesLayout, AspectPresetsResolve) {
  EXPECT_NEAR(resolveTileAspectRatio("16:9", 1.0), 16.0 / 9.0, kTol);
  EXPECT_NEAR(resolveTileAspectRatio("1:1", 1.0), 1.0, kTol);
  EXPECT_NEAR(resolveTileAspectRatio("9:16", 1.0), 9.0 / 16.0, kTol);
  EXPECT_NEAR(resolveTileAspectRatio("custom", 2.5), 2.5, kTol);
  EXPECT_NEAR(resolveTileAspectRatio("banana", 1.0), 16.0 / 9.0, kTol);
  // Custom ratios are clamped to the same [0.25, 4] band as the shell.
  EXPECT_NEAR(resolveTileAspectRatio("custom", 99.0), 4.0, kTol);
  EXPECT_NEAR(resolveTileAspectRatio("custom", 0.01), 0.25, kTol);
}

// The gutter spacing parameter produces an exact observable gap between tiles.
// For a 2-tile layout, the gap equals gutterX = gutterY / canvasAspect.
// Verify both the absolute value and that a larger gutter produces a larger gap.
TEST(TilesLayout, LargerGutterProducesLargerGap) {
  const double canvasAspect = 16.0 / 9.0;
  const double pctDefault = 0.741;
  const double pctLarger = 2.0;

  // Compute expected gaps using the divisor form, matching solveTilesLayout
  const double gutterYDefault = 1.0 / (100.0 / pctDefault);
  const double gutterXDefault = gutterYDefault / canvasAspect;

  const double gutterYLarger = 1.0 / (100.0 / pctLarger);
  const double gutterXLarger = gutterYLarger / canvasAspect;

  // Solve for 2 tiles with default gutter (gutter only, fixed margin)
  auto rectsDefault =
      solveTilesLayout(2, canvasAspect, "16:9", 16.0 / 9.0, pctDefault, 0.741);
  ASSERT_EQ(rectsDefault.size(), 2u);

  // For a 2-tile layout (1x2 or 2x1), measure the actual gap
  double gapDefault = 0.0;
  if (rectsDefault[0].x < rectsDefault[1].x) {
    // Horizontal gap: space between right edge of tile 0 and left edge of tile 1
    gapDefault = rectsDefault[1].x - (rectsDefault[0].x + rectsDefault[0].width);
  } else {
    // Vertical gap: space between bottom edge of tile 0 and top edge of tile 1
    gapDefault = rectsDefault[1].y - (rectsDefault[0].y + rectsDefault[0].height);
  }

  // Solve for 2 tiles with larger gutter (gutter only, same margin)
  auto rectsLarger =
      solveTilesLayout(2, canvasAspect, "16:9", 16.0 / 9.0, pctLarger, 0.741);
  ASSERT_EQ(rectsLarger.size(), 2u);

  double gapLarger = 0.0;
  if (rectsLarger[0].x < rectsLarger[1].x) {
    gapLarger = rectsLarger[1].x - (rectsLarger[0].x + rectsLarger[0].width);
  } else {
    gapLarger = rectsLarger[1].y - (rectsLarger[0].y + rectsLarger[0].height);
  }

  // Verify the absolute value for the default case matches the expected formula
  // (within floating-point tolerance after all the solver math)
  EXPECT_NEAR(gapDefault, gutterXDefault, 1e-6);

  // Verify a larger gutter parameter produces a demonstrably larger gap
  EXPECT_GT(gapLarger, gapDefault);
}

}  // namespace
