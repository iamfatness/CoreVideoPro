// The shared overlay raster front half (OverlayRasterFrontHalf.h; Metal
// graphics spec 5.1), tested directly and portably. The same rules are held
// through each raster's public surface by OverlayRasterContract.* on D3D11 and
// Metal; these cases pin the boundaries without a GPU.

#include "compositor/OverlayRasterFrontHalf.h"

#include <gtest/gtest.h>

#include <limits>
#include <memory>
#include <string>

namespace {

namespace front = corevideo::compositor::overlayRasterFront;
using corevideo::compositor::LayerRect;
using corevideo::graphics::GraphicsPreparedResource;
using corevideo::modules::CompositorOverlayContent;

CompositorOverlayContent lowerThird() {
  CompositorOverlayContent overlay;
  overlay.text = "FRONT HALF NAME";
  overlay.title = "FRONT HALF TITLE";
  overlay.graphicsContentHash[0] = 0x5a;
  overlay.hasGraphicsContentHash = true;
  return overlay;
}

// A resource as the raster would have made it for `overlay` at w x h.
GraphicsPreparedResource preparedFor(const CompositorOverlayContent& overlay, int w, int h, bool strict) {
  GraphicsPreparedResource resource;
  resource.backend = GraphicsPreparedResource::Backend::D3D11;
  resource.deviceIdentity = 42;
  resource.width = w;
  resource.height = h;
  resource.signature = front::rasterSignature(overlay, w, h, strict);
  resource.native = std::make_shared<int>(1);
  resource.contentHash = overlay.graphicsContentHash;
  resource.hasContentHash = overlay.hasGraphicsContentHash;
  return resource;
}

TEST(OverlayRasterFrontHalf, MetadataBoundIsTheTotalOfTheNineteenFields) {
  // Fixture invariant: every bounded field starts empty (defaults such as the
  // key phase and appearance preset are not), so the total is exactly ours.
  CompositorOverlayContent overlay;
  auto& a = overlay.appearance;
  for (auto* value : {&overlay.title, &overlay.org, &overlay.text, &overlay.speaker,
      &overlay.imageUri, &overlay.fontFamily, &overlay.keyPosition, &overlay.keyPhase, &overlay.keyer,
      &overlay.brandColor, &overlay.brandAccentColor, &overlay.brandBackgroundColor,
      &a.preset, &a.anchor, &a.fontFamily, &a.nameColor, &a.titleColor, &a.backgroundColor, &a.accentColor})
    value->clear();
  overlay.text.assign(40000, 'a');
  overlay.appearance.accentColor.assign(65536 - 40000, 'b');
  EXPECT_TRUE(front::metadataWithinBound(overlay)) << "exactly 65536 bytes is admitted";
  overlay.appearance.accentColor.push_back('c');
  EXPECT_FALSE(front::metadataWithinBound(overlay)) << "65537 bytes across two fields is refused";
}

TEST(OverlayRasterFrontHalf, DimensionsAreRoundedAndBounded) {
  int w = 0, h = 0;
  ASSERT_TRUE(front::rasterPixelSize(LayerRect{0, 0, 0.5f, 0.25f}, 1921, 1081, w, h));
  EXPECT_EQ(w, 961);  // lround(960.5)
  EXPECT_EQ(h, 270);  // lround(270.25)
  EXPECT_TRUE(front::rasterPixelSize(LayerRect{0, 0, 1.f, 1.f}, 4096, 4096, w, h));
  EXPECT_FALSE(front::rasterPixelSize(LayerRect{0, 0, 1.f, 1.f}, 4097, 100, w, h));
  EXPECT_FALSE(front::rasterPixelSize(LayerRect{0, 0, 1.f, 1.f}, 100, 0, w, h));
  EXPECT_FALSE(front::rasterPixelSize(LayerRect{0, 0, std::numeric_limits<float>::quiet_NaN(), 1.f}, 100, 100, w, h));
}

TEST(OverlayRasterFrontHalf, StrictSaltSeparatesTheKeys) {
  const auto overlay = lowerThird();
  EXPECT_EQ(front::rasterSignature(overlay, 600, 120, false) ^ front::rasterSignature(overlay, 600, 120, true),
            front::kStrictSignatureSalt);
}

TEST(OverlayRasterFrontHalf, CompatibilityNeedsEveryIdentityToMatch) {
  const auto overlay = lowerThird();
  const auto D3D11 = GraphicsPreparedResource::Backend::D3D11;
  const auto good = preparedFor(overlay, 300, 60, true);
  EXPECT_TRUE(front::preparedIsCompatible(&good, D3D11, 42, overlay, true)) << "other sizes are compatible";
  EXPECT_FALSE(front::preparedIsCompatible(nullptr, D3D11, 42, overlay, true));
  EXPECT_FALSE(front::preparedIsCompatible(&good, GraphicsPreparedResource::Backend::Metal, 42, overlay, true));
  EXPECT_FALSE(front::preparedIsCompatible(&good, D3D11, 43, overlay, true));
  EXPECT_FALSE(front::preparedIsCompatible(&good, D3D11, 42, overlay, false)) << "strict salt is part of identity";
  auto noNative = good;
  noNative.native.reset();
  EXPECT_FALSE(front::preparedIsCompatible(&noNative, D3D11, 42, overlay, true));
  auto otherHash = good;
  otherHash.contentHash[0] ^= 1;
  EXPECT_FALSE(front::preparedIsCompatible(&otherHash, D3D11, 42, overlay, true));
  auto otherText = overlay;
  otherText.text = "SOMEONE ELSE";
  EXPECT_FALSE(front::preparedIsCompatible(&good, D3D11, 42, otherText, true));
  const auto huge = preparedFor(overlay, 4097, 60, true);
  EXPECT_FALSE(front::preparedIsCompatible(&huge, D3D11, 42, overlay, true));
}

TEST(OverlayRasterFrontHalf, StateTable) {
  EXPECT_STREQ(front::preparationState(true, true, true, false), "failed");
  EXPECT_STREQ(front::preparationState(false, true, true, true), "preparing");
  EXPECT_STREQ(front::preparationState(false, false, false, false), "preparing");
  EXPECT_STREQ(front::preparationState(false, false, true, true), "ready");
  EXPECT_STREQ(front::preparationState(false, false, true, false), "failed");
}

TEST(OverlayRasterFrontHalf, FixedWordingIsInternedOnce) {
  const auto a = front::internedWarning(front::kPendingWarning);
  const auto b = front::internedWarning(front::kPendingWarning);
  ASSERT_TRUE(a);
  EXPECT_EQ(a, b);
  EXPECT_EQ(*a, "Graphics preparation pending.");
}

}  // namespace
