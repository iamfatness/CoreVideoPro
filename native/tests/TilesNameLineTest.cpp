// #1073: per-tile name line geometry (compositor/TilesNameLine.h).
#include "compositor/TilesNameLine.h"
#include "compositor/OverlayRasterRetention.h"
#include "compositor/TilesLayout.h"

#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <string>

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

// The threshold is judged against the REAL solver's tile heights (margins and
// gutters included), not an idealised 1/N: with defaults a 7-row wall still
// carries names and an 8-row wall does not.
TEST(TilesNameLine, SolverTileHeightsDecideTheThreshold) {
  const auto tileFor = [](int count) {
    const auto rects = solveTilesLayout(count, 16.0 / 9.0, "16:9", 16.0 / 9.0, 0.741, 0.741);
    return rects.front();
  };
  // 49 members = 7x7: 145.1 px tall at 1080p; the floor-size band (43.7 px)
  // is 30.1% of it, inside the 33% share.
  const auto seven = tileFor(49);
  EXPECT_NEAR(seven.height * 1080.f, 145.1f, .2f);
  EXPECT_TRUE(placeTilesNameLine(seven, kGutterX, kGutterY, sceneStyle()).shown);
  // 50 members: 8 columns x 7 rows, but 8 columns are width-bound, so the
  // tiles are 129.9 px tall (33.6% band): suppressed. 49 -> 50 is the edge.
  const auto fifty = tileFor(50);
  EXPECT_NEAR(fifty.height * 1080.f, 129.9f, .2f);
  EXPECT_FALSE(placeTilesNameLine(fifty, kGutterX, kGutterY, sceneStyle()).shown);
  // 42 members = 7x6: 149.1 px tall, 29.3%: named.
  EXPECT_NEAR(tileFor(42).height * 1080.f, 149.1f, .2f);
  EXPECT_TRUE(placeTilesNameLine(tileFor(42), kGutterX, kGutterY, sceneStyle()).shown);
  // 64 members = 8x8: 126 px tall, the band is 34.7%: suppressed.
  const auto eight = tileFor(64);
  EXPECT_NEAR(eight.height * 1080.f, 126.f, .2f);
  EXPECT_FALSE(placeTilesNameLine(eight, kGutterX, kGutterY, sceneStyle()).shown);
}

TEST(TilesNameLine, AStyleAuthoredBelowTheFloorKeepsItsOwnSize) {
  // nameSize is valid from 16; the floor must never exceed the style
  // (std::clamp with lo > hi is undefined behaviour).
  auto small = sceneStyle();
  small.nameSize = 16;
  const auto full = placeTilesNameLine({0, 0, 1, 1}, kGutterX, kGutterY, small);
  ASSERT_TRUE(full.shown);
  EXPECT_NEAR(full.appearance.nameSize, 16, 1e-9);
  const auto quarter = placeTilesNameLine({0, 0, .25f, .25f}, kGutterX, kGutterY, small);
  ASSERT_TRUE(quarter.shown);
  EXPECT_NEAR(quarter.appearance.nameSize, 16, 1e-9);  // floored at its own size, not 18
}

TEST(TilesNameLine, TheInsetIsTheGutterOrTheDecorationWhicheverIsLarger) {
  EXPECT_NEAR(tilesNameInsetPx(8, 0, 0), 8, 1e-9);
  // A 4 px border plus a 24 px radius bites 4 + 24 * (1 - 1/sqrt2) = 11.03 px.
  EXPECT_NEAR(tilesNameInsetPx(8, 4, 24), 4 + 24 * (1 - 1 / std::sqrt(2.0)), 1e-9);
  EXPECT_NEAR(tilesNameInsetPx(8, 4, 0), 8, 1e-9);     // border alone is under the gutter
  EXPECT_NEAR(tilesNameInsetPx(8, 12, 0), 12, 1e-9);   // a wide border wins
  EXPECT_NEAR(tilesNameInsetPx(-1, -1, -1), 0, 1e-9);  // never negative
}

