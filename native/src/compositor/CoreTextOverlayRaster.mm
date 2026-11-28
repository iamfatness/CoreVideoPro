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
#include "compositor/D2DCurveFlattening.h"
#include "compositor/CoreTextThumbnail.h"
#include "compositor/OverlayRasterFrontHalf.h"
#include "compositor/ShapeShadowRaster.h"
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
// surfaces the failure on the worker. (WIC does not report it at all: Windows
// verifies the PNG pixel stream itself, CompositorOverlayRaster.cpp
// pngPixelStreamDecodes.) The copy is the decoded allowance the budget already
// charges.
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

// Curves are drawn as D2D draws them: as the chords it flattens them to
// (D2DCurveFlattening.h, proven point for point against D2D on Windows). True
// Core Graphics curves sit outside D2D's chords and read heavier on every
// curved antialiased edge (G4 parity: ellipse edges up to 71/255, rounded
// corners up to 96/255). D2D flattens at 0.25 device px; geometry is built in
// user space, so the tolerance is divided by the CTM's largest scale
// (D2D1ComputeMaximumScaleFactor).
namespace d2dcurves = corevideo::compositor::d2dcurves;

double flatteningTolerance(CGContextRef cg) {
  const CGAffineTransform m = CGContextGetCTM(cg);
  // Largest singular value of [a b; c d].
  const double p = (m.a * m.a + m.b * m.b + m.c * m.c + m.d * m.d) / 2;
  const double q = std::fabs(m.a * m.d - m.b * m.c);
  const double scale = std::sqrt(p + std::sqrt(std::max(0.0, p * p - q * q)));
  return scale > 0 ? d2dcurves::kDefaultTolerance / scale : d2dcurves::kDefaultTolerance;
}

// A closed path through D2D-space points; `flipHeight` > 0 maps D2D's y-down
// space into a bottom-up context (y' = flipHeight - y) without reordering, so
// the chords stay the ones D2D draws.
CGPathRef createPolygonPath(const std::vector<d2dcurves::Point>& points, double flipHeight = 0) {
  if (points.size() < 3) return nullptr;
  CGMutablePathRef path = CGPathCreateMutable();
  const auto y = [&](double v) { return CGFloat(flipHeight > 0 ? flipHeight - v : v); };
  CGPathMoveToPoint(path, nullptr, CGFloat(points[0].x), y(points[0].y));
  for (size_t i = 1; i < points.size(); ++i) CGPathAddLineToPoint(path, nullptr, CGFloat(points[i].x), y(points[i].y));
  CGPathCloseSubpath(path);
  return path;
}

CGPathRef createRoundedPath(CGRect rect, CGFloat radius, double tolerance) {
  if (rect.size.width <= 0 || rect.size.height <= 0) return nullptr;
  // Core Graphics asserts on radii over half a side; D2D clamps (spec 5.7).
  radius = std::clamp<CGFloat>(radius, 0, std::min(rect.size.width, rect.size.height) * 0.5);
  if (radius <= 0) return CGPathCreateWithRect(rect, nullptr);
  return createPolygonPath(d2dcurves::roundedRect(rect.origin.x, rect.origin.y, rect.size.width, rect.size.height,
                                                  radius, tolerance));
}

