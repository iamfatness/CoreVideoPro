#include "compositor/CompositorOverlayRaster.h"

// The overlay raster is intentionally a dev-machine adapter. COREVIDEO_STUB
// builds compile this to nothing; the same three gates as D3D11CompositorAdapter
// must all be explicit: non-stub, dev adapters, and D3D11.
#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_D3D11

#include <dxgi.h>

#include <algorithm>
#include <cmath>
#include <string_view>

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
  if (!d2dFactory_ &&
      FAILED(D2D1CreateFactory(
          D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory), nullptr,
          reinterpret_cast<void**>(d2dFactory_.put())))) {
    return false;
  }
  if (!dwriteFactory_ &&
      FAILED(DWriteCreateFactory(
          DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
          reinterpret_cast<IUnknown**>(dwriteFactory_.put())))) {
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

CompositorOverlayRaster::ImageInput CompositorOverlayRaster::openOverlayImage(const std::string& imageUri) {
  if (!wicFactory_ || imageUri.empty()) return {};
  std::string path = imageUri;
  if (path.rfind("file:///", 0) == 0) {
    path = path.substr(8);
  } else if (path.rfind("file://", 0) == 0) {
    path = path.substr(7);
  }
  const std::wstring widePath = widenUtf8(path);
  if (widePath.empty()) {
    return {};
  }
  ComPtrLite<IWICBitmapDecoder> decoder;
  if (FAILED(wicFactory_->CreateDecoderFromFilename(
          widePath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, decoder.put()))) {
    return {};
  }
  ComPtrLite<IWICBitmapFrameDecode> frame;
  if (FAILED(decoder->GetFrame(0, frame.put()))) {
    return {};
  }
  UINT imageWidth = 0, imageHeight = 0;
  if (FAILED(frame->GetSize(&imageWidth, &imageHeight)) || imageWidth == 0 || imageHeight == 0 || imageWidth > 4096 || imageHeight > 4096 ||
      uint64_t(imageWidth) * imageHeight > 16ull * 1024 * 1024) return {};
  return {std::move(frame), uint64_t(imageWidth) * imageHeight * 8};
}

ComPtrLite<ID2D1Bitmap> CompositorOverlayRaster::decodeOverlayImage(
    ID2D1RenderTarget* target, IWICBitmapFrameDecode* frame) {
  ComPtrLite<ID2D1Bitmap> bitmap;
  if (!target || !frame) return bitmap;
  ComPtrLite<IWICFormatConverter> converter;
  if (FAILED(wicFactory_->CreateFormatConverter(converter.put()))) {
    return bitmap;
  }
  // 32bppPBGRA = premultiplied BGRA, D2D's native interchange format.
  if (FAILED(converter->Initialize(
          frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
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
             }, 64) {}
CompositorOverlayRaster::~CompositorOverlayRaster() = default;

bool CompositorOverlayRaster::drawTemplate(ID2D1RenderTarget* target,
    const graphics::GraphicsDrawing& drawing, int height) {
  ComPtrLite<IDWriteFontCollection> fonts;
  if (FAILED(dwriteFactory_->GetSystemFontCollection(fonts.put())) || !fonts) return false;
  const float scale = float(height / drawing.height);
  target->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));
  bool complete = true;
  for (const auto& p : drawing.primitives) {
    const auto& b = p.box; const auto& c = p.clip;
    target->PushAxisAlignedClip(D2D1::RectF(float(c.x), float(c.y), float(c.x + c.width), float(c.y + c.height)), D2D1_ANTIALIAS_MODE_ALIASED);
    ComPtrLite<ID2D1SolidColorBrush> solid;
    ComPtrLite<ID2D1LinearGradientBrush> gradient;
    ID2D1Brush* brush = nullptr;
    if (p.gradient) {
      const D2D1_GRADIENT_STOP stops[] = {{0.f, d2dColorFromArgb(p.color)}, {1.f, d2dColorFromArgb(p.endColor)}};
      ComPtrLite<ID2D1GradientStopCollection> collection;
      if (SUCCEEDED(target->CreateGradientStopCollection(stops, 2, collection.put())) &&
          SUCCEEDED(target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
              D2D1::Point2F(float(b.x), float(b.y)), D2D1::Point2F(float(b.x + (p.verticalGradient ? 0 : b.width)),
              float(b.y + (p.verticalGradient ? b.height : 0)))), collection.get(), gradient.put()))) brush = gradient.get();
    } else if (SUCCEEDED(target->CreateSolidColorBrush(d2dColorFromArgb(p.color), solid.put()))) brush = solid.get();
    if (!brush) complete = false;
    else {
      brush->SetOpacity(float(p.opacity));
      if (p.type == "rect") target->FillRoundedRectangle(D2D1::RoundedRect(
          D2D1::RectF(float(b.x), float(b.y), float(b.x + b.width), float(b.y + b.height)), float(p.cornerRadius), float(p.cornerRadius)), brush);
      else if (p.type == "text" && !p.text.empty()) {
        const auto family = widenUtf8(p.font), text = widenUtf8(p.text);
        UINT32 fontIndex = 0; BOOL exists = FALSE;
        ComPtrLite<IDWriteTextFormat> format;
        ComPtrLite<IDWriteTextLayout> layout;
        const bool ready = SUCCEEDED(fonts->FindFamilyName(family.c_str(), &fontIndex, &exists)) && exists &&
            SUCCEEDED(dwriteFactory_->CreateTextFormat(family.c_str(), fonts.get(), DWRITE_FONT_WEIGHT(p.fontWeight),
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, float(p.fontSize), L"", format.put()));
        if (!ready) { complete = false; workerWarning_ = "Requested graphics font is unavailable: " + p.font; }
        else {
          bool textReady = SUCCEEDED(format->SetTextAlignment(p.align == "center" ? DWRITE_TEXT_ALIGNMENT_CENTER : p.align == "right" ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING)) &&
              SUCCEEDED(format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR)) &&
              SUCCEEDED(format->SetWordWrapping(p.overflow == "wrap-then-ellipsis" ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP));
          const float lineHeight = float(p.fontSize * p.lineHeight);
          textReady = textReady && SUCCEEDED(dwriteFactory_->CreateTextLayout(text.c_str(), UINT32(text.size()), format.get(),
              float(b.width), float(std::min(b.height, double(lineHeight) * p.maxLines)), layout.put()));
          if (textReady) {
            ComPtrLite<IDWriteInlineObject> ellipsis;
            DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            textReady = SUCCEEDED(layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, lineHeight, float(p.fontSize * .8))) &&
                SUCCEEDED(dwriteFactory_->CreateEllipsisTrimmingSign(format.get(), ellipsis.put())) &&
                SUCCEEDED(layout->SetTrimming(&trim, ellipsis.get()));
          }
          if (textReady) target->DrawTextLayout(D2D1::Point2F(float(b.x), float(b.y)), layout.get(), brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
          else complete = false;
        }
      }
    }
    target->PopAxisAlignedClip();
  }
  target->SetTransform(D2D1::Matrix3x2F::Identity());
  return complete;
}

