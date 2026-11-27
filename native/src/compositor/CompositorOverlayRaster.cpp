#include "compositor/CompositorOverlayRaster.h"

// The overlay raster is intentionally a dev-machine adapter. COREVIDEO_STUB
// builds compile this to nothing; the same three gates as D3D11CompositorAdapter
// must all be explicit: non-stub, dev adapters, and D3D11.
#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_D3D11

#include <dwrite_1.h>
#include <dxgi.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string_view>

#include "compositor/CompositorFaultInjection.h"
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

// Pinned text coverage (see ensureOverlayRasterFactories). Shared numbers with
// the Mac coverage curve in CoreTextOverlayRaster.mm.
constexpr float kOverlayTextGamma = 2.2f;
constexpr float kOverlayTextEnhancedContrast = 0.5f;
constexpr float kOverlayTextGrayscaleEnhancedContrast = 1.0f;

// WIC's PNG decoder never reports a broken pixel stream. For a bad zlib
// header, a malformed deflate block, a truncated stream or a wrong Adler-32 it
// returns S_OK from CopyPixels (so CreateBitmapFromWicBitmap succeeds too) with
// zeroed pixels; a short stream yields a partial picture (measured on Windows
// build 26300). A broken asset would prepare "ready" and air a blank. So the
// raster verifies the PNG pixel stream itself before decoding it: the IDAT
// data must inflate completely, match its Adler-32, and carry every byte that
// IHDR implies. Non-PNG bytes are left to WIC. The cost is one inflate on the
// preparation worker (about 26 ms for a 1440p screenshot), never the render thread.
class PngPixelStream {
 public:
  static bool decodes(const std::vector<uint8_t>& b) {
    constexpr uint8_t signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (b.size() < 33 || !std::equal(std::begin(signature), std::end(signature), b.begin())) return true;
    const auto be32 = [&](size_t at) {
      return uint32_t(b[at]) << 24 | uint32_t(b[at + 1]) << 16 | uint32_t(b[at + 2]) << 8 | uint32_t(b[at + 3]);
    };
    const uint64_t width = be32(16), height = be32(20);
    const unsigned depth = b[24], colorType = b[25], interlace = b[28];
    const unsigned channels = colorType == 0 || colorType == 3 ? 1 : colorType == 2 ? 3 : colorType == 4 ? 2 : colorType == 6 ? 4 : 0;
    if (!channels || !depth) return true;  // not a header this check understands; WIC decides
    const auto rowBytes = [&](uint64_t pixels) { return (pixels * channels * depth + 7) / 8; };
    uint64_t expected = 0;
    if (!interlace) expected = height * (1 + rowBytes(width));
    else {
      constexpr int pass[7][4] = {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4}, {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}};
      for (const auto& p : pass) {
        const uint64_t w = width > uint64_t(p[0]) ? (width - p[0] + p[2] - 1) / p[2] : 0;
        const uint64_t h = height > uint64_t(p[1]) ? (height - p[1] + p[3] - 1) / p[3] : 0;
        if (w && h) expected += h * (1 + rowBytes(w));
      }
    }
    PngPixelStream check;
    for (size_t at = 8; at + 12 <= b.size();) {
      const size_t length = be32(at);
      if (length > b.size() - at - 12) break;
      if (std::equal(b.begin() + std::ptrdiff_t(at + 4), b.begin() + std::ptrdiff_t(at + 8), "IDAT")) check.spans_.push_back({b.data() + at + 8, length});
      if (std::equal(b.begin() + std::ptrdiff_t(at + 4), b.begin() + std::ptrdiff_t(at + 8), "IEND")) break;
      at += 12 + length;
    }
    // Bounds the work a hostile stream can demand; twice the image is far past
    // anything an encoder writes.
    check.limit_ = expected * 2 + 65536;
    return !check.spans_.empty() && check.inflate() && check.produced_ >= expected;
  }

 private:
  struct Huffman { uint16_t count[16]; uint16_t symbol[288]; };
  std::vector<std::pair<const uint8_t*, size_t>> spans_;
  size_t span_ = 0, at_ = 0;
  uint32_t bitBuffer_ = 0;
  int bitCount_ = 0;
  bool failed_ = false;
  uint64_t produced_ = 0, limit_ = 0;
  uint32_t adlerA_ = 1, adlerB_ = 0, adlerPending_ = 0;
  std::vector<uint8_t> window_ = std::vector<uint8_t>(32768);

  uint32_t byte() {
    while (span_ < spans_.size() && at_ == spans_[span_].second) { ++span_; at_ = 0; }
    if (span_ == spans_.size()) { failed_ = true; return 0; }
    return spans_[span_].first[at_++];
  }
  uint32_t bits(int need) {
    while (bitCount_ < need && !failed_) { bitBuffer_ |= byte() << bitCount_; bitCount_ += 8; }
    if (failed_) return 0;
    const uint32_t value = bitBuffer_ & ((1u << need) - 1);
    bitBuffer_ >>= need; bitCount_ -= need;
    return value;
  }
  void emit(uint8_t value) {
    window_[produced_ & 32767] = value;
    if (++produced_ > limit_) failed_ = true;
    adlerA_ += value; adlerB_ += adlerA_;
    if (++adlerPending_ == 5552) { adlerA_ %= 65521; adlerB_ %= 65521; adlerPending_ = 0; }
  }
  static bool build(Huffman& h, const uint8_t* lengths, int n) {
    std::fill(std::begin(h.count), std::end(h.count), uint16_t(0));
    for (int i = 0; i < n; ++i) ++h.count[lengths[i]];
    int left = 1;
    for (int len = 1; len < 16; ++len) { left = left * 2 - h.count[len]; if (left < 0) return false; }
    uint16_t offsets[16] = {};
    for (int len = 1; len < 15; ++len) offsets[len + 1] = uint16_t(offsets[len] + h.count[len]);
    for (int i = 0; i < n; ++i) if (lengths[i]) h.symbol[offsets[lengths[i]]++] = uint16_t(i);
    return true;
  }
  int decode(const Huffman& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
      code |= int(bits(1));
      if (failed_) return -1;
      const int count = h.count[len];
      if (code - count < first) return h.symbol[index + (code - first)];
      index += count; first = (first + count) << 1; code <<= 1;
    }
    failed_ = true;
    return -1;
  }
  bool codes(const Huffman& lengthCode, const Huffman& distanceCode) {
    static constexpr uint16_t lengthBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static constexpr uint8_t lengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static constexpr uint16_t distanceBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static constexpr uint8_t distanceExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    while (!failed_) {
      int symbol = decode(lengthCode);
      if (symbol < 0) return false;
      if (symbol < 256) { emit(uint8_t(symbol)); continue; }
      if (symbol == 256) return true;
      symbol -= 257;
      if (symbol >= 29) return false;
      const uint32_t length = lengthBase[symbol] + bits(lengthExtra[symbol]);
      const int distanceSymbol = decode(distanceCode);
      if (distanceSymbol < 0 || distanceSymbol >= 30) return false;
      const uint32_t distance = distanceBase[distanceSymbol] + bits(distanceExtra[distanceSymbol]);
      if (failed_ || distance > produced_) return false;
      for (uint32_t i = 0; i < length && !failed_; ++i) emit(window_[(produced_ - distance) & 32767]);
    }
    return false;
  }
  bool inflate() {
    const uint32_t cmf = byte(), flg = byte();
    if (failed_ || (cmf & 15) != 8 || (cmf >> 4) > 7 || (cmf * 256 + flg) % 31 || (flg & 0x20)) return false;
    Huffman lengthCode, distanceCode;
    for (bool last = false; !last;) {
      last = bits(1) != 0;
      const uint32_t type = bits(2);
      if (failed_) return false;
      if (type == 0) {
        bitBuffer_ = 0; bitCount_ = 0;
        uint32_t length = byte();  // the four bytes are read in order: | does not sequence its operands
        length |= byte() << 8;
        uint32_t complement = byte();
        complement |= byte() << 8;
        if (failed_ || length != (~complement & 0xffff)) return false;
        for (uint32_t i = 0; i < length && !failed_; ++i) emit(uint8_t(byte()));
        if (failed_) return false;
        continue;
      }
      uint8_t lengths[320] = {};
      int lengthCount = 288, distanceCount = 30;
      if (type == 1) {
        for (int i = 0; i < 144; ++i) lengths[i] = 8;
        for (int i = 144; i < 256; ++i) lengths[i] = 9;
        for (int i = 256; i < 280; ++i) lengths[i] = 7;
        for (int i = 280; i < 288; ++i) lengths[i] = 8;
        for (int i = 288; i < 318; ++i) lengths[i] = 5;
      } else if (type == 2) {
        lengthCount = int(bits(5)) + 257; distanceCount = int(bits(5)) + 1;
        const int codeCount = int(bits(4)) + 4;
        if (failed_ || lengthCount > 286 || distanceCount > 30) return false;
        static constexpr uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        uint8_t codeLengths[19] = {};
        for (int i = 0; i < codeCount; ++i) codeLengths[order[i]] = uint8_t(bits(3));
        Huffman codeCode;
        if (failed_ || !build(codeCode, codeLengths, 19)) return false;
        for (int i = 0; i < lengthCount + distanceCount;) {
          int symbol = decode(codeCode);
          if (symbol < 0) return false;
          if (symbol < 16) { lengths[i++] = uint8_t(symbol); continue; }
          uint8_t repeated = 0;
          int times = 0;
          if (symbol == 16) { if (!i) return false; repeated = lengths[i - 1]; times = 3 + int(bits(2)); }
          else if (symbol == 17) times = 3 + int(bits(3));
          else times = 11 + int(bits(7));
          if (failed_ || i + times > lengthCount + distanceCount) return false;
          while (times--) lengths[i++] = repeated;
        }
        if (!lengths[256]) return false;
        std::copy_backward(lengths + lengthCount, lengths + lengthCount + distanceCount, lengths + 288 + distanceCount);
        std::fill(lengths + lengthCount, lengths + 288, uint8_t(0));
      } else {
        return false;
      }
      if (!build(lengthCode, lengths, 288) || !build(distanceCode, lengths + 288, 30) || !codes(lengthCode, distanceCode)) return false;
    }
    bitBuffer_ = 0; bitCount_ = 0;
    uint32_t stored = 0;
    for (int i = 0; i < 4; ++i) stored = stored << 8 | byte();
    adlerA_ %= 65521; adlerB_ %= 65521;
    return !failed_ && stored == (adlerB_ << 16 | adlerA_);
  }
};

