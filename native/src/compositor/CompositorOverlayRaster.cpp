#include "compositor/CompositorOverlayRaster.h"

// The overlay raster is intentionally a dev-machine adapter. COREVIDEO_STUB
// builds compile this to nothing; the same three gates as D3D11CompositorAdapter
// must all be explicit: non-stub, dev adapters, and D3D11.
#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_D3D11

#include <dxgi.h>

#include <algorithm>
#include <cmath>

#include "modules/OverlayTileRaster.h"

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
  if (d2dFactory_ && dwriteFactory_) {
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
  // WIC is optional (image overlays only). CoInitializeEx may return S_FALSE
  // (already initialized) or RPC_E_CHANGED_MODE (STA thread) — CoCreateInstance
  // still works in both cases, so only the factory failure disables images.
  if (!wicFactory_) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(wicFactory_.put()));
  }
  return true;
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
    const D2D1_COLOR_F& color) {
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
  const float fontSize = (std::max)(4.f, (box.bottom - box.top) * 0.72f);
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

ID3D11ShaderResourceView* CompositorOverlayRaster::rasterOverlayTexture(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    const CompositorOverlayContent& overlay,
    const compositor::LayerRect& rect,
    int targetWidth,
    int targetHeight) {
  const bool hasText = !overlay.title.empty() || !overlay.org.empty() ||
                       !overlay.text.empty() || !overlay.speaker.empty();
  if (!hasText && overlay.imageUri.empty()) {
    return nullptr;
  }
  const int widthPx = std::clamp(
      static_cast<int>(std::lround(rect.width * static_cast<float>(targetWidth))), 0, 4096);
  const int heightPx = std::clamp(
      static_cast<int>(std::lround(rect.height * static_cast<float>(targetHeight))), 0, 4096);
  if (widthPx < 8 || heightPx < 8) {
    return nullptr;
  }
  if (!ensureOverlayRasterFactories()) {
    return nullptr;
  }

  const uint64_t signature = overlayContentSignature(overlay, widthPx, heightPx);

  ++overlayRasterClock_;
  if (auto existing = overlayTextTextures_.find(signature); existing != overlayTextTextures_.end()) {
    existing->second.lastUsed = overlayRasterClock_;
    return existing->second.view.get();
  }

  OverlayRasterTex entry;
  D3D11_TEXTURE2D_DESC textureDesc{};
  textureDesc.Width = static_cast<UINT>(widthPx);
  textureDesc.Height = static_cast<UINT>(heightPx);
  textureDesc.MipLevels = 1;
  textureDesc.ArraySize = 1;
  textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  textureDesc.SampleDesc.Count = 1;
  textureDesc.Usage = D3D11_USAGE_DEFAULT;
  textureDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  if (FAILED(device->CreateTexture2D(&textureDesc, nullptr, entry.texture.put()))) {
    return nullptr;
  }
  ComPtrLite<IDXGISurface> surface;
  if (FAILED(entry.texture->QueryInterface(__uuidof(IDXGISurface), reinterpret_cast<void**>(surface.put())))) {
    return nullptr;
  }
  const auto targetProperties = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_DEFAULT,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
      96.f, 96.f);
  ComPtrLite<ID2D1RenderTarget> d2dTarget;
  if (FAILED(d2dFactory_->CreateDxgiSurfaceRenderTarget(surface.get(), &targetProperties, d2dTarget.put()))) {
    return nullptr;
  }

  // D2D's EndDraw flushes through the shared device and can clobber the
  // immediate context's bound state mid-pass. Rasters only happen on content
  // change, so snapshot + restore the pipeline state we depend on.
  ComPtrLite<ID3D11RenderTargetView> savedRtv;
  context->OMGetRenderTargets(1, savedRtv.put(), nullptr);
  ComPtrLite<ID3D11BlendState> savedBlend;
  FLOAT savedBlendFactor[4] = {0.f, 0.f, 0.f, 0.f};
  UINT savedSampleMask = 0xffffffffu;
  context->OMGetBlendState(savedBlend.put(), savedBlendFactor, &savedSampleMask);
  ComPtrLite<ID3D11RasterizerState> savedRasterizer;
  context->RSGetState(savedRasterizer.put());
  D3D11_VIEWPORT savedViewport{};
  UINT viewportCount = 1;
  context->RSGetViewports(&viewportCount, &savedViewport);
  D3D11_PRIMITIVE_TOPOLOGY savedTopology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  context->IAGetPrimitiveTopology(&savedTopology);
  ComPtrLite<ID3D11VertexShader> savedVs;
  context->VSGetShader(savedVs.put(), nullptr, nullptr);
  const auto restorePipelineState = [&]() {
    ID3D11RenderTargetView* rtvs[] = {savedRtv.get()};
    context->OMSetRenderTargets(1, rtvs, nullptr);
    context->OMSetBlendState(savedBlend.get(), savedBlendFactor, savedSampleMask);
    context->RSSetState(savedRasterizer.get());
    if (viewportCount > 0) {
      context->RSSetViewports(1, &savedViewport);
    }
    context->IASetPrimitiveTopology(savedTopology);
    context->VSSetShader(savedVs.get(), nullptr, 0);
  };

  // Geometry + per-line colors come from the shared layout resolver, so this
  // DirectWrite raster and the portable CPU bitmap-font tile
  // (rasterizeOverlayTileBgra) place identical content — preview and program
  // agree by construction. The band background is NOT painted here (it stays
  // a separate quad so its alpha animation matches the CPU mirror).
  const auto layout = computeOverlayTileLayout(overlay, widthPx, heightPx);

  d2dTarget->BeginDraw();
  d2dTarget->Clear(D2D1::ColorF(0.f, 0.f, 0.f, 0.f));

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
    if (auto image = decodeOverlayImage(d2dTarget.get(), overlay.imageUri)) {
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
        d2dColorFromArgb(line.colorArgb));
  }

  const HRESULT endDrawResult = d2dTarget->EndDraw();
  restorePipelineState();
  if (FAILED(endDrawResult)) {
    return nullptr;
  }
  if (FAILED(device->CreateShaderResourceView(entry.texture.get(), nullptr, entry.view.put()))) {
    return nullptr;
  }
  entry.lastUsed = overlayRasterClock_;

  // Bounded cache: evict the least-recently-used raster beyond 8 entries
  // (program + preview + multiview lower-third/caption variants all fit).
  while (overlayTextTextures_.size() >= 8) {
    auto oldest = overlayTextTextures_.begin();
    for (auto it = overlayTextTextures_.begin(); it != overlayTextTextures_.end(); ++it) {
      if (it->second.lastUsed < oldest->second.lastUsed) {
        oldest = it;
      }
    }
    overlayTextTextures_.erase(oldest);
  }
  const auto inserted = overlayTextTextures_.emplace(signature, std::move(entry));
  return inserted.first->second.view.get();
}

}  // namespace corevideo::modules

#endif  // !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_D3D11
