#pragma once
#include "modules/Interfaces.h"
#include "modules/CpuSourceGpuView.h"
#include <optional>

namespace corevideo::modules {
// One reservation in the existing Program pool owns these immutable choices.
// CPU and optional-monitor payloads are not retained across composition ticks.
// Preparation may complete later; neither its identity nor observation changes.
struct PreparedProgramComposition {
  static constexpr size_t kMaxSources = 64;
  CompositorRenderPlan plan;
  std::vector<VideoFrame> frames;
  int64_t sequence = 0, productionSlot = -1, anchorNs = 0;

  static std::optional<PreparedProgramComposition> capture(CompositorRenderPlan plan,
      std::vector<VideoFrame> selected, int64_t sequence, int64_t slot, int64_t anchor) {
    if (selected.size() > kMaxSources || sequence <= 0 || slot < 0 || anchor <= 0) return {};
    for (auto& frame : selected) {
      const int width = frame.hasI420() ? frame.i420Width : frame.hasPixels() ? frame.pixelWidth : frame.width;
      const int height = frame.hasI420() ? frame.i420Height : frame.hasPixels() ? frame.pixelHeight : frame.height;
      const auto& token = frame.preparedGpu;
      if (token && token->sourceId == frame.participantId && token->sourceEpoch == frame.sourceEpoch &&
          token->frameId == frame.frameId && token->captureTimestamp100ns == frame.captureTimestamp100ns &&
          token->width == width && token->height == height)
        (void)token->acquire(true); // demand the chosen identity, never a future arrival
      frame.width = width; frame.height = height;
      frame.pixels.reset(); frame.i420.reset(); frame.monitorGpuPixels.reset();
    }
    return PreparedProgramComposition{std::move(plan), std::move(selected), sequence, slot, anchor};
  }
};
}
