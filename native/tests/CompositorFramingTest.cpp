#include "compositor/CompositorLayout.h"

#include <gtest/gtest.h>

#include <cmath>

namespace {

using corevideo::compositor::BorderFraming;
using corevideo::compositor::computeBorderFraming;
using corevideo::compositor::computeSourceFraming;
using corevideo::compositor::LayerRect;
using corevideo::compositor::SourceFraming;

constexpr float kTol = 1e-4f;

bool nearly(float a, float b, float tol = kTol) {
  return std::abs(a - b) < tol;
}

float uvWidth(const SourceFraming& f) {
  return f.u1 - f.u0;
}

float uvHeight(const SourceFraming& f) {
  return f.v1 - f.v0;
}

const LayerRect kFullRect{0.f, 0.f, 1.f, 1.f};

}  // namespace

// Stretch maps the full source to the full destination regardless of aspect:
// full UV window, content == dest, no letterbox.
TEST(CompositorFraming, StretchMapsFullSourceToFullDest) {
  const auto framing = computeSourceFraming(1920, 1080, kFullRect, "stretch", 1.f, 0.f, 0.f);
  EXPECT_TRUE(nearly(framing.u0, 0.f));
  EXPECT_TRUE(nearly(framing.v0, 0.f));
  EXPECT_TRUE(nearly(framing.u1, 1.f));
  EXPECT_TRUE(nearly(framing.v1, 1.f));
  EXPECT_TRUE(nearly(framing.contentX, 0.f));
  EXPECT_TRUE(nearly(framing.contentY, 0.f));
  EXPECT_TRUE(nearly(framing.contentW, 1.f));
  EXPECT_TRUE(nearly(framing.contentH, 1.f));
  EXPECT_TRUE(!framing.hasLetterbox);
}

// Fill/cover a 16:9 source into a square rect: the source is wider than the
// dest, so the horizontal axis is cropped (UV narrower in U), full in V, and
// the content covers the whole rect.
TEST(CompositorFraming, CoverWideSourceIntoSquareCropsHorizontally) {
  const LayerRect square{0.f, 0.f, 1.f, 1.f};
  const auto framing = computeSourceFraming(1920, 1080, square, "fill", 1.f, 0.f, 0.f);
  // destAspect=1, sourceAspect=16/9 -> uvW = 9/16 = 0.5625, uvH = 1.
  EXPECT_TRUE(nearly(uvWidth(framing), 9.f / 16.f));
  EXPECT_TRUE(nearly(uvHeight(framing), 1.f));
  // Centered crop.
  EXPECT_TRUE(nearly(framing.u0, (1.f - 9.f / 16.f) * 0.5f));
  EXPECT_TRUE(nearly(framing.v0, 0.f));
  // Content covers the whole rect, no bars.
  EXPECT_TRUE(nearly(framing.contentW, 1.f));
  EXPECT_TRUE(nearly(framing.contentH, 1.f));
  EXPECT_TRUE(!framing.hasLetterbox);
}

// Fill/cover a square source into a 16:9 rect: the source is taller than the
// dest, so the vertical axis is cropped (UV narrower in V, full in U).
TEST(CompositorFraming, CoverSquareSourceIntoWideRectCropsVertically) {
  const LayerRect wide{0.f, 0.f, 1.6f, 0.9f};  // 16:9.
  const auto framing = computeSourceFraming(512, 512, wide, "cover", 1.f, 0.f, 0.f);
  // destAspect=16/9, sourceAspect=1 -> uvH = sourceAspect/destAspect = 9/16.
  EXPECT_TRUE(nearly(uvWidth(framing), 1.f));
  EXPECT_TRUE(nearly(uvHeight(framing), 9.f / 16.f));
  EXPECT_TRUE(nearly(framing.u0, 0.f));
  EXPECT_TRUE(nearly(framing.v0, (1.f - 9.f / 16.f) * 0.5f));
  EXPECT_TRUE(!framing.hasLetterbox);
}

