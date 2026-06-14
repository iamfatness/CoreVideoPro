#include "zoom/I420Convert.h"

#include <algorithm>

namespace corevideo::zoom {

namespace {
uint8_t clampU8(int32_t v) {
  return static_cast<uint8_t>(std::clamp(v, 0, 255));
}
}  // namespace

std::vector<uint8_t> i420ToRgbaThumbnail(const uint8_t* i420, uint32_t srcWidth, uint32_t srcHeight,
                                         uint32_t dstWidth, uint32_t dstHeight) {
  if (i420 == nullptr || srcWidth == 0 || srcHeight == 0 || dstWidth == 0 || dstHeight == 0) {
    return {};
  }

  const uint32_t chromaWidth = srcWidth / 2u;
  const uint32_t chromaHeight = srcHeight / 2u;
  const uint8_t* yPlane = i420;
  const uint8_t* uPlane = yPlane + static_cast<std::size_t>(srcWidth) * srcHeight;
  const uint8_t* vPlane = uPlane + static_cast<std::size_t>(chromaWidth) * chromaHeight;

  std::vector<uint8_t> rgba(static_cast<std::size_t>(dstWidth) * dstHeight * 4u);

  for (uint32_t dy = 0; dy < dstHeight; ++dy) {
    // Nearest-neighbor source row, clamped to bounds.
    const uint32_t sy = std::min(dy * srcHeight / dstHeight, srcHeight - 1u);
    const uint32_t cy = std::min(sy / 2u, chromaHeight - 1u);
    for (uint32_t dx = 0; dx < dstWidth; ++dx) {
      const uint32_t sx = std::min(dx * srcWidth / dstWidth, srcWidth - 1u);
      const uint32_t cx = std::min(sx / 2u, chromaWidth - 1u);

      const int32_t c = static_cast<int32_t>(yPlane[static_cast<std::size_t>(sy) * srcWidth + sx]) - 16;
      const int32_t d = static_cast<int32_t>(uPlane[static_cast<std::size_t>(cy) * chromaWidth + cx]) - 128;
      const int32_t e = static_cast<int32_t>(vPlane[static_cast<std::size_t>(cy) * chromaWidth + cx]) - 128;

      const std::size_t out = (static_cast<std::size_t>(dy) * dstWidth + dx) * 4u;
      rgba[out + 0] = clampU8((298 * c + 409 * e + 128) >> 8);
      rgba[out + 1] = clampU8((298 * c - 100 * d - 208 * e + 128) >> 8);
      rgba[out + 2] = clampU8((298 * c + 516 * d + 128) >> 8);
      rgba[out + 3] = 255;
    }
  }

  return rgba;
}

}  // namespace corevideo::zoom
