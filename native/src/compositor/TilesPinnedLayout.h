#pragma once
#include "compositor/CompositorLayout.h"
#include <algorithm>
#include <vector>

namespace corevideo::compositor {
// Partition free space without overlap; pack automatic tiles in its largest
// rectangle. Separate smaller pockets are deliberately left as background.
inline LayerRect tilesLargestFreeRect(const std::vector<LayerRect>& pinned) {
  std::vector<LayerRect> free{{0, 0, 1, 1}};
  for (const auto& pin : pinned) {
    std::vector<LayerRect> next;
    for (const auto& cell : free) {
      const float left = std::max(cell.x, pin.x), top = std::max(cell.y, pin.y);
      const float right = std::min(cell.x + cell.width, pin.x + pin.width);
      const float bottom = std::min(cell.y + cell.height, pin.y + pin.height);
      if (left >= right || top >= bottom) { next.push_back(cell); continue; }
      if (top > cell.y) next.push_back({cell.x, cell.y, cell.width, top - cell.y});
      if (bottom < cell.y + cell.height) next.push_back({cell.x, bottom, cell.width, cell.y + cell.height - bottom});
      if (left > cell.x) next.push_back({cell.x, top, left - cell.x, bottom - top});
      if (right < cell.x + cell.width) next.push_back({right, top, cell.x + cell.width - right, bottom - top});
    }
    free = std::move(next);
  }
  LayerRect best{0, 0, 0, 0};
  for (const auto& cell : free) if (cell.width * cell.height > best.width * best.height) best = cell;
  return best;
}
} // namespace corevideo::compositor
