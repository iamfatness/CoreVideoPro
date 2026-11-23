// Metal instantiation of the overlay raster contract (OverlayRasterContractCases.h,
// spec 7.1; Metal graphics backend G3). The D3D11 instantiation lives in
// D3D11GraphicsReferenceTest.cpp and runs in the Windows session.

#if COREVIDEO_WITH_METAL && !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS

#import <Metal/Metal.h>

#include "OverlayRasterContractCases.h"
#include "compositor/CoreTextOverlayRaster.h"

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

#endif  // COREVIDEO_WITH_METAL && !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS
