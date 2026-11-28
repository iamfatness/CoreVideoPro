// Graphics style packs (graphics styles spec 6.2, slice 7): the pack lint that
// CI runs over every shipped pack, the pack reader, the font registry and the
// all-or-none pack install through graphics-template-install. Portable: runs
// in the stub builds on Linux and macOS CI as well as on Windows.

#include "gtest/gtest.h"

#include "graphics/GraphicsInstances.h"
#include "graphics/GraphicsPack.h"
#include "graphics/GraphicsTemplateRegistry.h"
#include "modules/Sha256.h"
#include "rpc/Json.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

namespace {
using corevideo::rpc::Json;
using namespace corevideo::graphics;
namespace fs = std::filesystem;

const std::string kRepo = COREVIDEO_REPO_ROOT;
const fs::path kShippedPacks = fs::path(kRepo) / "native" / "resources" / "graphics-packs";

std::string utf8(const fs::path& path) {
  const auto value = path.u8string();
  return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

std::string readFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

void writeFile(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

std::string sha256Hex(const std::string& bytes) {
  const auto digest = corevideo::modules::hashing::sha256(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
  static constexpr char hex[] = "0123456789abcdef";
  std::string out;
  for (auto byte : digest) { out += hex[byte >> 4]; out += hex[byte & 15]; }
  return out;
}

bool mentions(const std::vector<std::string>& problems, const std::string& text) {
  return std::any_of(problems.begin(), problems.end(), [&](const auto& p) { return p.find(text) != std::string::npos; });
}

std::string joined(const std::vector<std::string>& problems) {
  std::string out;
  for (const auto& p : problems) out += "\n  " + p;
  return out;
}

// A private copy of the shipped News pack under a unique temp folder named
// after the pack id, so a test can break one rule at a time.
struct PackCopy {
  fs::path base, root;
  explicit PackCopy(const std::string& id = "news-signal") {
    static std::atomic<int> sequence{0};
    base = fs::temp_directory_path() / ("cvp-pack-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                                        "-" + std::to_string(++sequence));
    root = base / id;
    fs::create_directories(base);
    fs::copy(kShippedPacks / "news-signal", root, fs::copy_options::recursive);
    if (id != "news-signal") {
      auto manifest = Json::parse(readFile(root / "pack.json"))->asObject();
      manifest["id"] = id;
      writeFile(root / "pack.json", Json(manifest).stringify());
    }
  }
  ~PackCopy() { std::error_code ec; fs::remove_all(base, ec); }
  Json::Object templateDocument() const { return Json::parse(readFile(root / "templates" / "anchor.json"))->asObject(); }
  void writeTemplate(const Json::Object& document) const { writeFile(root / "templates" / "anchor.json", Json(document).stringify()); }
  std::vector<std::string> lint() const { return lintGraphicsPack(utf8(root)); }
};

Json::Array layersOf(const Json::Object& document) { return document.at("layers").asArray(); }

}  // namespace

// CI gate: every pack that ships inside the app passes the full lint.
TEST(GraphicsPack, ShippedPacksPassTheLint) {
  ASSERT_TRUE(fs::is_directory(kShippedPacks)) << utf8(kShippedPacks);
  int packs = 0;
  for (const auto& entry : fs::directory_iterator(kShippedPacks)) {
    if (!entry.is_directory()) continue;
    ++packs;
    const auto problems = lintGraphicsPack(utf8(entry.path()));
    EXPECT_TRUE(problems.empty()) << utf8(entry.path().filename()) << " fails the pack lint:" << joined(problems);
  }
  EXPECT_GE(packs, 1) << "no shipped packs found";
}

TEST(GraphicsPack, ShippedPackFontsMatchTheirPinnedChecksums) {
  const auto fonts = kShippedPacks / "news-signal" / "fonts";
  EXPECT_EQ(sha256Hex(readFile(fonts / "BarlowSemiCondensed-SemiBold.ttf")),
            "bd299f4bc5b44d30be8d42e9bad3a5df7d66af1cd55d0ed72a8b8916360a1424");
  const auto licence = readFile(fonts / "OFL.txt");
  EXPECT_EQ(sha256Hex(licence), "186d750eb496a4c17a76385f82be6aea2ac1cf2de074a811d63786cf374ea73f")
      << "the OFL text ships verbatim (.gitattributes keeps its line endings)";
  EXPECT_NE(licence.find("SIL Open Font License, Version 1.1"), std::string::npos);
}

TEST(GraphicsPack, ReadsTheManifest) {
  GraphicsPackManifest manifest;
  std::string error;
  ASSERT_TRUE(readGraphicsPackManifest(utf8(kShippedPacks / "news-signal"), manifest, error)) << error;
  EXPECT_EQ("news-signal", manifest.id);
  EXPECT_EQ("news", manifest.genre);
  ASSERT_EQ(1u, manifest.fonts.size());
  EXPECT_EQ("Barlow Semi Condensed", manifest.fonts[0].family);
  EXPECT_EQ(600, manifest.fonts[0].weight);
  ASSERT_EQ(1u, manifest.templates.size());
  EXPECT_EQ("templates/anchor.json", manifest.templates[0]);
}

TEST(GraphicsPack, ManifestRefusesTraversalAndUnknownKeys) {
  PackCopy pack;
  auto manifest = Json::parse(readFile(pack.root / "pack.json"))->asObject();
  auto broken = manifest;
  broken["templates"] = Json::Array{"templates/../../secret.json"};
  writeFile(pack.root / "pack.json", Json(broken).stringify());
  EXPECT_TRUE(mentions(pack.lint(), "pack.json: templates entries must be unique"));
  broken = manifest;
  broken["scripts"] = Json::Array{};
  writeFile(pack.root / "pack.json", Json(broken).stringify());
  EXPECT_TRUE(mentions(pack.lint(), "unsupported key \"scripts\""));
  broken = manifest;
  broken["id"] = "Bad Id";
  writeFile(pack.root / "pack.json", Json(broken).stringify());
  EXPECT_TRUE(mentions(pack.lint(), "pack.json: id must be"));
}

TEST(GraphicsPack, LintRequiresTheIdToMatchTheFolder) {
  PackCopy pack("news-renamed");
  auto manifest = Json::parse(readFile(pack.root / "pack.json"))->asObject();
  manifest["id"] = "news-other";
  writeFile(pack.root / "pack.json", Json(manifest).stringify());
  EXPECT_TRUE(mentions(pack.lint(), "must match the folder name"));
}

TEST(GraphicsPack, LintRejectsRawHexUnlessTheLayerIsFixed) {
  PackCopy pack;
  ASSERT_TRUE(pack.lint().empty()) << joined(pack.lint());
  auto document = pack.templateDocument();
  auto layers = layersOf(document);
  // The shipped highlight is fixed: drop the mark and the lint names it.
  for (auto& layer : layers) if (layer.getString("id") == "highlight") {
    auto object = layer.asObject();
    object.erase("fixed");
    layer = Json(object);
  }
  document["layers"] = layers;
  pack.writeTemplate(document);
  auto problems = pack.lint();
  EXPECT_TRUE(mentions(problems, "layer highlight: raw hex colour fill = #FFFFFF40")) << joined(problems);
  // A raw hex text colour is caught the same way; text content is not a colour.
  for (auto& layer : layers) if (layer.getString("id") == "name-text") {
    auto object = layer.asObject();
    object["color"] = "#ffffff";
    object["text"] = "#1 IN NEWS";
    layer = Json(object);
  }
  document["layers"] = layers;
  pack.writeTemplate(document);
  problems = pack.lint();
  EXPECT_TRUE(mentions(problems, "layer name-text: raw hex colour color = #ffffff")) << joined(problems);
  EXPECT_FALSE(mentions(problems, "#1 IN NEWS")) << joined(problems);
}

TEST(GraphicsPack, LintRejectsFontsThePackDoesNotShip) {
  PackCopy pack;
  auto document = pack.templateDocument();
  auto layers = layersOf(document);
  for (auto& layer : layers) {
    auto object = layer.asObject();
    if (object["id"].asString() == "name-text") object["font"] = "Arial";
    if (object["id"].asString() == "title-text") object["fontWeight"] = 400;
    layer = Json(object);
  }
  document["layers"] = layers;
  pack.writeTemplate(document);
  const auto problems = pack.lint();
  EXPECT_TRUE(mentions(problems, "layer name-text: font \"Arial\" is not one of the pack's fonts")) << joined(problems);
  EXPECT_TRUE(mentions(problems, "layer title-text: the pack ships no 400 face of \"Barlow Semi Condensed\"")) << joined(problems);
}

TEST(GraphicsPack, LintRequiresFontFilesAndTheirLicence) {
  PackCopy pack;
  fs::remove(pack.root / "fonts" / "OFL.txt");
  auto problems = pack.lint();
  EXPECT_TRUE(mentions(problems, "fonts/OFL.txt: the font licence text is missing")) << joined(problems);
  fs::remove(pack.root / "fonts" / "BarlowSemiCondensed-SemiBold.ttf");
  problems = pack.lint();
  EXPECT_TRUE(mentions(problems, "fonts/BarlowSemiCondensed-SemiBold.ttf: declared font file is missing")) << joined(problems);
  writeFile(pack.root / "fonts" / "BarlowSemiCondensed-SemiBold.ttf", "not a font at all, just text");
  writeFile(pack.root / "fonts" / "Stray.ttf", std::string("\x00\x01\x00\x00", 4) + std::string(64, '\0'));
  problems = pack.lint();
  EXPECT_TRUE(mentions(problems, "not a TrueType/OpenType font")) << joined(problems);
  EXPECT_TRUE(mentions(problems, "fonts/Stray.ttf: not declared in pack.json fonts")) << joined(problems);
}

TEST(GraphicsPack, LintChecksPaletteTokensAndValues) {
  PackCopy pack;
  auto palette = Json::parse(readFile(pack.root / "palettes" / "navy-red.json"))->asObject();
  palette.erase("brand.surface");
  palette["brand.accent"] = "#E63946";  // six digits: palettes are #RRGGBBAA
  palette["brand.sparkle"] = "#FFFFFFFF";
  writeFile(pack.root / "palettes" / "navy-red.json", Json(palette).stringify());
  writeFile(pack.root / "palettes" / "unlisted.json", "{}");
  const auto problems = pack.lint();
  EXPECT_TRUE(mentions(problems, "missing required token brand.surface")) << joined(problems);
  EXPECT_TRUE(mentions(problems, "brand.accent must be \"#RRGGBBAA\"")) << joined(problems);
  EXPECT_TRUE(mentions(problems, "unknown token \"brand.sparkle\"")) << joined(problems);
  EXPECT_TRUE(mentions(problems, "palettes/unlisted.json: not listed in pack.json")) << joined(problems);
  EXPECT_TRUE(graphicsPackPaletteTokenKnown("brand.teamA"));
  EXPECT_TRUE(graphicsPackPaletteTokenKnown("brand.teamB"));
}

TEST(GraphicsPack, LintAllowsOnlySmallPngAssets) {
  PackCopy pack;
  static const std::string png("\x89PNG\r\n\x1a\n", 8);
  writeFile(pack.root / "assets" / "texture.png", png + std::string(64, '\0'));
  EXPECT_TRUE(pack.lint().empty()) << joined(pack.lint());
  writeFile(pack.root / "assets" / "logo.svg", "<svg/>");
  writeFile(pack.root / "assets" / "fake.png", "GIF89a");
  auto problems = pack.lint();
  EXPECT_TRUE(mentions(problems, "assets/logo.svg: assets are PNG only")) << joined(problems);
  EXPECT_TRUE(mentions(problems, "assets/fake.png: assets are PNG only")) << joined(problems);
  fs::remove(pack.root / "assets" / "logo.svg");
  fs::remove(pack.root / "assets" / "fake.png");
  writeFile(pack.root / "assets" / "huge.png", png + std::string(size_t(kGraphicsPackAssetBytesLimit), '\0'));
  problems = pack.lint();
  EXPECT_TRUE(mentions(problems, "assets/: more than 2 MB in one pack")) << joined(problems);
}

TEST(GraphicsPack, LintEnforcesTodaysTemplateLimits) {
  PackCopy pack;
  auto document = pack.templateDocument();
  // 65 layers.
  auto layers = layersOf(document);
  for (int i = 0; layers.size() < 65; ++i)
    layers.push_back(Json::Object{{"id", "extra-" + std::to_string(i)}, {"type", "rect"}, {"x", 0}, {"y", 0}, {"width", 4},
                                  {"height", 4}, {"fill", Json::Object{{"token", "brand.primary"}}}});
  auto many = document;
  many["layers"] = layers;
  pack.writeTemplate(many);
  auto problems = pack.lint();
  EXPECT_TRUE(mentions(problems, "more than 64 layers")) << joined(problems);
  EXPECT_TRUE(mentions(problems, "the template does not load")) << joined(problems);
  // Motion over 3000 ms.
  auto slow = document;
  slow["animations"] = Json::Object{{"in", Json::Object{{"type", "fade"}, {"durationMs", 3001}}}, {"out", Json::Object{{"type", "none"}}}};
  pack.writeTemplate(slow);
  problems = pack.lint();
  EXPECT_TRUE(mentions(problems, "the template does not load: template-animation")) << joined(problems);
  // 33 fields.
  auto fields = document.at("fields").asArray();
  for (int i = 0; fields.size() < 33; ++i)
    fields.push_back(Json::Object{{"id", "f" + std::to_string(i)}, {"type", "string"}, {"default", ""}});
  auto wide = document;
  wide["fields"] = fields;
  pack.writeTemplate(wide);
  problems = pack.lint();
  EXPECT_TRUE(mentions(problems, "more than 32 fields")) << joined(problems);
}

TEST(GraphicsPack, InstallManifestsRefuseAPackThatFailsTheLint) {
  PackCopy pack;
  std::vector<std::string> manifests;
  std::string error;
  ASSERT_TRUE(graphicsPackInstallManifests(utf8(pack.root), manifests, error)) << error;
  ASSERT_EQ(1u, manifests.size());
  EXPECT_NE(manifests[0].find("news-signal-anchor"), std::string::npos);
  fs::remove(pack.root / "fonts" / "OFL.txt");
  EXPECT_FALSE(graphicsPackInstallManifests(utf8(pack.root), manifests, error));
  EXPECT_TRUE(manifests.empty());
  EXPECT_NE(error.find("OFL.txt"), std::string::npos) << error;
}

// The font registry the platform text code consults.
TEST(GraphicsPack, FontRegistryListsDeclaredFacesAndMissingFiles) {
  PackCopy pack;
  setGraphicsPacksDirectoryForTesting(utf8(pack.base));
  auto declared = graphicsPackFontDeclaration("barlow semi condensed");
  ASSERT_TRUE(declared.has_value());
  EXPECT_EQ("news-signal", declared->packId);
  EXPECT_EQ("Barlow Semi Condensed", declared->family);
  EXPECT_TRUE(declared->present);
  EXPECT_EQ(600, declared->weight);
  EXPECT_FALSE(graphicsPackFontDeclaration("Arial").has_value());
  const auto generation = graphicsPackFontsGeneration();
  fs::remove(pack.root / "fonts" / "BarlowSemiCondensed-SemiBold.ttf");
  setGraphicsPacksDirectoryForTesting(utf8(pack.base));
  EXPECT_GT(graphicsPackFontsGeneration(), generation);
  declared = graphicsPackFontDeclaration("Barlow Semi Condensed");
  ASSERT_TRUE(declared.has_value());
  EXPECT_FALSE(declared->present);
  EXPECT_EQ("Segoe UI", graphicsPackFontFallback(*declared));
  EXPECT_EQ("Font Barlow Semi Condensed is missing from pack news-signal; using Segoe UI.",
            graphicsPackFontSubstitutionWarning("Barlow Semi Condensed", "news-signal", "Segoe UI"));
  EXPECT_EQ("Pack font is missing: Barlow Semi Condensed (pack news-signal)",
            graphicsPackFontMissingError("Barlow Semi Condensed", "news-signal"));
  setGraphicsPacksDirectoryForTesting({});
}

TEST(GraphicsPack, StrictPackFontModeIsATestSwitch) {
  setGraphicsPackFontsStrictForTesting(true);
  EXPECT_TRUE(graphicsPackFontsStrict());
  setGraphicsPackFontsStrictForTesting(false);
  EXPECT_FALSE(graphicsPackFontsStrict());
}

// Install through the existing graphics-template-install command: a pack is one
// queued install that publishes every template it lists, all or none.
namespace {
Json packRequest(const std::string& requestId, Json::Object fields) {
  fields["schemaVersion"] = 1;
  fields["requestId"] = requestId;
  fields["clientId"] = "pack-client";
  fields["coreEpoch"] = "test-core";
  fields["instanceId"] = "pack-installer";
  fields["expectedRevision"] = 1;
  return Json::Object{{"type", "graphics-template-install"}, {"graphics", std::move(fields)}};
}
Json installRow(GraphicsInstances& state, const std::string& requestId) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  for (;;) {
    state.observe({});
    for (const auto& row : state.snapshot().get("templateInstalls")->asArray())
      if (row.getString("requestId") == requestId && (row.getString("state") == "installed" || row.getString("state") == "failed"))
        return row;
    if (std::chrono::steady_clock::now() > until) return Json::Object{};
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}
bool catalogHas(GraphicsInstances& state, const std::string& id) {
  for (const auto& row : state.snapshot().get("templates")->asArray()) if (row.getString("templateId") == id) return true;
  return false;
}
}  // namespace

TEST(GraphicsPack, InstallsAPackThroughGraphicsTemplateInstall) {
  PackCopy pack;
  GraphicsInstances state{"test-core"};
  ASSERT_TRUE(state.apply(packRequest("install-pack", {{"packRoot", utf8(pack.root)}})));
  const auto row = installRow(state, "install-pack");
  ASSERT_EQ("installed", row.getString("state")) << row.stringify();
  EXPECT_EQ(utf8(pack.root), row.getString("packRoot"));
  ASSERT_TRUE(catalogHas(state, "news-signal-anchor"));
  for (const auto& item : state.snapshot().get("templates")->asArray())
    if (item.getString("templateId") == "news-signal-anchor") EXPECT_FALSE(item.getString("packageRoot").empty());
  // The same request id with a different pack is refused, as with manifests.
  EXPECT_FALSE(state.apply(packRequest("install-pack", {{"packRoot", utf8(pack.base)}})));
}

TEST(GraphicsPack, APackThatFailsTheLintInstallsNothing) {
  PackCopy pack("news-broken");
  auto document = pack.templateDocument();
  document["templateId"] = "news-broken-anchor";
  auto layers = layersOf(document);
  for (auto& layer : layers) if (layer.getString("id") == "name-plate") {
    auto object = layer.asObject();
    object["fill"] = "#0B1F3AFF";
    layer = Json(object);
  }
  document["layers"] = layers;
  pack.writeTemplate(document);
  GraphicsInstances state{"test-core"};
  ASSERT_TRUE(state.apply(packRequest("install-broken", {{"packRoot", utf8(pack.root)}})));
  const auto row = installRow(state, "install-broken");
  EXPECT_EQ("failed", row.getString("state")) << row.stringify();
  EXPECT_EQ("graphics-pack-invalid", row.getString("reason"));
  EXPECT_NE(row.getString("detail").find("raw hex colour"), std::string::npos) << row.stringify();
  EXPECT_FALSE(catalogHas(state, "news-broken-anchor"));
}

TEST(GraphicsPack, PackRootIsNeverMixedWithAManifest) {
  PackCopy pack;
  GraphicsInstances state{"test-core"};
  EXPECT_FALSE(state.apply(packRequest("mixed", {{"packRoot", utf8(pack.root)}, {"manifest", "{}"}})));
  EXPECT_EQ("graphics-pack-root-invalid", state.snapshot().get("lastResult")->getString("reason"));
}
