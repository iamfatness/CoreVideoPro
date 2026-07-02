#include "modules/DirectWriteOverlayRaster.h"

// The DirectWrite/D2D/WIC raster is a dev-machine adapter, gated exactly like
// the D3D11 compositor it feeds: portable/stub builds compile the fallback
// stub below so `-DCOREVIDEO_STUB=ON` stays green off-Windows.
#if defined(_WIN32) && !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_D3D11

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>

#include "modules/OverlayTileRaster.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace corevideo::modules {
namespace {

template <typename T>
class ComPtrLite {
 public:
  ComPtrLite() = default;
  ~ComPtrLite() { reset(); }
  ComPtrLite(const ComPtrLite&) = delete;
  ComPtrLite& operator=(const ComPtrLite&) = delete;

  T** put() {
    reset();
    return &value_;
  }
  T* get() const { return value_; }
  T* operator->() const { return value_; }
  explicit operator bool() const { return value_ != nullptr; }

  void reset() {
    if (value_ != nullptr) {
      value_->Release();
      value_ = nullptr;
    }
  }

 private:
  T* value_ = nullptr;
};

D2D1_COLOR_F colorFromArgb(uint32_t argb) {
  return D2D1::ColorF(
      static_cast<float>((argb >> 16) & 0xff) / 255.f,
      static_cast<float>((argb >> 8) & 0xff) / 255.f,
      static_cast<float>(argb & 0xff) / 255.f,
      static_cast<float>((argb >> 24) & 0xff) / 255.f);
}

D2D1_RECT_F rectFromTile(const OverlayTileRect& rect) {
  return D2D1::RectF(rect.x, rect.y, rect.x + rect.width, rect.y + rect.height);
}

std::wstring widen(const std::string& value) {
  if (value.empty()) {
    return std::wstring();
  }
  const int required = MultiByteToWideChar(
      CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
  if (required <= 0) {
    return std::wstring();
  }
  std::wstring wide(static_cast<size_t>(required), L'\0');
  MultiByteToWideChar(
      CP_UTF8, 0, value.data(), static_cast<int>(value.size()), wide.data(), required);
  return wide;
}

// Accepts a plain path, file:///C:/..., or file://C:/... image reference and
// returns a filesystem path WIC can open. Non-file URIs return empty (fail →
// CPU checker fallback).
std::wstring imagePathFromUri(const std::string& imageUri) {
  std::string path = imageUri;
  if (path.rfind("file:///", 0) == 0) {
    path = path.substr(8);
  } else if (path.rfind("file://", 0) == 0) {
    path = path.substr(7);
  } else if (path.find("://", 0) != std::string::npos) {
    return std::wstring();
  }
  std::replace(path.begin(), path.end(), '/', '\\');
  return widen(path);
}

// Process-lifetime rasterizer state. Created on first use on the compositor's
// render thread (the only caller) and reused across tiles.
struct RasterFactories {
  ComPtrLite<ID2D1Factory> d2d;
  ComPtrLite<IDWriteFactory> dwrite;
  ComPtrLite<IWICImagingFactory> wic;
  bool valid = false;
};

RasterFactories& factories() {
  static RasterFactories instance = [] {
    RasterFactories created;
    // Tolerate an already-initialized apartment (matches the MF/WASAPI
    // adapters); only a genuine failure disables the rasterizer.
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(co) && co != RPC_E_CHANGED_MODE) {
      return created;
    }
    if (FAILED(D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED, created.d2d.put()))) {
      return created;
    }
    if (FAILED(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(created.dwrite.put())))) {
      return created;
    }
    if (FAILED(CoCreateInstance(
            CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(created.wic.put())))) {
      return created;
    }
    created.valid = true;
    return created;
  }();
  return instance;
}