// Draws `image` into user-space `dest` as D2D's DrawBitmap with
// D2D1_BITMAP_INTERPOLATION_MODE_LINEAR does: bilinear on premultiplied
// pixels, sample positions at device pixel centres mapped into the source
// (centre-aligned, edges clamped), and a fractional destination edge
// antialiased by the pixel's area inside the rect. Probed on Windows
// 2026-11-27: one white pixel upscaled 8x reads 16, 48, ... 239 per column,
// exactly this kernel. Core Graphics' interpolation qualities are
// unspecified (Medium read sharper than bilinear, edges up to 66/255 off),
// so the resample is done here and composited 1:1 in device pixels. The
// caller's alpha (opacity) and clip apply to the composite.
bool drawImageBilinear(CGContextRef cg, CGColorSpaceRef space, CGImageRef image, CGRect dest) {
  const size_t srcW = CGImageGetWidth(image), srcH = CGImageGetHeight(image);
  if (srcW == 0 || srcH == 0 || dest.size.width <= 0 || dest.size.height <= 0) return true;
  const CGAffineTransform ctm = CGContextGetCTM(cg);
  if (ctm.b != 0 || ctm.c != 0) {  // not axis-aligned: no D2D-matching path; draw as before
    CGContextSetInterpolationQuality(cg, kCGInterpolationMedium);
    CGContextTranslateCTM(cg, dest.origin.x, dest.origin.y + dest.size.height);
    CGContextScaleCTM(cg, 1, -1);
    CGContextDrawImage(cg, CGRectMake(0, 0, dest.size.width, dest.size.height), image);
    return true;
  }
  // Source pixels, premultiplied B G R A; memory row 0 is the image's top row.
  CfRef<CGContextRef> source(CGBitmapContextCreate(nullptr, srcW, srcH, 8, srcW * 4, space,
                                                   CGBitmapInfo(kCGImageAlphaPremultipliedFirst) |
                                                       kCGBitmapByteOrder32Little));
  if (!source) return false;
  CGContextSetBlendMode(source.get(), kCGBlendModeCopy);
  CGContextSetInterpolationQuality(source.get(), kCGInterpolationNone);
  CGContextDrawImage(source.get(), CGRectMake(0, 0, CGFloat(srcW), CGFloat(srcH)), image);
  CGContextFlush(source.get());
  const auto* src = static_cast<const uint8_t*>(CGBitmapContextGetData(source.get()));
  const size_t srcStride = CGBitmapContextGetBytesPerRow(source.get());
  // Device positions of the image's top-left and bottom-right corners.
  const CGPoint p0 = CGPointApplyAffineTransform(dest.origin, ctm);
  const CGPoint p1 = CGPointApplyAffineTransform(
      CGPointMake(dest.origin.x + dest.size.width, dest.origin.y + dest.size.height), ctm);
  const double dx0 = std::min(p0.x, p1.x), dx1 = std::max(p0.x, p1.x);
  const double dy0 = std::min(p0.y, p1.y), dy1 = std::max(p0.y, p1.y);
  const double targetW = double(CGBitmapContextGetWidth(cg)), targetH = double(CGBitmapContextGetHeight(cg));
  const double ix0 = std::max(0.0, std::floor(dx0)), ix1 = std::min(targetW, std::ceil(dx1));
  const double iy0 = std::max(0.0, std::floor(dy0)), iy1 = std::min(targetH, std::ceil(dy1));
  if (ix1 <= ix0 || iy1 <= iy0) return true;  // off the bitmap
  const size_t width = size_t(ix1 - ix0), height = size_t(iy1 - iy0);
  CfRef<CGContextRef> layer(CGBitmapContextCreate(nullptr, width, height, 8, width * 4, space,
                                                  CGBitmapInfo(kCGImageAlphaPremultipliedFirst) |
                                                      kCGBitmapByteOrder32Little));
  if (!layer) return false;
  auto* out = static_cast<uint8_t*>(CGBitmapContextGetData(layer.get()));
  const size_t outStride = CGBitmapContextGetBytesPerRow(layer.get());
  const auto overlap = [](double a0, double a1, double b0, double b1) {
    return std::max(0.0, std::min(a1, b1) - std::max(a0, b0));
  };
  const auto at = [&](long sx, long sy) {
    sx = std::clamp<long>(sx, 0, long(srcW) - 1);
    sy = std::clamp<long>(sy, 0, long(srcH) - 1);
    return src + size_t(sy) * srcStride + size_t(sx) * 4;
  };
  for (size_t row = 0; row < height; ++row) {
    // Memory row 0 is the layer's top: the highest device y.
    const double deviceY = iy1 - 1 - double(row);
    const double coverY = overlap(deviceY, deviceY + 1, dy0, dy1);
    // Source row coordinate of this pixel's centre (p0 is the image's top edge).
    const double v = (deviceY + 0.5 - p0.y) / (p1.y - p0.y) * double(srcH) - 0.5;
    const long v0 = long(std::floor(v));
    const double fv = v - double(v0);
    for (size_t col = 0; col < width; ++col) {
      const double deviceX = ix0 + double(col);
      const double cover = coverY * overlap(deviceX, deviceX + 1, dx0, dx1);
      uint8_t* px = out + row * outStride + col * 4;
      if (cover <= 0) continue;
      const double u = (deviceX + 0.5 - p0.x) / (p1.x - p0.x) * double(srcW) - 0.5;
      const long u0 = long(std::floor(u));
      const double fu = u - double(u0);
      const uint8_t *a = at(u0, v0), *b = at(u0 + 1, v0), *c = at(u0, v0 + 1), *d = at(u0 + 1, v0 + 1);
      for (int k = 0; k < 4; ++k) {
        const double top = a[k] + (b[k] - a[k]) * fu, bottom = c[k] + (d[k] - c[k]) * fu;
        px[k] = uint8_t(std::clamp((top + (bottom - top) * fv) * cover + 0.5, 0.0, 255.0));
      }
    }
  }
  CfRef<CGImageRef> resampled(CGBitmapContextCreateImage(layer.get()));
  if (!resampled) return false;
  CGContextSaveGState(cg);
  CGContextConcatCTM(cg, CGAffineTransformInvert(ctm));  // device pixels; clip and alpha stay
  CGContextSetInterpolationQuality(cg, kCGInterpolationNone);
  CGContextDrawImage(cg, CGRectMake(CGFloat(ix0), CGFloat(iy0), CGFloat(width), CGFloat(height)), resampled.get());
  CGContextRestoreGState(cg);
  return true;
}