ID3D11ShaderResourceView* CompositorOverlayRaster::rasterOverlayTexture(
    ID3D11Device* device, ID3D11DeviceContext*, const CompositorOverlayContent& overlay,
    const compositor::LayerRect& rect, int targetWidth, int targetHeight,
    const std::string& preparationSlot, bool strictResources,
    std::shared_ptr<const graphics::GraphicsPreparedResource> prepared) {
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
  const Key key{reinterpret_cast<uintptr_t>(device), overlayContentSignature(overlay, width, height) ^
      (strictResources ? 0xd9e94a8e64b5fc21ULL : 0ULL), overlay.graphicsContentHash, overlay.hasGraphicsContentHash};
  const bool compatible = prepared && prepared->native && prepared->backend == graphics::GraphicsPreparedResource::Backend::D3D11 &&
      prepared->deviceIdentity == key.device && prepared->width > 0 && prepared->height > 0 &&
      prepared->hasContentHash == overlay.hasGraphicsContentHash && prepared->contentHash == overlay.graphicsContentHash &&
      prepared->width <= 4096 && prepared->height <= 4096 && prepared->signature ==
          (overlayContentSignature(overlay, prepared->width, prepared->height) ^ (strictResources ? 0xd9e94a8e64b5fc21ULL : 0ULL));
  activeResource_.reset();
  activeLease_.reset();
  if (compatible && prepared->width == width && prepared->height == height) {
    activeLease_ = std::static_pointer_cast<const Cache::Result>(prepared->native);
    activeResource_ = prepared;
  } else activeLease_ = cache_.find(key);
  if (!activeLease_) {
    device->AddRef();
    Request request{std::shared_ptr<ID3D11Device>(device, [](ID3D11Device* value) { value->Release(); }), overlay, width, height, strictResources};
    activeLease_ = cache_.findOrRequest(key, std::move(request),
        uint64_t(width) * height * 8 + (overlay.imageUri.empty() ? 0 : 128ull * 1024 * 1024), preparationSlot);
  }
  const bool failedScale = activeLease_ && !activeLease_->asset && compatible;
  const std::string scaleWarning = failedScale ? activeLease_->warning : "";
  bool retainingScale = false;
  if ((!activeLease_ || !activeLease_->asset) && compatible) {
    activeLease_ = std::static_pointer_cast<const Cache::Result>(prepared->native);
    activeResource_ = prepared; retainingScale = true;
  }
  activeDevice_ = key.device; activeSignature_ = key.signature;
  activeContentHash_ = overlay.graphicsContentHash; activeHasContentHash_ = overlay.hasGraphicsContentHash;
  activeWidth_ = width; activeHeight_ = height;
  lastState_ = failedScale ? "failed" : retainingScale ? "preparing" : !activeLease_ ? "preparing" : activeLease_->asset ? "ready" : "failed";
  lastWarning_ = failedScale ? scaleWarning + " Retaining the accepted raster." : retainingScale ? "Graphics scale preparation pending; retaining the accepted raster." :
      activeLease_ ? activeLease_->warning : "Graphics preparation pending.";
  pendingThisFrame_ = pendingThisFrame_ || (retainingScale && !failedScale) || !activeLease_;
  failedThisFrame_ = failedThisFrame_ || failedScale || (activeLease_ && !activeLease_->asset);
  return activeLease_ && activeLease_->asset ? activeLease_->asset->view.get() : nullptr;
}

