#include "compositor/CompositorOverlayRaster.h"

// The overlay raster is intentionally a dev-machine adapter. COREVIDEO_STUB
// builds compile this to nothing; the same three gates as D3D11CompositorAdapter
// must all be explicit: non-stub, dev adapters, and D3D11.
#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_D3D11

#include <dxgi.h>

#include <algorithm>
#include <cmath>

#include "modules/OverlayTileRaster.h"
#include "core/ComApartmentLifetime.h"

namespace corevideo::modules {
namespace {

// UTF-8 -> UTF-16 for DirectWrite (overlay text arrives as UTF-8 JSON strings,
// including non-ASCII — the core's \uXXXX parser decodes to UTF-8).
std::wstring widenUtf8(const std::string& text) {
  if (text.empty()) {
    return {};
  }
  const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  if (needed <= 0) {
    return {};
  }
  std::wstring wide(static_cast<size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), needed);
  return wide;
}

// Brand colors are 0xAARRGGBB (same packing writeLayerConstants consumes).
D2D1_COLOR_F d2dColorFromArgb(uint32_t argb) {
  return D2D1::ColorF(
      static_cast<float>((argb >> 16) & 0xff) / 255.f,
      static_cast<float>((argb >> 8) & 0xff) / 255.f,
      static_cast<float>(argb & 0xff) / 255.f,
      static_cast<float>((argb >> 24) & 0xff) / 255.f);
}

}  // namespace

bool CompositorOverlayRaster::ensureOverlayRasterFactories() {
  if (d2dFactory_ && dwriteFactory_ && wicFactory_) {
    return true;
  }
  if (overlayRasterUnavailable_) {
    return false;
  }
  if (!d2dFactory_ &&
      FAILED(D2D1CreateFactory(
          D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory), nullptr,
          reinterpret_cast<void**>(d2dFactory_.put())))) {
    overlayRasterUnavailable_ = true;
    return false;
  }
  if (!dwriteFactory_ &&
      FAILED(DWriteCreateFactory(
          DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
          reinterpret_cast<IUnknown**>(dwriteFactory_.put())))) {
    overlayRasterUnavailable_ = true;
    return false;
  }
  // All prepared tiles use a CPU WIC target. The preparation owner has already
  // initialized its COM apartment; optional image decoding uses this factory too.
  if (!wicFactory_) {
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(wicFactory_.put()));
  }
  return bool(wicFactory_);
}