bool drawImageFromUri(
    ID2D1RenderTarget* target,
    IWICImagingFactory* wic,
    const std::string& imageUri,
    const OverlayTileRect& imageRect) {
  const std::wstring path = imagePathFromUri(imageUri);
  if (path.empty()) {
    return false;
  }
  ComPtrLite<IWICBitmapDecoder> decoder;
  if (FAILED(wic->CreateDecoderFromFilename(
          path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
          decoder.put()))) {
    return false;
  }
  ComPtrLite<IWICBitmapFrameDecode> frame;
  if (FAILED(decoder->GetFrame(0, frame.put()))) {
    return false;
  }
  ComPtrLite<IWICFormatConverter> converter;
  if (FAILED(wic->CreateFormatConverter(converter.put()))) {
    return false;
  }
  if (FAILED(converter->Initialize(
          frame.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
          nullptr, 0.0, WICBitmapPaletteTypeMedianCut))) {
    return false;
  }
  ComPtrLite<ID2D1Bitmap> bitmap;
  if (FAILED(target->CreateBitmapFromWicBitmap(converter.get(), nullptr, bitmap.put()))) {
    return false;
  }
  // Aspect-fit the decoded image inside the layout's image rect, centered.
  const D2D1_SIZE_F imageSize = bitmap->GetSize();
  if (imageSize.width <= 0.f || imageSize.height <= 0.f ||
      imageRect.width <= 0.f || imageRect.height <= 0.f) {
    return false;
  }
  const float scale = std::min(
      imageRect.width / imageSize.width, imageRect.height / imageSize.height);
  const float drawWidth = imageSize.width * scale;
  const float drawHeight = imageSize.height * scale;
  const float drawX = imageRect.x + (imageRect.width - drawWidth) * 0.5f;
  const float drawY = imageRect.y + (imageRect.height - drawHeight) * 0.5f;
  target->DrawBitmap(
      bitmap.get(), D2D1::RectF(drawX, drawY, drawX + drawWidth, drawY + drawHeight),
      1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
  return true;
}

void drawTextLine(
    ID2D1RenderTarget* target,
    IDWriteFactory* dwrite,
    const OverlayTileTextLine& line,
    const std::wstring& fontFamily) {
  if (line.text.empty() || line.rect.width <= 0.f || line.rect.height <= 0.f) {
    return;
  }
  // Match the bitmap-font layout: the line box is the glyph cell, so the em
  // size fills most of the box height and the baseline centers vertically.
  const float fontSize = std::max(4.f, line.rect.height * 0.72f);
  ComPtrLite<IDWriteTextFormat> format;
  if (FAILED(dwrite->CreateTextFormat(
          fontFamily.c_str(), nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
          DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fontSize,
          L"en-us", format.put()))) {
    return;
  }
  format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
  format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  ComPtrLite<ID2D1SolidColorBrush> brush;
  if (FAILED(target->CreateSolidColorBrush(colorFromArgb(line.colorArgb), brush.put()))) {
    return;
  }
  const std::wstring text = widen(line.text);
  target->DrawText(
      text.c_str(), static_cast<UINT32>(text.size()), format.get(),
      rectFromTile(line.rect), brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

}  // namespace

bool rasterizeOverlayTileDirectWrite(
    const CompositorOverlayContent& overlay,
    int widthPx,
    int heightPx,
    std::vector<uint8_t>& outBgra) {
  if (widthPx <= 0 || heightPx <= 0) {
    return false;
  }
  auto& raster = factories();
  if (!raster.valid) {
    return false;
  }

  ComPtrLite<IWICBitmap> wicBitmap;
  if (FAILED(raster.wic->CreateBitmap(
          static_cast<UINT>(widthPx), static_cast<UINT>(heightPx),
          GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand, wicBitmap.put()))) {
    return false;
  }
  ComPtrLite<ID2D1RenderTarget> target;
  const auto targetProperties = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_SOFTWARE,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
      96.f, 96.f);
  if (FAILED(raster.d2d->CreateWicBitmapRenderTarget(
          wicBitmap.get(), targetProperties, target.put()))) {
    return false;
  }

  const auto layout = computeOverlayTileLayout(overlay, widthPx, heightPx);
  const std::wstring fontFamily =
      layout.fontFamily.empty() ? L"Segoe UI" : widen(layout.fontFamily);

  target->BeginDraw();
  // Background band covers the full tile (matches the CPU tile).
  target->Clear(colorFromArgb(layout.backgroundArgb));
  ComPtrLite<ID2D1SolidColorBrush> accentBrush;
  if (SUCCEEDED(target->CreateSolidColorBrush(colorFromArgb(layout.accentArgb), accentBrush.put()))) {
    target->FillRectangle(rectFromTile(layout.accentBar), accentBrush.get());
  }
  if (layout.hasImage) {
    // A failed decode keeps the DirectWrite text raster (band stays brand-
    // styled); the region just stays background rather than the CPU checker.
    (void)drawImageFromUri(target.get(), raster.wic.get(), overlay.imageUri, layout.imageRect);
  }
  for (const auto& line : layout.textLines) {
    drawTextLine(target.get(), raster.dwrite.get(), line, fontFamily);
  }
  if (FAILED(target->EndDraw())) {
    return false;
  }

  // Copy out and convert premultiplied -> straight alpha (the compositor's
  // blend state is SRC_ALPHA/INV_SRC_ALPHA over straight-alpha textures).
  outBgra.assign(static_cast<size_t>(widthPx) * static_cast<size_t>(heightPx) * 4u, 0);
  const UINT stride = static_cast<UINT>(widthPx) * 4u;
  if (FAILED(wicBitmap->CopyPixels(
          nullptr, stride, static_cast<UINT>(outBgra.size()), outBgra.data()))) {
    return false;
  }
  for (size_t offset = 0; offset + 3 < outBgra.size(); offset += 4) {
    const uint8_t alpha = outBgra[offset + 3];
    if (alpha == 0 || alpha == 0xff) {
      continue;
    }
    for (int channel = 0; channel < 3; ++channel) {
      const auto premultiplied = static_cast<uint32_t>(outBgra[offset + static_cast<size_t>(channel)]);
      outBgra[offset + static_cast<size_t>(channel)] =
          static_cast<uint8_t>(std::min<uint32_t>(255u, (premultiplied * 255u + alpha / 2u) / alpha));
    }
  }
  return true;
}

}  // namespace corevideo::modules

#else  // portable / stub builds

namespace corevideo::modules {

bool rasterizeOverlayTileDirectWrite(
    const CompositorOverlayContent& /*overlay*/,
    int /*widthPx*/,
    int /*heightPx*/,
    std::vector<uint8_t>& /*outBgra*/) {
  return false;
}

}  // namespace corevideo::modules

#endif