std::shared_ptr<const graphics::GraphicsPreparedResource> CompositorOverlayRaster::preparedResource() {
  if (!activeLease_ || !activeLease_->asset || std::string_view(lastState_) != "ready") return {};
  if (activeResource_) return activeResource_;
  auto resource = std::make_shared<graphics::GraphicsPreparedResource>();
  resource->backend = graphics::GraphicsPreparedResource::Backend::D3D11;
  resource->deviceIdentity = activeDevice_; resource->signature = activeSignature_;
  resource->width = activeWidth_; resource->height = activeHeight_; resource->native = activeLease_;
  resource->contentHash = activeContentHash_; resource->hasContentHash = activeHasContentHash_;
  activeResource_ = resource;
  return resource;
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
  graphics::GraphicsDrawing drawing;
  if (overlay.graphicsTemplate) {
    std::string error;
    if (!overlay.graphicsTemplate->definition->resolve(overlay.graphicsTemplate->fields,
        overlay.graphicsTemplate->brand, drawing, error)) return {{}, 0, error};
  }
  auto layout = computeOverlayTileLayout(overlay, widthPx, heightPx);
  const auto imageInput = layout.hasImage ? openOverlayImage(overlay.imageUri) : ImageInput{};
  const auto textureBytes = uint64_t(widthPx) * heightPx * 4;
  auto& sharedBudget = graphics::GraphicsResourceBudget::process();
  if (textureBytes + imageInput.cpuBytes > sharedBudget.counters().cpuLimit)
    return {{}, 0, "Graphics assets exceed the shared CPU preparation capacity."};
  auto budget = sharedBudget.tryReserve(textureBytes + imageInput.cpuBytes, textureBytes);
  if (!budget) return {{}, 0, "Graphics preparation is waiting for shared resource capacity.", true};
  auto entry = std::make_shared<OverlayRasterTex>();
  entry->budget = budget;
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
  if (!overlay.graphicsTemplate && overlay.appearance.enabled) {
    ComPtrLite<IDWriteFontCollection> fonts;BOOL exists=FALSE;UINT32 index=0;
    const auto family=widenUtf8(layout.fontFamily);
    const bool resolved = SUCCEEDED(dwriteFactory_->GetSystemFontCollection(fonts.put())) && fonts &&
        SUCCEEDED(fonts->FindFamilyName(family.c_str(),&index,&exists));
    if (request.strictResources && (!resolved || !exists))
      return {{}, 0, resolved ? "Requested graphics font is unavailable; choose an installed font." : "Graphics font resolution failed."};
    if (resolved && !exists) {
      layout.fontFamily="Segoe UI";workerWarning_="Font unavailable; using Segoe UI.";
    }
  }

  d2dTarget->BeginDraw();
  d2dTarget->Clear(D2D1::ColorF(0.f, 0.f, 0.f, 0.f));
  bool resourcesComplete = true;
  if (overlay.graphicsTemplate) {
    resourcesComplete = drawTemplate(d2dTarget.get(), drawing, heightPx);
  } else {
  ComPtrLite<ID2D1RoundedRectangleGeometry> rounded;
  bool clipped=false;
  if (overlay.appearance.enabled && !overlay.isCaption) {
    const auto band=D2D1::RoundedRect(D2D1::RectF(0,0,float(widthPx),float(heightPx)),layout.cornerRadius,layout.cornerRadius);
    if (layout.cornerRadius>0 && SUCCEEDED(d2dFactory_->CreateRoundedRectangleGeometry(band,rounded.put()))) {
      d2dTarget->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),rounded.get()),nullptr); clipped=true;
    }
    if (layout.cornerRadius > 0 && !rounded) resourcesComplete = false;
    ComPtrLite<ID2D1SolidColorBrush> bg;
    if (SUCCEEDED(d2dTarget->CreateSolidColorBrush(d2dColorFromArgb(layout.backgroundArgb),bg.put()))) d2dTarget->FillRoundedRectangle(band,bg.get());
    else resourcesComplete = false;
  }

  ComPtrLite<ID2D1SolidColorBrush> accentBrush;
  if (SUCCEEDED(d2dTarget->CreateSolidColorBrush(d2dColorFromArgb(layout.accentArgb), accentBrush.put()))) {
    d2dTarget->FillRectangle(
        D2D1::RectF(layout.accentBar.x, layout.accentBar.y,
                    layout.accentBar.x + layout.accentBar.width,
                    layout.accentBar.y + layout.accentBar.height),
        accentBrush.get());
  } else resourcesComplete = false;

  // Real WIC decode aspect-fitted inside the layout's image slot (the CPU
  // tile draws its deterministic checker in the same slot).
  if (layout.hasImage) {
    auto image=decodeOverlayImage(d2dTarget.get(), imageInput.frame.get());
    if (!image) { workerWarning_ += " Logo unavailable; image omitted."; resourcesComplete = false; }
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
    if (!drawOverlayTextLine(
        d2dTarget.get(), line.text, layout.fontFamily,
        emphasis ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
        D2D1::RectF(line.rect.x, line.rect.y,
                    line.rect.x + line.rect.width, line.rect.y + line.rect.height),
        d2dColorFromArgb(line.colorArgb),line.fontSize)) resourcesComplete = false;
  }

  if (clipped) d2dTarget->PopLayer();
  }
  const HRESULT endDrawResult = d2dTarget->EndDraw();
  if (FAILED(endDrawResult)) return {{}, 0, "Graphics rasterization failed."};
  if (request.strictResources && !resourcesComplete)
    return {{}, 0, workerWarning_.empty() ? "Graphics resources could not be prepared completely." : workerWarning_};
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
  budget->finishPreparation();
  return {std::move(entry), textureBytes, workerWarning_};
}

}  // namespace corevideo::modules

#endif  // !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_D3D11
