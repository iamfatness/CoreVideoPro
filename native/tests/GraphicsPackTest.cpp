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
  // Every shipped font file and licence text, as recorded in
  // native/resources/graphics-packs/SOURCE.md (wave 1 packs, slice 10).
  const std::vector<std::pair<std::string, std::string>> pinned{
      {"news-signal/fonts/BarlowSemiCondensed-Regular.ttf", "e8c7242f612b13d026e18a94b63b0fd079e91a3faf009d1b6981dbb5f16d1153"},
      {"news-signal/fonts/BarlowSemiCondensed-SemiBold.ttf", "bd299f4bc5b44d30be8d42e9bad3a5df7d66af1cd55d0ed72a8b8916360a1424"},
      {"news-signal/fonts/BarlowSemiCondensed-Bold.ttf", "5bd6757e459a110da81b44532f0f43058e2dab2401688abca84f2fdbde193cbb"},
      {"news-signal/fonts/OFL.txt", "186d750eb496a4c17a76385f82be6aea2ac1cf2de074a811d63786cf374ea73f"},
      {"sports-kinetic/fonts/BarlowCondensed-Regular.ttf", "583cec5da3b84bc4dc7c9c72e2a565c94d34e431518b19d7e250b7830ad5f996"},
      {"sports-kinetic/fonts/BarlowCondensed-Bold.ttf", "e476562ec9c1e16cf16475895b511f08c804f438cc9a9f80a44ea50a0eeb5b65"},
      {"sports-kinetic/fonts/BarlowCondensed-SemiBoldItalic.ttf", "dc83cbbb7545a117ab9e4c10ed07d35eef6f563b77621a1d357f25a3194987d1"},
      {"sports-kinetic/fonts/BarlowCondensed-BoldItalic.ttf", "d8694f65cd70c3e5e600ead37657aab0adb5f63b275d0c1f6f4a7046de222145"},
      {"sports-kinetic/fonts/OFL.txt", "186d750eb496a4c17a76385f82be6aea2ac1cf2de074a811d63786cf374ea73f"},
      {"worship-grace/fonts/CormorantGaramond-Regular.ttf", "aba4d0a0205a57abc489db3c1c1531b4f4e59fbe5c596912be5a3b821a06efc0"},
      {"worship-grace/fonts/CormorantGaramond-Italic.ttf", "006291ded05a9e8a58436474d9bb74e7477f494a5dfc11435ae2a2d9d74d4ed9"},
      {"worship-grace/fonts/CormorantGaramond-SemiBold.ttf", "de1b7923fbc20ce742f6498d085ad0b64dc3625efbd8bdb2f01dc70e1e7a6edd"},
      {"worship-grace/fonts/Montserrat-Regular.ttf", "3e8abe50c44c82e2242e97d1ec8c0d385c4890cdc50447bcdb8605c81a38cfb2"},
      {"worship-grace/fonts/Montserrat-Medium.ttf", "dae47428bb041f9716604e0e07b5b0c8585b3bdd8183362f75c69fe7bb3cfaf4"},
      {"worship-grace/fonts/OFL.txt", "a966906e1c04f674675833efbb46ce22021cb59d534c337469dc1b970d5e601e"},
      {"corporate-clarity/fonts/Montserrat-Regular.ttf", "3e8abe50c44c82e2242e97d1ec8c0d385c4890cdc50447bcdb8605c81a38cfb2"},
      {"corporate-clarity/fonts/Montserrat-Medium.ttf", "dae47428bb041f9716604e0e07b5b0c8585b3bdd8183362f75c69fe7bb3cfaf4"},
      {"corporate-clarity/fonts/Montserrat-SemiBold.ttf", "b4e1563393d73fdff491a869441245aef31add2ec03d9c97a6dae4de07c52fd0"},
      {"corporate-clarity/fonts/OFL.txt", "8b7141c03fa4f8d44e6345d5d4931709290f0f67875e452e95ac1fd3a027802e"},
      {"esports-neon/fonts/ChakraPetch-Regular.ttf", "98fcd638baa5c81ff0316b7538ce330ee3b23b1302726de3526d5933a8ecf986"},
      {"esports-neon/fonts/ChakraPetch-SemiBold.ttf", "45264de3204ddbd5fb3e14a2402acd5c630d16650ae5fc221d2c52da46a6734b"},
      {"esports-neon/fonts/ChakraPetch-Bold.ttf", "65fbf76d95651697275e19db4d717c0e95a789ddd3476478b05292104db278a0"},
      {"esports-neon/fonts/OFL.txt", "13831d02389d917d22fcfa6c79f98f8acbf61d230add0e73a4d4dc5f3bfb9e56"},
      {"talk-studio/fonts/Poppins-Regular.ttf", "7e65201e9b79159e2300267cc885e16c8dcef2424cdfa09a29bfb0980a94a7ba"},
      {"talk-studio/fonts/Poppins-Medium.ttf", "90373e7d838d32468438fc3e152dca0bdb12edcab99ea639f158790b1ba1fd05"},
      {"talk-studio/fonts/Poppins-SemiBold.ttf", "d3bf1bdaf0550e83da9ac0b1d1d9fe6db086835a83aa28578e609a394b9a0286"},
      {"talk-studio/fonts/OFL.txt", "6be04893d770899a015649c7aa3b582f871b272f8747a92b78b17c3e5c8b2573"},
      {"minimal-line/fonts/Inter-Regular.ttf", "40d692fce188e4471e2b3cba937be967878f631ad3ebbbdcd587687c7ebe0c82"},
      {"minimal-line/fonts/Inter-Italic.ttf", "bbc051dd204b5019a1aa0bc0ae2aa8a05ab13e7a3f979fa357631dc7feb6833a"},
      {"minimal-line/fonts/Inter-Bold.ttf", "288316099b1e0a47a4716d159098005eef7c0066921f34e3200393dbdb01947f"},
      {"minimal-line/fonts/OFL.txt", "262481e844521b326f5ecd053e59b98c8b2da78c8ee1bdbb6e8174305e54935a"},
  };
  std::string mismatched;
  for (const auto& [file, digest] : pinned)
    if (sha256Hex(readFile(kShippedPacks / file)) != digest) mismatched += " " + file;
  EXPECT_TRUE(mismatched.empty()) << "changed or missing (OFL texts ship verbatim; .gitattributes keeps their line endings):"
                                  << mismatched;
  for (const auto& [file, digest] : pinned)
    if (file.size() > 7 && file.compare(file.size() - 7, 7, "OFL.txt") == 0)
      EXPECT_NE(readFile(kShippedPacks / file).find("SIL Open Font License, Version 1.1"), std::string::npos) << file;
  // Every font file a shipped pack holds is pinned.
  size_t files = 0;
  for (const auto& pack : fs::directory_iterator(kShippedPacks))
    if (fs::is_directory(pack.path() / "fonts"))
      for (const auto& file : fs::directory_iterator(pack.path() / "fonts")) { (void)file; ++files; }
  EXPECT_EQ(pinned.size(), files) << "a font file is not pinned here (or a pinned one is gone)";
}

