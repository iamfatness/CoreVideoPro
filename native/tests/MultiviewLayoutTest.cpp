#include "compositor/CompositorLayout.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

namespace {

using corevideo::compositor::centeredAspectRect;
using corevideo::compositor::computeMultiviewLayout;
using corevideo::compositor::LayerRect;
using corevideo::compositor::MultiviewLayoutPlan;

// The multiview canvas the rects are resolved against.
constexpr float kCanvasW = 1920.f;
constexpr float kCanvasH = 1080.f;
constexpr float kTileAspect = 16.f / 9.f;

bool nearly(float a, float b, float tol) {
  return std::abs(a - b) < tol;
}

// Collect every final (centered-16:9) rect for a layout: the pgm/pvw cells (when
// present) plus one per source cell.
std::vector<LayerRect> finalRects(const MultiviewLayoutPlan& plan) {
  std::vector<LayerRect> rects;
  if (plan.hasProgramPreview) {
    rects.push_back(centeredAspectRect(plan.programCell, kCanvasW, kCanvasH));
    rects.push_back(centeredAspectRect(plan.previewCell, kCanvasW, kCanvasH));
  }
  for (const auto& cell : plan.sourceCells) {
    rects.push_back(centeredAspectRect(cell, kCanvasW, kCanvasH));
  }
  return rects;
}

// Pixel aspect ratio of a normalized rect on the canvas.
float pixelAspect(const LayerRect& r) {
  return (r.width * kCanvasW) / (r.height * kCanvasH);
}

bool isSixteenByNine(const LayerRect& r) {
  return r.width > 0.f && r.height > 0.f && nearly(pixelAspect(r), kTileAspect, 0.02f);
}

bool isWithinCanvas(const LayerRect& r) {
  constexpr float kTol = 1e-3f;
  return r.x >= -kTol && r.y >= -kTol && r.x + r.width <= 1.f + kTol && r.y + r.height <= 1.f + kTol;
}

// True when two rects share more than a hairline of area (touching edges are OK).
bool overlaps(const LayerRect& a, const LayerRect& b) {
  constexpr float kEps = 2e-3f;
  const float ox = std::min(a.x + a.width, b.x + b.width) - std::max(a.x, b.x);
  const float oy = std::min(a.y + a.height, b.y + b.height) - std::max(a.y, b.y);
  return ox > kEps && oy > kEps;
}

const std::vector<std::string> kModes = {"grid", "pgmPvwTop", "pgmPvwLarge", "pgmPvwSide"};
const std::vector<int> kCounts = {1, 2, 4, 6, 8};

}  // namespace

// Every tile (pgm/pvw + sources) in every mode/count is a centered 16:9 rect that
// stays inside the canvas.
TEST(MultiviewLayout, TilesAreSixteenByNineWithinCanvas) {
  for (const auto& mode : kModes) {
    for (const int count : kCounts) {
      const auto plan = computeMultiviewLayout(mode, count);
      const auto rects = finalRects(plan);
      const std::string ctx = "mode=" + mode + " count=" + std::to_string(count);
      EXPECT_FALSE(rects.empty()) << ctx;
      for (size_t i = 0; i < rects.size(); ++i) {
        const std::string rc = ctx + " rect=" + std::to_string(i);
        EXPECT_TRUE(isSixteenByNine(rects[i])) << rc << " aspect=" << pixelAspect(rects[i]);
        EXPECT_TRUE(isWithinCanvas(rects[i])) << rc;
      }
    }
  }
}

// No two tiles overlap (beyond a hairline tolerance) in any mode/count.
TEST(MultiviewLayout, TilesDoNotOverlap) {
  for (const auto& mode : kModes) {
    for (const int count : kCounts) {
      const auto plan = computeMultiviewLayout(mode, count);
      const auto rects = finalRects(plan);
      const std::string ctx = "mode=" + mode + " count=" + std::to_string(count);
      for (size_t i = 0; i < rects.size(); ++i) {
        for (size_t j = i + 1; j < rects.size(); ++j) {
          EXPECT_FALSE(overlaps(rects[i], rects[j]))
              << ctx << " rects " << i << " and " << j << " overlap";
        }
      }
    }
  }
}

