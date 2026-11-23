// Graphics raster parity on Metal (G4 Task 6): Mac goldens and the judgement
// against the Windows D2D reference.
//
// - GraphicsRasterGolden.MacMatchesMacGoldens: every fixture in
//   native/tests/fixtures/graphics-parity/index.json, rendered by
//   CoreTextOverlayRaster (strict), judged tier 1 + tier 2 + layout against
//   native/tests/golden/metal-graphics/. With COREVIDEO_WRITE_MAC_GOLDENS=1 it
//   writes the goldens instead and FAILS with "goldens written; review the
//   diff", so a write run can never pass silently. ctest label `golden`:
//   advisory on CI runners (other macOS builds), blocking in the release
//   ctest on the owner's Mac.
// - GraphicsRasterParity.MacMatchesWindowsReference: judges the same renders
//   against contracts/graphics-raster-reference.json and the Windows PNGs.
//   Until the Windows session writes them it prints UNVERIFIED and returns.
// - ParityPng.RoundTripKeepsPremultipliedSamplesVerbatim: the PNG container.

#if COREVIDEO_WITH_METAL && !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS

#import <Metal/Metal.h>
#include <sys/sysctl.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

#include "GraphicsRasterParity.h"
#include "compositor/CoreTextOverlayRaster.h"
#include "modules/Sha256.h"

namespace {
using namespace corevideo::graphics::parity;
using corevideo::rpc::Json;
namespace fs = std::filesystem;

const std::string kRepo = COREVIDEO_REPO_ROOT;
const std::string kFixtures = kRepo + "/native/tests/fixtures/graphics-parity";
const std::string kGoldens = kRepo + "/native/tests/golden/metal-graphics";

struct Rendered {
  ParityImage image;
  Json layout;
  std::string state, warning;
  double textTolerance = 1;
};

Rendered renderFixture(id<MTLDevice> device, const ParityFixture& fixture) {
  Rendered out;
  out.textTolerance = fixture.textTolerance;
  ParityInput input;
  std::string error;
  if (!parityInput(fixture, kFixtures, input, error)) {
    out.state = "input-failed";
    out.warning = error;
    return out;
  }
  out.layout = fixtureLayout(fixture, input);
  corevideo::modules::CoreTextOverlayRaster raster;
  id<MTLTexture> texture = nil;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  do {
    raster.beginFrame();
    texture = raster.rasterOverlayTexture(device, input.overlay, {0.f, 0.f, 1.f, 1.f}, input.width, input.height, {}, true);
    out.state = raster.preparationState();
    if (out.state == "ready" || out.state == "failed") break;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  } while (std::chrono::steady_clock::now() < deadline);
  out.warning = raster.lastWarning();
  if (!texture) return out;
  out.image.width = int(texture.width);
  out.image.height = int(texture.height);
  out.image.bgraPremultiplied.resize(size_t(texture.width) * texture.height * 4);
  [texture getBytes:out.image.bgraPremultiplied.data()
        bytesPerRow:texture.width * 4
         fromRegion:MTLRegionMake2D(0, 0, texture.width, texture.height)
        mipmapLevel:0];
  return out;
}

std::string hex(const std::array<uint8_t, 32>& digest) {
  static const char* digits = "0123456789abcdef";
  std::string s;
  for (auto b : digest) { s += digits[b >> 4]; s += digits[b & 15]; }
  return s;
}
std::string fileSha256(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
  return hex(corevideo::modules::hashing::sha256(bytes.data(), bytes.size()));
}
std::string macOSVersion() {
  char buffer[64] = {};
  size_t size = sizeof(buffer);
  return sysctlbyname("kern.osproductversion", buffer, &size, nullptr, 0) == 0 ? buffer : "unknown";
}
std::string readText(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file), {});
}

// Tier 1 + tier 2 + layout, as one list of failures named by fixture.
std::vector<std::string> judge(const std::string& name, const ParityImage& reference, const Json& referenceLayout,
                               const ParityImage& candidate, const Json& candidateLayout, double textTolerance) {
  std::vector<std::string> failures;
  const auto regions = regionsFromLayout(referenceLayout);
  for (const auto& verdict : {judgeGeometry(reference, candidate, regions),
                              judgeText(reference, candidate, regions, textTolerance),
                              judgeLayout(referenceLayout, candidateLayout)})
    for (const auto& f : verdict.failures) failures.push_back(name + ": " + f);
  return failures;
}

id<MTLDevice> metalOrSkip() {
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  if (!device) std::fprintf(stderr, "[parity] unavailable: no Metal device\n");
  return device;
}

}  // namespace

