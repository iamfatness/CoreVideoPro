#pragma once

// Shared fixtures for the P9 backdrop blur tests on both GPUs
// (D3D11BackdropBlurTest.cpp, MetalBackdropBlurTest.mm) and the portable
// reference tests (GraphicsBackdropTest.cpp). One definition, so Windows and
// Mac judge the same pictures against the same CPU reference.

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "compositor/BackdropBlur.h"
#include "graphics/GraphicsTemplate.h"
#include "modules/Interfaces.h"
#include "modules/ProgramFramePreview.h"

namespace backdrop_fixture {

// A hard-edged picture with every frequency a blur must remove: 24 px
// black/white checker, a colour ramp so tint and channel order are visible.
inline std::vector<uint8_t> checkerBgra(int width, int height, int cell = 24) {
  std::vector<uint8_t> pixels(size_t(width) * height * 4);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const bool on = ((x / cell) + (y / cell)) % 2 == 0;
      const size_t o = (size_t(y) * width + x) * 4;
      pixels[o + 0] = on ? uint8_t(255) : uint8_t((x * 255) / std::max(1, width - 1));  // B
      pixels[o + 1] = on ? uint8_t(255) : uint8_t(40);                                     // G
      pixels[o + 2] = on ? uint8_t(255) : uint8_t((y * 255) / std::max(1, height - 1));  // R
      pixels[o + 3] = 255;
    }
  }
  return pixels;
}

// Two lower-third-sized glass plates at 1080p: the "two pack graphics" of the
// outputs-gate workload. One at the documented default look (blur 32, dark
// tint), one at the schema maximum (blur 64) with a light tint.
inline std::vector<corevideo::compositor::BackdropDraw> twoPlates1080p() {
  using namespace corevideo;
  graphics::GraphicsBackdropSet set;
  set.width = 1920; set.height = 1080;
  graphics::GraphicsBackdropPlate first;
  first.box = {96, 820, 1060, 168}; first.clip = {0, 0, 1920, 1080};
  first.cornerRadius = 12; first.blur = 32; first.tint = 0x660a0f18u;
  graphics::GraphicsBackdropPlate second;
  second.box = {1180, 96, 640, 288}; second.clip = {0, 0, 1920, 1080};
  second.cornerRadius = 24; second.blur = 64; second.tint = 0x40ffffffu;
  std::vector<compositor::BackdropDraw> draws;
  for (const auto& plate : {first, second}) {
    compositor::BackdropDraw draw;
    if (compositor::resolveBackdropDraw(plate, set, {}, {0, 0, 1, 1}, {0, 0, 1, 1}, 1.f, 1920, 1080, draw))
      draws.push_back(draw);
  }
  return draws;
}

// A whole-screen glass template (1920x1080 template units), with or without
// the backdrop, for the compositor end-to-end checks on both platforms.
inline std::string glassTemplate(bool backdrop) {
  return std::string(R"({"schemaVersion":1,"templateId":"glass-test","templateVersion":1,"name":"Glass test","family":"title",
    "canvas":{"width":1920,"height":1080},"bounds":{"width":1920,"height":1080},"brandTokens":[],"fields":[],
    "animations":{"in":{"type":"none"},"out":{"type":"none"}},
    "layers":[{"id":"plate","type":"rect","x":480,"y":270,"width":960,"height":540,"fill":"#00000000","cornerRadius":24)") +
      (backdrop ? R"(,"backdrop":{"blur":48})" : "") + "}]}";
}

// Sum of absolute horizontal neighbour differences (all channels) inside a
// normalized region of a preview: a hard checker scores high, a blur low.
inline uint64_t edgeEnergy(const corevideo::modules::ProgramFramePreviewPixels& preview,
                           float x, float y, float w, float h) {
  uint64_t energy = 0;
  const int left = int(x * preview.width), top = int(y * preview.height);
  const int right = std::min(preview.width, int((x + w) * preview.width));
  const int bottom = std::min(preview.height, int((y + h) * preview.height));
  for (int py = top; py < bottom; ++py)
    for (int px = left + 1; px < right; ++px)
      for (int c = 0; c < 3; ++c) {
        const int a = preview.bgra[(size_t(py) * preview.width + px) * 4 + c];
        const int b = preview.bgra[(size_t(py) * preview.width + px - 1) * 4 + c];
        energy += uint64_t(a > b ? a - b : b - a);
      }
  return energy;
}

inline corevideo::modules::VideoFrame checkerFrame(int width, int height) {
  corevideo::modules::VideoFrame frame;
  frame.participantId = "glass-source";
  frame.width = frame.naturalWidth = frame.pixelWidth = width;
  frame.height = frame.naturalHeight = frame.pixelHeight = height;
  frame.pixelStride = width * 4;
  frame.timestampMs = 16;
  frame.frameId = 1;
  frame.pixels = std::make_shared<std::vector<uint8_t>>(checkerBgra(width, height));
  return frame;
}

inline corevideo::modules::CompositorRenderPlan glassPlan(int width, int height,
    std::shared_ptr<const corevideo::graphics::GraphicsTemplate> definition) {
  using namespace corevideo;
  modules::CompositorRenderPlan plan;
  plan.renderPlanId = "glass"; plan.width = width; plan.height = height;
  modules::CompositorRenderPlanLayer source;
  source.layerId = "route:glass-source"; source.kind = "participant-video";
  source.sourceId = "zoom:glass-source"; source.participantId = "glass-source";
  source.order = 0; source.rect = {0, 0, 1, 1}; source.opacity = 1;
  plan.layers.push_back(source);
  auto input = std::make_shared<graphics::GraphicsTemplateInput>();
  input->definition = std::move(definition);
  input->fields = rpc::Json::Object{};
  input->brand = rpc::Json::Object{};
  modules::CompositorRenderPlanLayer glass;
  glass.layerId = "glass"; glass.kind = "overlay"; glass.order = 10;
  glass.rect = {0, 0, 1, 1}; glass.opacity = 1; glass.hasOverlayContent = true;
  glass.overlay.appearance.enabled = true;
  glass.overlay.graphicsTemplate = input;
  plan.layers.push_back(glass);
  return plan;
}

}  // namespace backdrop_fixture
