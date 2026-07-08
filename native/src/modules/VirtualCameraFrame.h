#pragma once

// Virtual Camera frame conversion (docs/virtual-camera-spec.md V1). The OS
// webcam pipeline (Media Foundation Frame Server, and every app that grabs a
// camera through it) expects NV12: a full-res Y plane followed by a half-res
// interleaved UV plane (4:2:0). The program frame is packed BGRA; this converts
// it once per tick before the frame crosses into the shared-memory ring the
// virtual-camera DLL reads. Pure and unit-tested (no platform dependency).

#include <cstddef>
#include <cstdint>
#include <vector>

namespace corevideo::modules {

// NV12 byte size for a WxH frame: Y (w*h) + UV (w*h/2). Width/height must be
// even for 4:2:0; callers pass even dimensions (720p/1080p are).
inline std::size_t nv12FrameSize(int width, int height) {
  if (width <= 0 || height <= 0) {
    return 0;
  }
  const std::size_t luma = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  return luma + luma / 2;
}

// Convert packed BGRA (bgra[i*4] = B, +1 = G, +2 = R, +3 = A) to NV12 in `out`.
// BT.601 studio-swing coefficients (the webcam-conventional matrix). Chroma is
// 2x2-box-averaged. `out` is resized to nv12FrameSize(width, height). Returns
// false on bad input (out is left empty).
inline bool convertBgraToNv12(const std::uint8_t* bgra, int width, int height,
                              std::vector<std::uint8_t>& out) {
  if (bgra == nullptr || width <= 0 || height <= 0 || (width & 1) != 0 || (height & 1) != 0) {
    out.clear();
    return false;
  }
  out.assign(nv12FrameSize(width, height), 0);
  std::uint8_t* y = out.data();
  std::uint8_t* uv = out.data() + static_cast<std::size_t>(width) * static_cast<std::size_t>(height);

  auto clamp8 = [](int v) -> std::uint8_t {
    return static_cast<std::uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
  };

  // Y plane, per pixel.
  for (int row = 0; row < height; ++row) {
    const std::uint8_t* src = bgra + static_cast<std::size_t>(row) * width * 4;
    std::uint8_t* yRow = y + static_cast<std::size_t>(row) * width;
    for (int col = 0; col < width; ++col) {
      const int b = src[col * 4];
      const int g = src[col * 4 + 1];
      const int r = src[col * 4 + 2];
      // BT.601: Y = 16 + (65.481 R + 128.553 G + 24.966 B)/255, fixed-point.
      yRow[col] = clamp8((66 * r + 129 * g + 25 * b + 128) / 256 + 16);
    }
  }

  // UV plane, one sample per 2x2 block (box average of the block's BGRA).
  for (int row = 0; row < height; row += 2) {
    std::uint8_t* uvRow = uv + static_cast<std::size_t>(row / 2) * width;
    for (int col = 0; col < width; col += 2) {
      int sb = 0, sg = 0, sr = 0;
      for (int dy = 0; dy < 2; ++dy) {
        const std::uint8_t* p = bgra + (static_cast<std::size_t>(row + dy) * width + col) * 4;
        sb += p[0] + p[4];
        sg += p[1] + p[5];
        sr += p[2] + p[6];
      }
      const int b = sb / 4;
      const int g = sg / 4;
      const int r = sr / 4;
      // BT.601 U (Cb) then V (Cr), interleaved.
      const int u = (-38 * r - 74 * g + 112 * b + 128) / 256 + 128;
      const int v = (112 * r - 94 * g - 18 * b + 128) / 256 + 128;
      uvRow[col] = clamp8(u);
      uvRow[col + 1] = clamp8(v);
    }
  }
  return true;
}

// Mirror an NV12 frame horizontally in place (the webcam "mirror me" option -
// most virtual-cam users expect a mirrored self-view). Flips each Y row, and
// each UV pair as a unit (U/V stay paired; the pairs reverse across the row).
// V3 (docs/virtual-camera-spec.md). Pure; even width required (NV12 is even).
inline void mirrorNv12InPlace(std::uint8_t* nv12, int width, int height) {
  if (nv12 == nullptr || width <= 0 || height <= 0 || (width & 1) != 0) {
    return;
  }
  // Y plane: reverse each row.
  for (int row = 0; row < height; ++row) {
    std::uint8_t* y = nv12 + static_cast<std::size_t>(row) * width;
    for (int a = 0, b = width - 1; a < b; ++a, --b) {
      const std::uint8_t t = y[a];
      y[a] = y[b];
      y[b] = t;
    }
  }
  // UV plane: reverse the order of the 2-byte (U,V) chroma pairs per row.
  std::uint8_t* uv = nv12 + static_cast<std::size_t>(width) * height;
  const int pairsPerRow = width / 2;
  for (int row = 0; row < height / 2; ++row) {
    std::uint8_t* r = uv + static_cast<std::size_t>(row) * width;
    for (int a = 0, b = pairsPerRow - 1; a < b; ++a, --b) {
      const std::uint8_t u = r[a * 2];
      const std::uint8_t v = r[a * 2 + 1];
      r[a * 2] = r[b * 2];
      r[a * 2 + 1] = r[b * 2 + 1];
      r[b * 2] = u;
      r[b * 2 + 1] = v;
    }
  }
}

}  // namespace corevideo::modules