TEST(ParityPng, RoundTripKeepsPremultipliedSamplesVerbatim) {
  ParityImage image;
  image.width = 4;
  image.height = 1;
  // B,G,R,A per pixel; the first and last are premultiplied partial alpha.
  image.bgraPremultiplied = {10, 20, 30, 40, 0, 0, 0, 0, 255, 255, 255, 255, 1, 0, 0, 1};
  const auto path = (fs::temp_directory_path() / "corevideo-parity-roundtrip.png").string();
  std::string error;
  ASSERT_TRUE(writeParityPng(path, image, error)) << error;
  ParityImage back;
  ASSERT_TRUE(readParityPng(path, back, error)) << error;
  EXPECT_EQ(4, back.width);
  EXPECT_EQ(1, back.height);
  EXPECT_EQ(image.bgraPremultiplied, back.bgraPremultiplied);
  std::error_code ec;
  fs::remove(path, ec);
}

TEST(GraphicsRasterGolden, MacMatchesMacGoldens) {
  id<MTLDevice> device = metalOrSkip();
  // A golden run with no device must not read as a pass.
  ASSERT_TRUE(device != nil) << "no Metal device: goldens cannot be written or judged";
  if (!device) return;
  std::string error;
  const auto fixtures = loadParityIndex(kFixtures + "/index.json", error);
  ASSERT_TRUE(error.empty()) << error;
  const char* write = std::getenv("COREVIDEO_WRITE_MAC_GOLDENS");
  if (write && std::string(write) == "1") {
    fs::create_directories(kGoldens);
    Json::Array rows;
    for (const auto& fixture : fixtures) {
      const auto rendered = renderFixture(device, fixture);
      // Every fixture must prepare: a golden of a failure would pin the failure.
      EXPECT_EQ("ready", rendered.state) << fixture.name << ": " << rendered.warning;
      Json::Object row{{"name", fixture.name}, {"state", rendered.state}, {"warning", rendered.warning},
                       {"layout", rendered.layout}};
      if (!rendered.image.bgraPremultiplied.empty()) {
        const auto path = kGoldens + "/" + fixture.name + ".png";
        EXPECT_TRUE(writeParityPng(path, rendered.image, error)) << error;
        row["sha256"] = fileSha256(path);
      }
      rows.push_back(std::move(row));
    }
    const char* commit = std::getenv("COREVIDEO_GOLDEN_COMMIT");
    std::ofstream(kGoldens + "/manifest.json")
        << Json(Json::Object{{"macOS", macOSVersion()}, {"commit", commit ? commit : "unknown"},
                             {"generator", "GraphicsRasterGolden.MacMatchesMacGoldens"}, {"fixtures", std::move(rows)}})
               .stringify();
    EXPECT_TRUE(false) << "goldens written to " << kGoldens << "; review the diff (this run never passes)";
    return;
  }
  const auto manifest = Json::parse(readText(kGoldens + "/manifest.json"));
  ASSERT_TRUE(manifest && manifest->get("fixtures")) << "no Mac goldens at " << kGoldens
      << " (generate with COREVIDEO_WRITE_MAC_GOLDENS=1)";
  if (!manifest || !manifest->get("fixtures")) return;
  std::fprintf(stderr, "[golden] goldens from macOS %s, running on macOS %s\n",
               manifest->getString("macOS").c_str(), macOSVersion().c_str());
  std::map<std::string, Json> recorded;
  for (const auto& row : manifest->get("fixtures")->asArray()) recorded[row.getString("name")] = row;
  for (const auto& fixture : fixtures) {
    const auto found = recorded.find(fixture.name);
    EXPECT_TRUE(found != recorded.end()) << fixture.name << ": no golden (regenerate)";
    if (found == recorded.end()) continue;
    const auto rendered = renderFixture(device, fixture);
    EXPECT_EQ("ready", found->second.getString("state")) << fixture.name << ": the golden pins a failure";
    EXPECT_EQ("ready", rendered.state) << fixture.name << ": " << rendered.warning;
    EXPECT_EQ(found->second.getString("warning"), rendered.warning) << fixture.name;
    if (rendered.image.bgraPremultiplied.empty()) continue;
    const auto goldenPath = kGoldens + "/" + fixture.name + ".png";
    EXPECT_EQ(found->second.getString("sha256"), fileSha256(goldenPath)) << fixture.name << ": golden PNG changed outside a write run";
    ParityImage golden;
    ASSERT_TRUE(readParityPng(goldenPath, golden, error)) << error;
    const auto failures = judge(fixture.name, golden, *found->second.get("layout"), rendered.image, rendered.layout,
                                rendered.textTolerance);
    for (const auto& f : failures) EXPECT_TRUE(false) << f;
  }
}

TEST(GraphicsRasterParity, MacMatchesWindowsReference) {
  const auto referencePath = kRepo + "/contracts/graphics-raster-reference.json";
  if (!fs::exists(referencePath)) {
    std::fprintf(stderr, "[parity] UNVERIFIED: no Windows reference (Task 7 not run)\n");
    return;
  }
  id<MTLDevice> device = metalOrSkip();
  if (!device) return;
  const auto reference = Json::parse(readText(referencePath));
  ASSERT_TRUE(reference && reference->get("fixtures"));
  if (!reference || !reference->get("fixtures")) return;
  std::string error;
  const auto fixtures = loadParityIndex(kFixtures + "/index.json", error);
  ASSERT_TRUE(error.empty()) << error;
  const char* artifacts = std::getenv("COREVIDEO_GRAPHICS_PARITY_ARTIFACTS");
  if (artifacts) fs::create_directories(artifacts);
  std::string html = "<!doctype html><meta charset=utf-8><title>Graphics parity</title><table border=1 cellpadding=4>"
                     "<tr><th>fixture</th><th>Windows</th><th>Mac</th><th>diff</th><th>result</th></tr>";
  std::map<std::string, Json> rows;
  for (const auto& row : reference->get("fixtures")->asArray()) rows[row.getString("name")] = row;
  int failed = 0;
  for (const auto& fixture : fixtures) {
    const auto found = rows.find(fixture.name);
    EXPECT_TRUE(found != rows.end()) << fixture.name << ": not in the Windows reference";
    if (found == rows.end()) continue;
    ParityImage windows;
    const auto png = kRepo + "/" + found->second.getString("png");
    ASSERT_TRUE(readParityPng(png, windows, error)) << error;
    // The container is verbatim: the recorded samples must match the decoded PNG.
    if (const auto* samples = found->second.get("samples"))
      for (const auto& sample : samples->asArray()) {
        const int x = int(sample.getNumber("x")), y = int(sample.getNumber("y"));
        const auto& bgra = sample.get("bgra")->asArray();
        for (int c = 0; c < 4; ++c)
          EXPECT_EQ(int(bgra[size_t(c)].asNumber()),
                    int(windows.bgraPremultiplied[(size_t(y) * windows.width + x) * 4 + c]))
              << fixture.name << " sample " << x << "," << y << " channel " << c << ": the PNG was not read verbatim";
      }
    const auto mac = renderFixture(device, fixture);
    const auto failures = judge(fixture.name, windows, *found->second.get("layout"), mac.image, mac.layout,
                                mac.textTolerance);
    failed += failures.empty() ? 0 : 1;
    std::string cell;
    for (const auto& f : failures) { EXPECT_TRUE(false) << f; cell += f + "<br>"; }
    if (artifacts && !mac.image.bgraPremultiplied.empty()) {
      const std::string base = std::string(artifacts) + "/" + fixture.name;
      writeParityPng(base + "-windows.png", windows, error);
      writeParityPng(base + "-mac.png", mac.image, error);
      if (windows.bgraPremultiplied.size() == mac.image.bgraPremultiplied.size()) {
        ParityImage diff = windows;
        for (size_t i = 0; i < diff.bgraPremultiplied.size(); i += 4) {
          int worst = 0;
          for (int c = 0; c < 4; ++c)
            worst = std::max(worst, std::abs(int(windows.bgraPremultiplied[i + c]) - int(mac.image.bgraPremultiplied[i + c])));
          const uint8_t v = uint8_t(std::min(255, worst * 4));
          diff.bgraPremultiplied[i] = 0; diff.bgraPremultiplied[i + 1] = 0; diff.bgraPremultiplied[i + 2] = v;
          diff.bgraPremultiplied[i + 3] = 255;
        }
        writeParityPng(base + "-diff.png", diff, error);
      }
      html += "<tr><td>" + fixture.name + "</td><td><img src='" + fixture.name + "-windows.png' width=480></td><td><img src='" +
              fixture.name + "-mac.png' width=480></td><td><img src='" + fixture.name + "-diff.png' width=480></td><td>" +
              (failures.empty() ? std::string("pass") : cell) + "</td></tr>";
    }
  }
  if (artifacts) std::ofstream(std::string(artifacts) + "/index.html") << html << "</table>";
  std::fprintf(stderr, "[parity] %d of %zu fixtures outside tolerance\n", failed, fixtures.size());
}

#endif  // COREVIDEO_WITH_METAL && !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS
