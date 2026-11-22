// CoreTextOverlayRaster (Metal graphics backend G1) — pixel tests read the
// prepared MTLTexture back at FULL resolution with getBytes, so glyph coverage
// is judged per pixel inside the rects the shared layout resolver
// (computeOverlayTileLayout) places, not by "a texture exists".
//
// Every geometric constant below comes from computeOverlayTileLayout's
// documented fractions (OverlayTileRaster.cpp), never from the raster under
// test, so a raster that drew in the wrong place cannot move its own
// expectations (plan-authoring rule 2).

#if COREVIDEO_WITH_METAL && !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS

#include <gtest/gtest.h>

#import <Metal/Metal.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "compositor/CoreTextOverlayRaster.h"
#include "graphics/GraphicsInstances.h"
#include "graphics/GraphicsTemplate.h"
#include "GraphicsMotionFixture.h"
#include "modules/OverlayTileRaster.h"

// ── thread-local allocation counter ─────────────────────────────────────────
// Global operator new/delete replaced by a malloc pass-through for the whole
// test binary. It counts ONLY on a thread that armed it, so every other test
// and thread behaves exactly as with the default allocator.
namespace {
thread_local bool tAllocCountArmed = false;
thread_local size_t tAllocCount = 0;
}  // namespace

void* operator new(std::size_t size) {
  if (tAllocCountArmed) ++tAllocCount;
  if (void* p = std::malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
  if (tAllocCountArmed) ++tAllocCount;
  if (void* p = std::malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  if (tAllocCountArmed) ++tAllocCount;
  return std::malloc(size ? size : 1);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  if (tAllocCountArmed) ++tAllocCount;
  return std::malloc(size ? size : 1);
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

namespace {

using namespace corevideo::modules;

struct Pixels {
  int width = 0, height = 0;
  std::vector<uint8_t> bgra;
  const uint8_t* at(int x, int y) const { return &bgra[(size_t(y) * width + x) * 4]; }
  int alpha(int x, int y) const { return at(x, y)[3]; }
};

Pixels readBack(id<MTLTexture> texture) {
  Pixels out;
  out.width = int(texture.width);
  out.height = int(texture.height);
  out.bgra.resize(size_t(out.width) * out.height * 4);
  [texture getBytes:out.bgra.data()
        bytesPerRow:NSUInteger(out.width) * 4
         fromRegion:MTLRegionMake2D(0, 0, NSUInteger(out.width), NSUInteger(out.height))
        mipmapLevel:0];
  return out;
}

struct Prepared {
  id<MTLTexture> texture = nil;
  std::string state, warning;
};

// Polls the raster the way a render loop does (beginFrame + lookup) until the
// worker publishes a terminal state. Never blocks inside the raster.
Prepared waitPrepared(CoreTextOverlayRaster& raster, id<MTLDevice> device,
                      const CompositorOverlayContent& overlay, int width, int height,
                      bool strict = false) {
  Prepared out;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    raster.beginFrame();
    out.texture = raster.rasterOverlayTexture(device, overlay, {0.f, 0.f, 1.f, 1.f}, width, height,
                                              {}, strict);
    out.state = raster.preparationState();
    out.warning = raster.lastWarning();
    if (out.state == "ready" || out.state == "failed") return out;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return out;
}

bool inRect(const OverlayTileRect& r, int x, int y) {
  return x >= std::floor(r.x) && x < std::ceil(r.x + r.width) && y >= std::floor(r.y) &&
         y < std::ceil(r.y + r.height);
}

#define DEVICE_OR_SKIP(var)                                                  \
  id<MTLDevice> var = MTLCreateSystemDefaultDevice();                        \
  if (!var) {                                                                \
    std::fprintf(stderr, "[metal-test] unavailable: no Metal device\n");    \
    return;                                                                  \
  }

CompositorOverlayContent lowerThird(const std::string& name, const std::string& title) {
  CompositorOverlayContent overlay;
  overlay.text = name;
  overlay.title = title;
  overlay.brandColor = "#ff0000";
  return overlay;
}

TEST(MetalOverlayRaster, LowerThirdGlyphsLandInTheirLineRectsAndNowhereElse) {
  DEVICE_OR_SKIP(device);
  CoreTextOverlayRaster raster;
  const int w = 600, h = 120;
  const auto overlay = lowerThird("ADA OTIENO", "AETHELRED LABS");
  const auto prepared = waitPrepared(raster, device, overlay, w, h);
  ASSERT_EQ(prepared.state, "ready") << prepared.warning;
  ASSERT_NE(prepared.texture, nil);
  const auto px = readBack(prepared.texture);
  ASSERT_EQ(px.width, w);
  ASSERT_EQ(px.height, h);

  // Fixture-derived geometry (OverlayTileRaster.cpp lower-third fractions).
  const OverlayTileRect nameRect{w * 0.06f, h * 0.18f, w * 0.88f - w * 0.06f, h * 0.34f};
  const OverlayTileRect titleRect{w * 0.06f, h * 0.56f, w * 0.88f - w * 0.06f, h * 0.28f};
  const OverlayTileRect accent{0.f, 0.f, w * 0.04f, float(h)};
  // The stray check allows one pixel of antialiasing bleed round each line
  // rect: a glyph's ink may start a fraction left of its pen origin (the "A"
  // of "ADA" does), exactly as D2D's does. Ink further out is a placement bug.
  auto grown = [](OverlayTileRect r) { return OverlayTileRect{r.x - 1, r.y - 1, r.width + 2, r.height + 2}; };
  const auto nameBound = grown(nameRect), titleBound = grown(titleRect);

  int nameInk = 0, titleInk = 0, stray = 0;
  int strayMinX = w, strayMaxX = -1, strayMinY = h, strayMaxY = -1;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const int a = px.alpha(x, y);
      if (a > 128 && inRect(nameRect, x, y)) ++nameInk;
      if (a > 128 && inRect(titleRect, x, y)) ++titleInk;
      if (a > 8 && !inRect(nameBound, x, y) && !inRect(titleBound, x, y) && !inRect(accent, x, y)) {
        ++stray;
        strayMinX = std::min(strayMinX, x); strayMaxX = std::max(strayMaxX, x);
        strayMinY = std::min(strayMinY, y); strayMaxY = std::max(strayMaxY, y);
      }
    }
  }
  EXPECT_GE(nameInk, 150) << "the name line carries no glyph coverage";
  EXPECT_GE(titleInk, 150) << "the title line carries no glyph coverage";
  EXPECT_EQ(stray, 0) << "ink outside the layout's line rects and accent bar: " << stray << " px in x "
                      << strayMinX << ".." << strayMaxX << ", y " << strayMinY << ".." << strayMaxY;

  // Accent bar: premultiplied opaque #ff0000 (BGRA 00 00 ff ff), exactly.
  const uint8_t* bar = px.at(int(w * 0.02f), h / 2);
  EXPECT_EQ(bar[0], 0);
  EXPECT_EQ(bar[1], 0);
  EXPECT_EQ(bar[2], 255);
  EXPECT_EQ(bar[3], 255);
  // Right of the text column, between the lines: transparent (no band).
  EXPECT_EQ(px.alpha(int(w * 0.95f), int(h * 0.54f)), 0);
}

TEST(MetalOverlayRaster, NameLineIsSemiboldAndTitleIsRegular) {
  DEVICE_OR_SKIP(device);
  CoreTextOverlayRaster raster;
  CompositorOverlayContent overlay;
  overlay.appearance.enabled = true;
  overlay.appearance.fontFamily = "Helvetica Neue";  // ships with macOS and has heavier faces
  overlay.appearance.nameSize = overlay.appearance.titleSize = 30;
  overlay.appearance.nameColor = overlay.appearance.titleColor = "#FFFFFF";
  overlay.appearance.backgroundOpacity = 0;  // band alpha 0: only glyphs carry alpha
  overlay.appearance.showLogo = false;
  overlay.text = "SAME WORDS HERE";
  overlay.org = "SAME WORDS HERE";
  const int w = 640, h = 120;
  const auto prepared = waitPrepared(raster, device, overlay, w, h);
  ASSERT_EQ(prepared.state, "ready") << prepared.warning;
  const auto px = readBack(prepared.texture);
  const auto layout = computeOverlayTileLayout(overlay, w, h);
  ASSERT_EQ(layout.textLines.size(), 2u);
  auto sum = [&](const OverlayTileRect& r) {
    double total = 0;
    for (int y = 0; y < h; ++y)
      for (int x = int(w * 0.10f); x < w; ++x)  // right of the accent bar
        if (inRect(r, x, y)) total += px.alpha(x, y);
    return total;
  };
  const double name = sum(layout.textLines[0].rect);
  const double title = sum(layout.textLines[1].rect);
  ASSERT_GT(title, 0.0);
  EXPECT_GT(name, title * 1.08) << "line 0 must be the semibold emphasis line";
}

TEST(MetalOverlayRaster, AnOverflowingLineEndsInAnEllipsisNotAClippedGlyph) {
  DEVICE_OR_SKIP(device);
  CoreTextOverlayRaster raster;
  const int w = 300, h = 60;
  const auto overlay = lowerThird(std::string(60, 'M'), "");
  const auto prepared = waitPrepared(raster, device, overlay, w, h);
  ASSERT_EQ(prepared.state, "ready") << prepared.warning;
  const auto px = readBack(prepared.texture);
  const OverlayTileRect nameRect{w * 0.06f, h * 0.18f, w * 0.88f - w * 0.06f, h * 0.34f};
  const float em = 0.72f * nameRect.height;
  int rightmost = -1;
  for (int x = 0; x < w; ++x)
    for (int y = 0; y < h; ++y)
      if (x > int(w * 0.05f) && px.alpha(x, y) > 64) rightmost = std::max(rightmost, x);
  ASSERT_GT(rightmost, 0);
  EXPECT_LT(rightmost, int(std::ceil(nameRect.x + nameRect.width)) + 1)
      << "the truncated line must end inside its column";
  int top = h, bottom = -1;
  for (int x = rightmost - int(0.3f * em); x <= rightmost; ++x)
    for (int y = 0; y < h; ++y)
      if (px.alpha(x, y) > 64) { top = std::min(top, y); bottom = std::max(bottom, y); }
  ASSERT_GE(bottom, top);
  EXPECT_LE(bottom - top + 1, int(0.35f * em) + 1)
      << "the last glyph spans " << (bottom - top + 1) << " rows: a clipped M, not an ellipsis";
}

TEST(MetalOverlayRaster, AppearanceBandIsRoundedAndCarriesTheLayoutBackground) {
  DEVICE_OR_SKIP(device);
  CoreTextOverlayRaster raster;
  CompositorOverlayContent overlay;
  overlay.appearance.enabled = true;
  overlay.appearance.fontFamily = "Helvetica Neue";
  overlay.appearance.cornerRadius = 16;
  overlay.appearance.backgroundColor = "#204060";
  overlay.appearance.backgroundOpacity = 1;
  overlay.appearance.showLogo = false;
  overlay.text = "NAME";
  overlay.org = "ORG";
  const int w = 600, h = 120;
  const auto prepared = waitPrepared(raster, device, overlay, w, h);
  ASSERT_EQ(prepared.state, "ready") << prepared.warning;
  const auto px = readBack(prepared.texture);
  EXPECT_EQ(px.alpha(1, 1), 0) << "the band's top-left corner must be rounded off";
  EXPECT_EQ(px.alpha(w - 2, h - 2), 0) << "the band's bottom-right corner must be rounded off";
  const uint8_t* band = px.at(w / 2, h - 3);
  EXPECT_EQ(band[0], 0x60);
  EXPECT_EQ(band[1], 0x40);
  EXPECT_EQ(band[2], 0x20);
  EXPECT_EQ(band[3], 0xff);

  CompositorOverlayContent plain = overlay;
  plain.appearance.enabled = false;
  const auto plainPrepared = waitPrepared(raster, device, plain, w, h);
  ASSERT_EQ(plainPrepared.state, "ready") << plainPrepared.warning;
  EXPECT_EQ(readBack(plainPrepared.texture).alpha(w / 2, h - 3), 0)
      << "without an appearance the band is the adapter's quad, never the raster's";
}

TEST(MetalOverlayRaster, ACaptionDrawsNoBand) {
  DEVICE_OR_SKIP(device);
  CoreTextOverlayRaster raster;
  CompositorOverlayContent caption;
  caption.isCaption = true;
  caption.appearance.enabled = true;
  caption.speaker = "SPEAKER";
  caption.text = "Caption body";
  const int w = 600, h = 120;
  const auto prepared = waitPrepared(raster, device, caption, w, h);
  ASSERT_EQ(prepared.state, "ready") << prepared.warning;
  const auto px = readBack(prepared.texture);
  EXPECT_EQ(px.alpha(int(w * 0.95f), int(h * 0.95f)), 0);
  EXPECT_EQ(px.alpha(w / 2, int(h * 0.46f)), 0) << "between the speaker and body lines";
  // The caption accent bar is the top 10% of the tile.
  EXPECT_EQ(px.alpha(w / 2, 2), 255);
}

TEST(MetalOverlayRaster, ACacheHitAllocatesNothing) {
  DEVICE_OR_SKIP(device);
  CoreTextOverlayRaster raster;
  // Both strings are longer than libc++'s 22-byte small-string buffer, so ANY
  // copy of the overlay on the hit path (e.g. building a Request) allocates
  // and this test sees it. Short strings would hide that copy (SSO).
  const auto overlay = lowerThird("ALLOCATION PROBE NAME LONGER THAN SSO",
                                  "NOTHING PER TICK ON A CACHE HIT, EVER");
  const auto prepared = waitPrepared(raster, device, overlay, 400, 80);
  ASSERT_EQ(prepared.state, "ready") << prepared.warning;
  size_t allocations = 0;
  bool sameTexture = true;
  for (int i = 0; i < 100; ++i) {
    tAllocCount = 0;
    tAllocCountArmed = true;
    raster.beginFrame();
    id<MTLTexture> texture = raster.rasterOverlayTexture(device, overlay, {0.f, 0.f, 1.f, 1.f}, 400, 80);
    tAllocCountArmed = false;
    allocations += tAllocCount;
    sameTexture = sameTexture && texture == prepared.texture;
  }
  EXPECT_EQ(allocations, 0u) << "a cache hit on the render path must not allocate";
  EXPECT_TRUE(sameTexture);
  EXPECT_STREQ(raster.preparationState(), "ready");
}

// The cache key includes the raster's pixel size: the same content at a new
// band size (a Tiles reflow, a Program vs monitor canvas) is its own
// preparation, never the old texture stretched.
TEST(MetalOverlayRaster, ATileIsPreparedPerPixelSize) {
  DEVICE_OR_SKIP(device);
  CoreTextOverlayRaster raster;
  const auto overlay = lowerThird("SIZE PROBE", "TWO SIZES");
  const auto small = waitPrepared(raster, device, overlay, 200, 50);
  ASSERT_EQ(small.state, "ready") << small.warning;
  const auto large = waitPrepared(raster, device, overlay, 400, 100);
  ASSERT_EQ(large.state, "ready") << large.warning;
  EXPECT_EQ(small.texture.width, 200u);
  EXPECT_EQ(large.texture.width, 400u);
  EXPECT_EQ(large.texture.height, 100u);
  EXPECT_EQ(raster.cacheCounters().completed, 2u);
}

TEST(MetalOverlayRaster, AdmissionRulesMatchWindowsWording) {
  DEVICE_OR_SKIP(device);
  CoreTextOverlayRaster raster;
  auto huge = lowerThird(std::string(70000, 'x'), "");
  raster.beginFrame();
  EXPECT_EQ(raster.rasterOverlayTexture(device, huge, {0, 0, 1, 1}, 100, 100), nil);
  EXPECT_STREQ(raster.preparationState(), "failed");
  EXPECT_EQ(raster.lastWarning(), "Graphics content exceeds preparation metadata budget.");
  EXPECT_TRUE(raster.failedThisFrame());

  raster.beginFrame();
  EXPECT_EQ(raster.rasterOverlayTexture(device, lowerThird("A", ""), {0, 0, 1, 1}, 5000, 100), nil);
  EXPECT_STREQ(raster.preparationState(), "failed");
  EXPECT_EQ(raster.lastWarning(), "Invalid graphics raster dimensions.");

  raster.beginFrame();
  EXPECT_FALSE(raster.failedThisFrame()) << "beginFrame clears the sticky per-frame flags";
  EXPECT_STREQ(raster.preparationState(), "unprepared");

  auto templated = lowerThird("T", "");
  templated.graphicsTemplate = std::make_shared<corevideo::graphics::GraphicsTemplateInput>();
  const auto prepared = waitPrepared(raster, device, templated, 200, 50);
  EXPECT_EQ(prepared.state, "failed");
  // A template input with no definition cannot be resolved (G2: templates are
  // otherwise prepared on Metal).
  EXPECT_EQ(prepared.warning, "Graphics template definition is missing.");
}

TEST(MetalOverlayRaster, AMissingFontWithAnAppearanceWarnsAndStillDraws) {
  DEVICE_OR_SKIP(device);
  CoreTextOverlayRaster raster;
  CompositorOverlayContent overlay;
  overlay.appearance.enabled = true;
  overlay.appearance.fontFamily = "NoSuchFamily CVP";
  overlay.appearance.backgroundOpacity = 0;
  overlay.appearance.showLogo = false;
  overlay.text = "FALLBACK";
  const int w = 400, h = 100;
  const auto prepared = waitPrepared(raster, device, overlay, w, h);
  ASSERT_EQ(prepared.state, "ready") << prepared.warning;
  EXPECT_EQ(prepared.warning, "Font unavailable; using Helvetica Neue.");
  const auto px = readBack(prepared.texture);
  const auto layout = computeOverlayTileLayout(overlay, w, h);
  ASSERT_FALSE(layout.textLines.empty());
  int ink = 0;
  for (int y = 0; y < h; ++y)
    for (int x = int(w * 0.1f); x < w; ++x)
      if (inRect(layout.textLines[0].rect, x, y) && px.alpha(x, y) > 128) ++ink;
  EXPECT_GE(ink, 100);

  const auto strict = waitPrepared(raster, device, overlay, w, h, /*strict=*/true);
  EXPECT_EQ(strict.state, "failed");
  EXPECT_EQ(strict.warning, "Requested graphics font is unavailable; choose an installed font.");
}

// Owner decision 2026-11-21: a Windows family in the substitution table
// resolves (Segoe UI -> bundled Selawik) with a warning and NEVER fails, strict
// mode included. The appearance default family is "Segoe UI".
TEST(MetalOverlayRaster, SegoeUiResolvesToBundledSelawikWithAWarningEvenWhenStrict) {
  DEVICE_OR_SKIP(device);
  CoreTextOverlayRaster raster;
  CompositorOverlayContent overlay;
  overlay.appearance.enabled = true;
  overlay.appearance.fontFamily = "Segoe UI";
  overlay.appearance.backgroundOpacity = 0;
  overlay.appearance.showLogo = false;
  overlay.text = "SUBSTITUTED";
  const int w = 400, h = 100;
  for (const bool strict : {false, true}) {
    const auto prepared = waitPrepared(raster, device, overlay, w, h, strict);
    ASSERT_EQ(prepared.state, "ready") << "strict=" << strict << " " << prepared.warning;
    EXPECT_EQ(prepared.warning, "Font Segoe UI is not installed on this Mac; using Selawik.");
    const auto px = readBack(prepared.texture);
    const auto layout = computeOverlayTileLayout(overlay, w, h);
    int ink = 0;
    for (int y = 0; y < h; ++y)
      for (int x = int(w * 0.1f); x < w; ++x)
        if (inRect(layout.textLines[0].rect, x, y) && px.alpha(x, y) > 128) ++ink;
    EXPECT_GE(ink, 100) << "strict=" << strict;
  }
}

// ── Template raster (Metal graphics backend G2) ─────────────────────────────
//
// Each fixture is a 320x180 template rastered at 320x180, so template units
// ARE pixels (scale = heightPx / drawing.height = 1). Every expected value is
// computed from the manifest (box, fontSize, lineHeight, colours), never read
// back from the raster.

CompositorOverlayContent templated(const std::string& layers, std::string* loadError = nullptr) {
  const std::string manifest = R"({"schemaVersion":1,"templateId":"metal-g2","templateVersion":1,"name":"G2",)"
      R"("family":"title","canvas":{"width":1920,"height":1080},"bounds":{"width":320,"height":180},)"
      R"("brandTokens":[],"fields":[],"animations":{"in":{"type":"none"},"out":{"type":"none"}},"layers":)" + layers + "}";
  std::string error;
  auto input = std::make_shared<corevideo::graphics::GraphicsTemplateInput>();
  input->definition = corevideo::graphics::GraphicsTemplate::load(manifest, error);
  if (loadError) *loadError = error;
  input->fields = corevideo::rpc::Json::Object{};
  input->brand = corevideo::rpc::Json::Object{};
  CompositorOverlayContent content;
  content.appearance.enabled = true;
  content.graphicsTemplate = input;
  // Instances always carry a content hash; the raster keys on it (the overlay
  // signature does not hash template content). Without it two different
  // fixtures would share one cache entry.
  if (input->definition) {
    content.graphicsContentHash = input->definition->hash();
    content.hasGraphicsContentHash = true;
  }
  return content;
}

TEST(MetalOverlayRaster, TemplateShapesDrawCoverageAndInsetStrokes) {
  DEVICE_OR_SKIP(device);
  CoreTextOverlayRaster raster;
  std::string error;
  const auto content = templated(R"([
      {"id":"oval","type":"ellipse","x":20,"y":20,"width":80,"height":60,"fill":"#FF0000","stroke":"#00FF00","strokeWidth":4},
      {"id":"rule","type":"line","x":120,"y":20,"width":60,"height":60,"diagonal":"up","stroke":"#FFFFFF","strokeWidth":4},
      {"id":"plate","type":"rect","x":200,"y":20,"width":60,"height":60,"cornerRadius":8,"fill":"#FF0000","stroke":"#00FF00","strokeWidth":4}])", &error);
  ASSERT_TRUE(content.graphicsTemplate->definition) << error;
  const auto prepared = waitPrepared(raster, device, content, 320, 180, true);
  ASSERT_EQ(prepared.state, "ready") << prepared.warning;
  const auto px = readBack(prepared.texture);
  auto expectColor = [&](int x, int y, int b, int g, int r, const char* what) {
    const uint8_t* p = px.at(x, y);
    EXPECT_NEAR(p[0], b, 3) << what;
    EXPECT_NEAR(p[1], g, 3) << what;
    EXPECT_NEAR(p[2], r, 3) << what;
    EXPECT_EQ(p[3], 255) << what;
  };
  expectColor(60, 50, 0, 0, 255, "ellipse interior");
  expectColor(60, 22, 0, 255, 0, "ellipse inset stroke (top edge)");
  EXPECT_EQ(px.alpha(22, 22), 0) << "outside the ellipse";
  EXPECT_GT(px.alpha(150, 50), 200) << "the rising diagonal passes the box centre";
  EXPECT_EQ(px.alpha(125, 25), 0) << "a rising line never reaches the top-left corner";
  EXPECT_GT(px.at(150, 50)[2], 200);
  expectColor(230, 22, 0, 255, 0, "rect inset stroke (top edge)");
  expectColor(230, 50, 0, 0, 255, "rect interior");
  EXPECT_EQ(px.alpha(200, 20), 0) << "rounded corner";
}

TEST(MetalOverlayRaster, TemplateGradientRunsBetweenItsStops) {
  DEVICE_OR_SKIP(device);
  CoreTextOverlayRaster raster;
  const auto content = templated(R"([{"id":"g","type":"rect","x":0,"y":0,"width":320,"height":40,
      "fill":{"stops":["#FF0000","#0000FF"],"direction":"horizontal"}}])");
  ASSERT_TRUE(content.graphicsTemplate->definition);
  const auto prepared = waitPrepared(raster, device, content, 320, 180, true);
  ASSERT_EQ(prepared.state, "ready") << prepared.warning;
  const auto px = readBack(prepared.texture);
  EXPECT_GT(px.at(0, 20)[2], 245);
  EXPECT_LT(px.at(0, 20)[0], 10);
  EXPECT_GT(px.at(319, 20)[0], 245);
  EXPECT_LT(px.at(319, 20)[2], 10);
  int previousRed = 256;
  for (int x = 0; x < 320; x += 32) {
    const int red = px.at(x, 20)[2];
    EXPECT_LE(red, previousRed) << "red must fall monotonically left to right, x=" << x;
    previousRed = red;
  }
  EXPECT_NEAR(px.at(160, 20)[2], 128, 12);
  EXPECT_EQ(px.alpha(160, 60), 0) << "below the rect";
}

