#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>

namespace corevideo::modules {
// Explicit QA-only picture: 32 identity bits between two guard pairs. The
// bottom row is the complement, so a torn/malformed picture fails decoding.
inline constexpr int kDeliveryCounterCells = 36;
inline bool deliveryCounterBit(uint32_t sequence, int cell) {
  if (cell == 0 || cell == 35) return true;
  if (cell == 1 || cell == 34) return false;
  return ((sequence >> (cell - 2)) & 1u) != 0;
}
inline std::optional<uint32_t> decodeDeliveryCounter(const uint8_t* y, size_t bytes,
    int width, int height, int stride) {
  const int cellWidth = (width / kDeliveryCounterCells) & ~1;
  if (!y || cellWidth < 4 || height < 64 || stride < width ||
      bytes < static_cast<size_t>(stride) * height) return {};
  uint32_t sequence = 0;
  for (int cell = 0; cell < kDeliveryCounterCells; ++cell) {
    const int x = cell * cellWidth + cellWidth / 2;
    const auto top = y[static_cast<size_t>(16) * stride + x];
    const auto bottom = y[static_cast<size_t>(height - 16) * stride + x];
    if (!((top < 64 && bottom > 192) || (top > 192 && bottom < 64))) return {};
    const bool bit = top > 192;
    if (cell < 2 || cell >= 34) {
      if (bit != deliveryCounterBit(0, cell)) return {};
    } else if (bit) sequence |= uint32_t{1} << (cell - 2);
  }
  return sequence;
}
}
