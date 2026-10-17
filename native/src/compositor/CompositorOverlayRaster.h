#pragma once

// DirectWrite/WIC/D2D overlay-texture preparation. This owner manages the
// factories and the content-signature texture cache, and turns overlay content
// (lower-third / caption text + optional image) into a cached GPU texture whose
// SRV the compositor composites over the brand band.
//
// Cache misses run on a persistent preparation owner using a CPU WIC/D2D target.
// It never borrows the compositor's immediate context. The render thread only
// adopts immutable prepared textures; pending content uses the existing fallback.

// Only meaningful in the real D3D11 dev build; the stub build compiles this to
// nothing (same gate as D3D11CompositorAdapter.cpp).
#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_D3D11

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d2d1.h>
#include <d3d11.h>
#include <dwrite.h>
#include <wincodec.h>

#include <cstdint>
#include <map>
#include <string>
#include <compare>

#include "compositor/ComPtrLite.h"
#include "graphics/PreparationCache.h"
#include "graphics/GraphicsResourceBudget.h"
#include "graphics/GraphicsTemplate.h"
#include "compositor/CompositorLayout.h"
#include "modules/Interfaces.h"

namespace corevideo::modules {

class CompositorOverlayRaster {
 public:
  CompositorOverlayRaster();
  ~CompositorOverlayRaster();
  void beginFrame() {
    pendingThisFrame_ = failedThisFrame_ = false;
    activeLease_.reset(); activeResource_.reset(); lastState_ = "unprepared";
  }
  bool pendingThisFrame() const { return pendingThisFrame_; }
  bool failedThisFrame() const { return failedThisFrame_; }
  const char* preparationState() const {
    return lastState_;
  }
  std::shared_ptr<const void> lease() const { return activeLease_; }
  std::shared_ptr<const graphics::GraphicsPreparedResource> preparedResource();
  void refuseGpuLease() { failedThisFrame_ = true; lastWarning_ = "Graphics GPU retirement capacity unavailable."; }
  const std::string& lastWarning() const { return lastWarning_; }
  // Rasters the overlay's accent bar + text (+ optional WIC image) into a GPU
  // texture using DirectWrite/D2D and returns its SRV, or nullptr to fall back
  // to the accent-bar draw. The texture holds PREMULTIPLIED alpha over a
  // transparent background (the brand band is drawn separately as a quad so its
  // alpha animation matches the CPU mirror exactly). Cached by
  // overlayContentSignature: re-rastered only when text/brand/size changes.
  // Geometry comes from computeOverlayTileLayout (OverlayTileRaster.h) — the
  // same resolver the portable CPU tile rasters — so preview and program agree
  // on placement by construction.
  //
  // `device` supplies free-threaded immutable texture creation. `context` is
  // retained in the call signature for compatibility and is never accessed.
  // Target dimensions select the raster scale; cold content returns pending.
  ID3D11ShaderResourceView* rasterOverlayTexture(
      ID3D11Device* device,
      ID3D11DeviceContext* context,
      const CompositorOverlayContent& overlay,
      const compositor::LayerRect& rect,
      int targetWidth,
      int targetHeight,
      const std::string& preparationSlot = {},
      bool strictResources = false,
      std::shared_ptr<const graphics::GraphicsPreparedResource> prepared = {});

 private:
  // Lazily creates the D2D/DirectWrite (and best-effort WIC) factories used by
  // the overlay raster. Failed content keys stay failed rather than retrying
  // every frame; a repaired revision can retry without restarting the owner.
  bool ensureOverlayRasterFactories();

  struct ImageInput {
    ComPtrLite<IWICBitmapFrameDecode> frame;
    uint64_t cpuBytes = 0;
  };
  // Probe and retain the same decoder frame before reserving conversion memory.
  // No path is reopened between sizing and decode. Native instance assets will
  // additionally use the versioned package importer; this preserves legacy URIs.
  ImageInput openOverlayImage(const std::string& imageUri);
  ImageInput openPackagedImage(const graphics::GraphicsAsset& asset);
  ComPtrLite<ID2D1Bitmap> decodeOverlayImage(ID2D1RenderTarget* target, IWICBitmapFrameDecode* frame, bool referenceDpi = false);

  // Draws one line of text into `box`, vertically centered, trimmed with an
  // ellipsis when it overflows. Returns false only on factory/format failure.
  bool drawOverlayTextLine(
      ID2D1RenderTarget* target,
      const std::string& text,
      const std::string& fontFamily,
      DWRITE_FONT_WEIGHT weight,
      const D2D1_RECT_F& box,
      const D2D1_COLOR_F& color, float explicitFontSize = 0.f);
  bool drawTemplate(ID2D1RenderTarget* target, const graphics::GraphicsDrawing& drawing, int height,
                    const std::map<std::string, ImageInput>& images);

  // DirectWrite/D2D overlay raster cache: overlayContentSignature (FNV-1a over
  // text/brand/font/size, deliberately excluding animation state) -> rendered
  // text texture. Rasters happen only on content/size change; frames reuse the
  // SRV.
  struct OverlayRasterTex {
    // Declared first so the texture/view release before their budget charge.
    std::shared_ptr<graphics::GraphicsResourceBudget::Lease> budget;
    ComPtrLite<ID3D11Texture2D> texture;
    ComPtrLite<ID3D11ShaderResourceView> view;
  };
  struct Request {
    std::shared_ptr<ID3D11Device> device;
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
  using Cache = graphics::PreparationCache<Key, Request, OverlayRasterTex>;
  Cache::Result prepareRaster(const Request& request);
  ComPtrLite<ID2D1Factory> d2dFactory_;
  ComPtrLite<IDWriteFactory> dwriteFactory_;
  ComPtrLite<IWICImagingFactory> wicFactory_;
  std::string lastWarning_;
  std::string workerWarning_;
  bool pendingThisFrame_ = false, failedThisFrame_ = false;
  const char* lastState_ = "unprepared";
  std::shared_ptr<const Cache::Result> activeLease_;
  std::shared_ptr<const graphics::GraphicsPreparedResource> activeResource_;
  uintptr_t activeDevice_ = 0;
  uint64_t activeSignature_ = 0;
  int activeWidth_ = 0, activeHeight_ = 0;
  std::array<uint8_t, 32> activeContentHash_{};
  bool activeHasContentHash_ = false;
  // Last member: join the owner before destroying its factories or callback state.
  Cache cache_;
};

}  // namespace corevideo::modules

#endif  // !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_D3D11
