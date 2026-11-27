#include "compositor/CoreTextOverlayRaster.h"

// Same three gates as MetalCompositorAdapter.mm: non-stub, dev adapters, Metal.
#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_METAL

#import <CoreText/CoreText.h>
#import <ImageIO/ImageIO.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <string_view>

#include "compositor/CompositorFaultInjection.h"
#include "compositor/CoreTextThumbnail.h"
#include "graphics/GraphicsTextCoreText.h"
#include "graphics/GraphicsTemplate.h"
#include "modules/OverlayTileRaster.h"

namespace corevideo::modules {
namespace {

// Owns one Core Foundation reference (Create/Copy rule).
template <typename T>
class CfRef {
 public:
  CfRef() = default;
  explicit CfRef(T value) : value_(value) {}
  ~CfRef() { if (value_) CFRelease(value_); }
  CfRef(const CfRef&) = delete;
  CfRef& operator=(const CfRef&) = delete;
  CfRef(CfRef&& other) noexcept : value_(other.value_) { other.value_ = nullptr; }
  CfRef& operator=(CfRef&& other) noexcept {
    if (this != &other) { if (value_) CFRelease(value_); value_ = other.value_; other.value_ = nullptr; }
    return *this;
  }
  T get() const { return value_; }
  explicit operator bool() const { return value_ != nullptr; }
 private:
  T value_ = nullptr;
};

CfRef<CFStringRef> cfString(const std::string& text) {
  return CfRef<CFStringRef>(CFStringCreateWithBytes(kCFAllocatorDefault,
      reinterpret_cast<const UInt8*>(text.data()), CFIndex(text.size()), kCFStringEncodingUTF8, false));
}

// Font resolution and font creation live in graphics/GraphicsTextCoreText so
// GraphicsTemplate's measurement and this raster resolve identically (owner's
// substitution table, bundled Selawik, warn never fail for a table hit).
constexpr const char* kFallbackFontFamily = "Helvetica Neue";

CfRef<CTFontRef> createFont(const std::string& family, int weight, CGFloat size) {
  return CfRef<CTFontRef>(graphics::createGraphicsFont(family, weight, size));
}

CfRef<CGColorRef> colorFromArgb(uint32_t argb, CGColorSpaceRef space) {
  const CGFloat components[4] = {
      CGFloat((argb >> 16) & 0xff) / 255.0, CGFloat((argb >> 8) & 0xff) / 255.0,
      CGFloat(argb & 0xff) / 255.0, CGFloat((argb >> 24) & 0xff) / 255.0};
  return CfRef<CGColorRef>(CGColorCreate(space, components));
}

// Tile pixel space is top-down; Core Graphics is bottom-up. Rects convert per
// primitive so the text matrix is never mirrored (which would flip glyphs).
CGRect bottomUp(const OverlayTileRect& rect, int heightPx) {
  return CGRectMake(rect.x, CGFloat(heightPx) - rect.y - rect.height, rect.width, rect.height);
}

// One legacy text line (spec 3.4 legacy rule): em = line.fontSize when set,
// else 72% of the line box; single line, leading, vertically centred on
// ascent + descent (plan D3); truncated at the end with an ellipsis when it
// overflows the box width. Not clipped to its box (D2D DrawText, plan D4).
bool drawLegacyTextLine(CGContextRef context, CGColorSpaceRef space, const OverlayTileTextLine& line,
                        const std::string& family, bool emphasis, int heightPx) {
  if (line.text.empty() || line.rect.width <= 0.f || line.rect.height <= 0.f) return true;
  auto text = cfString(line.text);
  if (!text) {  // invalid UTF-8: lossy ASCII copy rather than nothing
    std::string ascii = line.text;
    for (auto& ch : ascii) if (static_cast<unsigned char>(ch) > 0x7f) ch = '?';
    text = cfString(ascii);
  }
  if (!text) return false;
  const CGFloat fontSize = line.fontSize > 0.f ? line.fontSize : std::max(4.f, line.rect.height * 0.72f);
  const auto font = createFont(family, emphasis ? 600 : 400, fontSize);
  if (!font) return false;
  const auto color = colorFromArgb(line.colorArgb, space);
  const void* keys[] = {kCTFontAttributeName, kCTForegroundColorAttributeName};
  const void* values[] = {font.get(), color.get()};
  CfRef<CFDictionaryRef> attributes(CFDictionaryCreate(kCFAllocatorDefault, keys, values, 2,
      &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
  CfRef<CFAttributedStringRef> attributed(CFAttributedStringCreate(kCFAllocatorDefault, text.get(), attributes.get()));
  if (!attributed) return false;
  CfRef<CTLineRef> ctLine(CTLineCreateWithAttributedString(attributed.get()));
  if (!ctLine) return false;
  const double width = CTLineGetTypographicBounds(ctLine.get(), nullptr, nullptr, nullptr);
  if (width > line.rect.width) {
    CfRef<CFAttributedStringRef> ellipsis(CFAttributedStringCreate(kCFAllocatorDefault, CFSTR("…"), attributes.get()));
    CfRef<CTLineRef> token(CTLineCreateWithAttributedString(ellipsis.get()));
    CfRef<CTLineRef> truncated(CTLineCreateTruncatedLine(ctLine.get(), line.rect.width, kCTLineTruncationEnd, token.get()));
    if (!truncated) return true;  // not even the ellipsis fits: nothing to draw
    ctLine = std::move(truncated);
  }
  const CGRect box = bottomUp(line.rect, heightPx);
  const CGFloat ascent = CTFontGetAscent(font.get());
  const CGFloat descent = CTFontGetDescent(font.get());
  CGContextSetTextMatrix(context, CGAffineTransformIdentity);
  CGContextSetTextPosition(context, box.origin.x, box.origin.y + (box.size.height - (ascent + descent)) / 2 + descent);
  CTLineDraw(ctLine.get(), context);
  return true;
}

struct ImageInput {
  CfRef<CGImageSourceRef> source;
  uint64_t cpuBytes = 0;
};

// Legacy file image: local only (never a fetch), at most 4096 square / 16 MP,
// probed from the header before any pixel is decoded (Windows rule).
ImageInput openFileImage(const std::string& imageUri) {
  if (imageUri.empty() || imageUri.rfind("http", 0) == 0) return {};
  std::string path = imageUri;
  if (path.rfind("file://", 0) == 0) path = path.substr(7);
  CfRef<CFURLRef> url(CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
      reinterpret_cast<const UInt8*>(path.c_str()), CFIndex(path.size()), false));
  if (!url) return {};
  CfRef<CGImageSourceRef> source(CGImageSourceCreateWithURL(url.get(), nullptr));
  if (!source || CGImageSourceGetCount(source.get()) < 1) return {};
  CfRef<CFDictionaryRef> properties(CGImageSourceCopyPropertiesAtIndex(source.get(), 0, nullptr));
  if (!properties) return {};
  int width = 0, height = 0;
  const auto w = static_cast<CFNumberRef>(CFDictionaryGetValue(properties.get(), kCGImagePropertyPixelWidth));
  const auto h = static_cast<CFNumberRef>(CFDictionaryGetValue(properties.get(), kCGImagePropertyPixelHeight));
  if (!w || !h || !CFNumberGetValue(w, kCFNumberIntType, &width) || !CFNumberGetValue(h, kCFNumberIntType, &height)) return {};
  if (width <= 0 || height <= 0 || width > 4096 || height > 4096 || uint64_t(width) * height > 16ull * 1024 * 1024) return {};
  return {std::move(source), uint64_t(width) * height * 8};
}

// Package image: at most 32 MiB encoded, decoding to exactly the declared size.
ImageInput openPackagedImage(const graphics::GraphicsAsset& asset) {
  if (!asset.encoded || asset.encoded->empty() || asset.encoded->size() > 32 * 1024 * 1024) return {};
  CfRef<CFDataRef> data(CFDataCreateWithBytesNoCopy(kCFAllocatorDefault, asset.encoded->data(),
      CFIndex(asset.encoded->size()), kCFAllocatorNull));
  CfRef<CGImageSourceRef> source(CGImageSourceCreateWithData(data.get(), nullptr));
  if (!source || CGImageSourceGetCount(source.get()) < 1) return {};
  CfRef<CFDictionaryRef> properties(CGImageSourceCopyPropertiesAtIndex(source.get(), 0, nullptr));
  if (!properties) return {};
  int width = 0, height = 0;
  const auto w = static_cast<CFNumberRef>(CFDictionaryGetValue(properties.get(), kCGImagePropertyPixelWidth));
  const auto h = static_cast<CFNumberRef>(CFDictionaryGetValue(properties.get(), kCGImagePropertyPixelHeight));
  if (!w || !h || !CFNumberGetValue(w, kCFNumberIntType, &width) || !CFNumberGetValue(h, kCFNumberIntType, &height) ||
      width != asset.width || height != asset.height) return {};
  // Keeps the bytes the no-copy CFData points at alive for the decode.
  return {std::move(source), uint64_t(width) * height * 12};
}

// Decodes the image NOW and returns it backed by its decoded bytes, or null
// when the pixels do not decode. ImageIO defers the decode: for a corrupt
// stream (a bad zlib/IDAT payload) CGImageSourceCreateImageAtIndex still
// returns an image, which then draws nothing, so a broken asset would prepare
// "ready" and air a blank. Copying the provider's bytes forces the decode and
// surfaces the failure on the worker, as WIC's CreateBitmapFromWicBitmap does
// on Windows. The copy is the decoded allowance the budget already charges.
CfRef<CGImageRef> decodeImageNow(CGImageSourceRef source) {
  if (!source) return {};
  CfRef<CGImageRef> lazy(CGImageSourceCreateImageAtIndex(source, 0, nullptr));
  if (!lazy) return {};
  CGDataProviderRef provider = CGImageGetDataProvider(lazy.get());
  CfRef<CFDataRef> pixels(provider ? CGDataProviderCopyData(provider) : nullptr);
  const size_t rowBytes = CGImageGetBytesPerRow(lazy.get()), height = CGImageGetHeight(lazy.get());
  if (!pixels || rowBytes == 0 || size_t(CFDataGetLength(pixels.get())) < rowBytes * height) return {};
  CfRef<CGDataProviderRef> decoded(CGDataProviderCreateWithCFData(pixels.get()));
  if (!decoded) return {};
  return CfRef<CGImageRef>(CGImageCreate(CGImageGetWidth(lazy.get()), height, CGImageGetBitsPerComponent(lazy.get()),
      CGImageGetBitsPerPixel(lazy.get()), rowBytes, CGImageGetColorSpace(lazy.get()), CGImageGetBitmapInfo(lazy.get()),
      decoded.get(), CGImageGetDecode(lazy.get()), CGImageGetShouldInterpolate(lazy.get()),
      CGImageGetRenderingIntent(lazy.get())));
}


// ── Template raster (G2): the Core Graphics port of CompositorOverlayRaster's
// drawTemplate / drawTextEffects. Coordinates are template units in a TOP-DOWN
// CTM (point * scale + offset, the D2D transform), so every primitive is placed
// exactly as on Windows; text uses a (1,-1) text matrix to stay upright.

using ImageInputs = std::map<std::string, ImageInput>;

CGRect cgRect(const graphics::GraphicsBox& box) { return CGRectMake(box.x, box.y, box.width, box.height); }

void setFillArgb(CGContextRef cg, CGColorSpaceRef space, uint32_t argb) {
  const auto color = colorFromArgb(argb, space);
  CGContextSetFillColorWithColor(cg, color.get());
}
void setStrokeArgb(CGContextRef cg, CGColorSpaceRef space, uint32_t argb) {
  const auto color = colorFromArgb(argb, space);
  CGContextSetStrokeColorWithColor(cg, color.get());
}

CGPathRef createRoundedPath(CGRect rect, CGFloat radius) {
  if (rect.size.width <= 0 || rect.size.height <= 0) return nullptr;
  // Core Graphics asserts on radii over half a side; D2D clamps (spec 5.7).
  radius = std::clamp<CGFloat>(radius, 0, std::min(rect.size.width, rect.size.height) * 0.5);
  return radius > 0 ? CGPathCreateWithRoundedRect(rect, radius, radius, nullptr) : CGPathCreateWithRect(rect, nullptr);
}

// The positioned outlines of every glyph in `lines`, for outline and shadow
// (spec 5.6: CTFontCreatePathForGlyph at each run's glyph positions, unioned).
void appendGlyphOutlines(CGMutablePathRef path, CTLineRef line, CGFloat x, CGFloat baseline) {
  CFArrayRef runs = CTLineGetGlyphRuns(line);
  for (CFIndex r = 0; r < CFArrayGetCount(runs); ++r) {
    CTRunRef run = static_cast<CTRunRef>(CFArrayGetValueAtIndex(runs, r));
    CTFontRef runFont = static_cast<CTFontRef>(CFDictionaryGetValue(CTRunGetAttributes(run), kCTFontAttributeName));
    const CFIndex count = CTRunGetGlyphCount(run);
    if (!runFont || count <= 0) continue;
    std::vector<CGGlyph> glyphs(static_cast<size_t>(count), CGGlyph{});
    std::vector<CGPoint> positions(static_cast<size_t>(count), CGPointZero);
    CTRunGetGlyphs(run, CFRangeMake(0, 0), glyphs.data());
    CTRunGetPositions(run, CFRangeMake(0, 0), positions.data());
    for (CFIndex g = 0; g < count; ++g) {
      CfRef<CGPathRef> glyph(CTFontCreatePathForGlyph(runFont, glyphs[size_t(g)], nullptr));
      if (!glyph) continue;
      const CGAffineTransform place = CGAffineTransformMake(1, 0, 0, -1, x + positions[size_t(g)].x,
                                                            baseline - positions[size_t(g)].y);
      CGPathAddPath(path, &place, glyph.get());
    }
  }
}

// Grayscale enhanced contrast of the Windows text raster: the value pinned as
// kOverlayTextGrayscaleEnhancedContrast in CompositorOverlayRaster.cpp. Keep
// the two equal; the G4 parity test judges the pair.
constexpr double kWindowsTextGrayscaleContrast = 1.0;

// Coverage a in [0,1] -> a * (k + 1) / (a * k + 1), the contrast-enhancement
// form DirectWrite applies to grayscale glyph coverage. Fitted check against
// the G4 reference (2026-11-27): with k = 1 the text-only fixtures' summed
// alpha moves from 8-10% under Windows to 2-3.4% under.
const std::array<uint8_t, 256>& windowsTextCoverageCurve() {
  static const std::array<uint8_t, 256> curve = [] {
    std::array<uint8_t, 256> out{};
    constexpr double k = kWindowsTextGrayscaleContrast;
    for (int i = 0; i < 256; ++i) {
      const double a = i / 255.0;
      out[size_t(i)] = uint8_t(std::lround(255.0 * a * (k + 1) / (a * k + 1)));
    }
    return out;
  }();
  return curve;
}

bool drawTemplateText(CGContextRef cg, CGColorSpaceRef space, const graphics::GraphicsPrimitive& p,
                      std::string& warning) {
  const auto resolved = graphics::resolveGraphicsFontFamily(p.font);
  if (!resolved.available) {
    warning = "Requested graphics font is unavailable: " + p.font;
    return false;
  }
  if (resolved.substituted && warning.empty())
    warning = graphics::graphicsFontSubstitutionWarning(p.font, resolved.family);
  CfRef<CTFontRef> font(graphics::createGraphicsFont(resolved.family, resolved.weight ? resolved.weight : p.fontWeight,
                                                     p.fontSize));
  if (!font) return false;
  const auto& b = p.box;
  const double lineHeight = p.fontSize * p.lineHeight;
  const double layoutHeight = std::min(b.height, lineHeight * p.maxLines);
  const bool wrap = p.overflow == "wrap-then-ellipsis";
  const int visible = wrap ? std::max(1, int(std::floor(layoutHeight / lineHeight + 1e-9))) : 1;
  const auto color = colorFromArgb(p.color, space);
  graphics::GraphicsTextLayout text(p.text, font.get(), color.get(), b.width, wrap, visible, b.width);
  const double flush = p.align == "center" ? 0.5 : p.align == "right" ? 1.0 : 0.0;
  auto lineX = [&](const graphics::GraphicsTextLine& line) {
    return b.x + flush * (b.width - (line.width - line.trailingWhitespace));
  };
  // Uniform line spacing: baseline of line n is n * lineHeight + 0.8 * fontSize
  // below the box top. Font ascent/descent never move it (spec 5.6).
  auto baseline = [&](size_t n) { return b.y + double(n) * lineHeight + 0.8 * p.fontSize; };

  if (p.outline || p.shadow) {
    CfRef<CGMutablePathRef> glyphs(CGPathCreateMutable());
    for (size_t n = 0; n < text.lines().size(); ++n)
      appendGlyphOutlines(glyphs.get(), text.lines()[n].line, lineX(text.lines()[n]), baseline(n));
    if (p.shadow) {
      const auto& shadow = *p.shadow;
      const CGAffineTransform offset = CGAffineTransformMakeTranslation(shadow.x, shadow.y);
      CfRef<CGPathRef> shifted(CGPathCreateCopyByTransformingPath(glyphs.get(), &offset));
      setStrokeArgb(cg, space, shadow.color);
      setFillArgb(cg, space, shadow.color);
      if (shadow.blur > 0) {
        constexpr int rings = 8;
        for (int ring = rings; ring > 0; --ring) {
          const double fraction = double(ring) / rings;
          CGContextSetAlpha(cg, CGFloat(p.opacity * (1.0 - fraction * 0.8) / rings));
          CGContextSetLineWidth(cg, CGFloat(2 * shadow.blur * fraction));
          CGContextAddPath(cg, shifted.get());
          CGContextStrokePath(cg);
        }
      }
      CGContextSetAlpha(cg, CGFloat(p.opacity));
      CGContextAddPath(cg, shifted.get());
      CGContextFillPath(cg);
    }
    if (p.outline && p.outline->width > 0) {
      setStrokeArgb(cg, space, p.outline->color);
      CGContextSetAlpha(cg, CGFloat(p.opacity));
      CGContextSetLineWidth(cg, CGFloat(2 * p.outline->width));
      CGContextAddPath(cg, glyphs.get());
      CGContextStrokePath(cg);
    }
  }
  // Text coverage matches the Windows raster (owner ruling 2026-11-27). D2D
  // draws grayscale glyphs with pinned params (CompositorOverlayRaster.cpp:
  // gamma 2.2, grayscale enhanced contrast 1.0), which lifts edge coverage
  // above Core Text's linear coverage. The lines are drawn unsmoothed into an
  // offscreen bitmap over the text clip, every pixel's coverage goes through
  // the same contrast curve (kWindowsTextGrayscaleContrast), and the result is
  // composited at the primitive's opacity. Font smoothing (the old small-text
  // dilation, plan default D9) is off: the curve replaces it at every size.
  const CGRect textClip = CGRectMake(b.x, b.y, b.width, layoutHeight);  // D2D1_DRAW_TEXT_OPTIONS_CLIP
  // D2D's DrawTextLayout (no D2D1_DRAW_TEXT_OPTIONS_NO_SNAP) rounds each
  // line's baseline to the nearest whole device pixel; the outline and shadow
  // above come from a custom renderer with snapping disabled, so they stay
  // unsnapped on both rasters. Snap in device space (the CTM carries
  // placement and scale) and only when the CTM is axis-aligned, as D2D does.
  const CGAffineTransform userToDevice = CGContextGetCTM(cg);
  const double deviceHeight = double(CGBitmapContextGetHeight(cg));
  auto snappedBaseline = [&](double x, double y) {
    if (userToDevice.b != 0 || userToDevice.c != 0 || userToDevice.d == 0) return y;
    CGPoint device = CGPointApplyAffineTransform(CGPointMake(CGFloat(x), CGFloat(y)), userToDevice);
    // Round in top-down rows, as D2D does, whichever way the CTM flips y.
    device.y = userToDevice.d < 0 ? CGFloat(deviceHeight - std::floor(deviceHeight - device.y + 0.5))
                                  : CGFloat(std::floor(device.y + 0.5));
    return double(CGPointApplyAffineTransform(device, CGAffineTransformInvert(userToDevice)).y);
  };
  auto drawLines = [&](CGContextRef target) {
    CGContextSetTextMatrix(target, CGAffineTransformMakeScale(1, -1));
    CGContextSetAllowsAntialiasing(target, true);
    CGContextSetShouldAntialias(target, true);
    CGContextSetAllowsFontSmoothing(target, false);
    CGContextSetShouldSmoothFonts(target, false);
    CGContextSetAllowsFontSubpixelPositioning(target, true);
    CGContextSetShouldSubpixelPositionFonts(target, true);
    for (size_t n = 0; n < text.lines().size(); ++n) {
      const double x = lineX(text.lines()[n]);
      CGContextSetTextPosition(target, CGFloat(x), CGFloat(snappedBaseline(x, baseline(n))));
      CTLineDraw(text.lines()[n].line, target);
    }
  };
  const size_t targetWidth = CGBitmapContextGetWidth(cg), targetHeight = CGBitmapContextGetHeight(cg);
  // The layer, the composite and this rect must all use the CTM. Do not use
  // CGContextConvertRectToDeviceSpace: on a CI run it returned the text clip
  // flipped vertically against the CTM, which cut text off at the clip edge.
  const CGAffineTransform ctm = CGContextGetCTM(cg);
  const CGRect device = CGRectIntersection(CGRectIntegral(CGRectApplyAffineTransform(textClip, ctm)),
                                           CGRectMake(0, 0, CGFloat(targetWidth), CGFloat(targetHeight)));
  if (CGRectIsNull(device) || CGRectIsEmpty(device)) return true;  // the text clip is off the bitmap
  const size_t width = size_t(device.size.width), height = size_t(device.size.height);
  CfRef<CGContextRef> layer(CGBitmapContextCreate(nullptr, width, height, 8, width * 4, space,
                                                  CGBitmapInfo(kCGImageAlphaPremultipliedFirst) |
                                                      kCGBitmapByteOrder32Little));
  if (!layer) {
    warning = "Graphics text layer allocation failed.";
    return false;
  }
  // Same user space as `cg`, shifted so the layer's origin is the device rect.
  CGContextTranslateCTM(layer.get(), -device.origin.x, -device.origin.y);
  CGContextConcatCTM(layer.get(), ctm);
  CGContextClipToRect(layer.get(), textClip);
  drawLines(layer.get());
  CGContextFlush(layer.get());
  auto* pixels = static_cast<uint8_t*>(CGBitmapContextGetData(layer.get()));
  const size_t stride = CGBitmapContextGetBytesPerRow(layer.get());
  const auto& curve = windowsTextCoverageCurve();
  for (size_t y = 0; y < height; ++y) {
    uint8_t* row = pixels + y * stride;
    for (size_t x = 0; x < width; ++x) {
      uint8_t* px = row + x * 4;  // B G R A, premultiplied
      const uint8_t a = px[3];
      if (a == 0 || a == 255) continue;
      const uint8_t lifted = curve[a];
      for (int c = 0; c < 3; ++c) px[c] = uint8_t(std::min<unsigned>(lifted, (unsigned(px[c]) * lifted + a / 2) / a));
      px[3] = lifted;
    }
  }
  CfRef<CGImageRef> image(CGBitmapContextCreateImage(layer.get()));
  if (!image) {
    warning = "Graphics text layer allocation failed.";
    return false;
  }
  CGContextSaveGState(cg);
  CGContextConcatCTM(cg, CGAffineTransformInvert(ctm));  // device pixels; the primitive clip stays
  CGContextSetInterpolationQuality(cg, kCGInterpolationNone);
  CGContextSetAlpha(cg, CGFloat(p.opacity));
  CGContextDrawImage(cg, device, image.get());
  CGContextRestoreGState(cg);
  return true;
}

// Draws primitives [first, first + count) of `drawing` at `scale`, translated
// by (offsetX, offsetY) pixels, into a context whose CTM is already top-down.
bool drawTemplate(CGContextRef cg, CGColorSpaceRef space, const graphics::GraphicsDrawing& drawing, double scale,
                  const ImageInputs& images, size_t first, size_t count, double offsetX, double offsetY,
                  std::string& warning) {
  CGContextSaveGState(cg);
  CGContextTranslateCTM(cg, CGFloat(offsetX), CGFloat(offsetY));
  CGContextScaleCTM(cg, CGFloat(scale), CGFloat(scale));
  bool complete = true;
  std::map<std::string, CfRef<CGImageRef>> bitmaps;
  for (size_t i = first; i < first + count && i < drawing.primitives.size(); ++i) {
    const auto& p = drawing.primitives[i];
    const auto& b = p.box;
    CGContextSaveGState(cg);
    // Axis-aligned, aliased clip (D2D PushAxisAlignedClip ALIASED).
    CGContextSetShouldAntialias(cg, false);
    CGContextClipToRect(cg, cgRect(p.clip));
    CGContextSetShouldAntialias(cg, true);
    if (p.type == "image") {
      if (!p.image) {
        complete = false;
      } else {
        auto bitmap = bitmaps.find(p.image->id);
        if (bitmap == bitmaps.end()) {
          const auto source = images.find(p.image->id);
          if (source != images.end() && source->second.source) {
            CfRef<CGImageRef> decoded(decodeImageNow(source->second.source.get()));
            if (decoded) {
              CfRef<CGImageRef> retagged(
                  CGColorSpaceGetModel(CGImageGetColorSpace(decoded.get())) == kCGColorSpaceModelRGB
                      ? CGImageCreateCopyWithColorSpace(decoded.get(), space) : nullptr);
              bitmap = bitmaps.emplace(p.image->id, retagged ? std::move(retagged) : std::move(decoded)).first;
            }
          }
        }
        if (bitmap == bitmaps.end()) {
          complete = false;
          warning = "Graphics image could not be decoded: " + p.image->id;
        } else {
          const double sx = b.width / p.image->width, sy = b.height / p.image->height;
          const double factor = p.fit == "cover" ? std::max(sx, sy) : std::min(sx, sy);
          const double w = p.image->width * factor, h = p.image->height * factor;
          const double x = b.x + (p.imageAlign == "right" ? b.width - w : p.imageAlign == "center" ? (b.width - w) / 2 : 0);
          const double y = b.y + (p.imageVerticalAlign == "bottom" ? b.height - h
                                  : p.imageVerticalAlign == "center" ? (b.height - h) / 2 : 0);
          CGContextSetAlpha(cg, CGFloat(p.opacity));
          CGContextSetInterpolationQuality(cg, kCGInterpolationMedium);
          // Images are bottom-up in Core Graphics; flip locally in the top-down CTM.
          CGContextTranslateCTM(cg, CGFloat(x), CGFloat(y + h));
          CGContextScaleCTM(cg, 1, -1);
          CGContextDrawImage(cg, CGRectMake(0, 0, w, h), bitmap->second.get());
        }
      }
      CGContextRestoreGState(cg);
      continue;
    }
    CGContextSetAlpha(cg, CGFloat(p.opacity));
    const double inset = p.strokeWidth / 2;
    const bool stroke = p.strokeWidth > 0;
    if (stroke) {
      setStrokeArgb(cg, space, p.strokeColor);
      CGContextSetLineWidth(cg, CGFloat(p.strokeWidth));
    }
    if (p.type == "line") {
      if (stroke) {
        CGContextMoveToPoint(cg, b.x + inset, p.lineRises ? b.y + b.height - inset : b.y + inset);
        CGContextAddLineToPoint(cg, b.x + b.width - inset, p.lineRises ? b.y + inset : b.y + b.height - inset);
        CGContextStrokePath(cg);
      }
      CGContextRestoreGState(cg);
      continue;
    }
    // Fill: solid colour, or a two-stop linear gradient across the box.
    auto fillShape = [&](CGPathRef shape) {
      if (!shape) return;
      if (p.gradient) {
        CGContextSaveGState(cg);
        CGContextAddPath(cg, shape);
        CGContextClip(cg);
        const CGFloat components[8] = {
            CGFloat((p.color >> 16) & 0xff) / 255, CGFloat((p.color >> 8) & 0xff) / 255, CGFloat(p.color & 0xff) / 255,
            CGFloat((p.color >> 24) & 0xff) / 255, CGFloat((p.endColor >> 16) & 0xff) / 255,
            CGFloat((p.endColor >> 8) & 0xff) / 255, CGFloat(p.endColor & 0xff) / 255,
            CGFloat((p.endColor >> 24) & 0xff) / 255};
        const CGFloat locations[2] = {0, 1};
        CfRef<CGGradientRef> gradient(CGGradientCreateWithColorComponents(space, components, locations, 2));
        const CGPoint start = CGPointMake(b.x, b.y);
        const CGPoint end = CGPointMake(b.x + (p.verticalGradient ? 0 : b.width), b.y + (p.verticalGradient ? b.height : 0));
        CGContextDrawLinearGradient(cg, gradient.get(), start, end,
                                    kCGGradientDrawsBeforeStartLocation | kCGGradientDrawsAfterEndLocation);
        CGContextRestoreGState(cg);
      } else {
        setFillArgb(cg, space, p.color);
        CGContextAddPath(cg, shape);
        CGContextFillPath(cg);
      }
    };
    if (p.type == "rect") {
      CfRef<CGPathRef> shape(createRoundedPath(cgRect(b), p.cornerRadius));
      fillShape(shape.get());
      if (stroke) {
        CfRef<CGPathRef> edge(createRoundedPath(
            CGRectMake(b.x + inset, b.y + inset, b.width - 2 * inset, b.height - 2 * inset),
            std::max(0.0, p.cornerRadius - inset)));
        if (edge) { CGContextAddPath(cg, edge.get()); CGContextStrokePath(cg); }
      }
    } else if (p.type == "ellipse") {
      if (b.width > 0 && b.height > 0) {
        CfRef<CGPathRef> shape(CGPathCreateWithEllipseInRect(cgRect(b), nullptr));
        fillShape(shape.get());
        if (stroke && b.width > 2 * inset && b.height > 2 * inset) {
          CGContextStrokeEllipseInRect(cg, CGRectMake(b.x + inset, b.y + inset, b.width - 2 * inset, b.height - 2 * inset));
        }
      }
    } else if (p.type == "text" && !p.text.empty()) {
      if (!drawTemplateText(cg, space, p, warning)) complete = false;
    }
    CGContextRestoreGState(cg);
  }
  CGContextRestoreGState(cg);
  return complete;
}

}  // namespace

CoreTextOverlayRaster::CoreTextOverlayRaster()
    : cache_([this](const Request& request) { return prepareRaster(request); },
             256ull * 1024 * 1024, 32, {}, 64) {}

CoreTextOverlayRaster::~CoreTextOverlayRaster() = default;

// Fixed operator wording, allocated once per process and shared by every fact.
std::shared_ptr<const std::string> CoreTextOverlayRaster::internedWarning(const char* text) {
  static std::mutex mutex;
  static std::map<std::string_view, std::shared_ptr<const std::string>> interned;
  std::lock_guard lock(mutex);
  auto& slot = interned[text];
  if (!slot) slot = std::make_shared<const std::string>(text);
  return slot;
}

void CoreTextOverlayRaster::setWarning(std::shared_ptr<const std::string> warning) {
  if (warning) lastWarning_.assign(*warning);  // reuses capacity: no steady-state allocation
  else lastWarning_.clear();
  sharedWarning_ = std::move(warning);
}

id<MTLTexture> CoreTextOverlayRaster::textureOfLease(const std::shared_ptr<const void>& lease) {
  if (!lease) return nil;
  const auto result = std::static_pointer_cast<const Cache::Result>(lease);
  return result->asset ? result->asset->texture : nil;
}

// Mirrors CompositorOverlayRaster::rasterOverlayTexture (Windows) rule for rule.
// Keep the two in step; the wording is part of the shared state machine.
id<MTLTexture> CoreTextOverlayRaster::rasterOverlayTexture(
    id<MTLDevice> device, const CompositorOverlayContent& overlay, const compositor::LayerRect& rect,
    int targetWidth, int targetHeight, const std::string& preparationSlot, bool strictResources,
    std::shared_ptr<const graphics::GraphicsPreparedResource> prepared) {
  // Rule 1: bound metadata before hashing or copying it into the queue.
  size_t metadataBytes = 0;
  const auto& a = overlay.appearance;
  for (const auto* value : {&overlay.title, &overlay.org, &overlay.text, &overlay.speaker,
      &overlay.imageUri, &overlay.fontFamily, &overlay.keyPosition, &overlay.keyPhase, &overlay.keyer,
      &overlay.brandColor, &overlay.brandAccentColor, &overlay.brandBackgroundColor,
      &a.preset, &a.anchor, &a.fontFamily, &a.nameColor, &a.titleColor, &a.backgroundColor, &a.accentColor}) {
    if (value->size() > 65536 - metadataBytes) {
      activeLease_.reset(); lastState_ = "failed"; failedThisFrame_ = true;
      setWarning(internedWarning("Graphics content exceeds preparation metadata budget."));
      return nil;
    }
    metadataBytes += value->size();
  }
  // Rule 2: dimensions.
  const double widthPixels = double(rect.width) * targetWidth;
  const double heightPixels = double(rect.height) * targetHeight;
  if (!device || !std::isfinite(widthPixels) || !std::isfinite(heightPixels) ||
      widthPixels < 1 || heightPixels < 1 || widthPixels > 4096 || heightPixels > 4096) {
    activeLease_.reset(); lastState_ = "failed"; failedThisFrame_ = true;
    setWarning(internedWarning("Invalid graphics raster dimensions."));
    return nil;
  }
  const int width = int(std::lround(widthPixels)), height = int(std::lround(heightPixels));
  // Rule 3: key. registryID is stable for the process and distinguishes GPUs.
  const Key key{uintptr_t(device.registryID), overlayContentSignature(overlay, width, height) ^
      (strictResources ? 0xd9e94a8e64b5fc21ULL : 0ULL), overlay.graphicsContentHash, overlay.hasGraphicsContentHash};
  // Rule 4: prepared-resource compatibility.
  const bool compatible = prepared && prepared->native && prepared->backend == graphics::GraphicsPreparedResource::Backend::Metal &&
      prepared->deviceIdentity == key.device && prepared->width > 0 && prepared->height > 0 &&
      prepared->hasContentHash == overlay.hasGraphicsContentHash && prepared->contentHash == overlay.graphicsContentHash &&
      prepared->width <= 4096 && prepared->height <= 4096 && prepared->signature ==
          (overlayContentSignature(overlay, prepared->width, prepared->height) ^ (strictResources ? 0xd9e94a8e64b5fc21ULL : 0ULL));
  activeResource_.reset();
  activeLease_.reset();
  // Rule 5: same size and compatible adopts directly.
  if (compatible && prepared->width == width && prepared->height == height) {
    activeLease_ = std::static_pointer_cast<const Cache::Result>(prepared->native);
    activeResource_ = prepared;
  } else activeLease_ = cache_.find(key);
  // Rule 6: miss -> bounded admission. The Request is built only here, so a
  // cache hit allocates nothing.
  if (!activeLease_) {
    Request request{device, overlay, width, height, strictResources};
    // Bounds the atlas for layered motion AND crawls. CompositorOverlayRaster.cpp
    // still inlines the layered-only bound, which refuses a crawl longer than
    // about two raster widths ("exceeded its admitted budget"): Windows-session
    // agenda item, switch it to this shared helper.
    const uint64_t admissionBytes = graphics::overlayAdmissionBytes(overlay.graphicsTemplate.get(), width, height);
    activeLease_ = cache_.findOrRequest(key, std::move(request),
        admissionBytes + (overlay.imageUri.empty() ? 0 : 128ull * 1024 * 1024), preparationSlot);
  }
  // Rule 7: scale retention.
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
  // Warnings travel to per-frame facts BY REFERENCE (sharedWarning()): the
  // prepared result's own string through an aliasing pointer (no allocation),
  // fixed wording interned once. Only the rare failed-scale message is built.
  if (failedScale) setWarning(std::make_shared<const std::string>(scaleWarning + " Retaining the accepted raster."));
  else if (retainingScale) setWarning(internedWarning("Graphics scale preparation pending; retaining the accepted raster."));
  else if (activeLease_) setWarning(activeLease_->warning.empty() ? nullptr
                                        : std::shared_ptr<const std::string>(activeLease_, &activeLease_->warning));
  else setWarning(internedWarning("Graphics preparation pending."));
  // Rule 8: capacity wording.
  if (!activeLease_ && overlay.hasGraphicsContentHash) {
    const auto capacity = graphics::GraphicsResourceBudget::process().counters();
    if (capacity.residentResources >= capacity.residentLimit)
      setWarning(internedWarning("Graphics preparation pending at the shared 16-revision capacity limit."));
  }
  pendingThisFrame_ = pendingThisFrame_ || (retainingScale && !failedScale) || !activeLease_;
  failedThisFrame_ = failedThisFrame_ || failedScale || (activeLease_ && !activeLease_->asset);
  return activeLease_ && activeLease_->asset ? activeLease_->asset->texture : nil;
}

std::shared_ptr<const graphics::GraphicsRunLayout> CoreTextOverlayRaster::activeRuns() const {
  return activeLease_ && activeLease_->asset ? activeLease_->asset->runs : nullptr;
}

std::shared_ptr<const graphics::GraphicsPreparedResource> CoreTextOverlayRaster::preparedResource() {
  if (!activeLease_ || !activeLease_->asset || std::string_view(lastState_) != "ready") return {};
  if (activeResource_) return activeResource_;
  // Same prepared texture -> same resource object, with no allocation, while
  // anyone (an instance that adopted it) still holds it. Keyed by the cache
  // result's address: a live weak entry proves that result is still alive, so
  // the address cannot have been reused. Mac-side only (plan G2 D7); the
  // Windows raster still allocates per call.
  const void* source = activeLease_.get();
  for (const auto& entry : resourceCache_) {
    if (entry.source != source) continue;
    if (auto cached = entry.resource.lock()) {
      activeResource_ = std::move(cached);
      return activeResource_;
    }
  }
  auto resource = std::make_shared<graphics::GraphicsPreparedResource>();
  resource->backend = graphics::GraphicsPreparedResource::Backend::Metal;
  resource->deviceIdentity = activeDevice_; resource->signature = activeSignature_;
  resource->width = activeWidth_; resource->height = activeHeight_; resource->native = activeLease_;
  resource->contentHash = activeContentHash_; resource->hasContentHash = activeHasContentHash_;
  if (const auto runs = activeRuns(); runs && runs->drawing && runs->drawing->crawl) {
    const auto& crawl = *runs->drawing->crawl;
    resource->crawl = graphics::GraphicsCrawlPlayback{crawl.speed, crawl.passWidth, crawl.loop};
  }
  activeResource_ = resource;
  resourceCache_[resourceCacheNext_] = {source, resource};
  resourceCacheNext_ = (resourceCacheNext_ + 1) % resourceCache_.size();
  return resource;
}

// Preparation worker only. Never runs on the render thread.
CoreTextOverlayRaster::Cache::Result CoreTextOverlayRaster::prepareRaster(const Request& request) {
  @autoreleasepool {
    compositor::invokeOverlayRasterPrepareHook();
    const auto& overlay = request.overlay;
    const int widthPx = request.width, heightPx = request.height;
    if (!request.device && !request.thumbnail) return {{}, 0, "Native graphics preparation unavailable."};
    std::string warning;
    // Template content resolves HERE, on the worker (fields, brand, layout with
    // Core Text measurement). A resolve error is a failed preparation.
    graphics::GraphicsDrawing drawing;
    ImageInputs images;
    uint64_t packagedImageBytes = 0;
    const bool templated = bool(overlay.graphicsTemplate);
    if (templated) {
      if (!overlay.graphicsTemplate->definition) return {{}, 0, "Graphics template definition is missing."};
      std::string error;
      if (!overlay.graphicsTemplate->definition->resolve(overlay.graphicsTemplate->fields,
          overlay.graphicsTemplate->brand, drawing, error, overlay.graphicsTemplate->logo)) return {{}, 0, error};
      if (!drawing.warnings.empty()) warning = drawing.warnings.front();
      for (const auto& primitive : drawing.primitives) {
        if (!primitive.image || images.count(primitive.image->id)) continue;
        auto input = openPackagedImage(*primitive.image);
        if (!input.source) return {{}, 0, "Graphics image header could not be decoded: " + primitive.image->id};
        packagedImageBytes += input.cpuBytes;
        images.emplace(primitive.image->id, std::move(input));
      }
    }
    auto layout = computeOverlayTileLayout(overlay, widthPx, heightPx);
    ImageInput image = layout.hasImage && !templated
        ? (overlay.graphicsImage ? openPackagedImage(*overlay.graphicsImage) : openFileImage(overlay.imageUri))
        : ImageInput{};

    // Legacy font: the Windows check runs only for an appearance; elsewhere a
    // missing family falls back silently, as DirectWrite's does (G1 D1). A
    // table substitution warns and never fails. Template text resolves per
    // primitive inside drawTemplateText.
    const auto font = graphics::resolveGraphicsFontFamily(layout.fontFamily);
    if (templated) {
    } else if (overlay.appearance.enabled && !font.available) {
      if (request.strictResources)
        return {{}, 0, "Requested graphics font is unavailable; choose an installed font."};
      warning = std::string("Font unavailable; using ") + kFallbackFontFamily + ".";
    } else if (font.substituted) {
      warning = graphics::graphicsFontSubstitutionWarning(layout.fontFamily, font.family);
    }
    layout.fontFamily = font.family;

    // Atlas: a crawl, or layered motion not using whole-graphic motion, draws
    // each run into its own slice (CompositorOverlayRaster.cpp parity).
    int textureWidth = widthPx, textureHeight = heightPx;
    const double scale = drawing.height > 0 ? double(heightPx) / drawing.height : 1.0;
    std::shared_ptr<graphics::GraphicsRunLayout> runs;
    // A thumbnail draws a crawl in one raster: each item at its start, clipped
    // to the crawl window (CompositorOverlayRaster.cpp thumbnail parity).
    if (drawing.crawl && request.thumbnail) {
      for (auto& primitive : drawing.primitives) {
        if (!primitive.crawlStart) continue;
        primitive.box.x += *primitive.crawlStart;
        primitive.clip = graphics::intersect(primitive.box, drawing.crawl->clip);
      }
    }
    if (templated && !request.thumbnail && (drawing.crawl || (overlay.graphicsTemplate->definition->motionSummary().layered &&
                                        !overlay.graphicsTemplate->wholeGraphicMotion))) {
      runs = std::make_shared<graphics::GraphicsRunLayout>();
      runs->width = drawing.width;
      runs->height = drawing.height;
      runs->runs = graphics::planRuns(drawing);
      if (!graphics::packAtlas(runs->runs, scale, runs->slices, runs->atlasWidth, runs->atlasHeight) ||
          runs->atlasWidth > int(16384) || runs->atlasHeight > int(16384))
        return {{}, 0, "Graphics motion needs more texture space than one graphic allows; give fewer layers their own motion."};
      textureWidth = std::max(1, runs->atlasWidth);
      textureHeight = std::max(1, runs->atlasHeight);
    }
    const uint64_t textureBytes = uint64_t(textureWidth) * textureHeight * 4;
    image.cpuBytes += packagedImageBytes;
    auto& sharedBudget = graphics::GraphicsResourceBudget::process();
    if (textureBytes + image.cpuBytes > sharedBudget.counters().cpuLimit)
      return {{}, 0, "Graphics assets exceed the shared CPU preparation capacity."};
    std::string revisionKey;
    if (overlay.hasGraphicsContentHash) {
      static constexpr char hex[] = "0123456789abcdef";
      revisionKey = "graphics:";
      for (auto byte : overlay.graphicsContentHash) { revisionKey += hex[byte >> 4]; revisionKey += hex[byte & 15]; }
    } else revisionKey = "legacy:" + std::to_string(overlayContentSignature(overlay, 1920, 1080));
    auto budget = sharedBudget.tryReserve(textureBytes + image.cpuBytes, textureBytes, revisionKey, overlay.hasGraphicsContentHash);
    if (!budget) return {{}, 0, "Graphics preparation is waiting for shared resource capacity.", true};

    const size_t stride = size_t(textureWidth) * 4;
    std::vector<uint8_t> pixels(stride * size_t(textureHeight), 0);  // transparent
    CfRef<CGColorSpaceRef> space(CGColorSpaceCreateDeviceRGB());
    // Premultiplied BGRA, exactly what the (One, 1-SrcAlpha) overlay pipeline
    // blends: no post pass (the #350 rule). DeviceRGB: colours are not managed.
    CfRef<CGContextRef> context(CGBitmapContextCreate(pixels.data(), size_t(textureWidth), size_t(textureHeight), 8, stride,
        space.get(), CGBitmapInfo(kCGImageAlphaPremultipliedFirst) | kCGBitmapByteOrder32Little));
    if (!context) return {{}, 0, "Graphics raster target creation failed."};
    CGContextRef cg = context.get();
    // Grayscale AA, no font smoothing (the target carries alpha), subpixel
    // positioning on (spec 5.6).
    CGContextSetAllowsAntialiasing(cg, true);
    CGContextSetShouldAntialias(cg, true);
    CGContextSetAllowsFontSmoothing(cg, false);
    CGContextSetShouldSmoothFonts(cg, false);
    CGContextSetAllowsFontSubpixelPositioning(cg, true);
    CGContextSetShouldSubpixelPositionFonts(cg, true);

    bool complete = true;
    if (templated) {
      CGContextSaveGState(cg);
      CGContextTranslateCTM(cg, 0, textureHeight);  // top-down template space
      CGContextScaleCTM(cg, 1, -1);
      if (runs) {
        for (size_t i = 0; i < runs->runs.size(); ++i) {
          const auto& run = runs->runs[i];
          const auto& slice = runs->slices[i];
          if (slice.width <= 0 || slice.height <= 0) continue;
          complete = drawTemplate(cg, space.get(), drawing, scale, images, run.first, run.count,
                                  slice.x - run.source.x * scale, slice.y - run.source.y * scale, warning) && complete;
        }
      } else {
        complete = drawTemplate(cg, space.get(), drawing, scale, images, 0, drawing.primitives.size(), 0, 0, warning);
      }
      CGContextRestoreGState(cg);
    }
    CGContextSaveGState(cg);
    if (templated) {
      // nothing more: the legacy layout below is for non-template content
    } else {
    // Band: painted here only for an appearance-styled lower third, rounded and
    // clipping everything after it (Windows PushLayer). Otherwise the adapter
    // draws the band as its own quad so its alpha animation is the CPU mirror's.
    if (overlay.appearance.enabled && !overlay.isCaption) {
      const CGRect band = CGRectMake(0, 0, widthPx, heightPx);
      const CGFloat radius = std::clamp<CGFloat>(layout.cornerRadius, 0, std::min(widthPx, heightPx) * 0.5);
      CfRef<CGPathRef> path(radius > 0 ? CGPathCreateWithRoundedRect(band, radius, radius, nullptr)
                                       : CGPathCreateWithRect(band, nullptr));
      if (!path) complete = false;
      else {
        CGContextAddPath(cg, path.get());
        CGContextClip(cg);
        const auto bg = colorFromArgb(layout.backgroundArgb, space.get());
        CGContextSetFillColorWithColor(cg, bg.get());
        CGContextFillRect(cg, band);
      }
    }
    if (layout.accentBar.width > 0 && layout.accentBar.height > 0) {
      const auto accent = colorFromArgb(layout.accentArgb, space.get());
      CGContextSetFillColorWithColor(cg, accent.get());
      CGContextFillRect(cg, bottomUp(layout.accentBar, heightPx));
    }
    if (layout.hasImage) {
      CfRef<CGImageRef> decoded(decodeImageNow(image.source.get()));
      if (!decoded) {
        warning += " Logo unavailable; image omitted.";
        complete = false;
      } else {
        // Re-tag an RGB image with the context space so an embedded ICC
        // profile is not converted (WIC converts nothing either; spec 5.4).
        CfRef<CGImageRef> retagged(CGColorSpaceGetModel(CGImageGetColorSpace(decoded.get())) == kCGColorSpaceModelRGB
            ? CGImageCreateCopyWithColorSpace(decoded.get(), space.get()) : nullptr);
        CGImageRef drawable = retagged ? retagged.get() : decoded.get();
        const CGFloat iw = CGImageGetWidth(drawable), ih = CGImageGetHeight(drawable);
        CGRect box = bottomUp(layout.imageRect, heightPx);
        if (iw > 0 && ih > 0) {
          const CGFloat scale = std::min(box.size.width / iw, box.size.height / ih);
          const CGFloat fw = iw * scale, fh = ih * scale;
          box = CGRectMake(box.origin.x + (box.size.width - fw) / 2, box.origin.y + (box.size.height - fh) / 2, fw, fh);
        }
        CGContextSetInterpolationQuality(cg, kCGInterpolationMedium);
        CGContextDrawImage(cg, box, drawable);
      }
    }
    // First line is the emphasis line (lower-third name, or caption speaker
    // attribution when present): semibold; the rest regular.
    for (size_t index = 0; index < layout.textLines.size(); ++index) {
      const bool emphasis = index == 0 && (!overlay.isCaption || !overlay.speaker.empty());
      if (!drawLegacyTextLine(cg, space.get(), layout.textLines[index], layout.fontFamily, emphasis, heightPx))
        complete = false;
    }
    }  // legacy
    CGContextRestoreGState(cg);
    CGContextFlush(cg);
    context = CfRef<CGContextRef>();
    image = ImageInput{};
    images.clear();
    if (runs) runs->drawing = std::make_shared<const graphics::GraphicsDrawing>(std::move(drawing));

    if (request.strictResources && !complete)
      return {{}, 0, warning.empty() ? "Graphics resources could not be prepared completely." : warning};

    if (request.thumbnail) {
      // PNG stores straight alpha: ImageIO un-premultiplies the premultiplied
      // BGRA bitmap on encode (WIC's format converter does the same on Windows).
      CfRef<CFDataRef> bytes(CFDataCreate(kCFAllocatorDefault, pixels.data(), CFIndex(pixels.size())));
      CfRef<CGDataProviderRef> provider(bytes ? CGDataProviderCreateWithCFData(bytes.get()) : nullptr);
      CfRef<CGImageRef> bitmap(provider ? CGImageCreate(size_t(textureWidth), size_t(textureHeight), 8, 32, stride,
          space.get(), CGBitmapInfo(kCGImageAlphaPremultipliedFirst) | kCGBitmapByteOrder32Little, provider.get(),
          nullptr, false, kCGRenderingIntentDefault) : nullptr);
      CfRef<CFMutableDataRef> encoded(CFDataCreateMutable(kCFAllocatorDefault, 0));
      CfRef<CGImageDestinationRef> destination(bitmap && encoded
          ? CGImageDestinationCreateWithData(encoded.get(), CFSTR("public.png"), 1, nullptr) : nullptr);
      if (!destination) return {{}, 0, "Native thumbnail PNG encoding failed."};
      CGImageDestinationAddImage(destination.get(), bitmap.get(), nullptr);
      if (!CGImageDestinationFinalize(destination.get())) return {{}, 0, "Native thumbnail PNG encoding failed."};
      const auto length = CFDataGetLength(encoded.get());
      if (length <= 0 || length > 256 * 1024) return {{}, 0, "Native thumbnail exceeds its encoded limit."};
      auto entry = std::make_shared<MetalRasterTex>();
      entry->thumbnail.assign(CFDataGetBytePtr(encoded.get()), CFDataGetBytePtr(encoded.get()) + length);
      std::vector<uint8_t>().swap(pixels);
      budget->finishPreparation();
      return {std::move(entry), 0, warning};
    }

    MTLTextureDescriptor* descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:NSUInteger(textureWidth)
                                                          height:NSUInteger(textureHeight)
                                                       mipmapped:NO];
    descriptor.usage = MTLTextureUsageShaderRead;
    descriptor.storageMode = MTLStorageModeShared;
    id<MTLTexture> texture = [request.device newTextureWithDescriptor:descriptor];
    if (!texture) return {{}, 0, "Graphics texture creation failed."};
    [texture replaceRegion:MTLRegionMake2D(0, 0, NSUInteger(textureWidth), NSUInteger(textureHeight))
               mipmapLevel:0
                 withBytes:pixels.data()
               bytesPerRow:stride];
    auto entry = std::make_shared<MetalRasterTex>();
    entry->budget = budget;
    entry->texture = texture;
    entry->runs = std::move(runs);
    // Drop the CPU copy before returning its admission charge.
    std::vector<uint8_t>().swap(pixels);
    budget->finishPreparation();
    return {std::move(entry), textureBytes, warning};
  }
}

std::vector<uint8_t> coreTextThumbnailPng(const CompositorOverlayContent& overlay, int width, int height,
                                          std::string& warning) {
  if (width < 1 || height < 1 || width > 320 || height > 180) {
    warning = "Invalid thumbnail dimensions.";
    return {};
  }
  CoreTextOverlayRaster::Request request;
  request.overlay = overlay;
  request.width = width;
  request.height = height;
  request.strictResources = true;
  request.thumbnail = true;
  const auto result = CoreTextOverlayRaster::prepareRaster(request);
  warning = result.warning;
  // The entry (and its budget lease) is released when `result` goes out of scope.
  return result.asset ? result.asset->thumbnail : std::vector<uint8_t>{};
}

}  // namespace corevideo::modules

#endif  // !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_METAL