bool pngPixelStreamDecodes(const std::vector<uint8_t>& bytes) { return PngPixelStream::decodes(bytes); }

}  // namespace

bool CompositorOverlayRaster::ensureOverlayRasterFactories() {
  if (d2dFactory_ && dwriteFactory_ && textRenderingParams_ && wicFactory_) {
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
  // Pin the text rendering params. With none set, D2D uses the PC's ClearType
  // tuning (HKCU/Software/Microsoft/Avalon.Graphics/DISPLAYn), so the weight
  // of on-air text varied from one machine to the next. These are the values
  // CreateRenderingParams() returned on the machine that wrote the Windows
  // parity reference (contracts/graphics-raster-reference.json), so the look
  // stays the same. CoreTextOverlayRaster.mm matches this coverage on the Mac.
  // The target is a premultiplied transparent bitmap, so text is grayscale and
  // only gamma and the grayscale enhanced contrast affect coverage.
  if (!textRenderingParams_) {
    ComPtrLite<IDWriteFactory1> factory1;
    if (FAILED(dwriteFactory_->QueryInterface(__uuidof(IDWriteFactory1),
                                              reinterpret_cast<void**>(factory1.put()))) ||
        FAILED(factory1->CreateCustomRenderingParams(
            kOverlayTextGamma, kOverlayTextEnhancedContrast, kOverlayTextGrayscaleEnhancedContrast,
            1.0f, DWRITE_PIXEL_GEOMETRY_RGB, DWRITE_RENDERING_MODE_DEFAULT,
            // IDWriteRenderingParams1 derives singly from IDWriteRenderingParams.
            reinterpret_cast<IDWriteRenderingParams1**>(textRenderingParams_.put())))) {
      return false;
    }
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
    ID2D1RenderTarget* target, const ImageInput& image, bool referenceDpi) {
  ComPtrLite<ID2D1Bitmap> bitmap;
  IWICBitmapFrameDecode* frame = image.frame.get();
  if (!target || !frame || !image.pixelsDecode) return bitmap;
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
  const auto properties = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
  if (FAILED(target->CreateBitmapFromWicBitmap(converter.get(), referenceDpi ? &properties : nullptr, bitmap.put()))) {
    bitmap = {};
  }
  return bitmap;
}

CompositorOverlayRaster::ImageInput CompositorOverlayRaster::openPackagedImage(const graphics::GraphicsAsset& asset) {
  if (!wicFactory_ || !asset.encoded || asset.encoded->empty() || asset.encoded->size() > 32 * 1024 * 1024) return {};
  ComPtrLite<IWICStream> stream;
  ComPtrLite<IWICBitmapDecoder> decoder;
  ComPtrLite<IWICBitmapFrameDecode> frame;
  if (FAILED(wicFactory_->CreateStream(stream.put())) ||
      FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(asset.encoded->data()), DWORD(asset.encoded->size()))) ||
      FAILED(wicFactory_->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.put())) ||
      FAILED(decoder->GetFrame(0, frame.put()))) return {};
  UINT width = 0, height = 0;
  if (FAILED(frame->GetSize(&width, &height)) || width != UINT(asset.width) || height != UINT(asset.height)) return {};
  // Decoder, format-conversion and D2D destination allowance. It is charged
  // before decoding any full-resolution pixels, and shared across workers.
  return {std::move(frame), uint64_t(width) * height * 12, pngPixelStreamDecodes(*asset.encoded)};
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
               wicFactory_ = {}; textRenderingParams_ = {}; dwriteFactory_ = {}; d2dFactory_ = {};
             }, 64) {}