CGPathRef createEllipsePath(CGRect rect, double tolerance) {
  if (rect.size.width <= 0 || rect.size.height <= 0) return nullptr;
  return createPolygonPath(d2dcurves::ellipse(CGRectGetMidX(rect), CGRectGetMidY(rect), rect.size.width / 2,
                                              rect.size.height / 2, tolerance));
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
//
// Very dark text gets a lower k (slice 10, 2026-11-28). D2D's grayscale
// alpha correction follows the brush: drawn alone on Windows (build 26300,
// Arial 700 28 px + Georgia 26 px), black glyphs carry 0.876 of the white
// alpha and #14213D 0.900, which fits k = -0.54 and -0.41 against the white
// curve. On the parity fixtures, where text sits on an opaque plate, that
// holds for near-black text (#05070B, #14213D, #1E1B2E on light plates read
// 8-14% heavy on the Mac with k = 1 and pass with these k), while mid and light
// text (#5A6578, #D8DCE3 and every earlier fixture) matches k = 1 and reads
// 8-10% light with a lower k. So k falls only for luma below 0.30, through
// the two measured dark points.
double windowsTextContrastForColor(uint32_t argb) {
  const double r = ((argb >> 16) & 0xff) / 255.0, g = ((argb >> 8) & 0xff) / 255.0, b = (argb & 0xff) / 255.0;
  const double luma = 0.2126 * r + 0.7152 * g + 0.0722 * b;
  constexpr std::array<std::pair<double, double>, 4> points{{
      {0.0, -0.54}, {0.127, -0.41}, {0.30, kWindowsTextGrayscaleContrast}, {1.0, kWindowsTextGrayscaleContrast}}};
  for (size_t i = 1; i < points.size(); ++i)
    if (luma <= points[i].first) {
      const double t = (luma - points[i - 1].first) / (points[i].first - points[i - 1].first);
      return points[i - 1].second + t * (points[i].second - points[i - 1].second);
    }
  return kWindowsTextGrayscaleContrast;
}

std::array<uint8_t, 256> windowsTextCoverageCurve(uint32_t argb) {
  std::array<uint8_t, 256> out{};
  const double k = windowsTextContrastForColor(argb);
  for (int i = 0; i < 256; ++i) {
    const double a = i / 255.0;
    out[size_t(i)] = uint8_t(std::clamp(std::lround(255.0 * a * (k + 1) / (a * k + 1)), 0L, 255L));
  }
  return out;
}

bool drawTemplateText(CGContextRef cg, CGColorSpaceRef space, const graphics::GraphicsPrimitive& p,
                      std::string& warning) {
  const auto resolved = graphics::resolveGraphicsFontFamily(p.font);
  if (!resolved.available) {
    warning = graphics::graphicsFontUnavailableMessage(p.font);
    return false;
  }
  if (resolved.substituted && warning.empty())
    warning = graphics::graphicsFontSubstitutionWarning(p.font, resolved.family);
  CfRef<CTFontRef> font(graphics::createGraphicsFont(resolved.family, resolved.weight ? resolved.weight : p.fontWeight,
                                                     p.fontSize, p.italic));
  if (!font) return false;
  const auto& b = p.box;
  const double lineHeight = p.fontSize * p.lineHeight;
  const double layoutHeight = std::min(b.height, lineHeight * p.maxLines);
  const bool wrap = p.overflow == "wrap-then-ellipsis";
  const int visible = wrap ? std::max(1, int(std::floor(layoutHeight / lineHeight + 1e-9))) : 1;
  const auto color = colorFromArgb(p.color, space);
  graphics::GraphicsTextLayout text(p.text, font.get(), color.get(), b.width, wrap, visible, b.width, p.letterSpacing);
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
  const auto curve = windowsTextCoverageCurve(p.color);
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

// Composites a shape's shadow (styles P4) under the shape: the same finished
// premultiplied pixels the Windows raster draws (ShapeShadowRaster.h, never
// CGContextSetShadowWithColor's Gaussian), placed 1:1 in device pixels inside
// the shadow clip. (x, y) are D2D's top-down device pixels; the CTM maps them
// to this context's device space.
bool drawShapeShadow(CGContextRef cg, CGColorSpaceRef space, const graphics::GraphicsPrimitive& p, double scale,
                     double offsetX, double offsetY) {
  namespace shadows = compositor::shapeshadow;
  shadows::Outline outline;
  outline.x = p.box.x; outline.y = p.box.y; outline.width = p.box.width; outline.height = p.box.height;
  outline.cornerRadius = p.cornerRadius;
  if (p.type == "ellipse") outline.kind = shadows::Outline::Kind::Ellipse;
  else if (!p.polygon.empty()) {
    outline.kind = shadows::Outline::Kind::Polygon;
    for (const auto& point : p.polygon) outline.points.push_back({point.x, point.y});
  }
  const auto& s = *p.shapeShadow;
  const auto& c = p.shadowClip;
  const auto shadow = shadows::rasterize(outline, s.color, s.dx, s.dy, s.blur, s.spread, p.opacity, c.x, c.y,
      c.width, c.height, scale, offsetX, offsetY, int(CGBitmapContextGetWidth(cg)), int(CGBitmapContextGetHeight(cg)));
  if (shadow.refused) return false;
  if (shadow.width <= 0 || shadow.height <= 0) return true;
  CfRef<CGContextRef> pixels(CGBitmapContextCreate(const_cast<uint8_t*>(shadow.bgra.data()), size_t(shadow.width),
      size_t(shadow.height), 8, size_t(shadow.width) * 4, space,
      CGBitmapInfo(kCGImageAlphaPremultipliedFirst) | kCGBitmapByteOrder32Little));
  if (!pixels) return false;
  CfRef<CGImageRef> image(CGBitmapContextCreateImage(pixels.get()));
  if (!image) return false;
  // The template-space rect of those device pixels, then through the CTM.
  const CGAffineTransform ctm = CGContextGetCTM(cg);
  const CGRect user = CGRectMake(CGFloat((shadow.x - offsetX) / scale), CGFloat((shadow.y - offsetY) / scale),
                                 CGFloat(shadow.width / scale), CGFloat(shadow.height / scale));
  const CGRect mapped = CGRectApplyAffineTransform(user, ctm);
  const CGRect device = CGRectMake(std::round(mapped.origin.x), std::round(mapped.origin.y), CGFloat(shadow.width),
                                   CGFloat(shadow.height));
  CGContextSaveGState(cg);
  CGContextSetShouldAntialias(cg, false);
  CGContextClipToRect(cg, cgRect(c));  // aliased, as every primitive clip
  CGContextSetShouldAntialias(cg, true);
  CGContextConcatCTM(cg, CGAffineTransformInvert(ctm));  // device pixels; the clip stays
  CGContextSetInterpolationQuality(cg, kCGInterpolationNone);
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
    if (p.shapeShadow && !drawShapeShadow(cg, space, p, scale, offsetX, offsetY)) {
      complete = false;
      warning = "Graphics shadow is too large to prepare.";
    }
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
          if (!drawImageBilinear(cg, space, bitmap->second.get(), CGRectMake(x, y, w, h))) {
            complete = false;
            warning = "Graphics image layer allocation failed.";
          }
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
    // Fill: solid colour, or a gradient (graphics styles P2): N stops, linear
    // along graphics::linearGradientLine or radial (an ellipse half the box,
    // drawn as a circle under a y scale), clamped past both ends as D2D's
    // default extend mode is.
    auto fillShape = [&](CGPathRef shape) {
      if (!shape) return;
      if (p.gradient) {
        CGContextSaveGState(cg);
        CGContextAddPath(cg, shape);
        CGContextClip(cg);
        const auto& stops = p.gradient->stops;
        const size_t count = std::min<size_t>(stops.size(), 8);
        CGFloat components[32], locations[8];
        for (size_t s = 0; s < count; ++s) {
          const uint32_t c = stops[s].color;
          components[s * 4 + 0] = CGFloat((c >> 16) & 0xff) / 255;
          components[s * 4 + 1] = CGFloat((c >> 8) & 0xff) / 255;
          components[s * 4 + 2] = CGFloat(c & 0xff) / 255;
          components[s * 4 + 3] = CGFloat((c >> 24) & 0xff) / 255;
          locations[s] = CGFloat(stops[s].offset);
        }
        CfRef<CGGradientRef> gradient(CGGradientCreateWithColorComponents(space, components, locations, count));
        const auto extend = kCGGradientDrawsBeforeStartLocation | kCGGradientDrawsAfterEndLocation;
        if (p.gradient->radial) {
          const double rx = b.width / 2, ry = b.height / 2;
          if (rx > 0 && ry > 0) {
            CGContextTranslateCTM(cg, CGFloat(b.x + b.width * p.gradient->centerX), CGFloat(b.y + b.height * p.gradient->centerY));
            CGContextScaleCTM(cg, 1, CGFloat(ry / rx));
            CGContextDrawRadialGradient(cg, gradient.get(), CGPointZero, 0, CGPointZero, CGFloat(rx), extend);
          }
        } else {
          const auto line = graphics::linearGradientLine(*p.gradient, b);
          CGContextDrawLinearGradient(cg, gradient.get(), CGPointMake(line[0], line[1]), CGPointMake(line[2], line[3]), extend);
        }
        CGContextRestoreGState(cg);
      } else {
        setFillArgb(cg, space, p.color);
        CGContextAddPath(cg, shape);
        CGContextFillPath(cg);
      }
    };
    // Strokes widen the same flattened polygon D2D widens; Core Graphics'
    // default miter join (limit 10) is D2D's default stroke style.
    const double tolerance = flatteningTolerance(cg);
    if (p.type == "rect" && !p.polygon.empty()) {
      // Polygon / skewed plate (styles P3): straight edges, no radius or stroke;
      // non-zero fill, as the D2D path geometry is set to winding.
      std::vector<d2dcurves::Point> points;
      for (const auto& point : p.polygon) points.push_back({point.x, point.y});
      CfRef<CGPathRef> shape(createPolygonPath(points));
      fillShape(shape.get());
    } else if (p.type == "rect") {
      CfRef<CGPathRef> shape(createRoundedPath(cgRect(b), p.cornerRadius, tolerance));
      fillShape(shape.get());
      if (stroke) {
        CfRef<CGPathRef> edge(createRoundedPath(
            CGRectMake(b.x + inset, b.y + inset, b.width - 2 * inset, b.height - 2 * inset),
            std::max(0.0, p.cornerRadius - inset), tolerance));
        if (edge) { CGContextAddPath(cg, edge.get()); CGContextStrokePath(cg); }
      }
    } else if (p.type == "ellipse") {
      if (b.width > 0 && b.height > 0) {
        CfRef<CGPathRef> shape(createEllipsePath(cgRect(b), tolerance));
        fillShape(shape.get());
        if (stroke && b.width > 2 * inset && b.height > 2 * inset) {
          CfRef<CGPathRef> edge(createEllipsePath(
              CGRectMake(b.x + inset, b.y + inset, b.width - 2 * inset, b.height - 2 * inset), tolerance));
          if (edge) { CGContextAddPath(cg, edge.get()); CGContextStrokePath(cg); }
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
  return compositor::overlayRasterFront::internedWarning(text);
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
  // Rules 1-4 and 7-8 are the shared front half (OverlayRasterFrontHalf.h,
  // spec 5.1); CompositorOverlayRaster::rasterOverlayTexture calls the same.
  namespace front = compositor::overlayRasterFront;
  // Rule 1: bound metadata before hashing or copying it into the queue.
  if (!front::metadataWithinBound(overlay)) {
    activeLease_.reset(); lastState_ = "failed"; failedThisFrame_ = true;
    setWarning(internedWarning(front::kMetadataBudgetWarning));
    return nil;
  }
  // Rule 2: dimensions.
  int width = 0, height = 0;
  if (!device || !front::rasterPixelSize(rect, targetWidth, targetHeight, width, height)) {
    activeLease_.reset(); lastState_ = "failed"; failedThisFrame_ = true;
    setWarning(internedWarning(front::kInvalidDimensionsWarning));
    return nil;
  }
  // Rule 3: key. registryID is stable for the process and distinguishes GPUs.
  const Key key{uintptr_t(device.registryID), front::rasterSignature(overlay, width, height, strictResources),
      overlay.graphicsContentHash, overlay.hasGraphicsContentHash};
  // Rule 4: prepared-resource compatibility.
  const bool compatible = front::preparedIsCompatible(prepared.get(), graphics::GraphicsPreparedResource::Backend::Metal,
                                                      key.device, overlay, strictResources);
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
    // Bounds the atlas for layered motion AND crawls (shared with D3D11).
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
  lastState_ = front::preparationState(failedScale, retainingScale, bool(activeLease_), activeLease_ && activeLease_->asset);
  // Warnings travel to per-frame facts BY REFERENCE (sharedWarning()): the
  // prepared result's own string through an aliasing pointer (no allocation),
  // fixed wording interned once. Only the rare failed-scale message is built.
  if (failedScale) setWarning(std::make_shared<const std::string>(scaleWarning + front::kRetainingSuffix));
  else if (retainingScale) setWarning(internedWarning(front::kScalePendingWarning));
  else if (activeLease_) setWarning(activeLease_->warning.empty() ? nullptr
                                        : std::shared_ptr<const std::string>(activeLease_, &activeLease_->warning));
  else setWarning(internedWarning(front::kPendingWarning));
  // Rule 8: capacity wording.
  if (front::pendingAtResidentLimit(bool(activeLease_), overlay.hasGraphicsContentHash))
    setWarning(internedWarning(front::kCapacityWarning));
  pendingThisFrame_ = pendingThisFrame_ || (retainingScale && !failedScale) || !activeLease_;
  failedThisFrame_ = failedThisFrame_ || failedScale || (activeLease_ && !activeLease_->asset);
  return activeLease_ && activeLease_->asset ? activeLease_->asset->texture : nil;
}

std::shared_ptr<const graphics::GraphicsRunLayout> CoreTextOverlayRaster::activeRuns() const {
  return activeLease_ && activeLease_->asset ? activeLease_->asset->runs : nullptr;
}

std::shared_ptr<const graphics::GraphicsBackdropSet> CoreTextOverlayRaster::activeBackdrops() const {
  return activeLease_ && activeLease_->asset ? activeLease_->asset->backdrops : nullptr;
}

std::shared_ptr<const graphics::GraphicsPreparedResource> CoreTextOverlayRaster::preparedResource() {
  if (!activeLease_ || !activeLease_->asset || std::string_view(lastState_) != "ready") return {};
  if (activeResource_) return activeResource_;
  // Same prepared texture -> same resource object, with no allocation, while
  // anyone (an instance that adopted it) still holds it. Keyed by the cache
  // result's address: a live weak entry proves that result is still alive, so
  // the address cannot have been reused. Plan G2 D7; the D3D11 twin is
  // CompositorOverlayRaster::preparedResource.
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
      // Windows pushes the rounded band as a layer mask AND fills the rounded
      // band inside it (CompositorOverlayRaster.cpp PushLayer +
      // FillRoundedRectangle), so a corner pixel's coverage is applied twice.
      // Clip to the same D2D polygon and fill it, not the rectangle, so the
      // Mac corner is squared the same way. This context is bottom-up.
      CfRef<CGPathRef> path(radius > 0
          ? createPolygonPath(d2dcurves::roundedRect(0, 0, widthPx, heightPx, radius, flatteningTolerance(cg)), heightPx)
          : CGPathCreateWithRect(band, nullptr));
      if (!path) complete = false;
      else {
        CGContextAddPath(cg, path.get());
        CGContextClip(cg);
        const auto bg = colorFromArgb(layout.backgroundArgb, space.get());
        CGContextSetFillColorWithColor(cg, bg.get());
        CGContextAddPath(cg, path.get());
        CGContextFillPath(cg);
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
    // P9: glass plates travel with the prepared raster (BackdropBlur.h); a
    // layered-motion atlas keeps the drawing they sample their motion from.
    std::shared_ptr<graphics::GraphicsBackdropSet> backdrops =
        templated && !request.thumbnail ? graphics::collectBackdrops(drawing) : nullptr;
    if (runs) runs->drawing = std::make_shared<const graphics::GraphicsDrawing>(std::move(drawing));
    if (backdrops && runs) backdrops->motion = runs->drawing;

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
    entry->backdrops = std::move(backdrops);
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
