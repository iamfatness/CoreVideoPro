#include "modules/GraphicsKeyPixels.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

using corevideo::modules::CompositorRenderPlanLayer;
using corevideo::modules::graphicsKeyFrameBytes;
using corevideo::modules::graphicsKeyLayerSelected;
using corevideo::modules::unpremultiplyBgra;
using corevideo::modules::unpremultiplyBgraRow;

namespace {

CompositorRenderPlanLayer layer(const std::string& kind, const std::string& instanceId, bool overlayContent) {
  CompositorRenderPlanLayer value;
  value.layerId = kind + ":" + (instanceId.empty() ? "none" : instanceId);
  value.kind = kind;
  value.graphicsInstanceId = instanceId;
  value.hasOverlayContent = overlayContent;
  return value;
}

}  // namespace

// The key carries exactly the native graphics instances the On Air strip shows
// on Program. Video layers and legacy overlays without an instance are
// Program-only: the whole point of the output is "our graphics over THEIR
// picture", so a Program pixel source must never be selected.
TEST(GraphicsKeyPixels, SelectsOnlyNativeGraphicsInstanceLayers) {
  EXPECT_TRUE(graphicsKeyLayerSelected(layer("overlay", "featured", true)));
  EXPECT_FALSE(graphicsKeyLayerSelected(layer("overlay", "", true)));            // legacy lower-third asset
  EXPECT_FALSE(graphicsKeyLayerSelected(layer("participant-video", "", false)));
  EXPECT_FALSE(graphicsKeyLayerSelected(layer("media-video", "", false)));
  EXPECT_FALSE(graphicsKeyLayerSelected(layer("overlay", "featured", false)));   // instance id without raster content
}

// Premultiplied -> straight: alpha 255 passes through exactly, alpha 0 becomes
// fully zero (no colour noise for receivers that ignore alpha), and a
// half-covered pixel's colour doubles back to what the graphic drew.
TEST(GraphicsKeyPixels, UnpremultiplyIsExactAtTheEndpointsAndRecoversMidAlpha) {
  const std::vector<uint8_t> src = {
      10, 20, 30, 255,   // opaque: unchanged
      7, 9, 11, 0,       // transparent: colour discarded
      64, 32, 128, 128,  // half alpha: colour doubles (rounded)
      200, 100, 50, 64,  // quarter alpha: x4, clamped at 255
  };
  std::vector<uint8_t> dst(src.size(), 0xCD);
  unpremultiplyBgraRow(src.data(), dst.data(), 4);
  const std::vector<uint8_t> expected = {
      10, 20, 30, 255,
      0, 0, 0, 0,
      128, 64, 255, 128,
      255, 255, 199, 64,
  };
  EXPECT_EQ(expected, dst);
}

// The staging surface a GPU hands back is row-padded; the published frame is
// tightly packed. Padding bytes must never reach the output and every row
// must be converted, so a receiver sees the key at the exact Program size.
TEST(GraphicsKeyPixels, WholeFrameHonoursSourcePitchAndPacksRows) {
  constexpr int kW = 3;
  constexpr int kH = 2;
  constexpr std::size_t kPitch = 20;  // 12 payload bytes + 8 padding per row
  std::vector<uint8_t> src(kPitch * kH, 0xEE);
  const auto put = [&](int x, int y, uint8_t b, uint8_t g, uint8_t r, uint8_t a) {
    uint8_t* p = src.data() + static_cast<std::size_t>(y) * kPitch + static_cast<std::size_t>(x) * 4u;
    p[0] = b; p[1] = g; p[2] = r; p[3] = a;
  };
  put(0, 0, 0, 0, 0, 0);
  put(1, 0, 255, 255, 255, 255);
  put(2, 0, 100, 50, 0, 200);
  put(0, 1, 1, 2, 3, 255);
  put(1, 1, 0, 0, 0, 0);
  put(2, 1, 60, 60, 60, 120);

  std::vector<uint8_t> dst(graphicsKeyFrameBytes(kW, kH), 0x00);
  ASSERT_EQ(dst.size(), 24u);
  unpremultiplyBgra(src.data(), kPitch, kW, kH, dst.data());

  const std::vector<uint8_t> expected = {
      0, 0, 0, 0,  255, 255, 255, 255,  128, 64, 0, 200,
      1, 2, 3, 255,  0, 0, 0, 0,  128, 128, 128, 120,
  };
  EXPECT_EQ(expected, dst);
  // No padding byte (0xEE) can appear anywhere in the packed output.
  for (const auto byte : dst) EXPECT_NE(byte, 0xEE);
}
