#pragma once

// Core Text / ImageIO overlay-texture preparation: the Metal twin of
// CompositorOverlayRaster (DirectWrite/WIC/D2D). Metal graphics backend G1,
// docs/superpowers/specs/2026-11-15-mac-metal-graphics-backend-design.md.
//
// Same public surface and the same admission rules as the Windows raster
// (CompositorOverlayRaster.cpp rasterOverlayTexture, rules 1-8 of spec 3.2),
// with id<MTLDevice>/id<MTLTexture> in place of the D3D types. The front half
// is deliberately DUPLICATED rather than extracted: extraction edits a Windows
// file that cannot be built on a Mac (spec 5.1). Keep the two in step.
//
// THREADING. The render thread only adopts immutable prepared textures: a
// metadata bound, a content-signature hash and a lock-free cache lookup. A cache
// HIT allocates nothing. A miss enqueues a bounded request and returns nil; the
// caller draws the legacy fallback (band + accent bar) until the persistent
// preparation worker (graphics::PreparationCache) publishes. The worker builds
// a premultiplied BGRA CGBitmapContext, draws the shared
// computeOverlayTileLayout geometry with Core Text, creates one MTLTexture and
// uploads it once. MTLDevice texture creation and replaceRegion are safe off
// the render thread; the texture is never written again after publication.
//
// Content: legacy overlays (lower third, caption, brand bug, Tiles name line,
// the failed-slate name label; G1) and template graphics (G2), resolved and laid
// out on the worker with Core Text measurement, drawn with Core Graphics, and
// packed into an atlas for crawls and layered motion. Fonts resolve through
// graphics/GraphicsTextCoreText (the owner's substitution table, bundled
// Selawik). The Metal adapter reports supportsGraphicsInstances() true.
//
// ObjC++ only: include from .mm translation units compiled with -fobjc-arc.

#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_METAL

#import <Metal/Metal.h>

#include <array>
#include <compare>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "compositor/CompositorLayout.h"
#include "graphics/GraphicsMotion.h"
#include "graphics/GraphicsPreparedResource.h"
#include "graphics/GraphicsResourceBudget.h"
#include "graphics/PreparationCache.h"
#include "modules/Interfaces.h"

namespace corevideo::modules {

class CoreTextOverlayRaster {
  struct MetalRasterTex {
    std::shared_ptr<const graphics::GraphicsRunLayout> runs;
    // Declared first so the texture releases before its budget charge.
    std::shared_ptr<graphics::GraphicsResourceBudget::Lease> budget;
    id<MTLTexture> texture = nil;
  };
  struct Request {
    id<MTLDevice> device = nil;
    CompositorOverlayContent overlay;
    int width = 0, height = 0;
    bool strictResources = false;
  };
  struct Key {
    uintptr_t device = 0;
    uint64_t signature = 0;
    std::array<uint8_t, 32> contentHash{};
    bool hasContentHash = false;
    auto operator<=>(const Key&) const = default;
  };
  using Cache = graphics::PreparationCache<Key, Request, MetalRasterTex>;

 public:
  CoreTextOverlayRaster();
  ~CoreTextOverlayRaster();
  CoreTextOverlayRaster(const CoreTextOverlayRaster&) = delete;
  CoreTextOverlayRaster& operator=(const CoreTextOverlayRaster&) = delete;

  void beginFrame() {
    pendingThisFrame_ = failedThisFrame_ = false;
    activeLease_.reset();
    activeResource_.reset();
    lastState_ = "unprepared";
  }
  bool pendingThisFrame() const { return pendingThisFrame_; }
  bool failedThisFrame() const { return failedThisFrame_; }
  // unprepared | preparing | ready | failed, for the LAST rasterOverlayTexture call.
  const char* preparationState() const { return lastState_; }
  const std::string& lastWarning() const { return lastWarning_; }
  // The same text by reference, for per-frame facts (null when empty): copying
  // it into each fact would allocate on the render thread every frame.
  const std::shared_ptr<const std::string>& sharedWarning() const { return sharedWarning_; }
  // Keeps the last call's texture alive (e.g. across OverlayRasterRetention).
  std::shared_ptr<const void> lease() const { return activeLease_; }
  // Atlas runs (crawls, layered motion) for the last call's prepared raster;
  // null for a single-image raster.
  std::shared_ptr<const graphics::GraphicsRunLayout> activeRuns() const;
  std::shared_ptr<const graphics::GraphicsPreparedResource> preparedResource();
  // Surface parity with the D3D raster. A Metal command buffer retains the
  // textures bound to it, so there is no GPU read fence to refuse; never called.
  void refuseGpuLease() {
    failedThisFrame_ = true;
    setWarning(internedWarning("Graphics GPU retirement capacity unavailable."));
  }

  // Returns the prepared premultiplied-BGRA texture for `overlay` at
  // rect * target pixels, or nil while it prepares (or failed). Never blocks.
  id<MTLTexture> rasterOverlayTexture(
      id<MTLDevice> device,
      const CompositorOverlayContent& overlay,
      const compositor::LayerRect& rect,
      int targetWidth,
      int targetHeight,
      const std::string& preparationSlot = {},
      bool strictResources = false,
      std::shared_ptr<const graphics::GraphicsPreparedResource> prepared = {});

  // The texture a lease() keeps alive, or nil.
  static id<MTLTexture> textureOfLease(const std::shared_ptr<const void>& lease);

  Cache::Counters cacheCounters() const { return cache_.counters(); }

 private:
  Cache::Result prepareRaster(const Request& request);
  static std::shared_ptr<const std::string> internedWarning(const char* text);
  void setWarning(std::shared_ptr<const std::string> warning);

  std::string lastWarning_;
  std::shared_ptr<const std::string> sharedWarning_;
  bool pendingThisFrame_ = false, failedThisFrame_ = false;
  const char* lastState_ = "unprepared";
  std::shared_ptr<const Cache::Result> activeLease_;
  std::shared_ptr<const graphics::GraphicsPreparedResource> activeResource_;
  uintptr_t activeDevice_ = 0;
  uint64_t activeSignature_ = 0;
  int activeWidth_ = 0, activeHeight_ = 0;
  std::array<uint8_t, 32> activeContentHash_{};
  bool activeHasContentHash_ = false;
  // Weak, fixed-size memo of resources handed out (preparedResource()).
  struct CachedResource {
    const void* source = nullptr;
    std::weak_ptr<const graphics::GraphicsPreparedResource> resource;
  };
  std::array<CachedResource, 32> resourceCache_{};
  size_t resourceCacheNext_ = 0;
  // Last member: join the preparation worker before anything it touches dies.
  Cache cache_;
};

}  // namespace corevideo::modules

#endif  // !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_METAL