// Fit/contain a 16:9 source into a square rect produces top/bottom bars: the
// content is shorter than the dest and centered, hasLetterbox true, full UV.
TEST(CompositorFraming, FitWideSourceIntoSquareLetterboxesTopBottom) {
  const LayerRect square{0.1f, 0.2f, 0.8f, 0.8f};
  const auto framing = computeSourceFraming(1920, 1080, square, "fit", 1.f, 0.f, 0.f);
  EXPECT_TRUE(framing.hasLetterbox);
  // Whole source is visible -> full UV window.
  EXPECT_TRUE(nearly(uvWidth(framing), 1.f));
  EXPECT_TRUE(nearly(uvHeight(framing), 1.f));
  // Width spans the full dest, height shrinks to 9/16 of it, centered.
  EXPECT_TRUE(nearly(framing.contentW, 0.8f));
  const float expectedH = 0.8f * (1.f / (16.f / 9.f));  // dest square aspect over source 16:9.
  EXPECT_TRUE(nearly(framing.contentH, expectedH));
  EXPECT_TRUE(nearly(framing.contentX, 0.1f));
  // Centered vertically within the dest rect.
  EXPECT_TRUE(nearly(framing.contentY, 0.2f + (0.8f - expectedH) * 0.5f));
  // Content rect is strictly smaller than the dest on the letterboxed axis.
  EXPECT_TRUE(framing.contentH < square.height);
}

// Fit/contain a tall source into a wide rect produces left/right pillarbox bars.
TEST(CompositorFraming, FitTallSourceIntoWideRectPillarboxes) {
  const LayerRect wide{0.f, 0.f, 1.6f, 0.9f};  // 16:9.
  const auto framing = computeSourceFraming(1080, 1920, wide, "contain", 1.f, 0.f, 0.f);  // 9:16 source.
  EXPECT_TRUE(framing.hasLetterbox);
  EXPECT_TRUE(nearly(framing.contentH, 0.9f));
  EXPECT_TRUE(framing.contentW < wide.width);
  // Centered horizontally.
  EXPECT_TRUE(nearly(framing.contentX, (wide.width - framing.contentW) * 0.5f));
  EXPECT_TRUE(nearly(framing.contentY, 0.f));
}

// Matched aspect under fit produces no bars (content == dest, hasLetterbox false).
TEST(CompositorFraming, FitMatchedAspectHasNoLetterbox) {
  const LayerRect wide{0.f, 0.f, 1.6f, 0.9f};
  const auto framing = computeSourceFraming(1920, 1080, wide, "fit", 1.f, 0.f, 0.f);
  EXPECT_TRUE(!framing.hasLetterbox);
  EXPECT_TRUE(nearly(framing.contentW, 1.6f));
  EXPECT_TRUE(nearly(framing.contentH, 0.9f));
}

// sourceScale=2 samples a centered half-size region (zoom in).
TEST(CompositorFraming, ScaleTwoSamplesCenteredHalfRegion) {
  // Matched aspect so only the zoom drives the UV window.
  const auto framing = computeSourceFraming(1000, 1000, kFullRect, "fill", 2.f, 0.f, 0.f);
  EXPECT_TRUE(nearly(uvWidth(framing), 0.5f));
  EXPECT_TRUE(nearly(uvHeight(framing), 0.5f));
  // Centered: window from 0.25 to 0.75 on both axes.
  EXPECT_TRUE(nearly(framing.u0, 0.25f));
  EXPECT_TRUE(nearly(framing.v0, 0.25f));
  EXPECT_TRUE(nearly(framing.u1, 0.75f));
  EXPECT_TRUE(nearly(framing.v1, 0.75f));
}

// Offset moves the rendered source behind the destination. With scale 2 the
// visible window is half-size at the extremes and progresses from the centered
// position toward the right edge.
TEST(CompositorFraming, OffsetShiftsSampledRegion) {
  const auto centered = computeSourceFraming(1000, 1000, kFullRect, "fill", 2.f, 0.f, 0.f);
  const auto shifted = computeSourceFraming(1000, 1000, kFullRect, "fill", 2.f, 0.5f, 0.f);
  // Same window size, shifted to the right (larger u0), still inside bounds.
  EXPECT_TRUE(nearly(uvWidth(shifted), uvWidth(centered)));
  EXPECT_TRUE(shifted.u0 > centered.u0);
  EXPECT_TRUE(nearly(shifted.u0, 0.375f));
  EXPECT_TRUE(nearly(shifted.v0, 0.25f));  // Y unchanged.
}

