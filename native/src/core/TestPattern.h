#pragma once
#include <cstdint>
#include <memory>
#include <vector>

namespace corevideo::core {
// Deterministic 7-bar SMPTE-style BGRA test pattern, immutable/shared so each
// frame is a cheap shared_ptr copy. Center bar (index 3) is green. Byte-identical
// to the former StubModules private copy — the one source of truth now.
inline std::shared_ptr<const std::vector<uint8_t>> makeSmpteBarsBgra(int width, int height) {
  static const uint8_t bars[7][3] = {
      {255, 255, 255}, {0, 255, 255}, {255, 255, 0}, {0, 255, 0},
      {255, 0, 255},   {0, 0, 255},   {255, 0, 0},
  };
  auto pixels = std::make_shared<std::vector<uint8_t>>(
      static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const int bar = (x * 7) / (width > 0 ? width : 1);
      const int b = bar < 7 ? bar : 6;
      const size_t offset = (static_cast<size_t>(y) * width + x) * 4;
      (*pixels)[offset + 0] = bars[b][0];
      (*pixels)[offset + 1] = bars[b][1];
      (*pixels)[offset + 2] = bars[b][2];
      (*pixels)[offset + 3] = 255;
    }
  }
  return pixels;
}
}  // namespace corevideo::core