ComPtrLite<ID2D1Bitmap> CompositorOverlayRaster::decodeOverlayImage(
    ID2D1RenderTarget* target, const std::string& imageUri) {
  ComPtrLite<ID2D1Bitmap> bitmap;
  if (!wicFactory_ || !target || imageUri.empty()) {
    return bitmap;
  }
  std::string path = imageUri;
  if (path.rfind("file:///", 0) == 0) {
    path = path.substr(8);
  } else if (path.rfind("file://", 0) == 0) {
    path = path.substr(7);
  }
  const std::wstring widePath = widenUtf8(path);
  if (widePath.empty()) {
    return bitmap;
  }
  ComPtrLite<IWICBitmapDecoder> decoder;
  if (FAILED(wicFactory_->CreateDecoderFromFilename(
          widePath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, decoder.put()))) {
    return bitmap;
  }
  ComPtrLite<IWICBitmapFrameDecode> frame;
  if (FAILED(decoder->GetFrame(0, frame.put()))) {
    return bitmap;
  }
  UINT imageWidth = 0, imageHeight = 0;
  if (FAILED(frame->GetSize(&imageWidth, &imageHeight)) || imageWidth == 0 || imageHeight == 0 || imageWidth > 4096 || imageHeight > 4096 ||
      uint64_t(imageWidth) * imageHeight > 16ull * 1024 * 1024) return bitmap;
  ComPtrLite<IWICFormatConverter> converter;
  if (FAILED(wicFactory_->CreateFormatConverter(converter.put()))) {
    return bitmap;
  }
  // 32bppPBGRA = premultiplied BGRA, D2D's native interchange format.
  if (FAILED(converter->Initialize(
          frame.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
          nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
    return bitmap;
  }
  if (FAILED(target->CreateBitmapFromWicBitmap(converter.get(), nullptr, bitmap.put()))) {
    bitmap = {};
  }
  return bitmap;
}

bool CompositorOverlayRaster::drawOverlayTextLine(
    ID2D1RenderTarget* target,
    const std::string& text,
    const std::string& fontFamily,
    DWRITE_FONT_WEIGHT weight,
    const D2D1_RECT_F& box,
    const D2D1_COLOR_F& color, float explicitFontSize) {
  if (text.empty() || box.right <= box.left || box.bottom <= box.top) {
    return true;
  }
  const std::wstring wideText = widenUtf8(text);
  if (wideText.empty()) {
    return true;
  }
  const std::wstring wideFamily = widenUtf8(fontFamily.empty() ? "Segoe UI" : fontFamily);
  // Em size ~72% of the line box: leaves room for ascenders/descenders so the
  // layout's vertical centering doesn't clip.
  const float fontSize = explicitFontSize>0 ? explicitFontSize : (std::max)(4.f, (box.bottom - box.top) * 0.72f);
  ComPtrLite<IDWriteTextFormat> format;
  if (FAILED(dwriteFactory_->CreateTextFormat(
          wideFamily.c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
          DWRITE_FONT_STRETCH_NORMAL, fontSize, L"en-us", format.put()))) {
    return false;
  }
  format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
  format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  ComPtrLite<IDWriteInlineObject> ellipsis;
  if (SUCCEEDED(dwriteFactory_->CreateEllipsisTrimmingSign(format.get(), ellipsis.put()))) {
    DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    format->SetTrimming(&trimming, ellipsis.get());
  }
  ComPtrLite<ID2D1SolidColorBrush> brush;
  if (FAILED(target->CreateSolidColorBrush(color, brush.put()))) {
    return false;
  }
  target->DrawText(
      wideText.c_str(), static_cast<UINT32>(wideText.size()), format.get(), box, brush.get());
  return true;
}

CompositorOverlayRaster::CompositorOverlayRaster()
    : cache_([this](const Request& request) { return prepareRaster(request); },
             256ull * 1024 * 1024, 32, [this] {
               // Release apartment-owned factories before the owner's thread-local
               // COM lifetime ends, including tests with no process-wide MTA.
               wicFactory_ = {}; dwriteFactory_ = {}; d2dFactory_ = {};
             }, 16) {}
CompositorOverlayRaster::~CompositorOverlayRaster() = default;

ID3D11ShaderResourceView* CompositorOverlayRaster::rasterOverlayTexture(
    ID3D11Device* device, ID3D11DeviceContext*, const CompositorOverlayContent& overlay,
    const compositor::LayerRect& rect, int targetWidth, int targetHeight) {
  // Bound metadata before hashing or copying it into the queue. In particular,
  // a legacy caller cannot amplify one oversized caption into 32 queued copies.
  size_t metadataBytes = 0;
  const auto& a = overlay.appearance;
  for (const auto* value : {&overlay.title, &overlay.org, &overlay.text, &overlay.speaker,
      &overlay.imageUri, &overlay.fontFamily, &overlay.keyPosition, &overlay.keyPhase, &overlay.keyer,
      &overlay.brandColor, &overlay.brandAccentColor, &overlay.brandBackgroundColor,
      &a.preset, &a.anchor, &a.fontFamily, &a.nameColor, &a.titleColor, &a.backgroundColor, &a.accentColor}) {
    if (value->size() > 65536 - metadataBytes) {
      activeLease_.reset(); lastState_ = "failed"; failedThisFrame_ = true;
      lastWarning_ = "Graphics content exceeds preparation metadata budget.";
      return nullptr;
    }
    metadataBytes += value->size();
  }
  const double widthPixels = double(rect.width) * targetWidth;
  const double heightPixels = double(rect.height) * targetHeight;
  if (!device || !std::isfinite(widthPixels) || !std::isfinite(heightPixels) ||
      widthPixels < 1 || heightPixels < 1 || widthPixels > 4096 || heightPixels > 4096) {
    activeLease_.reset(); lastState_ = "failed"; failedThisFrame_ = true;
    lastWarning_ = "Invalid graphics raster dimensions.";
    return nullptr;
  }
  const int width = int(std::lround(widthPixels)), height = int(std::lround(heightPixels));
  const Key key{reinterpret_cast<uintptr_t>(device), overlayContentSignature(overlay, width, height)};
  activeLease_ = cache_.find(key);
  if (!activeLease_) {
    device->AddRef();
    Request request{std::shared_ptr<ID3D11Device>(device, [](ID3D11Device* value) { value->Release(); }), overlay, width, height};
    activeLease_ = cache_.findOrRequest(key, std::move(request), uint64_t(width) * height * 8 + (overlay.imageUri.empty() ? 0 : 64ull * 1024 * 1024));
  }
  lastState_ = !activeLease_ ? "preparing" : activeLease_->asset ? "ready" : "failed";
  lastWarning_ = activeLease_ ? activeLease_->warning : "Graphics preparation pending.";
  pendingThisFrame_ = pendingThisFrame_ || !activeLease_;
  failedThisFrame_ = failedThisFrame_ || (activeLease_ && !activeLease_->asset);
  return activeLease_ && activeLease_->asset ? activeLease_->asset->view.get() : nullptr;
}

CompositorOverlayRaster::Cache::Result CompositorOverlayRaster::prepareRaster(const Request& request) {
  thread_local core::ComApartmentLifetime apartment;
  const auto& overlay = request.overlay;
  const int widthPx = request.width, heightPx = request.height;
  auto* device = request.device.get();
  workerWarning_.clear();
  if (!apartment.initialized() || !ensureOverlayRasterFactories() || !wicFactory_) {
    return {{}, 0, "Native graphics preparation unavailable."};
  }
  auto entry = std::make_shared<OverlayRasterTex>();
  ComPtrLite<IWICBitmap> bitmap;
  if (FAILED(wicFactory_->CreateBitmap(widthPx, heightPx, GUID_WICPixelFormat32bppPBGRA,
      WICBitmapCacheOnLoad, bitmap.put()))) return {{}, 0, "Graphics bitmap allocation failed."};
  const auto targetProperties = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_SOFTWARE,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
  ComPtrLite<ID2D1RenderTarget> d2dTarget;
  if (FAILED(d2dFactory_->CreateWicBitmapRenderTarget(bitmap.get(), &targetProperties, d2dTarget.put())))
    return {{}, 0, "Graphics raster target creation failed."};

  // Geometry + per-line colors come from the shared layout resolver, so this
  // DirectWrite raster and the portable CPU bitmap-font tile
  // (rasterizeOverlayTileBgra) place identical content — preview and program
  // agree by construction. The band background is NOT painted here (it stays
  // a separate quad so its alpha animation matches the CPU mirror).
  auto layout = computeOverlayTileLayout(overlay, widthPx, heightPx);
  if (overlay.appearance.enabled) {
    ComPtrLite<IDWriteFontCollection> fonts;BOOL exists=FALSE;UINT32 index=0;
    const auto family=widenUtf8(layout.fontFamily);
    if (SUCCEEDED(dwriteFactory_->GetSystemFontCollection(fonts.put())) &&
        SUCCEEDED(fonts->FindFamilyName(family.c_str(),&index,&exists)) && !exists) {
      layout.fontFamily="Segoe UI";workerWarning_="Font unavailable; using Segoe UI.";
    }
  }

  d2dTarget->BeginDraw();
  d2dTarget->Clear(D2D1::ColorF(0.f, 0.f, 0.f, 0.f));
  ComPtrLite<ID2D1RoundedRectangleGeometry> rounded;
  bool clipped=false;
  if (overlay.appearance.enabled && !overlay.isCaption) {
    const auto band=D2D1::RoundedRect(D2D1::RectF(0,0,float(widthPx),float(heightPx)),layout.cornerRadius,layout.cornerRadius);
    if (layout.cornerRadius>0 && SUCCEEDED(d2dFactory_->CreateRoundedRectangleGeometry(band,rounded.put()))) {
      d2dTarget->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),rounded.get()),nullptr); clipped=true;
    }
    ComPtrLite<ID2D1SolidColorBrush> bg;
    if (SUCCEEDED(d2dTarget->CreateSolidColorBrush(d2dColorFromArgb(layout.backgroundArgb),bg.put()))) d2dTarget->FillRoundedRectangle(band,bg.get());
  }

  ComPtrLite<ID2D1SolidColorBrush> accentBrush;
  if (SUCCEEDED(d2dTarget->CreateSolidColorBrush(d2dColorFromArgb(layout.accentArgb), accentBrush.put()))) {
    d2dTarget->FillRectangle(
        D2D1::RectF(layout.accentBar.x, layout.accentBar.y,
                    layout.accentBar.x + layout.accentBar.width,
                    layout.accentBar.y + layout.accentBar.height),
        accentBrush.get());
  }

  // Real WIC decode aspect-fitted inside the layout's image slot (the CPU
  // tile draws its deterministic checker in the same slot).
  if (layout.hasImage) {
    auto image=decodeOverlayImage(d2dTarget.get(),overlay.imageUri);
    if (!image) workerWarning_ += " Logo unavailable; image omitted.";
    if (image) {
      const auto imageSize = image->GetSize();
      float drawWidth = layout.imageRect.width;
      float drawHeight = layout.imageRect.height;
      if (imageSize.width > 0.f && imageSize.height > 0.f) {
        const float scale = (std::min)(
            layout.imageRect.width / imageSize.width, layout.imageRect.height / imageSize.height);
        drawWidth = imageSize.width * scale;
        drawHeight = imageSize.height * scale;
      }
      const float imageLeft = layout.imageRect.x + (layout.imageRect.width - drawWidth) * 0.5f;
      const float imageTop = layout.imageRect.y + (layout.imageRect.height - drawHeight) * 0.5f;
      d2dTarget->DrawBitmap(
          image.get(), D2D1::RectF(imageLeft, imageTop, imageLeft + drawWidth, imageTop + drawHeight));
    }
  }

  // First line is the emphasis line (lower-third title, or caption speaker
  // attribution when present) — semibold; the rest render normal weight.
  for (size_t lineIndex = 0; lineIndex < layout.textLines.size(); ++lineIndex) {
    const auto& line = layout.textLines[lineIndex];
    const bool emphasis =
        lineIndex == 0 && (!overlay.isCaption || !overlay.speaker.empty());
    drawOverlayTextLine(
        d2dTarget.get(), line.text, layout.fontFamily,
        emphasis ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
        D2D1::RectF(line.rect.x, line.rect.y,
                    line.rect.x + line.rect.width, line.rect.y + line.rect.height),
        d2dColorFromArgb(line.colorArgb),line.fontSize);
  }

  if (clipped) d2dTarget->PopLayer();
  const HRESULT endDrawResult = d2dTarget->EndDraw();
  if (FAILED(endDrawResult)) return {{}, 0, "Graphics rasterization failed."};
  WICRect lockRect{0, 0, widthPx, heightPx};
  ComPtrLite<IWICBitmapLock> pixels;
  UINT stride = 0, length = 0; BYTE* data = nullptr;
  if (FAILED(bitmap->Lock(&lockRect, WICBitmapLockRead, pixels.put())) ||
      FAILED(pixels->GetStride(&stride)) || FAILED(pixels->GetDataPointer(&length, &data)))
    return {{}, 0, "Graphics pixels unavailable."};
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = widthPx; desc.Height = heightPx; desc.MipLevels = desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_IMMUTABLE; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA initial{data, stride, 0};
  if (FAILED(device->CreateTexture2D(&desc, &initial, entry->texture.put())) ||
      FAILED(device->CreateShaderResourceView(entry->texture.get(), nullptr, entry->view.put())))
    return {{}, 0, "Graphics texture creation failed."};
  return {std::move(entry), uint64_t(widthPx) * heightPx * 4, workerWarning_};
}

}  // namespace corevideo::modules

#endif  // !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_D3D11