// Grid mode has no pgm/pvw and exactly `count` source cells.
TEST(MultiviewLayout, GridHasNoProgramPreview) {
  for (const int count : kCounts) {
    const auto plan = computeMultiviewLayout("grid", count);
    EXPECT_FALSE(plan.hasProgramPreview) << "count=" << count;
    EXPECT_EQ(static_cast<int>(plan.sourceCells.size()), count) << "count=" << count;
  }
}

// The pgmPvw modes always expose a PROGRAM and PREVIEW cell plus `count` sources.
TEST(MultiviewLayout, PgmPvwModesExposeProgramAndPreview) {
  const std::vector<std::string> pgmModes = {"pgmPvwTop", "pgmPvwLarge", "pgmPvwSide"};
  for (const auto& mode : pgmModes) {
    for (const int count : kCounts) {
      const auto plan = computeMultiviewLayout(mode, count);
      const std::string ctx = "mode=" + mode + " count=" + std::to_string(count);
      EXPECT_TRUE(plan.hasProgramPreview) << ctx;
      EXPECT_EQ(static_cast<int>(plan.sourceCells.size()), count) << ctx;
      const auto pgm = centeredAspectRect(plan.programCell, kCanvasW, kCanvasH);
      const auto pvw = centeredAspectRect(plan.previewCell, kCanvasW, kCanvasH);
      EXPECT_TRUE(isSixteenByNine(pgm)) << ctx << " pgm";
      EXPECT_TRUE(isSixteenByNine(pvw)) << ctx << " pvw";
      EXPECT_FALSE(overlaps(pgm, pvw)) << ctx << " pgm/pvw overlap";
    }
  }
}

// Broadcast convention (ATEM/Riedel): PREVIEW on the left, PROGRAM on the right,
// side by side on the top half, for the pgmPvw top modes.
TEST(MultiviewLayout, TopModesPutPreviewLeftOfProgram) {
  for (const std::string mode : {"pgmPvwTop", "pgmPvwLarge"}) {
    for (const int count : kCounts) {
      const auto plan = computeMultiviewLayout(mode, count);
      const std::string ctx = "mode=" + mode + " count=" + std::to_string(count);
      EXPECT_LT(plan.previewCell.x, plan.programCell.x) << ctx;
      EXPECT_TRUE(nearly(plan.previewCell.y, plan.programCell.y, 1e-6f)) << ctx;
    }
  }
}

// Ten sources (the full Show Input capacity) wrap into two readable rows in the
// pgmPvw top modes rather than one sliver-thin strip.
TEST(MultiviewLayout, TenSourcesWrapToTwoRows) {
  for (const std::string mode : {"pgmPvwTop", "pgmPvwLarge"}) {
    const auto plan = computeMultiviewLayout(mode, 10);
    ASSERT_EQ(plan.sourceCells.size(), 10u) << mode;
    EXPECT_GT(plan.sourceCells.back().y, plan.sourceCells.front().y)
        << mode << " expected a second source row";
  }
}

// An unknown mode falls back to grid.
TEST(MultiviewLayout, UnknownModeFallsBackToGrid) {
  const auto plan = computeMultiviewLayout("bogus", 4);
  const auto grid = computeMultiviewLayout("grid", 4);
  EXPECT_FALSE(plan.hasProgramPreview);
  EXPECT_EQ(plan.sourceCells.size(), grid.sourceCells.size());
  for (size_t i = 0; i < plan.sourceCells.size() && i < grid.sourceCells.size(); ++i) {
    EXPECT_TRUE(nearly(plan.sourceCells[i].x, grid.sourceCells[i].x, 1e-6f));
    EXPECT_TRUE(nearly(plan.sourceCells[i].width, grid.sourceCells[i].width, 1e-6f));
  }
}

// Zero sources is valid: grid yields no cells; pgmPvw still yields pgm/pvw.
TEST(MultiviewLayout, ZeroSourcesIsValid) {
  const auto grid = computeMultiviewLayout("grid", 0);
  EXPECT_TRUE(grid.sourceCells.empty());
  const auto pgm = computeMultiviewLayout("pgmPvwTop", 0);
  EXPECT_TRUE(pgm.hasProgramPreview);
  EXPECT_TRUE(pgm.sourceCells.empty());
}
