// Graphics font resolution on macOS (Metal graphics backend G2).
//
// Owner decision 2026-11-21: Windows families macOS lacks resolve through a
// fixed table (Segoe UI -> bundled Selawik; Bahnschrift, Cambria, Consolas ->
// system families) and a table substitution WARNS, never fails. Expectations
// are the table itself and the pinned checksums from
// native/resources/graphics-fonts/SOURCE.md, never values read back from the
// resolver (plan-authoring rule 2).

#if defined(__APPLE__)

#include <gtest/gtest.h>

#include <CommonCrypto/CommonDigest.h>
#include <CoreText/CoreText.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "graphics/GraphicsTextCoreText.h"

namespace {
using namespace corevideo::graphics;

std::string postScriptName(CTFontRef font) {
  if (!font) return {};
  CFStringRef name = CTFontCopyPostScriptName(font);
  char buffer[256] = {};
  CFStringGetCString(name, buffer, sizeof(buffer), kCFStringEncodingUTF8);
  CFRelease(name);
  return buffer;
}

std::string sha256OfFile(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return "missing:" + path;
  const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  unsigned char digest[CC_SHA256_DIGEST_LENGTH];
  CC_SHA256(bytes.data(), CC_LONG(bytes.size()), digest);
  char hex[CC_SHA256_DIGEST_LENGTH * 2 + 1] = {};
  for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; ++i) std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
  return hex;
}

TEST(GraphicsFontResolver, EveryTableRowResolvesToItsMacFamilyAndWarns) {
  struct Row { const char* requested; const char* mac; };
  for (const Row row : {Row{"Segoe UI", "Selawik"}, Row{"Segoe UI Semibold", "Selawik"},
                        Row{"Bahnschrift", "Avenir Next Condensed"}, Row{"Cambria", "Georgia"},
                        Row{"Consolas", "Menlo"}}) {
    const auto resolved = resolveGraphicsFontFamily(row.requested);
    EXPECT_TRUE(resolved.available) << row.requested;
    EXPECT_TRUE(resolved.substituted) << row.requested;
    EXPECT_EQ(resolved.family, row.mac) << row.requested;
    EXPECT_EQ(graphicsFontSubstitutionWarning(row.requested, resolved.family),
              std::string("Font ") + row.requested + " is not installed on this Mac; using " + row.mac + ".");
  }
}

TEST(GraphicsFontResolver, InstalledFamiliesResolveToThemselvesAndUnknownOnesAreUnavailable) {
  for (const char* family : {"Arial", "Georgia", "Verdana", "Trebuchet MS"}) {
    const auto resolved = resolveGraphicsFontFamily(family);
    EXPECT_TRUE(resolved.available) << family;
    EXPECT_FALSE(resolved.substituted) << family;
    EXPECT_EQ(resolved.family, family);
  }
  const auto unknown = resolveGraphicsFontFamily("CoreVideo Missing Font 9c2a8f77");
  EXPECT_FALSE(unknown.available);
  EXPECT_FALSE(unknown.substituted);
}

TEST(GraphicsFontResolver, SelawikIsRegisteredFromTheBundleAndWeightsPickTheirFaces) {
  ASSERT_TRUE(resolveGraphicsFontFamily("Segoe UI").available);
  EXPECT_EQ(registeredBundledGraphicsFonts(), 3u) << "from " << graphicsFontsDirectory();
  struct Weight { int weight; const char* face; };
  for (const Weight w : {Weight{400, "Selawik-Regular"}, Weight{600, "Selawik-Semibold"}, Weight{700, "Selawik-Bold"}}) {
    CTFontRef font = createGraphicsFont("Selawik", w.weight, 24);
    EXPECT_EQ(postScriptName(font), w.face) << w.weight;
    if (font) CFRelease(font);
  }
  // "Segoe UI Semibold" is a Windows FAMILY; on the Mac it is Selawik at 600
  // whatever weight the primitive asked for.
  const auto semibold = resolveGraphicsFontFamily("Segoe UI Semibold");
  EXPECT_EQ(semibold.weight, 600);
  CTFontRef font = createGraphicsFont(semibold.family, semibold.weight ? semibold.weight : 400, 24);
  EXPECT_EQ(postScriptName(font), "Selawik-Semibold");
  if (font) CFRelease(font);
}

TEST(GraphicsFontResolver, BundledFilesMatchTheirPinnedChecksums) {
  const auto directory = graphicsFontsDirectory();
  EXPECT_EQ(sha256OfFile(directory + "/selawk.ttf"), "e9d98518d8ac2817782a9a382430463a2e0793ea68350b695bb727d9a830ee1c");
  EXPECT_EQ(sha256OfFile(directory + "/selawksb.ttf"), "0a9e9d0549a10f24bef9b3a29e06fe6e0b5c21e7a784c503b048a307841a7783");
  EXPECT_EQ(sha256OfFile(directory + "/selawkb.ttf"), "f0db5e174a90e0956ad7d2844bdca1d5e6da92ec65b2c04e57ba9b180668c904");
  std::ifstream licence(directory + "/OFL.txt");
  const std::string text((std::istreambuf_iterator<char>(licence)), std::istreambuf_iterator<char>());
  EXPECT_NE(text.find("SIL Open Font License, Version 1.1"), std::string::npos) << "the OFL text must ship with the fonts";
}

}  // namespace

#endif  // __APPLE__
