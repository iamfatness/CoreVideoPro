#pragma once

// The platform-neutral front half of rasterOverlayTexture (Metal graphics
// backend spec 5.1; admission rules 1-8 in spec 3.2). CompositorOverlayRaster
// (D3D11) and CoreTextOverlayRaster (Metal) both call these, so the admission
// decisions and the operator wording live in one place. What stays per
// backend is only what differs by type: the device identity, the cache's
// Request, and the texture handle returned. OverlayRasterContract.* holds both
// rasters to the same observable results.
//
// Portable C++ (no D3D or Apple headers): compiled into every build that
// includes either raster, and into the portable unit test.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "compositor/CompositorLayout.h"
#include "graphics/GraphicsPreparedResource.h"
#include "graphics/GraphicsResourceBudget.h"
#include "modules/Interfaces.h"
#include "modules/OverlayTileRaster.h"

namespace corevideo::compositor::overlayRasterFront {

// Operator wording. Part of the shared state machine: GraphicsInstances and
// the shell show it verbatim, and the contract cases assert it exactly.
inline constexpr const char* kMetadataBudgetWarning = "Graphics content exceeds preparation metadata budget.";
inline constexpr const char* kInvalidDimensionsWarning = "Invalid graphics raster dimensions.";
inline constexpr const char* kPendingWarning = "Graphics preparation pending.";
inline constexpr const char* kScalePendingWarning =
    "Graphics scale preparation pending; retaining the accepted raster.";
inline constexpr const char* kRetainingSuffix = " Retaining the accepted raster.";
inline constexpr const char* kCapacityWarning =
    "Graphics preparation pending at the shared 16-revision capacity limit.";

inline constexpr size_t kMetadataBudgetBytes = 65536;
inline constexpr int kMaxRasterPixels = 4096;
inline constexpr uint64_t kStrictSignatureSalt = 0xd9e94a8e64b5fc21ULL;

// Rule 1: bound metadata before hashing or copying it into the queue. A legacy
// caller cannot amplify one oversized caption into 32 queued copies.
inline bool metadataWithinBound(const modules::CompositorOverlayContent& overlay) {
  size_t metadataBytes = 0;
  const auto& a = overlay.appearance;
  for (const auto* value : {&overlay.title, &overlay.org, &overlay.text, &overlay.speaker,
      &overlay.imageUri, &overlay.fontFamily, &overlay.keyPosition, &overlay.keyPhase, &overlay.keyer,
      &overlay.brandColor, &overlay.brandAccentColor, &overlay.brandBackgroundColor,
      &a.preset, &a.anchor, &a.fontFamily, &a.nameColor, &a.titleColor, &a.backgroundColor, &a.accentColor}) {
    if (value->size() > kMetadataBudgetBytes - metadataBytes) return false;
    metadataBytes += value->size();
  }
  return true;
}

// Rule 2: rect * target must be finite, at least 1 and at most 4096 in each
// axis. Width and height are lround. The caller folds a missing device into
// the same failure.
inline bool rasterPixelSize(const LayerRect& rect, int targetWidth, int targetHeight, int& width, int& height) {
  const double widthPixels = double(rect.width) * targetWidth;
  const double heightPixels = double(rect.height) * targetHeight;
  if (!std::isfinite(widthPixels) || !std::isfinite(heightPixels) || widthPixels < 1 || heightPixels < 1 ||
      widthPixels > kMaxRasterPixels || heightPixels > kMaxRasterPixels)
    return false;
  width = int(std::lround(widthPixels));
  height = int(std::lround(heightPixels));
  return true;
}

// Rule 3: the content signature at a size, salted when resources are strict.
// Excludes animation state (overlayContentSignature).
inline uint64_t rasterSignature(const modules::CompositorOverlayContent& overlay, int width, int height,
                                bool strictResources) {
  return modules::overlayContentSignature(overlay, width, height) ^ (strictResources ? kStrictSignatureSalt : 0ULL);
}

// Rule 4: a supplied prepared resource is compatible only when backend, device
// identity, content hash and its own signature at its own size all match, and
// it is at most 4096 square.
inline bool preparedIsCompatible(const graphics::GraphicsPreparedResource* prepared,
                                 graphics::GraphicsPreparedResource::Backend backend, uintptr_t deviceIdentity,
                                 const modules::CompositorOverlayContent& overlay, bool strictResources) {
  return prepared && prepared->native && prepared->backend == backend && prepared->deviceIdentity == deviceIdentity &&
      prepared->width > 0 && prepared->height > 0 && prepared->hasContentHash == overlay.hasGraphicsContentHash &&
      prepared->contentHash == overlay.graphicsContentHash && prepared->width <= kMaxRasterPixels &&
      prepared->height <= kMaxRasterPixels &&
      prepared->signature == rasterSignature(overlay, prepared->width, prepared->height, strictResources);
}

// Rule 7: the reported state after lookup and scale retention.
inline const char* preparationState(bool failedScale, bool retainingScale, bool haveLease, bool haveAsset) {
  return failedScale ? "failed" : retainingScale ? "preparing" : !haveLease ? "preparing" : haveAsset ? "ready" : "failed";
}

// Rule 8: a pending content-hashed revision at the process resident limit
// reports the capacity wording instead of the generic pending wording.
inline bool pendingAtResidentLimit(bool haveLease, bool hasContentHash) {
  if (haveLease || !hasContentHash) return false;
  const auto capacity = graphics::GraphicsResourceBudget::process().counters();
  return capacity.residentResources >= capacity.residentLimit;
}

// Fixed operator wording, allocated once per process and shared by every
// per-frame fact (no render-thread copy; G2 review 2).
inline std::shared_ptr<const std::string> internedWarning(const char* text) {
  static std::mutex mutex;
  static std::map<std::string_view, std::shared_ptr<const std::string>> interned;
  std::lock_guard lock(mutex);
  auto& slot = interned[text];
  if (!slot) slot = std::make_shared<const std::string>(text);
  return slot;
}

}  // namespace corevideo::compositor::overlayRasterFront
