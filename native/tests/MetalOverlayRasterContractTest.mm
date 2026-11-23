// Metal instantiation of the overlay raster contract (OverlayRasterContractCases.h,
// spec 7.1; Metal graphics backend G3). The D3D11 instantiation lives in
// D3D11GraphicsReferenceTest.cpp and runs in the Windows session.

#if COREVIDEO_WITH_METAL && !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS

#import <ImageIO/ImageIO.h>
#include <cstring>
#import <Metal/Metal.h>

#include "OverlayRasterContractCases.h"
#include "compositor/CoreTextOverlayRaster.h"
#include "compositor/CoreTextThumbnail.h"

namespace {

struct MetalContractBackend {
  using Raster = corevideo::modules::CoreTextOverlayRaster;
  using Device = id<MTLDevice>;
  static constexpr auto kind = corevideo::graphics::GraphicsPreparedResource::Backend::Metal;
  static constexpr bool kCountsCacheFailures = true;

  static Device device() { return MTLCreateSystemDefaultDevice(); }

  static bool raster(Raster& raster, Device device, const corevideo::modules::CompositorOverlayContent& overlay,
                     int width, int height, bool strict, overlayContract::PreparedPtr prepared) {
    raster.beginFrame();
    return raster.rasterOverlayTexture(device, overlay, {0.f, 0.f, 1.f, 1.f}, width, height, {}, strict,
                                       std::move(prepared)) != nil;
  }

  static uint64_t failedPreparations(Raster& raster) { return raster.cacheCounters().failed; }
  static uint64_t completedPreparations(Raster& raster) { return raster.cacheCounters().completed; }
  static uint64_t refusedPreparations(Raster& raster) { return raster.cacheCounters().refused; }

  static std::vector<uint8_t> lastPixels(Raster& raster) {
    id<MTLTexture> texture = Raster::textureOfLease(raster.lease());
    if (!texture) return {};
    std::vector<uint8_t> bgra(size_t(texture.width) * texture.height * 4);
    [texture getBytes:bgra.data()
          bytesPerRow:texture.width * 4
           fromRegion:MTLRegionMake2D(0, 0, texture.width, texture.height)
          mipmapLevel:0];
    return bgra;
  }
};

using B = MetalContractBackend;

}  // namespace

TEST(OverlayRasterContract, MetadataBound) { overlayContract::contractMetadataBound<B>(); }
TEST(OverlayRasterContract, Dimensions) { overlayContract::contractDimensions<B>(); }
TEST(OverlayRasterContract, CompatibleSameSizeIsAdoptedOnTheFirstCall) {
  overlayContract::contractCompatibleSameSizeIsAdoptedOnTheFirstCall<B>();
}
TEST(OverlayRasterContract, ForeignDeviceOrHashIsNeverAdopted) {
  overlayContract::contractForeignDeviceOrHashIsNeverAdopted<B>();
}
TEST(OverlayRasterContract, ScaleRetentionDrawsTheAcceptedRaster) {
  overlayContract::contractScaleRetentionDrawsTheAcceptedRaster<B>();
}
TEST(OverlayRasterContract, FailedScaleKeepsTheAcceptedRaster) {
  overlayContract::contractFailedScaleKeepsTheAcceptedRaster<B>();
}
TEST(OverlayRasterContract, CapacityWordingAtTheResidentLimit) {
  overlayContract::contractCapacityWordingAtTheResidentLimit<B>();
}
TEST(OverlayRasterContract, BudgetWaitIsRetryableNotSticky) {
  overlayContract::contractBudgetWaitIsRetryableNotSticky<B>();
}
TEST(OverlayRasterContract, AtlasSpaceFailureWording) { overlayContract::contractAtlasSpaceFailureWording<B>(); }
TEST(OverlayRasterContract, ImageDecodeFailureWording) { overlayContract::contractImageDecodeFailureWording<B>(); }
TEST(OverlayRasterContract, CrawlAtTheItemLimitPreparesOrFailsWithAtlasWording) {
  overlayContract::contractCrawlAtTheItemLimitPreparesOrFailsWithAtlasWording<B>();
}
TEST(OverlayRasterContract, StrictSubstitutedTextWithEmojiAndCjkPrepares) {
  overlayContract::contractStrictSubstitutedTextWithEmojiAndCjkPrepares<B>();
}

// ── Library thumbnails on Metal (G5) ─────────────────────────────────────────

namespace {
// The PNG's samples verbatim (CGDataProviderCopyData, never a drawing
// context, which would premultiply): RGBA 8-bit, straight alpha.
struct DecodedPng { int width = 0, height = 0; bool straightRgba = false; std::vector<uint8_t> rgba; };
DecodedPng decodePng(const std::vector<uint8_t>& png) {
  DecodedPng out;
  CFDataRef data = CFDataCreate(kCFAllocatorDefault, png.data(), CFIndex(png.size()));
  CGImageSourceRef source = data ? CGImageSourceCreateWithData(data, nullptr) : nullptr;
  CGImageRef image = source ? CGImageSourceCreateImageAtIndex(source, 0, nullptr) : nullptr;
  if (image) {
    out.width = int(CGImageGetWidth(image));
    out.height = int(CGImageGetHeight(image));
    out.straightRgba = CGImageGetAlphaInfo(image) == kCGImageAlphaLast && CGImageGetBitsPerComponent(image) == 8 &&
                       CGImageGetBitsPerPixel(image) == 32;
    CFDataRef pixels = CGDataProviderCopyData(CGImageGetDataProvider(image));
    if (pixels) {
      const size_t row = CGImageGetBytesPerRow(image);
      const auto* bytes = CFDataGetBytePtr(pixels);
      for (int y = 0; y < out.height; ++y) out.rgba.insert(out.rgba.end(), bytes + y * row, bytes + y * row + out.width * 4);
      CFRelease(pixels);
    }
    CGImageRelease(image);
  }
  if (source) CFRelease(source);
  if (data) CFRelease(data);
  return out;
}
uint32_t bigEndian(const std::vector<uint8_t>& b, size_t at) {
  return uint32_t(b[at]) << 24 | uint32_t(b[at + 1]) << 16 | uint32_t(b[at + 2]) << 8 | uint32_t(b[at + 3]);
}
}  // namespace

