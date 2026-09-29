#pragma once

#include "modules/Interfaces.h"

#include <string_view>
#include <vector>

namespace corevideo::core {

// Test-only content probe. The fake engine paints the entire I420 luma plane
// at 235 for one frame. Sampling below its animated top stripe avoids treating
// ordinary fake video pulses as claps. This never requests a GPU readback.
inline bool isWhiteClapFrame(const modules::VideoFrame& frame, std::string_view participantId) {
  if (frame.participantId != participantId || !frame.hasI420()) return false;
  const auto& y = *frame.i420;
  const auto width = static_cast<size_t>(frame.i420Width);
  const auto height = static_cast<size_t>(frame.i420Height);
  for (const auto fy : {1u, 2u, 3u}) {
    for (const auto fx : {1u, 2u, 3u}) {
      if (y[(height * fy / 4) * width + width * fx / 4] < 230) return false;
    }
  }
  return true;
}

inline bool hasWhiteClapSource(const std::vector<modules::VideoFrame>& frames,
                               std::string_view participantId) {
  for (const auto& frame : frames) {
    if (isWhiteClapFrame(frame, participantId)) return true;
  }
  return false;
}

}  // namespace corevideo::core