// Uniform line spacing (spec 3.4 / 5.6): line n's baseline is box.y +
// n * fontSize * lineHeight + 0.8 * fontSize, whatever the font's ascent.
// "H" sits on the baseline, so the last heavily-inked row of each band is the
// baseline row minus one. maxLines 2 + wrap-then-ellipsis: two bands, nothing
// in a third, and the second line ends in an ellipsis.
TEST(MetalOverlayRaster, TemplateTextBaselinesAreUniformAndWrapStopsAtMaxLinesWithAnEllipsis) {
  DEVICE_OR_SKIP(device);
  CoreTextOverlayRaster raster;
  const auto content = templated(R"([{"id":"t","type":"text","x":10,"y":10,"width":150,"height":150,
      "text":"HHHH HHHH HHHH HHHH HHHH HHHH HHHH HHHH","font":"Arial","fontSize":30,"lineHeight":1.2,
      "color":"#FFFFFF","maxLines":2,"overflow":"wrap-then-ellipsis"}])");
  const auto prepared = waitPrepared(raster, device, content, 320, 180, true);
  ASSERT_EQ(prepared.state, "ready") << prepared.warning;
  const auto px = readBack(prepared.texture);
  const int y0 = 10, line = 36, baseline = 24;  // 30 * 1.2, 0.8 * 30
  auto lastHeavyRow = [&](int top, int bottom) {
    int last = -1;
    for (int y = top; y < bottom; ++y)
      for (int x = 0; x < 320; ++x)
        if (px.alpha(x, y) > 128) { last = y; break; }
    return last;
  };
  EXPECT_NEAR(lastHeavyRow(y0, y0 + line), y0 + baseline - 1, 1) << "line 1 baseline";
  EXPECT_NEAR(lastHeavyRow(y0 + line, y0 + 2 * line), y0 + line + baseline - 1, 1) << "line 2 baseline";
  int belowTwoLines = 0;
  for (int y = y0 + 2 * line + 2; y < 180; ++y)
    for (int x = 0; x < 320; ++x)
      if (px.alpha(x, y) > 8) ++belowTwoLines;
  EXPECT_EQ(belowTwoLines, 0) << "maxLines 2 draws no third line";
  // The second line's last glyph is an ellipsis: its ink spans a few rows at
  // the baseline, not an "H" stem (cap height ~0.72 em = 21 rows).
  int rightmost = -1;
  for (int x = 0; x < 320; ++x)
    for (int y = y0 + line; y < y0 + 2 * line; ++y)
      if (px.alpha(x, y) > 64) rightmost = std::max(rightmost, x);
  ASSERT_GT(rightmost, 0);
  EXPECT_LE(rightmost, 160) << "inside the box";
  int top = 180, bottom = -1;
  for (int x = rightmost - 6; x <= rightmost; ++x)
    for (int y = y0 + line; y < y0 + 2 * line; ++y)
      if (px.alpha(x, y) > 64) { top = std::min(top, y); bottom = std::max(bottom, y); }
  EXPECT_LE(bottom - top + 1, 8) << "the second line must end in an ellipsis";
}

TEST(MetalOverlayRaster, TemplateCenterAlignmentCentresTheInk) {
  DEVICE_OR_SKIP(device);
  CoreTextOverlayRaster raster;
  const auto content = templated(R"([{"id":"t","type":"text","x":20,"y":20,"width":280,"height":60,
      "text":"HHH","font":"Arial","fontSize":40,"color":"#FFFFFF","align":"center"}])");
  const auto prepared = waitPrepared(raster, device, content, 320, 180, true);
  ASSERT_EQ(prepared.state, "ready") << prepared.warning;
  const auto px = readBack(prepared.texture);
  int left = 320, right = -1;
  for (int y = 0; y < 180; ++y)
    for (int x = 0; x < 320; ++x)
      if (px.alpha(x, y) > 128) { left = std::min(left, x); right = std::max(right, x); }
  ASSERT_GE(right, left);
  EXPECT_NEAR((left + right) / 2.0, 20 + 280 / 2.0, 2.5) << left << ".." << right;
}

TEST(MetalOverlayRaster, TemplateTextEffectsDrawShadowOutlineAndFill) {
  DEVICE_OR_SKIP(device);
  CoreTextOverlayRaster raster;
  const auto content = templated(R"([{"id":"text","type":"text","x":20,"y":15,"width":260,"height":130,
      "text":"Ada","font":"Segoe UI","fontSize":64,"color":"#FFFFFF",
      "outline":{"color":"#FF0000","width":3},"shadow":{"color":"#00FF00","x":12,"y":20,"blur":5}}])");
  const auto prepared = waitPrepared(raster, device, content, 320, 180, true);
  ASSERT_EQ(prepared.state, "ready") << prepared.warning;
  EXPECT_EQ(prepared.warning, "Font Segoe UI is not installed on this Mac; using Selawik.");
  const auto px = readBack(prepared.texture);
  int fill = 0, outline = 0, shadow = 0;
  for (int y = 0; y < 180; ++y)
    for (int x = 0; x < 320; ++x) {
      const uint8_t* p = px.at(x, y);
      if (p[0] > 220 && p[1] > 220 && p[2] > 220) ++fill;
      if (p[2] > 180 && p[1] < 50 && p[0] < 50) ++outline;
      if (p[1] > 180 && p[2] < 50 && p[0] < 50) ++shadow;
    }
  EXPECT_GT(fill, 100);
  EXPECT_GT(outline, 100);
  EXPECT_GT(shadow, 100);
}

TEST(MetalOverlayRaster, TemplateFontsFailOnlyWhenNeitherInstalledNorInTheTable) {
  DEVICE_OR_SKIP(device);
  CoreTextOverlayRaster raster;
  const auto missing = templated(R"([{"id":"t","type":"text","x":10,"y":10,"width":300,"height":60,
      "text":"Missing","font":"CoreVideo Missing Font 9c2a8f77","fontSize":30,"color":"#FFFFFF"}])");
  const auto failed = waitPrepared(raster, device, missing, 320, 180, true);
  EXPECT_EQ(failed.state, "failed");
  EXPECT_EQ(failed.warning, "Requested graphics font is unavailable: CoreVideo Missing Font 9c2a8f77");
  const auto consolas = templated(R"([{"id":"t","type":"text","x":10,"y":10,"width":300,"height":60,
      "text":"Mono","font":"Consolas","fontSize":30,"color":"#FFFFFF"}])");
  const auto substituted = waitPrepared(raster, device, consolas, 320, 180, true);
  EXPECT_EQ(substituted.state, "ready") << substituted.warning;
  EXPECT_EQ(substituted.warning, "Font Consolas is not installed on this Mac; using Menlo.");
}

TEST(MetalOverlayRaster, ALayeredTemplatePreparesAnAtlasWithOneSlicePerRun) {
  DEVICE_OR_SKIP(device);
  CoreTextOverlayRaster raster;
  std::string error;
  auto input = std::make_shared<corevideo::graphics::GraphicsTemplateInput>();
  input->definition = corevideo::graphics::GraphicsTemplate::load(
      corevideo::graphics::test::motionFixture("bar-first.json"), error);
  ASSERT_TRUE(input->definition) << error;
  ASSERT_TRUE(input->definition->motionSummary().layered);
  input->fields = input->definition->defaultFields();
  input->brand = corevideo::graphics::test::motionBrand();
  CompositorOverlayContent content;
  content.appearance.enabled = true;
  content.graphicsTemplate = input;
  const auto prepared = waitPrepared(raster, device, content, 1060, 168, true);
  ASSERT_EQ(prepared.state, "ready") << prepared.warning;
  const auto runs = raster.activeRuns();
  ASSERT_TRUE(runs);
  EXPECT_GT(runs->runs.size(), 1u);
  EXPECT_EQ(runs->slices.size(), runs->runs.size());
  EXPECT_EQ(int(prepared.texture.width), runs->atlasWidth);
  EXPECT_EQ(int(prepared.texture.height), runs->atlasHeight);
  int inkedSlices = 0;
  const auto px = readBack(prepared.texture);
  for (const auto& slice : runs->slices) {
    bool ink = false;
    for (int y = slice.y; y < slice.y + slice.height && !ink; ++y)
      for (int x = slice.x; x < slice.x + slice.width && !ink; ++x)
        ink = px.alpha(x, y) > 0;
    inkedSlices += ink ? 1 : 0;
  }
  EXPECT_EQ(inkedSlices, int(runs->slices.size())) << "every run is drawn into its own slice";
}

// G2's facts loop calls preparedResource() for every preparation on every
// Program tick. Once anyone holds the resource (the instance adopts it), the
// same prepared texture must yield the SAME resource pointer, with no heap
// allocation. Identity matters too: the adapter only pushes a "ready" fact
// when the resource differs from the plan's accepted one (D3D11 parity).
TEST(MetalOverlayRaster, PreparedResourceIsStableAndAllocationFree) {
  DEVICE_OR_SKIP(device);
  CoreTextOverlayRaster raster;
  const auto overlay = lowerThird("PREPARED RESOURCE NAME LONGER THAN SSO", "STABLE IDENTITY ACROSS TICKS");
  ASSERT_EQ(waitPrepared(raster, device, overlay, 400, 80).state, "ready");
  const auto held = raster.preparedResource();
  ASSERT_TRUE(held);
  EXPECT_EQ(held->backend, corevideo::graphics::GraphicsPreparedResource::Backend::Metal);
  EXPECT_EQ(held->width, 400);
  EXPECT_EQ(held->height, 80);
  size_t allocations = 0;
  bool same = true;
  for (int tick = 0; tick < 50; ++tick) {
    raster.beginFrame();
    raster.rasterOverlayTexture(device, overlay, {0.f, 0.f, 1.f, 1.f}, 400, 80);
    tAllocCount = 0;
    tAllocCountArmed = true;
    const auto again = raster.preparedResource();
    tAllocCountArmed = false;
    allocations += tAllocCount;
    same = same && again == held;
  }
  EXPECT_TRUE(same) << "the same prepared texture must keep one resource identity";
  EXPECT_EQ(allocations, 0u) << "preparedResource() allocated on the per-tick path";
  // Different content is a different resource.
  const auto other = lowerThird("ANOTHER NAME", "ANOTHER TITLE");
  ASSERT_EQ(waitPrepared(raster, device, other, 400, 80).state, "ready");
  EXPECT_NE(raster.preparedResource(), held);
}

