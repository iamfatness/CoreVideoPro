#pragma once

#include "compositor/CompositorLayout.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace corevideo::compositor {

// Ported from the shell's DynamicGalleryLayoutService, which remains the
// reference implementation (its tests are this port's acceptance criteria).
// The shell no longer solves at runtime — two live solvers would drift, and the
// day they disagree the editor draws boxes where the program is not rendering.
inline std::string normalizeTileAspectPreset(const std::string& value) {
  if (value == "4:3" || value == "5:4" || value == "1:1" || value == "3:4" ||
      value == "9:16" || value == "custom") {
    return value;
  }
  return "16:9";
}

inline double resolveTileAspectRatio(const std::string& preset, double customAspectRatio) {
  const std::string normalized = normalizeTileAspectPreset(preset);
  if (normalized == "4:3") return 4.0 / 3.0;
  if (normalized == "5:4") return 5.0 / 4.0;
  if (normalized == "1:1") return 1.0;
  if (normalized == "3:4") return 3.0 / 4.0;
  if (normalized == "9:16") return 9.0 / 16.0;
  if (normalized == "custom") return std::clamp(customAspectRatio, 0.25, 4.0);
  return 16.0 / 9.0;
}

inline std::vector<LayerRect> solveTilesLayout(int tileCount,
                                               double canvasAspectRatio,
                                               const std::string& tileAspectPreset,
                                               double customAspectRatio,
                                               double gutterPercent,
                                               double marginPercent) {
  if (tileCount <= 0) {
    return {};
  }

  const double canvasAspect = std::clamp(canvasAspectRatio, 0.25, 4.0);
  const double tileAspect = resolveTileAspectRatio(tileAspectPreset, customAspectRatio);
  // Divisor form: 1.0 / (100.0 / percent) is bit-identical to percent / 100.0 in normalized
  // space [0,1], but matches the C# reference exactly. The distinction becomes load-bearing
  // only at pixel magnitudes (e.g., h=1080); a future pixel-conversion task may inherit this
  // and need to know divisor form preserves the historic constant round-trip (h / (100/0.741)
  // = 8.0028). Never simplify to multiplicative form without checking that task's spec.
  const double gutterY = gutterPercent <= 0.0 ? 0.0 : std::clamp(1.0 / (100.0 / gutterPercent), 0.0, 0.1);
  const double marginY = marginPercent <= 0.0 ? 0.0 : std::clamp(1.0 / (100.0 / marginPercent), 0.0, 0.2);
  const double gutterX = gutterY / canvasAspect;
  const double marginX = marginY / canvasAspect;

  int bestColumns = 0;
  int bestRows = 0;
  double bestWidth = 0.0;
  double bestHeight = 0.0;
  double bestArea = -1.0;

  for (int columns = 1; columns <= tileCount; ++columns) {
    const int rows = static_cast<int>(std::ceil(static_cast<double>(tileCount) / columns));
    const double availableWidth = 1.0 - (2 * marginX) - ((columns - 1) * gutterX);
    const double availableHeight = 1.0 - (2 * marginY) - ((rows - 1) * gutterY);
    if (availableWidth <= 0.0 || availableHeight <= 0.0) {
      continue;
    }
    const double width = std::min(availableWidth / columns,
                                  (availableHeight / rows) * tileAspect / canvasAspect);
    const double height = width * canvasAspect / tileAspect;
    const double area = width * height;
    if (bestArea < 0.0 || area > bestArea + 1e-7 ||
        (std::abs(area - bestArea) < 1e-7 && columns < bestColumns)) {
      bestColumns = columns;
      bestRows = rows;
      bestWidth = width;
      bestHeight = height;
      bestArea = area;
    }
  }

  if (bestArea < 0.0) {
    bestColumns = 1;
    bestRows = tileCount;
    bestWidth = 1.0;
    bestHeight = 1.0 / tileCount;
  }

  const double gridHeight = (bestRows * bestHeight) + ((bestRows - 1) * gutterY);
  const double top = (1.0 - gridHeight) / 2.0;
  std::vector<LayerRect> result;
  result.reserve(static_cast<size_t>(tileCount));

  for (int row = 0; row < bestRows; ++row) {
    const int rowStart = row * bestColumns;
    const int rowCount = std::min(bestColumns, tileCount - rowStart);
    const double rowWidth = (rowCount * bestWidth) + ((rowCount - 1) * gutterX);
    const double left = (1.0 - rowWidth) / 2.0;
    for (int column = 0; column < rowCount; ++column) {
      result.push_back(LayerRect{
          static_cast<float>(left + column * (bestWidth + gutterX)),
          static_cast<float>(top + row * (bestHeight + gutterY)),
          static_cast<float>(bestWidth),
          static_cast<float>(bestHeight)});
    }
  }

  return result;
}

}  // namespace corevideo::compositor