TEST(CompositorFraming, FillPanUsesOriginalSourceEdgesAfterZoom) {
  const LayerRect square{0.f, 0.f, 1.f, 1.f};
  const auto centered = computeSourceFraming(1600, 900, square, "fill", 2.f, 0.f, 0.f);
  const auto left = computeSourceFraming(1600, 900, square, "fill", 2.f, -1.f, 0.f);
  const auto right = computeSourceFraming(1600, 900, square, "fill", 2.f, 1.f, 0.f);

  // The visible source window is the square box's aspect against the original
  // 16:9 feed, then zoomed 2x. X pan must reach the original source edges, not
  // the edges of an intermediate cover crop.
  EXPECT_TRUE(nearly(uvWidth(centered), 0.28125f));
  EXPECT_TRUE(nearly(left.u0, 0.f));
  EXPECT_TRUE(nearly(left.u1, 0.28125f));
  EXPECT_TRUE(nearly(right.u0, 0.71875f));
  EXPECT_TRUE(nearly(right.u1, 1.f));
}

// Offset clamps so the sampled region never leaves the source bounds.
TEST(CompositorFraming, OffsetClampsAtBounds) {
  // Far-right pan beyond the available travel pins the window to the edge.
  const auto maxRight = computeSourceFraming(1000, 1000, kFullRect, "fill", 2.f, 5.f, 0.f);
  EXPECT_TRUE(nearly(maxRight.u1, 1.f));
  EXPECT_TRUE(nearly(maxRight.u0, 0.5f));  // half-size window pinned to the right edge.
  // Far-left/up pan beyond the available travel pins to the origin.
  const auto maxLeft = computeSourceFraming(1000, 1000, kFullRect, "fill", 2.f, -5.f, -5.f);
  EXPECT_TRUE(nearly(maxLeft.u0, 0.f));
  EXPECT_TRUE(nearly(maxLeft.v0, 0.f));
  // At scale 1 (full window) there is no travel, so offsets cannot move it.
  const auto noTravel = computeSourceFraming(1000, 1000, kFullRect, "stretch", 1.f, 0.8f, -0.8f);
  EXPECT_TRUE(nearly(noTravel.u0, 0.f));
  EXPECT_TRUE(nearly(noTravel.v0, 0.f));
  EXPECT_TRUE(nearly(noTravel.u1, 1.f));
  EXPECT_TRUE(nearly(noTravel.v1, 1.f));
}

// Scaling below 1 shrinks the rendered source inside the destination instead
// of expanding the UV window beyond the source bounds. Offset then positions
// that smaller source inside the source box.
TEST(CompositorFraming, ScaleBelowOnePositionsWholeSourceInsideDestination) {
  const auto centered = computeSourceFraming(1000, 1000, kFullRect, "stretch", 0.5f, 0.f, 0.f);
  EXPECT_TRUE(nearly(centered.u0, 0.f));
  EXPECT_TRUE(nearly(centered.v0, 0.f));
  EXPECT_TRUE(nearly(centered.u1, 1.f));
  EXPECT_TRUE(nearly(centered.v1, 1.f));
  EXPECT_TRUE(nearly(centered.contentX, 0.25f));
  EXPECT_TRUE(nearly(centered.contentY, 0.25f));
  EXPECT_TRUE(nearly(centered.contentW, 0.5f));
  EXPECT_TRUE(nearly(centered.contentH, 0.5f));
  EXPECT_TRUE(centered.hasLetterbox);

  const auto lowerRight = computeSourceFraming(1000, 1000, kFullRect, "stretch", 0.5f, 1.f, 1.f);
  EXPECT_TRUE(nearly(lowerRight.contentX, 0.5f));
  EXPECT_TRUE(nearly(lowerRight.contentY, 0.5f));
  EXPECT_TRUE(nearly(lowerRight.contentW, 0.5f));
  EXPECT_TRUE(nearly(lowerRight.contentH, 0.5f));
  EXPECT_TRUE(nearly(lowerRight.u0, 0.f));
  EXPECT_TRUE(nearly(lowerRight.u1, 1.f));
}

