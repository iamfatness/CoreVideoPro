#pragma once

// Bilinear BGRA resize (docs/virtual-camera-spec.md V3 size negotiation). The
// virtual-camera DLL declares a fixed output size (1280x720), but the program
// preview arrives at whatever size the compositor produced. The publisher scales
// the program to the declared size before NV12 conversion so the camera shows
// the real program at a stable format (not the standby slate). Pure and tested.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace corevideo::modules {

// Resize packed BGRA (4 bytes/px) from src (srcW x srcH) to dst (dstW x dstH)
// with bilinear sampling. `out` is resized to dstW*dstH*4. Returns false on bad
// input. If sizes already match, this is a straight copy.
inline bool resizeBgraBilinear(const std::uint8_t* src, int srcW, int srcH, int dstW, int dstH,
                               std::vector<std::uint8_t>& out) {
  if (src == nullptr || srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) {
    out.clear();
    return false;
  }
  out.assign(static_cast<std::size_t>(dstW) * dstH * 4, 0);
  if (srcW == dstW && srcH == dstH) {
    std::memcpy(out.data(), src, out.size());
    return true;
  }
  // Map dst pixel centers back into src space. Use max(1, dim-1) to avoid /0.
  const double sx = static_cast<double>(srcW - 1) / (dstW > 1 ? (dstW - 1) : 1);
  const double sy = static_cast<double>(srcH - 1) / (dstH > 1 ? (dstH - 1) : 1);
  for (int dy = 0; dy < dstH; ++dy) {
    const double fy = dy * sy;
    int y0 = static_cast<int>(fy);
    int y1 = y0 + 1 < srcH ? y0 + 1 : y0;
    const double wy = fy - y0;
    for (int dx = 0; dx < dstW; ++dx) {
      const double fx = dx * sx;
      int x0 = static_cast<int>(fx);
      int x1 = x0 + 1 < srcW ? x0 + 1 : x0;
      const double wx = fx - x0;

      const std::uint8_t* p00 = src + (static_cast<std::size_t>(y0) * srcW + x0) * 4;
      const std::uint8_t* p01 = src + (static_cast<std::size_t>(y0) * srcW + x1) * 4;
      const std::uint8_t* p10 = src + (static_cast<std::size_t>(y1) * srcW + x0) * 4;
      const std::uint8_t* p11 = src + (static_cast<std::size_t>(y1) * srcW + x1) * 4;
      std::uint8_t* d = out.data() + (static_cast<std::size_t>(dy) * dstW + dx) * 4;
      for (int c = 0; c < 4; ++c) {
        const double top = p00[c] * (1.0 - wx) + p01[c] * wx;
        const double bot = p10[c] * (1.0 - wx) + p11[c] * wx;
        const double v = top * (1.0 - wy) + bot * wy;
        d[c] = static_cast<std::uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v + 0.5));
      }
    }
  }
  return true;
}

}  // namespace corevideo::modules
