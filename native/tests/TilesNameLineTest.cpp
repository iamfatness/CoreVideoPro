// #1073: per-tile name line geometry (compositor/TilesNameLine.h).
#include "compositor/TilesNameLine.h"

#include <gtest/gtest.h>

using namespace corevideo::compositor;

namespace {

LowerThirdAppearance sceneStyle() {
  LowerThirdAppearance a;
  a.enabled = true;
  a.nameSize = 42;
  a.titleSize = 28;
  a.padding = 20;
  a.width = .6;
  a.cornerRadius = 8;
  a.anchor = "lower-left";
  return a;
}

constexpr float kGutterX = 0.00417f;  // 0.741% of a 16:9 wall, in canvas fractions
constexpr float kGutterY = 0.00741f;

}  // namespace

TEST(TilesNameLine, AFullCanvasTileReadsExactlyLikeTheSceneKey) {
  const auto placed = placeTilesNameLine({0, 0, 1, 1}, kGutterX, kGutterY, sceneStyle());
  ASSERT_TRUE(placed.shown);
  EXPECT_NEAR(placed.appearance.nameSize, 42, 1e-9);
  EXPECT_NEAR(placed.appearance.padding, 20, 1e-9);
  EXPECT_NEAR(placed.appearance.titleSize, 0, 1e-9);  // top line only
  EXPECT_FALSE(placed.appearance.showLogo);
  // height = (nameSize + padding*3) / 1080, the raster's own band formula.
  EXPECT_NEAR(placed.rect.height, (42 + 60) / 1080.f, 1e-6f);
  EXPECT_NEAR(placed.rect.width, .6f, 1e-6f);
  // Bottom-left of the tile, inset by the grid gutter.
  EXPECT_NEAR(placed.rect.x, kGutterX, 1e-6f);
  EXPECT_NEAR(placed.rect.y + placed.rect.height, 1.f - kGutterY, 1e-6f);
}

TEST(TilesNameLine, TextScalesWithTheTileAndSitsInsideIt) {
  const LayerRect tile{.5f, .5f, .5f, .5f};  // one cell of a 2x2 wall
  const auto placed = placeTilesNameLine(tile, kGutterX, kGutterY, sceneStyle());
  ASSERT_TRUE(placed.shown);
  EXPECT_NEAR(placed.appearance.nameSize, 21, 1e-9);  // 42 * 0.5
  EXPECT_NEAR(placed.appearance.padding, 10, 1e-9);
  EXPECT_NEAR(placed.appearance.cornerRadius, 4, 1e-9);
  EXPECT_GE(placed.rect.x, tile.x);
  EXPECT_GE(placed.rect.y, tile.y);
  EXPECT_LE(placed.rect.x + placed.rect.width, tile.x + tile.width + 1e-6f);
  EXPECT_LE(placed.rect.y + placed.rect.height, tile.y + tile.height + 1e-6f);
  EXPECT_NEAR(placed.rect.width, tile.width * .6f, 1e-6f);
}

TEST(TilesNameLine, TextNeverShrinksBelowTheReadableFloorNorGrowsPastTheStyle) {
  // 1/3 height: 42 * .333 = 14 px would be unreadable; the floor holds at 18.
  const auto third = placeTilesNameLine({0, 0, 1 / 3.f, 1 / 3.f}, kGutterX, kGutterY, sceneStyle());
  ASSERT_TRUE(third.shown);
  EXPECT_NEAR(third.appearance.nameSize, kTilesNameMinimumFontPx, 1e-9);
  EXPECT_NEAR(third.appearance.padding, 20 * 18 / 42., 1e-9);
  // A tile taller than the canvas (clamped input) never exceeds the style.
  const auto oversized = placeTilesNameLine({0, 0, 1, 1.5f}, kGutterX, kGutterY, sceneStyle());
  ASSERT_TRUE(oversized.shown);
  EXPECT_NEAR(oversized.appearance.nameSize, 42, 1e-9);
}

TEST(TilesNameLine, ATileTooShortForAReadableLineIsLeftUnlabelled) {
  // 8x8 at 1080p: 135 px tall. Floor font 18 + pad 8.57*3 = 43.7 px band,
  // 32% of the tile: over the 30% share, suppressed.
  const float h = 1 / 8.f;
  EXPECT_FALSE(placeTilesNameLine({0, 0, h, h}, kGutterX, kGutterY, sceneStyle()).shown);
  // 7x7: 154 px tall, 28%: shown.
  const float h7 = 1 / 7.f;
  EXPECT_TRUE(placeTilesNameLine({0, 0, h7, h7}, kGutterX, kGutterY, sceneStyle()).shown);
}

TEST(TilesNameLine, ATileTooNarrowForAFewGlyphsIsLeftUnlabelled) {
  // Tall and thin: 1/3 canvas high but only 50 px wide at 1080p.
  EXPECT_FALSE(placeTilesNameLine({0, 0, 50 / 1920.f, 1 / 3.f}, kGutterX, kGutterY, sceneStyle()).shown);
}

