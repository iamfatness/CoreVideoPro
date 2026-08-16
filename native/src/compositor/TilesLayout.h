#pragma once

#include "CompositorLayout.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace corevideo::compositor {

// Deterministic CoreVideo Tiles layout in normalized canvas space.
// This is the C++ port of DynamicGalleryLayoutService.cs.
class TilesLayout {
 public:
  // Computes tile rectangles for the given tile count and layout parameters.
  // All dimensions are in normalized canvas space [0, 1].
  // canvasAspectRatio: canvas aspect ratio (width / height), clamped [0.25, 4].
  // tileAspectPreset: one of "16:9", "4:3", "5:4", "1:1", "3:4", "9:16", "custom".
  // customAspectRatio: used if tileAspectPreset is "custom".
  // gutterPercent: percentage of canvas height for spacing between tiles, clamped [0, 10].
  // marginPercent: percentage of canvas height for outer margin, clamped [0, 20].
  static std::vector<LayerRect> BuildRects(
      int tileCount,
      double canvasAspectRatio = 16.0 / 9.0,
      const std::string& tileAspectPreset = "16:9",
      double customAspectRatio = 16.0 / 9.0,
      double gutterPercent = 0.741,
      double marginPercent = 0.741);

  // Normalizes an aspect preset token; unknown tokens fall back to "16:9".
  static std::string NormalizeAspectPreset(const std::string& value);

  // Resolves an aspect preset token to its numeric aspect ratio (width / height).
  static double ResolveAspectRatio(const std::string& preset, double customAspectRatio);

 private:
  struct Candidate {
    int columns = 0;
    int rows = 0;
    double width = 0.0;
    double height = 0.0;
    double area = 0.0;

    Candidate() = default;
    Candidate(int cols, int rows_, double w, double h, double a)
        : columns(cols), rows(rows_), width(w), height(h), area(a) {}
  };
};

// Inline implementations.

inline std::string TilesLayout::NormalizeAspectPreset(const std::string& value) {
  if (value == "4:3" || value == "5:4" || value == "1:1" || value == "3:4" ||
      value == "9:16" || value == "custom") {
    return value;
  }
  return "16:9";
}

inline double TilesLayout::ResolveAspectRatio(const std::string& preset,
                                              double customAspectRatio) {
  std::string normalized = NormalizeAspectPreset(preset);
  if (normalized == "4:3") {
    return 4.0 / 3.0;
  } else if (normalized == "5:4") {
    return 5.0 / 4.0;
  } else if (normalized == "1:1") {
    return 1.0;
  } else if (normalized == "3:4") {
    return 3.0 / 4.0;
  } else if (normalized == "9:16") {
    return 9.0 / 16.0;
  } else if (normalized == "custom") {
    return std::clamp(customAspectRatio, 0.25, 4.0);
  }
  return 16.0 / 9.0;
}

inline std::vector<LayerRect> TilesLayout::BuildRects(
    int tileCount,
    double canvasAspectRatio,
    const std::string& tileAspectPreset,
    double customAspectRatio,
    double gutterPercent,
    double marginPercent) {
  if (tileCount <= 0) {
    return {};
  }

  double canvasAspect = std::clamp(canvasAspectRatio, 0.25, 4.0);
  double tileAspect = ResolveAspectRatio(tileAspectPreset, customAspectRatio);
  // Use the divisor form for spacing: gutterPercent / 100.0, never multiplicative.
  double gutterY = std::clamp(gutterPercent / 100.0, 0.0, 0.1);
  double marginY = std::clamp(marginPercent / 100.0, 0.0, 0.2);
  double gutterX = gutterY / canvasAspect;
  double marginX = marginY / canvasAspect;

  Candidate* best = nullptr;
  Candidate bestStorage;

  for (int columns = 1; columns <= tileCount; ++columns) {
    int rows = static_cast<int>(std::ceil(tileCount / static_cast<double>(columns)));
    double availableWidth = 1.0 - (2.0 * marginX) - ((columns - 1) * gutterX);
    double availableHeight = 1.0 - (2.0 * marginY) - ((rows - 1) * gutterY);

    if (availableWidth <= 0.0 || availableHeight <= 0.0) {
      continue;
    }

    double width = std::min(availableWidth / columns,
                            (availableHeight / rows) * tileAspect / canvasAspect);
    double height = width * canvasAspect / tileAspect;
    double area = width * height;

    Candidate candidate(columns, rows, width, height, area);
    if (best == nullptr || candidate.area > best->area + 0.0000001 ||
        (std::abs(candidate.area - best->area) < 0.0000001 && candidate.columns < best->columns)) {
      bestStorage = candidate;
      best = &bestStorage;
    }
  }

  Candidate chosen = (best != nullptr) ? *best : Candidate(1, tileCount, 1.0, 1.0 / tileCount, 1.0 / tileCount);

  double gridHeight = (chosen.rows * chosen.height) + ((chosen.rows - 1) * gutterY);
  double top = (1.0 - gridHeight) / 2.0;
  std::vector<LayerRect> result;
  result.reserve(tileCount);

  for (int row = 0; row < chosen.rows; ++row) {
    int rowStart = row * chosen.columns;
    int rowCount = std::min(chosen.columns, tileCount - rowStart);
    double rowWidth = (rowCount * chosen.width) + ((rowCount - 1) * gutterX);
    double left = (1.0 - rowWidth) / 2.0;

    for (int column = 0; column < rowCount; ++column) {
      result.push_back(LayerRect{
          static_cast<float>(left + column * (chosen.width + gutterX)),
          static_cast<float>(top + row * (chosen.height + gutterY)),
          static_cast<float>(chosen.width),
          static_cast<float>(chosen.height)});
    }
  }

  return result;
}

}  // namespace corevideo::compositor