// A half-alpha white rect: the PNG holds straight alpha (white stays 255, not
// 128), within the 256 KiB bound, with an IHDR that matches its size (the
// checks GraphicsThumbnailBytes.Decode makes on the shell side).
TEST(OverlayRasterContract, ThumbnailPngIsStraightAlphaAndMatchesItsHeader) {
  std::string error;
  const auto overlay = overlayContract::templateOverlay(overlayContract::simpleManifest("thumb-alpha", 200, 100,
      R"([{"id":"r","type":"rect","x":0,"y":0,"width":200,"height":100,"fill":"#FFFFFF80"}])"), {}, &error);
  ASSERT_TRUE(overlay.graphicsTemplate && overlay.graphicsTemplate->definition) << error;
  if (!overlay.graphicsTemplate || !overlay.graphicsTemplate->definition) return;
  std::string warning;
  const auto png = corevideo::modules::coreTextThumbnailPng(overlay, 200, 100, warning);
  ASSERT_FALSE(png.empty()) << warning;
  if (png.size() < 24) return;
  EXPECT_LE(png.size(), 256u * 1024);
  const uint8_t signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  EXPECT_TRUE(std::equal(signature, signature + 8, png.begin()));
  EXPECT_EQ(0, std::memcmp(png.data() + 12, "IHDR", 4));
  EXPECT_EQ(200u, bigEndian(png, 16));
  EXPECT_EQ(100u, bigEndian(png, 20));
  const auto decoded = decodePng(png);
  ASSERT_TRUE(decoded.straightRgba) << "the PNG does not decode as straight 8-bit RGBA";
  ASSERT_EQ(size_t(200) * 100 * 4, decoded.rgba.size());
  if (decoded.rgba.size() != size_t(200) * 100 * 4) return;
  const size_t centre = (size_t(50) * 200 + 100) * 4;
  for (int c = 0; c < 3; ++c) EXPECT_NEAR(255, decoded.rgba[centre + c], 1) << "channel " << c << " is premultiplied";
  EXPECT_NEAR(128, decoded.rgba[centre + 3], 1);
  EXPECT_EQ(corevideo::modules::coreTextThumbnailPng(overlay, 321, 100, warning).size(), 0u);
  EXPECT_EQ(warning, "Invalid thumbnail dimensions.");
}

// A crawl thumbnail draws each item at its start inside the crawl clip, with
// no atlas. Manifest from GraphicsCompositionPixel.CrawlUsesPreparedTextAndClipsBothDirections:
// a 320x180 template whose crawl sits in a 60x50 crop group at (90,65), so at
// 320x180 the clip is x 90..150, y 65..115. Fixture invariant: the item
// (MMMMMMMM at 32 px) is wider than the crop, so a missing clip shows ink
// outside it.
TEST(OverlayRasterContract, CrawlThumbnailPlacesItemsAtTheirStartsInsideTheClip) {
  std::string error;
  const auto overlay = overlayContract::templateOverlay(R"({
      "schemaVersion":1,"templateId":"pixel-crawl","templateVersion":1,"name":"Crawl","family":"title",
      "canvas":{"width":1920,"height":1080},"bounds":{"width":320,"height":180},"brandTokens":[],
      "fields":[{"id":"crawl-items","type":"string","default":"[\"MMMMMMMM\"]","maxLength":4096}],
      "animations":{"in":{"type":"none"},"out":{"type":"none"}},
      "layers":[{"id":"crop","type":"group","x":90,"y":65,"width":60,"height":50,"children":[
        {"id":"crawl","type":"crawl","x":0,"y":0,"width":80,"height":50,"font":"Arial",
         "fontSize":32,"color":"#FFFFFF","items":{"field":"crawl-items"},"direction":"left"}]}]})", {}, &error);
  ASSERT_TRUE(overlay.graphicsTemplate && overlay.graphicsTemplate->definition) << error;
  if (!overlay.graphicsTemplate || !overlay.graphicsTemplate->definition) return;
  std::string warning;
  const auto png = corevideo::modules::coreTextThumbnailPng(overlay, 320, 180, warning);
  ASSERT_FALSE(png.empty()) << warning;
  const auto decoded = decodePng(png);
  ASSERT_EQ(size_t(320) * 180 * 4, decoded.rgba.size());
  if (decoded.rgba.size() != size_t(320) * 180 * 4) return;
  size_t inside = 0, outside = 0;
  for (int y = 0; y < 180; ++y)
    for (int x = 0; x < 320; ++x) {
      if (decoded.rgba[(size_t(y) * 320 + x) * 4 + 3] <= 8) continue;
      (x >= 90 && x < 150 && y >= 65 && y < 115 ? inside : outside)++;
    }
  EXPECT_GT(inside, 50u) << "no crawl ink at the item's start";
  EXPECT_EQ(outside, 0u) << "crawl ink outside its clip";
}

#endif  // COREVIDEO_WITH_METAL && !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS
