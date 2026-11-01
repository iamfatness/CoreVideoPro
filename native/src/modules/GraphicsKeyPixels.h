#pragma once

#include "modules/Interfaces.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace corevideo::modules {

// NDI graphics key (titler spec Phase 1, "NDI fill+key"): the pure rules the
// GPU pass and the sender share, kept out of the D3D adapter so they are unit
// testable without a device.
//
// WHAT GOES ON THE KEY: exactly the native graphics instances the On Air strip
// shows on Program — the plan layers GraphicsInstances::append produced, which
// are the only layers carrying a graphicsInstanceId. Legacy scene overlays
// (lower-third assets, captions) and every video layer are NOT key content:
// the key output is "CoreVideo's graphics over a downstream switcher's own
// picture", so a legacy overlay without an instance stays Program-only until it
// is migrated. The rule is deliberately a single predicate so the compositor,
// the tests and the docs cannot disagree about it.
[[nodiscard]] inline bool graphicsKeyLayerSelected(const CompositorRenderPlanLayer& layer) {
  return !layer.graphicsInstanceId.empty() && layer.hasOverlayContent;
}

// ALPHA CONVENTION. The compositor draws graphics with the "over" blend
// (SRC_ALPHA/INV_SRC_ALPHA on colour, ONE/INV_SRC_ALPHA on alpha, and the
// premultiplied variant for the DirectWrite/D2D rasters), so a transparent
// render target accumulates PREMULTIPLIED colour: each channel already holds
// colour * alpha. NDI's BGRA frame carries STRAIGHT (non-premultiplied) alpha,
// so a receiver keying the stream multiplies by alpha itself; shipping the
// premultiplied buffer unchanged would darken every edge twice. This divides
// the colour back out. A fully transparent pixel is published as 0,0,0,0 —
// its colour is undefined after premultiplication and must not leak noise
// into a receiver that ignores alpha. The division rounds to nearest and
// clamps, so alpha 255 is an exact pass-through.
inline void unpremultiplyBgraRow(const uint8_t* src, uint8_t* dst, int width) {
  for (int x = 0; x < width; ++x) {
    const uint8_t a = src[3];
    if (a == 0) {
      dst[0] = dst[1] = dst[2] = dst[3] = 0;
    } else if (a == 255) {
      dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 255;
    } else {
      const unsigned half = a / 2u;
      for (int c = 0; c < 3; ++c) {
        const unsigned value = (static_cast<unsigned>(src[c]) * 255u + half) / a;
        dst[c] = static_cast<uint8_t>(value > 255u ? 255u : value);
      }
      dst[3] = a;
    }
    src += 4; dst += 4;
  }
}

// Whole-frame form over a mapped staging surface (rows may be padded to
// `srcPitch`); the destination is tightly packed width*4 per row.
inline void unpremultiplyBgra(const uint8_t* src, std::size_t srcPitch, int width, int height, uint8_t* dst) {
  for (int y = 0; y < height; ++y) {
    unpremultiplyBgraRow(src + static_cast<std::size_t>(y) * srcPitch,
                         dst + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4u, width);
  }
}

[[nodiscard]] inline std::size_t graphicsKeyFrameBytes(int width, int height) {
  return static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
}

// The output destination id and the sender's snapshot id. One place, so the
// shell's destination list, ModuleComposition's "which destinations are
// network senders" rule and the supervisor's isolation traits agree.
inline constexpr const char* kNdiGraphicsKeyDestination = "ndi-graphics";
inline constexpr const char* kNdiGraphicsKeySenderId = "ndi:graphics";

}  // namespace corevideo::modules