TEST(TilesNameLine, DegenerateTilesAndGuttersNeverPlaceAnything) {
  EXPECT_FALSE(placeTilesNameLine({0, 0, 0, .5f}, kGutterX, kGutterY, sceneStyle()).shown);
  EXPECT_FALSE(placeTilesNameLine({0, 0, .5f, 0}, kGutterX, kGutterY, sceneStyle()).shown);
  EXPECT_FALSE(placeTilesNameLine({0, 0, .5f, .5f}, kGutterX, kGutterY, sceneStyle(), 0).shown);
  // A gutter wider than the tile leaves no room for a band.
  EXPECT_FALSE(placeTilesNameLine({0, 0, .5f, .5f}, .3f, .3f, sceneStyle()).shown);
  // A negative gutter is treated as none, not as an outset.
  const auto placed = placeTilesNameLine({0, 0, 1, 1}, -1.f, -1.f, sceneStyle());
  ASSERT_TRUE(placed.shown);
  EXPECT_EQ(placed.rect.x, 0.f);
}

TEST(TilesNameLine, ASceneWithoutAnAppearanceStillGetsTheDefaultLook) {
  LowerThirdAppearance off;
  off.enabled = false;
  off.nameSize = 999;  // must be ignored: the disabled style is not the look
  const auto placed = placeTilesNameLine({0, 0, 1, 1}, kGutterX, kGutterY, off);
  ASSERT_TRUE(placed.shown);
  EXPECT_TRUE(placed.appearance.enabled);
  EXPECT_NEAR(placed.appearance.nameSize, LowerThirdAppearance{}.nameSize, 1e-9);
  EXPECT_EQ(placed.appearance.preset, LowerThirdAppearance{}.preset);
}

TEST(TilesNameLine, TheStylesAnchorPicksTheTileCorner) {
  auto style = sceneStyle();
  const LayerRect tile{.25f, .25f, .5f, .5f};
  style.anchor = "lower-right";
  const auto lowerRight = placeTilesNameLine(tile, kGutterX, kGutterY, style);
  ASSERT_TRUE(lowerRight.shown);
  EXPECT_NEAR(lowerRight.rect.x + lowerRight.rect.width, tile.x + tile.width - kGutterX, 1e-6f);
  EXPECT_NEAR(lowerRight.rect.y + lowerRight.rect.height, tile.y + tile.height - kGutterY, 1e-6f);
  style.anchor = "upper-left";
  const auto upperLeft = placeTilesNameLine(tile, kGutterX, kGutterY, style);
  ASSERT_TRUE(upperLeft.shown);
  EXPECT_NEAR(upperLeft.rect.x, tile.x + kGutterX, 1e-6f);
  EXPECT_NEAR(upperLeft.rect.y, tile.y + kGutterY, 1e-6f);
}

TEST(TilesNameLine, FollowingKeepsTheCornerOffsetWhileATileMovesAndResizes) {
  const LayerRect target{.5f, .5f, .5f, .5f};
  const auto placed = placeTilesNameLine(target, kGutterX, kGutterY, sceneStyle());
  ASSERT_TRUE(placed.shown);
  // Mid-flight the tile is still larger and further left (it is shrinking
  // from a 1-up into a 2x2 cell).
  const LayerRect sampled{.3f, .4f, .7f, .6f};
  const auto moved = followTilesNameLine(placed.rect, target, sampled, "lower-left");
  EXPECT_NEAR(moved.x - sampled.x, placed.rect.x - target.x, 1e-6f);
  EXPECT_NEAR((sampled.y + sampled.height) - (moved.y + moved.height),
              (target.y + target.height) - (placed.rect.y + placed.rect.height), 1e-6f);
  EXPECT_EQ(moved.width, placed.rect.width);   // size does not breathe mid-flight
  EXPECT_EQ(moved.height, placed.rect.height);
  // At rest (sampled == target) following is the identity.
  const auto still = followTilesNameLine(placed.rect, target, target, "lower-left");
  EXPECT_EQ(still.x, placed.rect.x);
  EXPECT_EQ(still.y, placed.rect.y);
}

TEST(TilesNameLine, FollowingHonoursTheRightAndTopAnchors) {
  const LayerRect target{.5f, 0, .5f, .5f};
  auto style = sceneStyle();
  style.anchor = "upper-right";
  const auto placed = placeTilesNameLine(target, kGutterX, kGutterY, style);
  ASSERT_TRUE(placed.shown);
  const LayerRect sampled{.6f, .1f, .3f, .3f};
  const auto moved = followTilesNameLine(placed.rect, target, sampled, "upper-right");
  EXPECT_NEAR((sampled.x + sampled.width) - (moved.x + moved.width),
              (target.x + target.width) - (placed.rect.x + placed.rect.width), 1e-6f);
  EXPECT_NEAR(moved.y - sampled.y, placed.rect.y - target.y, 1e-6f);
}
