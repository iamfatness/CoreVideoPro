// Bundled pack fonts on Windows (graphics styles P8, slice 7): DirectWrite
// resolves a pack's font from the private collection (never installed
// system-wide), measurement and the raster agree, and a missing pack font
// substitutes and warns even for a strict graphic. Strict pack-font mode is
// the test switch that turns the substitution into a failure.

#if COREVIDEO_WITH_D3D11 && !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d11.h>
#include <dwrite.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

#include "compositor/ComPtrLite.h"
#include "compositor/CompositorOverlayRaster.h"
#include "graphics/GraphicsFontsDirectWrite.h"
#include "graphics/GraphicsPack.h"
#include "graphics/GraphicsTemplate.h"

namespace {
using corevideo::modules::ComPtrLite;
using namespace corevideo::graphics;
namespace fs = std::filesystem;

const fs::path kShippedPacks = fs::path(COREVIDEO_REPO_ROOT) / "native" / "resources" / "graphics-packs";

std::string utf8(const fs::path& path) {
  const auto value = path.u8string();
  return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

struct Device {
  ComPtrLite<ID3D11Device> device;
  ComPtrLite<ID3D11DeviceContext> context;
  Device() {
    for (const auto type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
      if (SUCCEEDED(D3D11CreateDevice(nullptr, type, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                      D3D11_SDK_VERSION, device.put(), nullptr, context.put())))
        return;
    }
  }
};
Device& sharedDevice() {
  static Device device;
  return device;
}

ComPtrLite<IDWriteFactory> factory() {
  ComPtrLite<IDWriteFactory> out;
  DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(out.put()));
  return out;
}

bool systemHas(const std::wstring& family) {
  auto f = factory();
  ComPtrLite<IDWriteFontCollection> system;
  UINT32 index = 0;
  BOOL exists = FALSE;
  return f && SUCCEEDED(f->GetSystemFontCollection(system.put())) &&
         SUCCEEDED(system->FindFamilyName(family.c_str(), &index, &exists)) && exists;
}

// One text line in the pack's font, literal colours: a plain template, not a
// pack template, so the brand stays empty.
corevideo::modules::CompositorOverlayContent packFontGraphic(const std::string& text) {
  const std::string manifest =
      R"({"schemaVersion":1,"templateId":"pack-font-probe","templateVersion":1,"name":"Pack font probe","family":"title",)"
      R"("canvas":{"width":1920,"height":1080},"bounds":{"width":600,"height":80},"brandTokens":[],"fields":[],)"
      R"("animations":{"in":{"type":"none"},"out":{"type":"none"}},"layers":[)"
      R"({"id":"line","type":"text","x":10,"y":8,"width":580,"height":64,"text":")" + text +
      R"(","font":"Barlow Semi Condensed","fontWeight":600,"fontSize":44,"color":"#FFFFFF"}]})";
  std::string error;
  auto input = std::make_shared<GraphicsTemplateInput>();
  input->definition = GraphicsTemplate::load(manifest, error);
  input->fields = corevideo::rpc::Json::Object{};
  input->brand = corevideo::rpc::Json::Object{};
  corevideo::modules::CompositorOverlayContent content;
  content.appearance.enabled = true;
  content.graphicsTemplate = input;
  if (input->definition) {
    content.graphicsContentHash = input->definition->hash();
    content.hasGraphicsContentHash = true;
  }
  return content;
}

struct Outcome { std::string state, warning; };
Outcome prepare(const corevideo::modules::CompositorOverlayContent& overlay, bool strict) {
  corevideo::modules::CompositorOverlayRaster raster;
  Outcome out;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  do {
    raster.beginFrame();
    raster.rasterOverlayTexture(sharedDevice().device.get(), sharedDevice().context.get(), overlay, {0.f, 0.f, 1.f, 1.f},
                                600, 80, {}, strict);
    out.state = raster.preparationState();
    out.warning = raster.lastWarning();
    if (out.state == "ready" || out.state == "failed") break;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  } while (std::chrono::steady_clock::now() < deadline);
  return out;
}

// A copy of the shipped News pack with its font file removed.
struct PackWithoutFont {
  fs::path base;
  PackWithoutFont() {
    static std::atomic<int> sequence{0};
    base = fs::temp_directory_path() / ("cvp-pack-font-test-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(++sequence));
    fs::create_directories(base);
    fs::copy(kShippedPacks / "news-signal", base / "news-signal", fs::copy_options::recursive);
    // Every face of the family: one present face would resolve the family.
    for (const char* face : {"BarlowSemiCondensed-Regular.ttf", "BarlowSemiCondensed-SemiBold.ttf", "BarlowSemiCondensed-Bold.ttf"})
      fs::remove(base / "news-signal" / "fonts" / face);
    setGraphicsPacksDirectoryForTesting(utf8(base));
  }
  ~PackWithoutFont() {
    setGraphicsPacksDirectoryForTesting({});
    setGraphicsPackFontsStrictForTesting(false);
    std::error_code ec;
    fs::remove_all(base, ec);
  }
};

struct ShippedPacks {
  ShippedPacks() { setGraphicsPacksDirectoryForTesting(utf8(kShippedPacks)); }
  ~ShippedPacks() { setGraphicsPacksDirectoryForTesting({}); }
};

}  // namespace

