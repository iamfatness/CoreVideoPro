// P9 true backdrop blur: the shared graphics model (schema, limits, plate
// collection) and the portable kernel/geometry/reference in
// compositor/BackdropBlur.h. Runs on every platform, stub builds included.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "BackdropBlurFixture.h"
#include "compositor/BackdropBlur.h"
#include "graphics/GraphicsTemplate.h"

namespace {

using corevideo::graphics::GraphicsDrawing;
using corevideo::graphics::GraphicsTemplate;
using corevideo::rpc::Json;
namespace compositor = corevideo::compositor;
namespace graphics = corevideo::graphics;

std::string glassDocument(const std::string& plates, const std::string& brandTokens = "[]") {
  return R"({"schemaVersion":1,"templateId":"glass","templateVersion":1,"name":"Glass","family":"lower-third",
    "canvas":{"width":1920,"height":1080},"bounds":{"width":1200,"height":200},"brandTokens":)" + brandTokens +
         R"(,"fields":[],"animations":{"in":{"type":"none"},"out":{"type":"none"}},"layers":[)" + plates + "]}";
}
std::string plate(const std::string& id, const std::string& backdrop, const std::string& type = "rect") {
  return R"({"id":")" + id + R"(","type":")" + type + R"(","x":10,"y":20,"width":400,"height":120,"fill":"#10203040")" +
         (type == "rect" ? R"(,"cornerRadius":16)" : "") + (backdrop.empty() ? "" : R"(,"backdrop":)" + backdrop) + "}";
}