CompositorOverlayRaster::~CompositorOverlayRaster() = default;

namespace {
// DirectWrite retains shaping, alignment, fallback and trimming. Capture its
// positioned glyph runs as vector geometry only for text with effects.
class TextEffectGeometry final : public IDWriteTextRenderer {
 public:
  explicit TextEffectGeometry(ID2D1Factory& factory) : factory_(factory) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** result) override {
    if (!result) return E_POINTER;
    *result = nullptr;
    if (id == __uuidof(IUnknown) || id == __uuidof(IDWriteTextRenderer))
      *result = static_cast<IDWriteTextRenderer*>(this);
    else if (id == __uuidof(IDWritePixelSnapping))
      *result = static_cast<IDWritePixelSnapping*>(this);
    else return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override { return --references_; }
  HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* disabled) override {
    *disabled = TRUE;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* matrix) override {
    *matrix = {1, 0, 0, 1, 0, 0};
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* pixels) override {
    *pixels = 1;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE,
      const DWRITE_GLYPH_RUN* run, const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown*) override {
    ComPtrLite<ID2D1PathGeometry> path;
    ComPtrLite<ID2D1GeometrySink> sink;
    HRESULT result = factory_.CreatePathGeometry(path.put());
    if (SUCCEEDED(result)) result = path->Open(sink.put());
    if (FAILED(result)) return result;
    result = run->fontFace->GetGlyphRunOutline(run->fontEmSize, run->glyphIndices,
        run->glyphAdvances, run->glyphOffsets, run->glyphCount, run->isSideways, (run->bidiLevel & 1) != 0, sink.get());
    const HRESULT closed = sink->Close();
    if (FAILED(result)) return result;
    if (FAILED(closed)) return closed;
    ComPtrLite<ID2D1TransformedGeometry> positioned;
    const auto transform = D2D1::Matrix3x2F::Translation(x, y);
    result = factory_.CreateTransformedGeometry(path.get(), transform, positioned.put());
    if (SUCCEEDED(result)) runs_.push_back(std::move(positioned));
    return result;
  }
  HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DrawInlineObject(void* context, FLOAT x, FLOAT y,
      IDWriteInlineObject* object, BOOL sideways, BOOL rtl, IUnknown* effect) override {
    return object->Draw(context, this, x, y, sideways, rtl, effect);
  }

