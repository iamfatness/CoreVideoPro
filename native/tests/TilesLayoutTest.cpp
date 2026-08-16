#include "compositor/TilesLayout.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

namespace {

using corevideo::compositor::TilesLayout;
using corevideo::compositor::LayerRect;

// LayoutIsBoundedUniformAndUsesRequestedAspect tests that:
// - All rects are within normalized [0, 1] bounds
// - All rects have uniform width and height
// - Tiles respect the requested aspect ratio
TEST(TilesLayout, LayoutIsBoundedUniformAndUsesRequestedAspect_1) {
  int count = 1;
  auto rects = TilesLayout::BuildRects(count, 16.0 / 9.0, "4:3");

  ASSERT_EQ(count, static_cast<int>(rects.size()));
  auto first = rects[0];
  for (const auto& rect : rects) {
    EXPECT_GE(rect.x, 0.0f);
    EXPECT_LE(rect.x, 1.0f);
    EXPECT_GE(rect.y, 0.0f);
    EXPECT_LE(rect.y, 1.0f);
    EXPECT_GE(rect.x + rect.width, 0.0f);
    EXPECT_LE(rect.x + rect.width, 1.000001f);
    EXPECT_GE(rect.y + rect.height, 0.0f);
    EXPECT_LE(rect.y + rect.height, 1.000001f);
    EXPECT_NEAR(first.width, rect.width, 1e-6);
    EXPECT_NEAR(first.height, rect.height, 1e-6);
    double aspect = rect.width * (16.0 / 9.0) / rect.height;
    EXPECT_NEAR(4.0 / 3.0, aspect, 1e-5);
  }
}

TEST(TilesLayout, LayoutIsBoundedUniformAndUsesRequestedAspect_2) {
  int count = 2;
  auto rects = TilesLayout::BuildRects(count, 16.0 / 9.0, "4:3");

  ASSERT_EQ(count, static_cast<int>(rects.size()));
  auto first = rects[0];
  for (const auto& rect : rects) {
    EXPECT_GE(rect.x, 0.0f);
    EXPECT_LE(rect.x, 1.0f);
    EXPECT_GE(rect.y, 0.0f);
    EXPECT_LE(rect.y, 1.0f);
    EXPECT_GE(rect.x + rect.width, 0.0f);
    EXPECT_LE(rect.x + rect.width, 1.000001f);
    EXPECT_GE(rect.y + rect.height, 0.0f);
    EXPECT_LE(rect.y + rect.height, 1.000001f);
    EXPECT_NEAR(first.width, rect.width, 1e-6);
    EXPECT_NEAR(first.height, rect.height, 1e-6);
    double aspect = rect.width * (16.0 / 9.0) / rect.height;
    EXPECT_NEAR(4.0 / 3.0, aspect, 1e-5);
  }
}

TEST(TilesLayout, LayoutIsBoundedUniformAndUsesRequestedAspect_5) {
  int count = 5;
  auto rects = TilesLayout::BuildRects(count, 16.0 / 9.0, "4:3");

  ASSERT_EQ(count, static_cast<int>(rects.size()));
  auto first = rects[0];
  for (const auto& rect : rects) {
    EXPECT_GE(rect.x, 0.0f);
    EXPECT_LE(rect.x, 1.0f);
    EXPECT_GE(rect.y, 0.0f);
    EXPECT_LE(rect.y, 1.0f);
    EXPECT_GE(rect.x + rect.width, 0.0f);
    EXPECT_LE(rect.x + rect.width, 1.000001f);
    EXPECT_GE(rect.y + rect.height, 0.0f);
    EXPECT_LE(rect.y + rect.height, 1.000001f);
    EXPECT_NEAR(first.width, rect.width, 1e-6);
    EXPECT_NEAR(first.height, rect.height, 1e-6);
    double aspect = rect.width * (16.0 / 9.0) / rect.height;
    EXPECT_NEAR(4.0 / 3.0, aspect, 1e-5);
  }
}

TEST(TilesLayout, LayoutIsBoundedUniformAndUsesRequestedAspect_8) {
  int count = 8;
  auto rects = TilesLayout::BuildRects(count, 16.0 / 9.0, "4:3");

  ASSERT_EQ(count, static_cast<int>(rects.size()));
  auto first = rects[0];
  for (const auto& rect : rects) {
    EXPECT_GE(rect.x, 0.0f);
    EXPECT_LE(rect.x, 1.0f);
    EXPECT_GE(rect.y, 0.0f);
    EXPECT_LE(rect.y, 1.0f);
    EXPECT_GE(rect.x + rect.width, 0.0f);
    EXPECT_LE(rect.x + rect.width, 1.000001f);
    EXPECT_GE(rect.y + rect.height, 0.0f);
    EXPECT_LE(rect.y + rect.height, 1.000001f);
    EXPECT_NEAR(first.width, rect.width, 1e-6);
    EXPECT_NEAR(first.height, rect.height, 1e-6);
    double aspect = rect.width * (16.0 / 9.0) / rect.height;
    EXPECT_NEAR(4.0 / 3.0, aspect, 1e-5);
  }
}

TEST(TilesLayout, LayoutIsBoundedUniformAndUsesRequestedAspect_16) {
  int count = 16;
  auto rects = TilesLayout::BuildRects(count, 16.0 / 9.0, "4:3");

  ASSERT_EQ(count, static_cast<int>(rects.size()));
  auto first = rects[0];
  for (const auto& rect : rects) {
    EXPECT_GE(rect.x, 0.0f);
    EXPECT_LE(rect.x, 1.0f);
    EXPECT_GE(rect.y, 0.0f);
    EXPECT_LE(rect.y, 1.0f);
    EXPECT_GE(rect.x + rect.width, 0.0f);
    EXPECT_LE(rect.x + rect.width, 1.000001f);
    EXPECT_GE(rect.y + rect.height, 0.0f);
    EXPECT_LE(rect.y + rect.height, 1.000001f);
    EXPECT_NEAR(first.width, rect.width, 1e-6);
    EXPECT_NEAR(first.height, rect.height, 1e-6);
    double aspect = rect.width * (16.0 / 9.0) / rect.height;
    EXPECT_NEAR(4.0 / 3.0, aspect, 1e-5);
  }
}

TEST(TilesLayout, ShortFinalRowIsCentered) {
  auto rects = TilesLayout::BuildRects(5);

  float lastRowY = rects[0].y;
  for (const auto& rect : rects) {
    lastRowY = std::max(lastRowY, rect.y);
  }

  std::vector<LayerRect> lastRow;
  for (const auto& rect : rects) {
    if (std::abs(rect.y - lastRowY) < 0.000001f) {
      lastRow.push_back(rect);
    }
  }

  ASSERT_EQ(2, static_cast<int>(lastRow.size()));
  float leftMargin = lastRow[0].x;
  float rightMargin = 1.0f - (lastRow.back().x + lastRow.back().width);
  EXPECT_NEAR(leftMargin, rightMargin, 1e-6);
}

TEST(TilesLayout, EmptyGalleryProducesNoRects) {
  auto rects = TilesLayout::BuildRects(0);
  EXPECT_TRUE(rects.empty());
}

}  // namespace
