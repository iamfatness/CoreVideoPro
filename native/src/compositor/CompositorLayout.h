#pragma once

#include <cstdint>
#include <string>

namespace corevideo::compositor {

struct LayerRect {
  float x = 0.f;
  float y = 0.f;
  float width = 1.f;
  float height = 1.f;
};

inline int gridColumns(int layerCount) {
  if (layerCount <= 1) {
    return 1;
  }
  if (layerCount <= 4) {
    return 2;
  }
  if (layerCount <= 6) {
    return 3;
  }
  return 4;
}

inline LayerRect gridCell(int layerCount, int index, float padding = 0.01f) {
  const int cols = gridColumns(layerCount);
  const int rows = (layerCount + cols - 1) / cols;
  const int col = index % cols;
  const int row = index / cols;
  const float cellW = 1.f / static_cast<float>(cols);
  const float cellH = 1.f / static_cast<float>(rows);
  return {
      col * cellW + padding,
      row * cellH + padding,
      cellW - 2.f * padding,
      cellH - 2.f * padding,
  };
}

inline LayerRect lowerThirdOverlay() {
  return {0.05f, 0.78f, 0.9f, 0.16f};
}

inline LayerRect topRightOverlay() {
  return {0.78f, 0.04f, 0.18f, 0.12f};
}

inline uint32_t colorFromParticipantId(const std::string& participantId) {
  uint32_t hash = 2166136261u;
  for (const unsigned char ch : participantId) {
    hash ^= ch;
    hash *= 16777619u;
  }
  const uint8_t r = static_cast<uint8_t>(72 + (hash & 0x7fu));
  const uint8_t g = static_cast<uint8_t>(72 + ((hash >> 8) & 0x7fu));
  const uint8_t b = static_cast<uint8_t>(72 + ((hash >> 16) & 0x7fu));
  return 0xff000000u | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

}  // namespace corevideo::compositor