  HRESULT group(ID2D1GeometryGroup** result) const {
    std::vector<ID2D1Geometry*> geometries;
    geometries.reserve(runs_.size());
    for (const auto& run : runs_) geometries.push_back(run.get());
    return factory_.CreateGeometryGroup(D2D1_FILL_MODE_WINDING, geometries.data(),
        static_cast<UINT32>(geometries.size()), result);
  }
  bool empty() const { return runs_.empty(); }

 private:
  ID2D1Factory& factory_;
  ULONG references_ = 1;
  std::vector<ComPtrLite<ID2D1TransformedGeometry>> runs_;
};

bool drawTextEffects(ID2D1Factory& factory, ID2D1RenderTarget& target,
    IDWriteTextLayout& text, const graphics::GraphicsPrimitive& primitive) {
  TextEffectGeometry glyphs(factory);
  if (FAILED(text.Draw(nullptr, &glyphs, float(primitive.box.x), float(primitive.box.y)))) return false;
  if (glyphs.empty()) return true;
  ComPtrLite<ID2D1GeometryGroup> geometry;
  if (FAILED(glyphs.group(geometry.put()))) return false;
  if (primitive.shadow) {
    const auto& shadow = *primitive.shadow;
    ComPtrLite<ID2D1TransformedGeometry> shifted;
    const auto transform = D2D1::Matrix3x2F::Translation(float(shadow.x), float(shadow.y));
    if (FAILED(factory.CreateTransformedGeometry(geometry.get(), transform, shifted.put()))) return false;
    ComPtrLite<ID2D1SolidColorBrush> brush;
    if (FAILED(target.CreateSolidColorBrush(d2dColorFromArgb(shadow.color), brush.put()))) return false;
    // A bounded soft edge over the positioned glyph geometry works on the
    // software WIC render target as well as during atlas preparation.
    if (shadow.blur > 0) {
      constexpr int rings = 8;
      for (int ring = rings; ring > 0; --ring) {
        const float fraction = float(ring) / rings;
        brush->SetOpacity(float(primitive.opacity) * (1.f - fraction * .8f) / rings);
        target.DrawGeometry(shifted.get(), brush.get(), float(2 * shadow.blur * fraction));
      }
    }
    brush->SetOpacity(float(primitive.opacity));
    target.FillGeometry(shifted.get(), brush.get());
  }
  if (primitive.outline && primitive.outline->width > 0) {
    ComPtrLite<ID2D1SolidColorBrush> brush;
    if (FAILED(target.CreateSolidColorBrush(d2dColorFromArgb(primitive.outline->color), brush.put()))) return false;
    brush->SetOpacity(float(primitive.opacity));
    target.DrawGeometry(geometry.get(), brush.get(), float(2 * primitive.outline->width));
  }
  return true;
}
}  // namespace

