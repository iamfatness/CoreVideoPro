#include "compositor/CoreTextOverlayRaster.h"

// Same three gates as MetalCompositorAdapter.mm: non-stub, dev adapters, Metal.
#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_METAL

#import <CoreText/CoreText.h>
#import <ImageIO/ImageIO.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string_view>

#include "compositor/CompositorFaultInjection.h"
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

// ── Font resolution: the ONE seam G2's GraphicsFontResolver replaces ─────────
//
// Order: (1) the exact family installed in the system collection; (2) the
// explicit substitution table; (3) unavailable. Core Text's own automatic
// fallback is never used for the PRIMARY family, so the same document always
// resolves the same way. Glyph fallback for characters a family lacks is
// still Core Text's cascade.
//
// G2 (owner decision, 2026-11-21): the table becomes Segoe UI / Segoe UI
// Semibold -> bundled Selawik (after the OFL licence check), Bahnschrift,
// Cambria, Consolas -> chosen system fonts, plus bundled-font registration.
// A table substitution WARNS and never fails. Until then it is empty.
struct FontSubstitution {
  std::string_view requested;
  std::string_view macFamily;
};
constexpr std::array<FontSubstitution, 0> kFontSubstitutions{};

// The family a missing legacy font falls back to (plan default D1): the
// fallback the shipped Mac raster already used.
constexpr const char* kFallbackFontFamily = "Helvetica Neue";

