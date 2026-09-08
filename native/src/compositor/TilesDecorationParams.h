#pragma once
#include <algorithm>
#include <cmath>
#include <cstring>
#include "compositor/CompositorLayout.h"
#include "compositor/CompositorShaderParams.h"

namespace corevideo::modules {
inline void applyTilesDecoration(LayerShaderConstants& c, const CompositorRenderPlanLayer& layer,
                                 int width, int height) {
  std::fill_n(c.tileRect, 4, 0.f);
  std::fill_n(c.tileShape, 4, 0.f);
  std::fill_n(c.tileBorder, 4, 0.f);
  std::fill_n(c.tileGlow, 4, 0.f);
  std::fill_n(c.tileFalloff, 4, 0.f);
  const auto& style = layer.tilesDecoration;
  if (!style.enabled || width <= 0 || height <= 0 ||
      (!style.glowPass && style.borderWidth <= 0.f && style.radius <= 0.f)) return;
  const float halfW = layer.rect.width * width * 0.5f;
  const float halfH = layer.rect.height * height * 0.5f;
  const float limit = std::min(halfW, halfH);
  if (!(limit > 0.f)) return;
  const auto finiteClamp = [](float value, float maximum) {
    return std::isfinite(value) ? std::clamp(value, 0.f, maximum) : 0.f;
  };
  c.tileRect[0] = layer.rect.x * width + halfW;
  c.tileRect[1] = layer.rect.y * height + halfH;
  c.tileRect[2] = halfW; c.tileRect[3] = halfH;
  c.tileShape[0] = 1.f;
  c.tileShape[1] = finiteClamp(style.radius, limit);
  c.tileShape[2] = finiteClamp(style.borderWidth, limit);
  c.tileShape[3] = style.glowPass ? 1.f : 0.f;
  const auto border = compositor::parseHexColorRgba(style.borderColor, 0xff000000u);
  const auto glow = compositor::parseHexColorRgba(style.glowColor, 0xffffffffu);
  for (int channel = 0; channel < 3; ++channel) {
    const int shift = (2 - channel) * 8;
    c.tileBorder[channel] = ((border >> shift) & 255) / 255.f;
    c.tileGlow[channel] = ((glow >> shift) & 255) / 255.f;
  }
  c.tileGlow[3] = finiteClamp(style.glowIntensity, 1.f);
  c.tileFalloff[0] = finiteClamp(style.glowSize, 128.f);
  c.tileFalloff[1] = finiteClamp(style.glowSoftness, 1.f) * 2.f;
}
inline compositor::LayerRect tilesGlowRect(const CompositorRenderPlanLayer& layer, int width, int height) {
  const float extent = std::isfinite(layer.tilesDecoration.glowSize)
      ? std::clamp(layer.tilesDecoration.glowSize, 0.f, 128.f) : 0.f;
  const float dx = width > 0 ? extent / width : 0.f;
  const float dy = height > 0 ? extent / height : 0.f;
  const float left = std::max(0.f, layer.rect.x - dx);
  const float top = std::max(0.f, layer.rect.y - dy);
  return {left, top, std::max(0.f, std::min(1.f, layer.rect.x + layer.rect.width + dx) - left),
                    std::max(0.f, std::min(1.f, layer.rect.y + layer.rect.height + dy) - top)};
}
} // namespace corevideo::modules
