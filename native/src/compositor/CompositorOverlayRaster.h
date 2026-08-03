#pragma once

// DirectWrite/WIC/D2D overlay-texture rasterization, extracted VERBATIM from
// D3D11CompositorAdapter.cpp (move-only refactor, no behavior change). This is
// the self-contained overlay-raster cluster: it owns the D2D/DirectWrite/WIC
// factories and the content-signature texture cache, and turns overlay content
// (lower-third / caption text + optional image) into a cached GPU texture whose
// SRV the compositor composites over the brand band.
//
// Ownership/threading unchanged: this runs ONLY on the render thread, driven by
// D3D11Compositor::drawOverlayLayer. It touches no locks. The render device and
// immediate context are passed in per call (the compositor owns them); the
// state save/restore around D2D EndDraw lives inside rasterOverlayTexture,
// exactly as before.

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

#include "compositor/ComPtrLite.h"
#include "compositor/CompositorLayout.h"
#include "modules/Interfaces.h"

namespace corevideo::modules {

class CompositorOverlayRaster {
 public:
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
  // `device`/`context` are the compositor's render device + immediate context;
  // `targetWidth`/`targetHeight` are the current render-target dimensions (the
  // former targetWidth_/targetHeight_ members). Behavior is identical to the
  // former D3D11Compositor::rasterOverlayTexture.
  ID3D11ShaderResourceView* rasterOverlayTexture(
      ID3D11Device* device,
      ID3D11DeviceContext* context,
      const CompositorOverlayContent& overlay,
      const compositor::LayerRect& rect,
      int targetWidth,
      int targetHeight);

 private:
  // Lazily creates the D2D/DirectWrite (and best-effort WIC) factories used by
  // the overlay raster. A failure latches so we don't retry per frame; the
  // caller then keeps the band+accent fallback.
  bool ensureOverlayRasterFactories();

  // Decodes `imageUri` (a local path or file:/// URI) via WIC into a D2D bitmap
  // for the given render target. Returns an empty ComPtrLite on any failure —
  // the raster then simply omits the image.
  ComPtrLite<ID2D1Bitmap> decodeOverlayImage(ID2D1RenderTarget* target, const std::string& imageUri);

  // Draws one line of text into `box`, vertically centered, trimmed with an
  // ellipsis when it overflows. Returns false only on factory/format failure.
  bool drawOverlayTextLine(
      ID2D1RenderTarget* target,
      const std::string& text,
      const std::string& fontFamily,
      DWRITE_FONT_WEIGHT weight,
      const D2D1_RECT_F& box,
      const D2D1_COLOR_F& color);

  // DirectWrite/D2D overlay raster cache: overlayContentSignature (FNV-1a over
  // text/brand/font/size, deliberately excluding animation state) -> rendered
  // text texture. Rasters happen only on content/size change; frames reuse the
  // SRV.
  struct OverlayRasterTex {
    ComPtrLite<ID3D11Texture2D> texture;
    ComPtrLite<ID3D11ShaderResourceView> view;
    uint64_t lastUsed = 0;
  };
  std::map<uint64_t, OverlayRasterTex> overlayTextTextures_;
  uint64_t overlayRasterClock_ = 0;
  ComPtrLite<ID2D1Factory> d2dFactory_;
  ComPtrLite<IDWriteFactory> dwriteFactory_;
  ComPtrLite<IWICImagingFactory> wicFactory_;
  bool overlayRasterUnavailable_ = false;
};

}  // namespace corevideo::modules

#endif  // !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_D3D11