bool CompositorOverlayRaster::drawTemplate(ID2D1RenderTarget* target,
    const graphics::GraphicsDrawing& drawing, float scale, const std::map<std::string, ImageInput>& images,
    size_t first, size_t count, float offsetX, float offsetY) {
  ComPtrLite<IDWriteFontCollection> fonts;
  if (FAILED(dwriteFactory_->GetSystemFontCollection(fonts.put())) || !fonts) return false;
  target->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) *
      D2D1::Matrix3x2F::Translation(offsetX, offsetY));
  bool complete = true;
  std::map<std::string, ComPtrLite<ID2D1Bitmap>> bitmaps;
  for (size_t i = first; i < first + count; ++i) {
    const auto& p = drawing.primitives[i];
    const auto& b = p.box; const auto& c = p.clip;
    target->PushAxisAlignedClip(D2D1::RectF(float(c.x), float(c.y), float(c.x + c.width), float(c.y + c.height)), D2D1_ANTIALIAS_MODE_ALIASED);
    if (p.type == "image") {
      if (!p.image) complete = false;
      else {
        auto bitmap = bitmaps.find(p.image->id);
        if (bitmap == bitmaps.end()) {
          const auto source = images.find(p.image->id);
          if (source != images.end()) {
            auto decoded = decodeOverlayImage(target, source->second, true);
            if (decoded) bitmap = bitmaps.emplace(p.image->id, std::move(decoded)).first;
          }
        }
        if (bitmap == bitmaps.end()) { complete = false; workerWarning_ = "Graphics image could not be decoded: " + p.image->id; }
        else {
          const double sx = b.width / p.image->width, sy = b.height / p.image->height;
          const auto factor = p.fit == "cover" ? std::max(sx, sy) : std::min(sx, sy);
          const auto width = p.image->width * factor, imageHeight = p.image->height * factor;
          const auto x = b.x + (p.imageAlign == "right" ? b.width - width : p.imageAlign == "center" ? (b.width - width) / 2 : 0);
          const auto y = b.y + (p.imageVerticalAlign == "bottom" ? b.height - imageHeight : p.imageVerticalAlign == "center" ? (b.height - imageHeight) / 2 : 0);
          target->DrawBitmap(bitmap->second.get(), D2D1::RectF(float(x), float(y), float(x + width), float(y + imageHeight)), float(p.opacity), D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
      }
      target->PopAxisAlignedClip(); continue;
    }
    ComPtrLite<ID2D1SolidColorBrush> stroke;
    if (p.strokeWidth > 0) {
      if (SUCCEEDED(target->CreateSolidColorBrush(d2dColorFromArgb(p.strokeColor), stroke.put())))
        stroke->SetOpacity(float(p.opacity));
      else complete = false;
    }
    const auto inset = p.strokeWidth / 2;
    if (p.type == "line") {
      if (stroke) target->DrawLine(
          D2D1::Point2F(float(b.x + inset), float(p.lineRises ? b.y + b.height - inset : b.y + inset)),
          D2D1::Point2F(float(b.x + b.width - inset), float(p.lineRises ? b.y + inset : b.y + b.height - inset)),
          stroke.get(), float(p.strokeWidth));
      target->PopAxisAlignedClip();
      continue;
    }
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
      if (p.type == "rect") {
        target->FillRoundedRectangle(D2D1::RoundedRect(
            D2D1::RectF(float(b.x), float(b.y), float(b.x + b.width), float(b.y + b.height)), float(p.cornerRadius), float(p.cornerRadius)), brush);
        if (stroke) {
          const auto radius = float(std::max(0., p.cornerRadius - inset));
          target->DrawRoundedRectangle(D2D1::RoundedRect(
              D2D1::RectF(float(b.x + inset), float(b.y + inset), float(b.x + b.width - inset), float(b.y + b.height - inset)),
              radius, radius), stroke.get(), float(p.strokeWidth));
        }
      } else if (p.type == "ellipse") {
        const auto centre = D2D1::Point2F(float(b.x + b.width / 2), float(b.y + b.height / 2));
        target->FillEllipse(D2D1::Ellipse(centre, float(b.width / 2), float(b.height / 2)), brush);
        if (stroke) target->DrawEllipse(D2D1::Ellipse(centre, float(b.width / 2 - inset), float(b.height / 2 - inset)),
            stroke.get(), float(p.strokeWidth));
      }
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
          if (textReady && (p.outline || p.shadow))
            textReady = drawTextEffects(*d2dFactory_.get(), *target, *layout.get(), p);
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
    // Bounds the atlas for layered motion AND crawls (shared with Metal).
    // The worker charges the actual atlas dimensions after layout.
    const uint64_t admissionBytes = graphics::overlayAdmissionBytes(overlay.graphicsTemplate.get(), width, height);
    activeLease_ = cache_.findOrRequest(key, std::move(request),
        admissionBytes + (overlay.imageUri.empty() ? 0 : 128ull * 1024 * 1024), preparationSlot);
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
  if(!activeLease_ && overlay.hasGraphicsContentHash) {
    const auto capacity=graphics::GraphicsResourceBudget::process().counters();
    if(capacity.residentResources>=capacity.residentLimit) lastWarning_="Graphics preparation pending at the shared 16-revision capacity limit.";
  }
  pendingThisFrame_ = pendingThisFrame_ || (retainingScale && !failedScale) || !activeLease_;
  failedThisFrame_ = failedThisFrame_ || failedScale || (activeLease_ && !activeLease_->asset);
  return activeLease_ && activeLease_->asset ? activeLease_->asset->view.get() : nullptr;
}

std::shared_ptr<const graphics::GraphicsRunLayout> CompositorOverlayRaster::activeRuns() const {
  return activeLease_ && activeLease_->asset ? activeLease_->asset->runs : nullptr;
}

std::shared_ptr<const graphics::GraphicsPreparedResource> CompositorOverlayRaster::preparedResource() {
  if (!activeLease_ || !activeLease_->asset || std::string_view(lastState_) != "ready") return {};
  if (activeResource_) return activeResource_;
  auto resource = std::make_shared<graphics::GraphicsPreparedResource>();
  resource->backend = graphics::GraphicsPreparedResource::Backend::D3D11;
  resource->deviceIdentity = activeDevice_; resource->signature = activeSignature_;
  resource->width = activeWidth_; resource->height = activeHeight_; resource->native = activeLease_;
  resource->contentHash = activeContentHash_; resource->hasContentHash = activeHasContentHash_;
  if (const auto runs = activeRuns(); runs && runs->drawing->crawl) {
    const auto& crawl = *runs->drawing->crawl;
    resource->crawl = graphics::GraphicsCrawlPlayback{crawl.speed, crawl.passWidth, crawl.loop};
  }
  activeResource_ = resource;
  return resource;
}

std::vector<uint8_t> CompositorOverlayRaster::thumbnailPng(const CompositorOverlayContent& overlay,int width,int height,std::string& warning) {
  if(width<1 || height<1 || width>320 || height>180) {warning="Invalid thumbnail dimensions.";return {};}
  Request request;request.overlay=overlay;request.width=width;request.height=height;request.strictResources=true;request.thumbnail=true;
  const auto result=prepareRaster(request);warning=result.warning;
  return result.asset ? result.asset->thumbnail : std::vector<uint8_t>{};
}
CompositorOverlayRaster::Cache::Result CompositorOverlayRaster::prepareRaster(const Request& request) {
  compositor::invokeOverlayRasterPrepareHook();  // test seam; a no-op unless a test installs a hook
  thread_local core::ComApartmentLifetime apartment;
  const auto& overlay = request.overlay;
  const int widthPx = request.width, heightPx = request.height;
  auto* device = request.device.get();
  workerWarning_.clear();
  if (!apartment.initialized() || !ensureOverlayRasterFactories() || !wicFactory_) {
    return {{}, 0, "Native graphics preparation unavailable."};
  }
  graphics::GraphicsDrawing drawing;
  std::map<std::string, ImageInput> images;
  uint64_t packagedImageBytes = 0;
  if (overlay.graphicsTemplate) {
    std::string error;
    if (!overlay.graphicsTemplate->definition->resolve(overlay.graphicsTemplate->fields,
        overlay.graphicsTemplate->brand, drawing, error, overlay.graphicsTemplate->logo)) return {{}, 0, error};
    if (!drawing.warnings.empty()) workerWarning_ = drawing.warnings.front();
    for (const auto& primitive : drawing.primitives) if (primitive.image && !images.count(primitive.image->id)) {
      auto image = openPackagedImage(*primitive.image);
      if (!image.frame) return {{}, 0, "Graphics image header could not be decoded: " + primitive.image->id};
      packagedImageBytes += image.cpuBytes; images.emplace(primitive.image->id, std::move(image));
    }
  }
  auto layout = computeOverlayTileLayout(overlay, widthPx, heightPx);
  auto imageInput = layout.hasImage && !overlay.graphicsTemplate ? (overlay.graphicsImage ? openPackagedImage(*overlay.graphicsImage) : openOverlayImage(overlay.imageUri)) : ImageInput{};
  int textureWidth = widthPx, textureHeight = heightPx;
  const float scale = drawing.height > 0 ? float(heightPx / drawing.height) : 1.f;
  std::shared_ptr<graphics::GraphicsRunLayout> runs;
  if (drawing.crawl && request.thumbnail) {
    for (auto& primitive : drawing.primitives) {
      if (!primitive.crawlStart) continue;
      primitive.box.x += *primitive.crawlStart;
      primitive.clip = graphics::intersect(primitive.box, drawing.crawl->clip);
    }
  }
  if (overlay.graphicsTemplate && !request.thumbnail &&
      (drawing.crawl || (overlay.graphicsTemplate->definition->motionSummary().layered &&
      !overlay.graphicsTemplate->wholeGraphicMotion))) {
    runs = std::make_shared<graphics::GraphicsRunLayout>();
    runs->width = drawing.width;
    runs->height = drawing.height;
    runs->runs = graphics::planRuns(drawing);
    if (!graphics::packAtlas(runs->runs, scale, runs->slices, runs->atlasWidth, runs->atlasHeight))
      return {{}, 0, "Graphics motion needs more texture space than one graphic allows; give fewer layers their own motion."};
    textureWidth = std::max(1, runs->atlasWidth);
    textureHeight = std::max(1, runs->atlasHeight);
  }
  const auto textureBytes = uint64_t(textureWidth) * textureHeight * 4;
  auto& sharedBudget = graphics::GraphicsResourceBudget::process();
  if (textureBytes + imageInput.cpuBytes + packagedImageBytes > sharedBudget.counters().cpuLimit)
    return {{}, 0, "Graphics assets exceed the shared CPU preparation capacity."};
  std::string revisionKey;
  if(overlay.hasGraphicsContentHash) {static constexpr char hex[]="0123456789abcdef";revisionKey="graphics:";for(auto byte:overlay.graphicsContentHash) {revisionKey+=hex[byte>>4];revisionKey+=hex[byte&15];}}
  else revisionKey="legacy:"+std::to_string(overlayContentSignature(overlay,1920,1080));
  auto budget = sharedBudget.tryReserve(textureBytes + imageInput.cpuBytes + packagedImageBytes, textureBytes,revisionKey,overlay.hasGraphicsContentHash);
  if (!budget) return {{}, 0, "Graphics preparation is waiting for shared resource capacity.", true};
  auto entry = std::make_shared<OverlayRasterTex>();
  entry->budget = budget;
  ComPtrLite<IWICBitmap> bitmap;
  if (FAILED(wicFactory_->CreateBitmap(textureWidth, textureHeight, GUID_WICPixelFormat32bppPBGRA,
      WICBitmapCacheOnLoad, bitmap.put()))) return {{}, 0, "Graphics bitmap allocation failed."};
  const auto targetProperties = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_SOFTWARE,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
  ComPtrLite<ID2D1RenderTarget> d2dTarget;
  if (FAILED(d2dFactory_->CreateWicBitmapRenderTarget(bitmap.get(), &targetProperties, d2dTarget.put())))
    return {{}, 0, "Graphics raster target creation failed."};
  d2dTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
  d2dTarget->SetTextRenderingParams(textRenderingParams_.get());

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
    if (runs) {
      for (size_t i = 0; i < runs->runs.size(); ++i) {
        const auto& run = runs->runs[i];
        const auto& slice = runs->slices[i];
        if (slice.width <= 0 || slice.height <= 0) continue;
        resourcesComplete = drawTemplate(d2dTarget.get(), drawing, scale, images, run.first, run.count,
            float(slice.x - run.source.x * scale), float(slice.y - run.source.y * scale)) && resourcesComplete;
      }
      runs->drawing = std::make_shared<const graphics::GraphicsDrawing>(std::move(drawing));
      entry->runs = std::move(runs);
    } else {
      resourcesComplete = drawTemplate(d2dTarget.get(), drawing, scale, images, 0, drawing.primitives.size());
    }
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
    auto image=decodeOverlayImage(d2dTarget.get(), imageInput);
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
  if(request.thumbnail) {
    // Release the drawing target before WIC reads the completed bitmap. PNG
    // stores straight alpha; the native raster remains premultiplied internally.
    d2dTarget={};
    ComPtrLite<IStream> stream;
    ComPtrLite<IWICBitmapEncoder> encoder;
    ComPtrLite<IWICBitmapFrameEncode> frame;
    ComPtrLite<IWICFormatConverter> converter;
    WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;
    if(FAILED(CreateStreamOnHGlobal(nullptr,TRUE,stream.put())) ||
        FAILED(wicFactory_->CreateEncoder(GUID_ContainerFormatPng,nullptr,encoder.put())) ||
        FAILED(encoder->Initialize(stream.get(),WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(frame.put(),nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(widthPx,heightPx)) || FAILED(frame->SetPixelFormat(&format)) ||
        FAILED(wicFactory_->CreateFormatConverter(converter.put())) ||
        FAILED(converter->Initialize(bitmap.get(),format,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)) ||
        FAILED(frame->WriteSource(converter.get(),nullptr)) || FAILED(frame->Commit()) || FAILED(encoder->Commit()))
      return {{},0,"Native thumbnail PNG encoding failed."};
    STATSTG size{};if(FAILED(stream->Stat(&size,STATFLAG_NONAME)) || size.cbSize.QuadPart>256*1024 || !size.cbSize.QuadPart)
      return {{},0,"Native thumbnail exceeds its encoded limit."};
    LARGE_INTEGER start{};if(FAILED(stream->Seek(start,STREAM_SEEK_SET,nullptr))) return {{},0,"Native thumbnail stream unavailable."};
    entry->thumbnail.resize(size_t(size.cbSize.QuadPart));ULONG read=0;
    if(FAILED(stream->Read(entry->thumbnail.data(),ULONG(entry->thumbnail.size()),&read)) || read!=entry->thumbnail.size()) return {{},0,"Native thumbnail stream incomplete."};
    bitmap={};images.clear();imageInput.frame={};budget->finishPreparation();
    return {std::move(entry),0,workerWarning_};
  }
  WICRect lockRect{0, 0, textureWidth, textureHeight};
  ComPtrLite<IWICBitmapLock> pixels;
  UINT stride = 0, length = 0; BYTE* data = nullptr;
  if (FAILED(bitmap->Lock(&lockRect, WICBitmapLockRead, pixels.put())) ||
      FAILED(pixels->GetStride(&stride)) || FAILED(pixels->GetDataPointer(&length, &data)))
    return {{}, 0, "Graphics pixels unavailable."};
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = textureWidth; desc.Height = textureHeight; desc.MipLevels = desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_IMMUTABLE; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA initial{data, stride, 0};
  if (FAILED(device->CreateTexture2D(&desc, &initial, entry->texture.put())) ||
      FAILED(device->CreateShaderResourceView(entry->texture.get(), nullptr, entry->view.put())))
    return {{}, 0, "Graphics texture creation failed."};
  // Drop CPU owners before returning their shared admission charge. GPU views
  // retain their own charge through cache/device retirement.
  pixels = {}; d2dTarget = {}; bitmap = {}; images.clear(); imageInput.frame = {};
  budget->finishPreparation();
  return {std::move(entry), textureBytes, workerWarning_};
}

}  // namespace corevideo::modules

#endif  // !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_D3D11