TEST(TilesNameLine, AGrowingTileScalesItsNameAboutTheAnchoredCornerSoItStaysInside) {
  // Target: a 1-up tile. Sampled: still a quarter of the way out of its
  // former 2x2 cell. Without scaling the full-size band would be wider than
  // the tile it rides.
  const LayerRect target{0, 0, 1, 1};
  const auto placed = placeTilesNameLine(target, kGutterX, kGutterY, sceneStyle());
  ASSERT_TRUE(placed.shown);
  const LayerRect sampled{0, .4f, .6f, .6f};
  const auto moved = followTilesNameLine(placed.rect, target, sampled, "lower-left");
  EXPECT_NEAR(moved.width, placed.rect.width * .6f, 1e-6f);
  EXPECT_NEAR(moved.height, placed.rect.height * .6f, 1e-6f);
  EXPECT_NEAR(moved.x - sampled.x, (placed.rect.x - target.x) * .6f, 1e-6f);
  EXPECT_NEAR((sampled.y + sampled.height) - (moved.y + moved.height),
              ((target.y + target.height) - (placed.rect.y + placed.rect.height)) * .6f, 1e-6f);
  EXPECT_GE(moved.x, sampled.x - 1e-6f);
  EXPECT_LE(moved.x + moved.width, sampled.x + sampled.width + 1e-6f);
  EXPECT_GE(moved.y, sampled.y - 1e-6f);
  EXPECT_LE(moved.y + moved.height, sampled.y + sampled.height + 1e-6f);
  // A SHRINKING tile (sampled larger than target) never scales the band up.
  const auto larger = followTilesNameLine(placed.rect, {.5f, .5f, .5f, .5f}, {0, 0, 1, 1}, "lower-left");
  EXPECT_EQ(larger.width, placed.rect.width);
  // Non-uniform: the smaller ratio wins.
  const auto wide = followTilesNameLine(placed.rect, target, {0, 0, 1, .5f}, "lower-left");
  EXPECT_NEAR(wide.width, placed.rect.width * .5f, 1e-6f);
}

TEST(OverlayRasterRetention, ThePreviousRasterIsDrawnWhileTheNextOneIsPreparing) {
  struct View { int id; };
  View first{1}, second{2};
  OverlayRasterRetention<View> retention;
  auto leaseA = std::make_shared<int>(1), leaseB = std::make_shared<int>(2);
  // Nothing retained yet and nothing ready: draw nothing (never a bare bar).
  EXPECT_EQ(retention.resolve("tile-name:zoom:1", nullptr, nullptr, 100, 40, 1), nullptr);
  EXPECT_EQ(retention.resolve("tile-name:zoom:1", &first, leaseA, 100, 40, 2), &first);
  // Reflow: the band resized, the new raster is pending; the old one is drawn.
  EXPECT_EQ(retention.resolve("tile-name:zoom:1", nullptr, nullptr, 80, 32, 3), &first);
  ASSERT_NE(retention.find("tile-name:zoom:1"), nullptr);
  EXPECT_EQ(retention.find("tile-name:zoom:1")->width, 100);  // sized for the OLD band
  EXPECT_EQ(retention.find("tile-name:zoom:1")->lease, leaseA);
  EXPECT_EQ(retention.resolve("tile-name:zoom:1", &second, leaseB, 80, 32, 4), &second);
  EXPECT_EQ(retention.find("tile-name:zoom:1")->width, 80);
  // Another layer never borrows this one's raster.
  EXPECT_EQ(retention.resolve("tile-name:zoom:2", nullptr, nullptr, 80, 32, 4), nullptr);
}

TEST(OverlayRasterRetention, IdleAndOverflowingEntriesArePruned) {
  struct View { int id; };
  View v{1};
  OverlayRasterRetention<View> retention;
  auto lease = std::make_shared<int>(0);
  retention.resolve("a", &v, lease, 10, 10, 1);
  retention.resolve("b", &v, lease, 10, 10, 100);
  retention.prune(200);  // "a" idle for 199 frames, "b" for 100: only "a" goes
  EXPECT_EQ(retention.find("a"), nullptr);
  EXPECT_NE(retention.find("b"), nullptr);
  for (int i = 0; i < 600; ++i) retention.resolve("x" + std::to_string(i), &v, lease, 1, 1, 300);
  retention.resolve("fresh", &v, lease, 1, 1, 301);
  retention.prune(301, 120, 512);
  EXPECT_EQ(retention.size(), 1u);  // everything but this frame's entries
  EXPECT_NE(retention.find("fresh"), nullptr);
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
  // The sampled tile is 0.6 of its target on both axes, so band and corner
  // offset scale by 0.6 about the upper-right corner.
  const LayerRect sampled{.6f, .1f, .3f, .3f};
  const auto moved = followTilesNameLine(placed.rect, target, sampled, "upper-right");
  EXPECT_NEAR((sampled.x + sampled.width) - (moved.x + moved.width),
              ((target.x + target.width) - (placed.rect.x + placed.rect.width)) * .6f, 1e-6f);
  EXPECT_NEAR(moved.y - sampled.y, (placed.rect.y - target.y) * .6f, 1e-6f);
  EXPECT_NEAR(moved.width, placed.rect.width * .6f, 1e-6f);
  // Unscaled when the sampled tile is as large as its target.
  const auto settled = followTilesNameLine(placed.rect, target, {.4f, .2f, .5f, .5f}, "upper-right");
  EXPECT_NEAR((.4f + .5f) - (settled.x + settled.width),
              (target.x + target.width) - (placed.rect.x + placed.rect.width), 1e-6f);
}