TEST(GraphicsBackdrop, ParsesBlurAndTintOnARectAndCollectsThePlate) {
  std::string error;
  const auto definition = GraphicsTemplate::load(glassDocument(plate("glass", R"({"blur":32,"tint":"#0A0F1866"})")), error);
  ASSERT_TRUE(definition) << error;
  GraphicsDrawing drawing;
  ASSERT_TRUE(definition->resolve(Json::Object{}, Json::Object{}, drawing, error)) << error;
  ASSERT_EQ(1u, drawing.primitives.size());
  ASSERT_TRUE(drawing.primitives[0].backdrop.has_value());
  EXPECT_EQ(32.0, drawing.primitives[0].backdrop->blur);
  EXPECT_EQ(0x660a0f18u, drawing.primitives[0].backdrop->tint);  // ARGB like every primitive colour
  const auto set = graphics::collectBackdrops(drawing);
  ASSERT_TRUE(set != nullptr);
  ASSERT_EQ(1u, set->plates.size());
  EXPECT_EQ(1200.0, set->width);
  EXPECT_EQ(200.0, set->height);
  EXPECT_EQ(16.0, set->plates[0].cornerRadius);
  EXPECT_EQ(400.0, set->plates[0].box.width);
  EXPECT_TRUE(set->motion == nullptr);
}

TEST(GraphicsBackdrop, TintIsOptionalAndMayComeFromABrandToken) {
  std::string error;
  auto definition = GraphicsTemplate::load(glassDocument(plate("glass", R"({"blur":8})")), error);
  ASSERT_TRUE(definition) << error;
  GraphicsDrawing drawing;
  ASSERT_TRUE(definition->resolve(Json::Object{}, Json::Object{}, drawing, error)) << error;
  EXPECT_EQ(0u, drawing.primitives[0].backdrop->tint);
  definition = GraphicsTemplate::load(glassDocument(plate("glass", R"({"blur":8,"tint":{"token":"brand.background"}})"),
                                                    R"(["brand.background"])"), error);
  ASSERT_TRUE(definition) << error;
  drawing = {};
  ASSERT_TRUE(definition->resolve(Json::Object{}, Json::Object{{"brand.background", "#11223380"}}, drawing, error)) << error;
  EXPECT_EQ(0x80112233u, drawing.primitives[0].backdrop->tint);
}

TEST(GraphicsBackdrop, RefusesOutOfRangeMalformedAndNonRectBackdrops) {
  std::string error;
  for (const char* bad : {R"({"blur":0})", R"({"blur":65})", R"({"blur":-4})", R"({"tint":"#000000"})",
                          R"({"blur":8,"tint":"black"})", R"({"blur":8,"strength":2})", R"(true)"}) {
    EXPECT_FALSE(GraphicsTemplate::load(glassDocument(plate("glass", bad)), error)) << bad;
  }
  EXPECT_FALSE(GraphicsTemplate::load(glassDocument(plate("glass", R"({"blur":8})", "ellipse")), error));
  // Glass is masked to a rounded rectangle: a skewed or polygon plate is refused, not drawn wrong.
  std::string skewed = plate("glass", R"({"blur":8})");
  skewed.replace(skewed.find(R"(,"cornerRadius":16)"), std::string(R"(,"cornerRadius":16)").size(), R"(,"skewX":12)");
  EXPECT_FALSE(GraphicsTemplate::load(glassDocument(skewed), error));
  EXPECT_EQ("template-backdrop-shape", error);
  EXPECT_TRUE(GraphicsTemplate::load(glassDocument(plate("glass", R"({"blur":1})")), error)) << error;
  EXPECT_TRUE(GraphicsTemplate::load(glassDocument(plate("glass", R"({"blur":64})")), error)) << error;
}

TEST(GraphicsBackdrop, AtMostFourGlassPlatesPerTemplate) {
  std::string error;
  std::string four, five;
  for (int i = 0; i < 5; ++i) {
    const auto item = plate("glass-" + std::to_string(i), R"({"blur":16})");
    if (i < 4) four += (i ? "," : "") + item;
    five += (i ? "," : "") + item;
  }
  EXPECT_TRUE(GraphicsTemplate::load(glassDocument(four), error)) << error;
  EXPECT_FALSE(GraphicsTemplate::load(glassDocument(five), error));
  EXPECT_EQ("template-backdrop-limit", error);
}

TEST(GraphicsBackdrop, AGraphicWithoutGlassCostsNothing) {
  std::string error;
  const auto definition = GraphicsTemplate::load(glassDocument(plate("plain", "")), error);
  ASSERT_TRUE(definition) << error;
  GraphicsDrawing drawing;
  ASSERT_TRUE(definition->resolve(Json::Object{}, Json::Object{}, drawing, error));
  EXPECT_TRUE(graphics::collectBackdrops(drawing) == nullptr);
}

TEST(BackdropKernel, WeightsAreNormalizedSymmetricAndBounded) {
  for (double sigma : {0.5, 2.0, 8.0, 16.0, 32.0, 64.0, 200.0}) {
    const auto kernel = compositor::backdropKernel(sigma);
    EXPECT_GE(kernel.downsample, compositor::kBackdropMinDownsample);
    EXPECT_LE(kernel.downsample, compositor::kBackdropMaxDownsample);
    EXPECT_GE(kernel.radius, 1);
    EXPECT_LE(kernel.radius, compositor::kBackdropMaxRadius);
    double sum = kernel.weights[0];
    for (int i = 1; i <= kernel.radius; ++i) {
      sum += 2.0 * kernel.weights[i];
      EXPECT_LE(kernel.weights[i], kernel.weights[i - 1]);
    }
    for (int i = kernel.radius + 1; i < 16; ++i) EXPECT_EQ(0.f, kernel.weights[i]);
    EXPECT_NEAR(1.0, sum, 1e-5);
  }
  // Downsample grows by powers of two as the blur grows; the 1080p maximum
  // (blur 64 -> sigma 32) runs at 1/8 resolution with the full 25-tap kernel.
  EXPECT_EQ(2, compositor::backdropKernel(4).downsample);
  EXPECT_EQ(4, compositor::backdropKernel(16).downsample);
  EXPECT_EQ(8, compositor::backdropKernel(32).downsample);
  EXPECT_EQ(12, compositor::backdropKernel(32).radius);
}

TEST(BackdropRegion, IsAlignedToAFixedGridSoAMovingPlateDoesNotShimmer) {
  const auto kernel = compositor::backdropKernel(16);  // d = 4
  for (int shift = 0; shift < 9; ++shift) {
    const compositor::BackdropPixelRect bounds{300 + shift, 500 + shift, 900 + shift, 700 + shift};
    const auto region = compositor::backdropRegion(bounds, kernel, 1920, 1080);
    EXPECT_EQ(0, region.x0 % kernel.downsample);
    EXPECT_EQ(0, region.y0 % kernel.downsample);
    // The region covers the plate plus the kernel support.
    const int pad = kernel.radius * kernel.downsample;
    EXPECT_LE(region.x0, bounds.x0 - pad);
    EXPECT_GE(region.x0 + region.lowWidth * kernel.downsample, bounds.x1 + pad);
  }
  // Clamped at the frame, never past what the scratch planes hold.
  const auto edge = compositor::backdropRegion({0, 0, 1920, 1080}, kernel, 1920, 1080);
  EXPECT_EQ(0, edge.x0);
  EXPECT_EQ(480, edge.lowWidth);
  EXPECT_EQ(270, edge.lowHeight);
  EXPECT_EQ(1920, edge.copyWidth);
  EXPECT_EQ(1080, edge.copyHeight);
}

TEST(BackdropDraw, FollowsPlacementClipOpacityAndScale) {
  graphics::GraphicsBackdropSet set;
  set.width = 1200; set.height = 200;
  graphics::GraphicsBackdropPlate plate;
  plate.box = {100, 50, 400, 100}; plate.clip = {0, 0, 1200, 200};
  plate.cornerRadius = 20; plate.blur = 32; plate.opacity = .5;
  compositor::BackdropDraw draw;
  // The graphic placed at (0.1, 0.8) of a 1920x1080 target at 1:1 scale.
  const compositor::LayerRect animated{0.1f, 0.8f, 1200.f / 1920.f, 200.f / 1080.f};
  ASSERT_TRUE(compositor::resolveBackdropDraw(plate, set, {}, animated, {0, 0, 1, 1}, .8f, 1920, 1080, draw));
  EXPECT_NEAR(192 + 100, draw.plate[0], 1e-3);
  EXPECT_NEAR(864 + 50, draw.plate[1], 1e-3);
  EXPECT_NEAR(192 + 500, draw.plate[2], 1e-3);
  EXPECT_NEAR(20, draw.radiusPx, 1e-3);
  EXPECT_NEAR(.4, draw.opacity, 1e-6);
  EXPECT_EQ(compositor::backdropKernel(16).downsample, draw.kernel.downsample);
  // Half size (a 960x540 target): geometry and blur both halve.
  ASSERT_TRUE(compositor::resolveBackdropDraw(plate, set, {}, animated, {0, 0, 1, 1}, 1.f, 960, 540, draw));
  EXPECT_NEAR(10, draw.radiusPx, 1e-3);
  EXPECT_EQ(compositor::backdropKernel(8).downsample, draw.kernel.downsample);
  // A clip that removes the plate draws nothing; one that cuts it narrows the scissor.
  EXPECT_FALSE(compositor::resolveBackdropDraw(plate, set, {}, animated, {0, 0, .1f, 1}, 1.f, 1920, 1080, draw));
  ASSERT_TRUE(compositor::resolveBackdropDraw(plate, set, {}, animated, {0, 0, .25f, 1}, 1.f, 1920, 1080, draw));
  EXPECT_EQ(480, draw.bounds.x1);
  // Transparent draws nothing.
  EXPECT_FALSE(compositor::resolveBackdropDraw(plate, set, {}, animated, {0, 0, 1, 1}, 0.f, 1920, 1080, draw));
}

TEST(BackdropReference, BlursEdgesTintsAndMasksTheRoundedPlate) {
  const int width = 320, height = 180;
  auto pixels = backdrop_fixture::checkerBgra(width, height, 8);
  const auto original = pixels;
  graphics::GraphicsBackdropSet set;
  set.width = width; set.height = height;
  graphics::GraphicsBackdropPlate plate;
  plate.box = {40, 40, 240, 100}; plate.clip = {0, 0, double(width), double(height)};
  plate.cornerRadius = 30; plate.blur = 24;
  compositor::BackdropDraw draw;
  ASSERT_TRUE(compositor::resolveBackdropDraw(plate, set, {}, {0, 0, 1, 1}, {0, 0, 1, 1}, 1.f, width, height, draw));
  compositor::backdropReference(pixels, width, height, draw);
  const auto energy = [&](const std::vector<uint8_t>& image, int x0, int y0, int x1, int y1) {
    uint64_t total = 0;
    for (int y = y0; y < y1; ++y)
      for (int x = x0 + 1; x < x1; ++x)
        for (int c = 0; c < 3; ++c)
          total += uint64_t(std::abs(int(image[(size_t(y) * width + x) * 4 + c]) -
                                     int(image[(size_t(y) * width + x - 1) * 4 + c])));
    return total;
  };
  // Inside the plate the checker is gone; outside it is untouched.
  EXPECT_LT(energy(pixels, 100, 60, 220, 120) * 10, energy(original, 100, 60, 220, 120));
  EXPECT_EQ(energy(pixels, 0, 0, width, 30), energy(original, 0, 0, width, 30));
  // The rounded corner is outside the mask: the corner pixel keeps its value.
  for (int c = 0; c < 4; ++c) EXPECT_EQ(original[(size_t(41) * width + 41) * 4 + c], pixels[(size_t(41) * width + 41) * 4 + c]);
  // An opaque tint replaces the blurred picture with the tint colour.
  plate.tint = 0xff336699u;
  pixels = original;
  ASSERT_TRUE(compositor::resolveBackdropDraw(plate, set, {}, {0, 0, 1, 1}, {0, 0, 1, 1}, 1.f, width, height, draw));
  compositor::backdropReference(pixels, width, height, draw);
  const size_t centre = (size_t(90) * width + 160) * 4;
  EXPECT_EQ(0x99, int(pixels[centre + 0]));
  EXPECT_EQ(0x66, int(pixels[centre + 1]));
  EXPECT_EQ(0x33, int(pixels[centre + 2]));
}

TEST(BackdropReference, HalfRoundingMatchesBinary16) {
  EXPECT_EQ(1.f, compositor::backdropRoundHalf(1.f));
  EXPECT_EQ(0.5f, compositor::backdropRoundHalf(0.5f));
  // 1/3 in binary16 is 0x3555 = 0.333251953125.
  EXPECT_EQ(0.333251953125f, compositor::backdropRoundHalf(1.f / 3.f));
  EXPECT_EQ(0.f, compositor::backdropRoundHalf(0.f));
}

TEST(BackdropFixture, TwoPlates1080pResolve) {
  const auto draws = backdrop_fixture::twoPlates1080p();
  ASSERT_EQ(2u, draws.size());
  EXPECT_EQ(4, draws[0].kernel.downsample);  // blur 32 -> sigma 16 px -> 1/4 resolution
  EXPECT_EQ(8, draws[1].kernel.downsample);  // blur 64 (maximum) -> sigma 32 px -> 1/8
  EXPECT_EQ(12, draws[1].kernel.radius);
}

}  // namespace
