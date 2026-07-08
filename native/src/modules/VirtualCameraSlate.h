#pragma once

// Virtual camera standby slate (docs/virtual-camera-spec.md V3, law 2: the
// camera must serve SOMETHING at the declared fps - a black or frozen frame
// reads as "broken" in the consuming app). Before the operator goes live, the
// virtual camera serves this slate: a dark, clearly-intentional NV12 frame with
// a centered horizontal bar so it reads as "standby, working" rather than a
// dead feed. A fully branded logo/text slate needs glyph rasterization and is a
// later refinement; this pure generator is the safe baseline. Unit-tested.

#include "modules/VirtualCameraFrame.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace corevideo::modules {

// Fill `out` with a standby-slate NV12 frame of the given (even) dimensions.
// Dark field (Y = 24, the design near-black), neutral chroma (UV = 128), with a
// brighter centered horizontal bar (Y = 130) ~1/12 of the height. Returns false
// on bad/odd dimensions (out cleared).
inline bool fillVirtualCameraStandbySlate(int width, int height, std::vector<std::uint8_t>& out) {
  if (width <= 0 || height <= 0 || (width & 1) != 0 || (height & 1) != 0) {
    out.clear();
    return false;
  }
  out.assign(nv12FrameSize(width, height), 0);
  constexpr std::uint8_t kFieldY = 24;
  constexpr std::uint8_t kBarY = 130;
  constexpr std::uint8_t kNeutralUv = 128;

  const int barHeight = height / 12 > 0 ? height / 12 : 1;
  const int barTop = (height - barHeight) / 2;
  const int barBottom = barTop + barHeight;

  std::uint8_t* y = out.data();
  for (int row = 0; row < height; ++row) {
    const std::uint8_t value = (row >= barTop && row < barBottom) ? kBarY : kFieldY;
    std::uint8_t* yRow = y + static_cast<std::size_t>(row) * width;
    for (int col = 0; col < width; ++col) {
      yRow[col] = value;
    }
  }
  // Neutral (achromatic) chroma across the whole UV plane.
  std::uint8_t* uv = out.data() + static_cast<std::size_t>(width) * height;
  const std::size_t uvBytes = static_cast<std::size_t>(width) * (height / 2);
  for (std::size_t i = 0; i < uvBytes; ++i) {
    uv[i] = kNeutralUv;
  }
  return true;
}

}  // namespace corevideo::modules