// Border helper: "none" or zero thickness is invisible; styles map to colors.
TEST(CompositorFraming, BorderVisibilityAndStyleColors) {
  const auto none = computeBorderFraming("none", "#FFFFFF", 4.f);
  EXPECT_TRUE(!none.visible);

  const auto zero = computeBorderFraming("solid", "#FFFFFF", 0.f);
  EXPECT_TRUE(!zero.visible);

  const auto accent = computeBorderFraming("accent", "#FFFFFF", 4.f);
  EXPECT_TRUE(accent.visible);
  EXPECT_EQ(accent.colorRgba, 0xff3ddc97u);
  EXPECT_TRUE(accent.thickness > 0.f);

  const auto program = computeBorderFraming("program", "#FFFFFF", 4.f);
  EXPECT_EQ(program.colorRgba, 0xfff5a623u);

  const auto warning = computeBorderFraming("warning", "#FFFFFF", 4.f);
  EXPECT_EQ(warning.colorRgba, 0xffe5484du);

  // Solid uses the explicit color: "#RRGGBB" -> opaque ARGB.
  const auto solid = computeBorderFraming("solid", "#102030", 6.f);
  EXPECT_TRUE(solid.visible);
  EXPECT_EQ(solid.colorRgba, 0xff102030u);

  // "#RRGGBBAA" maps the trailing alpha into the high byte.
  const auto alpha = computeBorderFraming("solid", "#10203040", 6.f);
  EXPECT_EQ(alpha.colorRgba, 0x40102030u);

  // Thicker borders produce larger thickness values.
  EXPECT_TRUE(solid.thickness > accent.thickness);
}

// Degenerate inputs do not divide by zero and yield a full window.
TEST(CompositorFraming, DegenerateSourceFallsBackToFullWindow) {
  const auto framing = computeSourceFraming(0, 0, kFullRect, "fill", 1.f, 0.f, 0.f);
  EXPECT_TRUE(nearly(uvWidth(framing), 1.f));
  EXPECT_TRUE(nearly(uvHeight(framing), 1.f));
  EXPECT_TRUE(!framing.hasLetterbox);
}

// --- Item 9: overlay keyPhase animation transform. ---

using corevideo::compositor::computeOverlayKeyTransform;

// "hidden" is invisible; "on-air" is fully settled (alpha 1, no slide, scale 1).
TEST(CompositorFraming, OverlayKeyPhaseHiddenAndOnAir) {
  const auto hidden = computeOverlayKeyTransform("hidden", 0.f, "lower-left");
  EXPECT_TRUE(!hidden.visible);
  EXPECT_TRUE(nearly(hidden.alpha, 0.f));

  const auto onAir = computeOverlayKeyTransform("on-air", 1.f, "lower-left");
  EXPECT_TRUE(onAir.visible);
  EXPECT_TRUE(nearly(onAir.alpha, 1.f));
  EXPECT_TRUE(nearly(onAir.slideY, 0.f));
  EXPECT_TRUE(nearly(onAir.contentScale, 1.f));
}

// "building-in" eases alpha 0 -> 1 and slide -> 0 monotonically over progress.
TEST(CompositorFraming, OverlayKeyPhaseBuildingInAnimatesOverProgress) {
  const auto start = computeOverlayKeyTransform("building-in", 0.f, "lower-left");
  const auto mid = computeOverlayKeyTransform("building-in", 0.5f, "lower-left");
  const auto end = computeOverlayKeyTransform("building-in", 1.f, "lower-left");

  EXPECT_TRUE(start.alpha < mid.alpha);
  EXPECT_TRUE(mid.alpha < end.alpha);
  EXPECT_TRUE(nearly(start.alpha, 0.f));
  EXPECT_TRUE(nearly(end.alpha, 1.f));
  // Slide shrinks toward zero as the overlay settles in.
  EXPECT_TRUE(std::abs(start.slideY) > std::abs(end.slideY));
  EXPECT_TRUE(nearly(end.slideY, 0.f));
}

// "building-out" fades alpha 1 -> 0 and slides away.
TEST(CompositorFraming, OverlayKeyPhaseBuildingOutFadesOut) {
  const auto start = computeOverlayKeyTransform("building-out", 0.f, "lower-left");
  const auto end = computeOverlayKeyTransform("building-out", 1.f, "lower-left");
  EXPECT_TRUE(nearly(start.alpha, 1.f));
  EXPECT_TRUE(end.alpha < 0.01f);
  EXPECT_TRUE(!end.visible);
}

// keyPosition flips the slide direction: lower-third slides up (+), upper down.
TEST(CompositorFraming, OverlayKeyPositionFlipsSlideDirection) {
  const auto lower = computeOverlayKeyTransform("building-in", 0.f, "lower-left");
  const auto upper = computeOverlayKeyTransform("building-in", 0.f, "upper-left");
  EXPECT_TRUE(lower.slideY > 0.f);
  EXPECT_TRUE(upper.slideY < 0.f);
}