// Review finding (G2): the adapter's graphicsPreparations loop copied the
// raster's warning into every fact, every Program frame. Segoe UI is the
// default family, so nearly every graphic carries the 57-character Selawik
// substitution warning: a steady-state heap allocation per graphic per frame
// on the render thread. The facts must carry the warning without copying it.
// Fixture: the SAME look and fields twice, differing only in the font. Arial
// is installed (no warning); Segoe UI is substituted (a warning far longer
// than the small-string buffer). Steady-state render() must allocate exactly
// as much in both. Facts still deliver the warning text (confirmatory).
TEST(MetalOverlayRaster, AdapterFactsCarryTheWarningWithoutAPerTickCopy) {
  DEVICE_OR_SKIP(device);
  using corevideo::rpc::Json;
  std::string lastReason;
  size_t preparationFacts = 0;
  auto steadyAllocations = [&](const std::string& font) -> size_t {
    auto compositor = createMetalCompositor();
    EXPECT_TRUE(compositor);
    if (!compositor) return 0;
    corevideo::graphics::GraphicsInstances graphics{"test-core"};
    int sequence = 0;
    const auto command = [&](const std::string& type, Json::Object payload) {
      payload["schemaVersion"] = 1; payload["requestId"] = "alloc-" + std::to_string(++sequence);
      payload["clientId"] = "alloc-qa"; payload["coreEpoch"] = "test-core"; payload["instanceId"] = "guest";
      payload["expectedRevision"] = 1;
      return graphics.apply(Json::Object{{"type", type}, {"graphics", std::move(payload)}});
    };
    EXPECT_TRUE(command("graphics-instance-create", {{"templateId", "compact-solid"}, {"templateVersion", 1},
        {"scope", "global"}, {"fields", Json::Object{{"name", "Allocation guest"}}},
        {"style", Json::Object{{"fontFamily", font}}}}));
    // Cued and waiting (an operator's everyday state): the accepted cue is
    // re-admitted on EVERY Program frame through the graphicsPreparations
    // facts loop, so its warning travels in a fact every frame.
    EXPECT_TRUE(command("graphics-instance-cue", {}));
    const auto row = [&]() -> Json { return graphics.snapshot().get("instances")->asArray().front(); };
    const auto render = [&](bool counted) {
      CompositorRenderPlan plan; plan.width = 320; plan.height = 180;
      graphics.append(plan, false);
      tAllocCount = 0; tAllocCountArmed = counted;
      auto frame = compositor->render(plan, {});
      tAllocCountArmed = false;
      const size_t allocations = tAllocCount;
      for (const auto& fact : frame.graphicsFacts) {
        if (fact.instanceId == "guest" && fact.state == "ready" && !fact.reasonText().empty()) lastReason = fact.reasonText();
        if (counted && fact.instanceId == "guest" && fact.state == "ready") ++preparationFacts;
      }
      graphics.observe(frame);
      return allocations;
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (row().getNumber("cueRevision") != 1 && std::chrono::steady_clock::now() < deadline) {
      render(false); std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_EQ(row().getNumber("cueRevision"), 1.) << font;
    for (int i = 0; i < 5; ++i) render(false);
    size_t total = 0;
    for (int i = 0; i < 20; ++i) total += render(true);
    return total;
  };
  lastReason.clear();
  const size_t withoutWarning = steadyAllocations("Arial");
  EXPECT_TRUE(lastReason.empty()) << lastReason;
  EXPECT_GE(preparationFacts, 20u) << "the fixture must exercise the per-frame facts loop";
  preparationFacts = 0;
  const size_t withWarning = steadyAllocations("Segoe UI");
  EXPECT_GE(preparationFacts, 20u);
  EXPECT_EQ(lastReason, "Font Segoe UI is not installed on this Mac; using Selawik.") << "facts still carry the warning";
  EXPECT_EQ(withWarning, withoutWarning)
      << "a substitution warning added " << (long long)(withWarning - withoutWarning) << " allocations over 20 frames";
}

}  // namespace

#endif
