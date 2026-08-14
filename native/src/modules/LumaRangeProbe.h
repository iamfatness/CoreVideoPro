#pragma once

#include <cstddef>
#include <cstdint>

namespace corevideo::modules {

// Evidence about the luma values actually delivered by a YUV source. This does
// not guess the range from one camera frame; it records the facts needed to
// compare the SDK request with real meeting media.
struct LumaRangeProbe {
  std::uint8_t minimum = 255;
  std::uint8_t maximum = 0;
  std::uint32_t below16 = 0;
  std::uint32_t above235 = 0;
  std::uint32_t sampled = 0;
};

inline LumaRangeProbe probeLumaRange(const std::uint8_t* yPlane,
                                     std::uint32_t width,
                                     std::uint32_t height,
                                     std::uint32_t stride,
                                     std::uint32_t step = 8) {
  LumaRangeProbe result;
  if (yPlane == nullptr || width == 0 || height == 0 || stride < width) {
    return result;
  }
  if (step == 0) {
    step = 1;
  }

  for (std::uint32_t y = 0; y < height; y += step) {
    const auto* row = yPlane + static_cast<std::size_t>(y) * stride;
    for (std::uint32_t x = 0; x < width; x += step) {
      const auto value = row[x];
      if (value < result.minimum) result.minimum = value;
      if (value > result.maximum) result.maximum = value;
      if (value < 16) ++result.below16;
      if (value > 235) ++result.above235;
      ++result.sampled;
    }
  }
  if (result.sampled == 0) {
    result.minimum = 0;
  }
  return result;
}

}  // namespace corevideo::modules