TEST(GraphicsPack, ReadsTheManifest) {
  GraphicsPackManifest manifest;
  std::string error;
  ASSERT_TRUE(readGraphicsPackManifest(utf8(kShippedPacks / "news-signal"), manifest, error)) << error;
  EXPECT_EQ("news-signal", manifest.id);
  EXPECT_EQ("news", manifest.genre);
  // Slice 7 shipped the anchor and the SemiBold face; slice 10 completed the pack.
  ASSERT_EQ(3u, manifest.fonts.size());
  EXPECT_EQ("Barlow Semi Condensed", manifest.fonts[1].family);
  EXPECT_EQ("fonts/BarlowSemiCondensed-SemiBold.ttf", manifest.fonts[1].file);
  EXPECT_EQ(600, manifest.fonts[1].weight);
  ASSERT_EQ(8u, manifest.templates.size());
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
    if (object["id"].asString() == "title-text") object["fontWeight"] = 500;
    layer = Json(object);
  }
  document["layers"] = layers;
  pack.writeTemplate(document);
  const auto problems = pack.lint();
  EXPECT_TRUE(mentions(problems, "layer name-text: font \"Arial\" is not one of the pack's fonts")) << joined(problems);
  EXPECT_TRUE(mentions(problems, "layer title-text: the pack ships no 500 face of \"Barlow Semi Condensed\"")) << joined(problems);
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
  ASSERT_EQ(8u, manifests.size());
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
  EXPECT_EQ(400, declared->weight);  // the first declared face (slice 10 ships 400, 600 and 700)
  EXPECT_FALSE(graphicsPackFontDeclaration("Arial").has_value());
  const auto generation = graphicsPackFontsGeneration();
  for (const char* face : {"BarlowSemiCondensed-Regular.ttf", "BarlowSemiCondensed-SemiBold.ttf", "BarlowSemiCondensed-Bold.ttf"})
    fs::remove(pack.root / "fonts" / face);
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