TEST(GraphicsPackFonts, PackFontResolvesFromThePrivateCollection) {
  if (systemHas(L"Barlow Semi Condensed")) {
    std::fprintf(stderr, "[pack-fonts] skipped: Barlow Semi Condensed is installed system-wide on this PC\n");
    return;
  }
  ShippedPacks packs;
  size_t present = 0;
  for (const auto& font : graphicsPackFonts()) present += font.present ? 1 : 0;
  EXPECT_GE(present, 3u);
  EXPECT_EQ(present, directWritePackFontFaces());
  auto f = factory();
  const auto font = resolveDirectWriteGraphicsFont(f.get(), "Barlow Semi Condensed");
  ASSERT_TRUE(font.available()) << font.error;
  EXPECT_TRUE(font.warning.empty()) << font.warning;
  EXPECT_EQ(std::wstring(L"Barlow Semi Condensed"), font.family);
  ComPtrLite<IDWriteFontCollection> system;
  ASSERT_TRUE(SUCCEEDED(f->GetSystemFontCollection(system.put())));
  EXPECT_NE(system.get(), font.collection.get()) << "a pack font must come from the private collection";
  // The face is the shipped SemiBold, not a synthesized one.
  UINT32 index = 0;
  BOOL exists = FALSE;
  ASSERT_TRUE(SUCCEEDED(font.collection->FindFamilyName(L"Barlow Semi Condensed", &index, &exists)) && exists);
  ComPtrLite<IDWriteFontFamily> family;
  ComPtrLite<IDWriteFont> face;
  ASSERT_TRUE(SUCCEEDED(font.collection->GetFontFamily(index, family.put())));
  ASSERT_TRUE(SUCCEEDED(family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STRETCH_NORMAL,
                                                     DWRITE_FONT_STYLE_NORMAL, face.put())));
  EXPECT_EQ(DWRITE_FONT_WEIGHT_SEMI_BOLD, face->GetWeight());
  EXPECT_EQ(DWRITE_FONT_SIMULATIONS_NONE, face->GetSimulations());
  // A family that is installed keeps resolving from the system collection.
  const auto arial = resolveDirectWriteGraphicsFont(f.get(), "Arial");
  ASSERT_TRUE(arial.available());
  EXPECT_EQ(system.get(), arial.collection.get());
}

TEST(GraphicsPackFonts, AStrictGraphicInAPackFontPreparesWithoutAWarning) {
  if (!sharedDevice().device) { std::fprintf(stderr, "[pack-fonts] unavailable: no graphics device\n"); return; }
  if (systemHas(L"Barlow Semi Condensed")) return;
  ShippedPacks packs;
  const auto ready = prepare(packFontGraphic("BARLOW FROM THE PACK"), true);
  EXPECT_EQ("ready", ready.state) << ready.warning;
  EXPECT_TRUE(ready.warning.empty()) << ready.warning;
}

TEST(GraphicsPackFonts, AMissingPackFontSubstitutesAndWarnsNeverFailsOnAir) {
  if (!sharedDevice().device) { std::fprintf(stderr, "[pack-fonts] unavailable: no graphics device\n"); return; }
  if (systemHas(L"Barlow Semi Condensed")) return;
  PackWithoutFont pack;
  auto f = factory();
  const auto font = resolveDirectWriteGraphicsFont(f.get(), "Barlow Semi Condensed");
  ASSERT_TRUE(font.available()) << font.error;
  EXPECT_EQ(std::wstring(L"Segoe UI"), font.family);
  // Strict graphics (every template instance) still air, in the fallback.
  const auto ready = prepare(packFontGraphic("FALLBACK ON AIR"), true);
  EXPECT_EQ("ready", ready.state) << ready.warning;
  EXPECT_EQ("Font Barlow Semi Condensed is missing from pack news-signal; using Segoe UI.", ready.warning);
}

TEST(GraphicsPackFonts, StrictPackFontModeFailsAMissingPackFont) {
  if (!sharedDevice().device) { std::fprintf(stderr, "[pack-fonts] unavailable: no graphics device\n"); return; }
  if (systemHas(L"Barlow Semi Condensed")) return;
  PackWithoutFont pack;
  setGraphicsPackFontsStrictForTesting(true);
  auto f = factory();
  const auto font = resolveDirectWriteGraphicsFont(f.get(), "Barlow Semi Condensed");
  EXPECT_FALSE(font.available());
  EXPECT_EQ("Pack font is missing: Barlow Semi Condensed (pack news-signal)", font.error);
  const auto failed = prepare(packFontGraphic("STRICT TEST MODE"), true);
  EXPECT_EQ("failed", failed.state);
  EXPECT_EQ("Pack font is missing: Barlow Semi Condensed (pack news-signal)", failed.warning);
}

TEST(GraphicsPackFonts, AFamilyNoPackDeclaresKeepsTheExistingRule) {
  if (!sharedDevice().device) { std::fprintf(stderr, "[pack-fonts] unavailable: no graphics device\n"); return; }
  auto f = factory();
  const auto font = resolveDirectWriteGraphicsFont(f.get(), "CoreVideo Missing Font 7f3a91");
  EXPECT_FALSE(font.available());
  EXPECT_TRUE(font.error.empty());
}

#endif  // COREVIDEO_WITH_D3D11 && !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS
