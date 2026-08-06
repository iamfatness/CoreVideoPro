#pragma once

#include <cstdint>
#include <vector>

namespace corevideo::modules {

// NV12 -> UYVY for the NDI sender.
//
// WHY THIS EXISTS: the NDI sender used to publish `ProgramFrame::preview` — the
// 320x180 UI thumbnail — so every NDI receiver saw a postage stamp instead of the
// program. The full-resolution program is available, but on Windows only as NV12
// (the GPU tap that already feeds the virtual camera and RTMP), while NDI's proven
// path here is UYVY. This is the bridge.
//
// It is a pure BYTE SHUFFLE, not a color conversion: NV12 and UYVY are both
// BT.601-range YUV, so no matrix or clamping is involved. The only real work is
// the chroma resample — NV12 is 4:2:0 (one UV pair per 2x2 luma block) and UYVY is
// 4:2:2 (one UV pair per 2x1), so each NV12 chroma row is read by the TWO luma
// rows it covers. That keeps it memory-bound rather than compute-bound, which
// matters because this runs on the audio/output worker's deadline.
//
// `width` must be even (UYVY packs pixels in pairs); an odd width drops the last
// column rather than reading past the row. Returns false and leaves `uyvy`
// untouched when the input cannot describe a full NV12 frame.
inline bool nv12ToUyvy(const uint8_t* nv12, int width, int height, std::size_t nv12ByteLen,
                       std::vector<uint8_t>& uyvy) {
  if (nv12 == nullptr || width <= 1 || height <= 1) {
    return false;
  }
  const std::size_t lumaBytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  const std::size_t chromaBytes = lumaBytes / 2u;  // half-height, full-width interleaved UV
  if (nv12ByteLen < lumaBytes + chromaBytes) {
    return false;
  }
  const int pairWidth = width & ~1;  // never read past the row on an odd width
  uyvy.resize(lumaBytes * 2u);

  const uint8_t* lumaPlane = nv12;
  const uint8_t* chromaPlane = nv12 + lumaBytes;
  std::size_t out = 0;
  for (int y = 0; y < height; ++y) {
    const uint8_t* lumaRow = lumaPlane + static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
    // 4:2:0 -> 4:2:2: luma rows 2n and 2n+1 share NV12 chroma row n.
    const uint8_t* chromaRow =
        chromaPlane + static_cast<std::size_t>(y / 2) * static_cast<std::size_t>(width);
    for (int x = 0; x < pairWidth; x += 2) {
      uyvy[out++] = chromaRow[x];      // U  (interleaved as U0 V0 U1 V1 ...)
      uyvy[out++] = lumaRow[x];        // Y0
      uyvy[out++] = chromaRow[x + 1];  // V
      uyvy[out++] = lumaRow[x + 1];    // Y1
    }
    // Odd trailing column (never produced by our pinned-even tap, but keep the
    // buffer fully defined rather than leaving a stale tail).
    for (int x = pairWidth; x < width; ++x) {
      uyvy[out++] = 128;
      uyvy[out++] = lumaRow[x];
    }
  }
  return true;
}

}  // namespace corevideo::modules