bool familyInstalled(const std::string& family) {
  if (family.empty()) return false;
  const auto name = cfString(family);
  if (!name) return false;
  const void* keys[] = {kCTFontFamilyNameAttribute};
  const void* values[] = {name.get()};
  CfRef<CFDictionaryRef> attributes(CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
      &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
  CfRef<CTFontDescriptorRef> descriptor(CTFontDescriptorCreateWithAttributes(attributes.get()));
  const void* mandatoryKeys[] = {kCTFontFamilyNameAttribute};
  CfRef<CFSetRef> mandatory(CFSetCreate(kCFAllocatorDefault, mandatoryKeys, 1, &kCFTypeSetCallBacks));
  CfRef<CTFontDescriptorRef> match(CTFontDescriptorCreateMatchingFontDescriptor(descriptor.get(), mandatory.get()));
  return bool(match);
}

struct ResolvedFontFamily {
  std::string family;
  bool available = false;     // requested family, or a table substitute, is installed
  bool substituted = false;   // came from kFontSubstitutions
};

ResolvedFontFamily resolveRasterFont(const std::string& requested) {
  if (familyInstalled(requested)) return {requested, true, false};
  for (const auto& row : kFontSubstitutions) {
    if (row.requested == requested && familyInstalled(std::string(row.macFamily)))
      return {std::string(row.macFamily), true, true};
  }
  return {kFallbackFontFamily, false, false};
}

// Nearest real face in `family` to `weight` (0 regular, 0.3 semibold). Core
// Text does not synthesize bold; a family with no heavier face draws regular.
CfRef<CTFontRef> createFont(const std::string& family, CGFloat weight, CGFloat size) {
  auto name = cfString(family);
  if (!name) name = cfString(kFallbackFontFamily);
  CFNumberRef weightNumber = CFNumberCreate(kCFAllocatorDefault, kCFNumberCGFloatType, &weight);
  const void* traitKeys[] = {kCTFontWeightTrait};
  const void* traitValues[] = {weightNumber};
  CfRef<CFDictionaryRef> traits(CFDictionaryCreate(kCFAllocatorDefault, traitKeys, traitValues, 1,
      &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
  CFRelease(weightNumber);
  const void* keys[] = {kCTFontFamilyNameAttribute, kCTFontTraitsAttribute};
  const void* values[] = {name.get(), traits.get()};
  CfRef<CFDictionaryRef> attributes(CFDictionaryCreate(kCFAllocatorDefault, keys, values, 2,
      &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
  CfRef<CTFontDescriptorRef> descriptor(CTFontDescriptorCreateWithAttributes(attributes.get()));
  const void* mandatoryKeys[] = {kCTFontFamilyNameAttribute};
  CfRef<CFSetRef> mandatory(CFSetCreate(kCFAllocatorDefault, mandatoryKeys, 1, &kCFTypeSetCallBacks));
  CfRef<CTFontDescriptorRef> match(CTFontDescriptorCreateMatchingFontDescriptor(descriptor.get(), mandatory.get()));
  return CfRef<CTFontRef>(CTFontCreateWithFontDescriptor(match ? match.get() : descriptor.get(), size, nullptr));
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
  const auto font = createFont(family, emphasis ? 0.3 : 0.0, fontSize);
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

}  // namespace

CoreTextOverlayRaster::CoreTextOverlayRaster()
    : cache_([this](const Request& request) { return prepareRaster(request); },
             256ull * 1024 * 1024, 32, {}, 64) {}

CoreTextOverlayRaster::~CoreTextOverlayRaster() = default;

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
      lastWarning_ = "Graphics content exceeds preparation metadata budget.";
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
    lastWarning_ = "Invalid graphics raster dimensions.";
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
    uint64_t admissionBytes = uint64_t(width) * height * 8;
    if (overlay.graphicsTemplate && overlay.graphicsTemplate->definition) {
      const auto& definition = *overlay.graphicsTemplate->definition;
      const auto& motion = definition.motionSummary();
      if (motion.layered && !overlay.graphicsTemplate->wholeGraphicMotion) {
        const auto maxRuns = 2 * motion.units + 1;
        const auto maxHeight = std::min<uint64_t>(16384, maxRuns * (height + 3) - 2);
        const auto maxWidth = uint64_t(std::min(16384., 1 + std::ceil(height * definition.width() / definition.height())));
        admissionBytes = std::min<uint64_t>(256ull * 1024 * 1024, maxWidth * maxHeight * 4);
      }
    }
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
  if (failedScale) lastWarning_ = scaleWarning + " Retaining the accepted raster.";
  else if (retainingScale) lastWarning_ = "Graphics scale preparation pending; retaining the accepted raster.";
  else if (activeLease_) lastWarning_ = activeLease_->warning;
  else lastWarning_ = "Graphics preparation pending.";
  // Rule 8: capacity wording.
  if (!activeLease_ && overlay.hasGraphicsContentHash) {
    const auto capacity = graphics::GraphicsResourceBudget::process().counters();
    if (capacity.residentResources >= capacity.residentLimit)
      lastWarning_ = "Graphics preparation pending at the shared 16-revision capacity limit.";
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
  return resource;
}

// Preparation worker only. Never runs on the render thread.
CoreTextOverlayRaster::Cache::Result CoreTextOverlayRaster::prepareRaster(const Request& request) {
  @autoreleasepool {
    compositor::invokeOverlayRasterPrepareHook();
    const auto& overlay = request.overlay;
    const int widthPx = request.width, heightPx = request.height;
    if (!request.device) return {{}, 0, "Native graphics preparation unavailable."};
    if (overlay.graphicsTemplate) return {{}, 0, "Template graphics are not prepared on Metal yet."};
    std::string warning;
    auto layout = computeOverlayTileLayout(overlay, widthPx, heightPx);
    ImageInput image = layout.hasImage
        ? (overlay.graphicsImage ? openPackagedImage(*overlay.graphicsImage) : openFileImage(overlay.imageUri))
        : ImageInput{};

    // Font: the Windows check runs only for an appearance; elsewhere a missing
    // family falls back silently, as DirectWrite's does (plan D1).
    const auto font = resolveRasterFont(layout.fontFamily);
    if (overlay.appearance.enabled && !font.available) {
      if (request.strictResources)
        return {{}, 0, "Requested graphics font is unavailable; choose an installed font."};
      warning = std::string("Font unavailable; using ") + kFallbackFontFamily + ".";
    } else if (font.substituted) {
      warning = "Font " + layout.fontFamily + " is not installed on this Mac; using " + font.family + ".";
    }
    layout.fontFamily = font.family;

    const uint64_t textureBytes = uint64_t(widthPx) * heightPx * 4;
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

    const size_t stride = size_t(widthPx) * 4;
    std::vector<uint8_t> pixels(stride * size_t(heightPx), 0);  // transparent
    CfRef<CGColorSpaceRef> space(CGColorSpaceCreateDeviceRGB());
    // Premultiplied BGRA, exactly what the (One, 1-SrcAlpha) overlay pipeline
    // blends: no post pass (the #350 rule). DeviceRGB: colours are not managed.
    CfRef<CGContextRef> context(CGBitmapContextCreate(pixels.data(), size_t(widthPx), size_t(heightPx), 8, stride,
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
    CGContextSaveGState(cg);
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
      CfRef<CGImageRef> decoded(image.source ? CGImageSourceCreateImageAtIndex(image.source.get(), 0, nullptr) : nullptr);
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
    CGContextRestoreGState(cg);
    CGContextFlush(cg);
    context = CfRef<CGContextRef>();
    image = ImageInput{};

    if (request.strictResources && !complete)
      return {{}, 0, warning.empty() ? "Graphics resources could not be prepared completely." : warning};

    MTLTextureDescriptor* descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:NSUInteger(widthPx)
                                                          height:NSUInteger(heightPx)
                                                       mipmapped:NO];
    descriptor.usage = MTLTextureUsageShaderRead;
    descriptor.storageMode = MTLStorageModeShared;
    id<MTLTexture> texture = [request.device newTextureWithDescriptor:descriptor];
    if (!texture) return {{}, 0, "Graphics texture creation failed."};
    [texture replaceRegion:MTLRegionMake2D(0, 0, NSUInteger(widthPx), NSUInteger(heightPx))
               mipmapLevel:0
                 withBytes:pixels.data()
               bytesPerRow:stride];
    auto entry = std::make_shared<MetalRasterTex>();
    entry->budget = budget;
    entry->texture = texture;
    // Drop the CPU copy before returning its admission charge.
    std::vector<uint8_t>().swap(pixels);
    budget->finishPreparation();
    return {std::move(entry), textureBytes, warning};
  }
}

}  // namespace corevideo::modules

#endif  // !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_METAL
