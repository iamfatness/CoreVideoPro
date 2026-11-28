// Graphics styles slice 11 (the Builder reshaped around packs), core half:
// built-in style packs join the catalog without spending library slots, the
// library thumbnails render in the show palette the Builder sends and re-render
// when it changes, and `animations.speed` plays motion faster or slower.
// Portable: the stub builds run everything except the pixel comparison.

#include "gtest/gtest.h"

#include "graphics/GraphicsInstances.h"
#include "graphics/GraphicsMotion.h"
#include "graphics/GraphicsPack.h"
#include "graphics/GraphicsTemplate.h"
#include "graphics/GraphicsTemplateRegistry.h"
#include "graphics/GraphicsTimeline.h"
#include "rpc/Json.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <thread>

namespace {
using corevideo::rpc::Json;
using namespace corevideo::graphics;
namespace fs = std::filesystem;

const fs::path kShippedPacks = fs::path(std::string(COREVIDEO_REPO_ROOT)) / "native" / "resources" / "graphics-packs";

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

// A temp packs directory holding copies of shipped packs (optionally renamed).
struct PacksDir {
  fs::path root;
  PacksDir() {
    static std::atomic<int> sequence{0};
    root = fs::temp_directory_path() / ("cvp-builtin-packs-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                                        "-" + std::to_string(++sequence));
    fs::create_directories(root);
  }
  ~PacksDir() { std::error_code ec; fs::remove_all(root, ec); }
  // Copies news-signal as `id`, renaming the pack and every template id so the
  // copy never collides with the original.
  fs::path add(const std::string& id) {
    const auto dir = root / id;
    fs::copy(kShippedPacks / "news-signal", dir, fs::copy_options::recursive);
    if (id == "news-signal") return dir;
    auto manifest = Json::parse(readFile(dir / "pack.json"))->asObject();
    manifest["id"] = id;
    writeFile(dir / "pack.json", Json(manifest).stringify());
    for (const auto& entry : fs::directory_iterator(dir / "templates")) {
      auto document = Json::parse(readFile(entry.path()))->asObject();
      document["templateId"] = id + document.at("templateId").asString().substr(std::string("news-signal").size());
      writeFile(entry.path(), Json(document).stringify());
    }
    return dir;
  }
};

bool waitFor(const std::function<bool()>& done, std::chrono::seconds limit = std::chrono::seconds(20)) {
  const auto until = std::chrono::steady_clock::now() + limit;
  while (!done()) {
    if (std::chrono::steady_clock::now() > until) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return true;
}

const Json* packRow(const Json& packs, const std::string& id) {
  for (const auto& row : packs.asArray()) if (row.getString("id") == id) return &row;
  return nullptr;
}
const Json* catalogRow(const Json& catalog, const std::string& templateId) {
  for (const auto& row : catalog.asArray()) if (row.getString("templateId") == templateId) return &row;
  return nullptr;
}

Json request(const std::string& type, Json::Object fields) {
  static int sequence = 0;
  fields["schemaVersion"] = 1;
  fields["requestId"] = "builtin-packs-" + std::to_string(++sequence);
  fields["clientId"] = "builder";
  fields["coreEpoch"] = "test-core";
  fields["instanceId"] = fields.count("instanceId") ? fields.at("instanceId") : Json("speaker");
  if (!fields.count("expectedRevision")) fields["expectedRevision"] = 1;
  return Json::Object{{"type", type}, {"graphics", std::move(fields)}};
}

Json manifestWithId(const std::string& id) {
  auto document = GraphicsTemplate::corporateLowerThird()->document().asObject();
  document["templateId"] = id;
  return document;
}
}  // namespace

TEST(GraphicsBuiltInPacks, ShippedPacksJoinTheCatalogAsBuiltInRowsThatNamePack) {
  GraphicsTemplateRegistry registry;
  const auto before = registry.poll();
  ASSERT_TRUE(registry.loadBuiltInPacks(utf8(kShippedPacks)));
  std::shared_ptr<const GraphicsTemplateRegistry::Publication> published;
  ASSERT_TRUE(waitFor([&] { published = registry.poll(); const auto* row = packRow(published->packs, "news-signal"); return row && row->getString("state") != ""; }));
  const auto* news = packRow(published->packs, "news-signal");
  ASSERT_NE(news, nullptr);
  ASSERT_EQ("ready", news->getString("state")) << news->stringify();
  EXPECT_EQ("News: Signal", news->getString("name"));
  EXPECT_EQ("news", news->getString("genre"));
  ASSERT_GE(news->get("templates")->asArray().size(), 1u);
  const auto* anchor = catalogRow(published->catalog, "news-signal-anchor");
  ASSERT_NE(anchor, nullptr);
  ASSERT_NE(anchor->get("pack"), nullptr);
  EXPECT_EQ("news-signal", anchor->get("pack")->getString("id"));
  EXPECT_EQ("News: Signal", anchor->get("pack")->getString("name"));
  EXPECT_EQ("news", anchor->get("pack")->getString("genre"));
  EXPECT_TRUE(anchor->get("builtIn")->asBool());
  EXPECT_EQ("declarative", anchor->getString("runtime"));
  // Built-in packs do not install operator templates and do not touch the
  // imported document budget.
  EXPECT_EQ(before->documentBytes, published->documentBytes);
  EXPECT_TRUE(published->installs.asArray().empty());
  // Every shipped pack loads: none is invalid on main.
  for (const auto& row : published->packs.asArray()) EXPECT_EQ("ready", row.getString("state")) << row.stringify();
  // A built-in pack template can never be removed.
  EXPECT_EQ("template-built-in", registry.remove("news-signal-anchor", 1));
}

TEST(GraphicsBuiltInPacks, BuiltInPacksNeverSpendTheImportedLibrarysSlots) {
  PacksDir dir;
  for (const auto* id : {"news-signal", "pack-a", "pack-b", "pack-c"}) dir.add(id);
  GraphicsInstances state{"test-core"};
  ASSERT_TRUE(state.loadBuiltInPacks(utf8(dir.root)));
  ASSERT_TRUE(waitFor([&] {
    state.observe({});
    const auto* packs = state.snapshot().get("templatePacks");
    return packs && packs->asArray().size() == 4;
  }));
  size_t builtInRows = 0;
  for (const auto& row : state.snapshot().get("templates")->asArray()) if (row.get("builtIn")) ++builtInRows;
  ASSERT_GE(builtInRows, 4u);
  // The library still takes 16 versions minus the curated looks, exactly as
  // before packs: 16 - 5 = 11 imports fit, the 12th is refused.
  const size_t curated = GraphicsTemplate::curated().size();
  for (size_t i = 0; i <= 16 - curated; ++i) {
    const auto id = "imported-look-" + std::to_string(i);
    ASSERT_TRUE(state.apply(request("graphics-template-install", {{"instanceId", "installer"}, {"manifest", Json(manifestWithId(id)).stringify()}})));
    Json row;
    ASSERT_TRUE(waitFor([&] {
      state.observe({});
      for (const auto& install : state.snapshot().get("templateInstalls")->asArray())
        if (install.getString("requestId") == state.snapshot().get("lastResult")->getString("requestId") &&
            (install.getString("state") == "installed" || install.getString("state") == "failed")) { row = install; return true; }
      return false;
    }, std::chrono::seconds(5)));
    if (i < 16 - curated) EXPECT_EQ("installed", row.getString("state")) << id << ": " << row.stringify();
    else EXPECT_EQ("template-library-capacity", row.getString("reason")) << row.stringify();
  }
  // The shell cannot remove one either.
  EXPECT_FALSE(state.apply(request("graphics-template-remove", {{"instanceId", "installer"}, {"templateId", "pack-a-anchor"}, {"templateVersion", 1}})));
  EXPECT_EQ("template-built-in", state.snapshot().get("lastResult")->getString("reason"));
}

TEST(GraphicsBuiltInPacks, APackThatFailsTheLintIsSkippedAndReportedWhileTheOthersLoad) {
  PacksDir dir;
  dir.add("news-signal");
  const auto broken = dir.add("broken-pack");
  auto document = Json::parse(readFile(broken / "templates" / "anchor.json"))->asObject();
  auto layers = document.at("layers").asArray();
  for (auto& layer : layers) if (layer.getString("id") == "name-plate") {
    auto object = layer.asObject();
    object["fill"] = "#0B1F3AFF";  // raw hex without "fixed": the lint refuses it
    layer = Json(object);
  }
  document["layers"] = layers;
  writeFile(broken / "templates" / "anchor.json", Json(document).stringify());
  GraphicsTemplateRegistry registry;
  ASSERT_TRUE(registry.loadBuiltInPacks(utf8(dir.root)));
  std::shared_ptr<const GraphicsTemplateRegistry::Publication> published;
  ASSERT_TRUE(waitFor([&] { published = registry.poll(); return published->packs.asArray().size() == 2; }));
  const auto* bad = packRow(published->packs, "broken-pack");
  ASSERT_NE(bad, nullptr);
  EXPECT_EQ("invalid", bad->getString("state"));
  EXPECT_NE(bad->getString("detail").find("raw hex colour"), std::string::npos) << bad->stringify();
  EXPECT_TRUE(bad->get("templates")->asArray().empty());
  EXPECT_EQ(nullptr, catalogRow(published->catalog, "broken-pack-anchor"));
  EXPECT_EQ("ready", packRow(published->packs, "news-signal")->getString("state"));
  EXPECT_NE(nullptr, catalogRow(published->catalog, "news-signal-anchor"));
}

TEST(GraphicsBuiltInPacks, ThumbnailsRenderInTheRequestedPaletteAndRerenderWhenItChanges) {
  GraphicsTemplateRegistry registry;
  ASSERT_TRUE(registry.loadBuiltInPacks(utf8(kShippedPacks)));
  ASSERT_TRUE(waitFor([&] { return packRow(registry.poll()->packs, "news-signal") != nullptr; }));
  const auto paletteA = Json::Object{{"brand.primary", "#0B1F3AFF"}, {"brand.secondary", "#13315CFF"}, {"brand.accent", "#E63946FF"},
      {"brand.onPrimary", "#FFFFFFFF"}, {"brand.onSecondary", "#D8E2F0FF"}, {"brand.onAccent", "#FFFFFFFF"}, {"brand.surface", "#0A0F18B3"}};
  auto paletteB = paletteA;
  paletteB["brand.primary"] = "#1B5E20FF";
  paletteB["brand.accent"] = "#FFC400FF";
  const auto settled = [&](double revision, std::shared_ptr<const GraphicsTemplateRegistry::Publication>& out) {
    return waitFor([&] {
      out = registry.poll();
      if (out->thumbnailPalette.getNumber("revision") != revision) return false;
      for (const auto& row : out->catalog.asArray()) {
        const auto* thumb = row.get("thumbnail");
        if (!thumb || thumb->getString("state") == "pending" || thumb->getString("state") == "not-requested") return false;
        if (row.getString("runtime") == "declarative" && thumb->getNumber("paletteRevision", -1) != revision) return false;
      }
      return true;
    }, std::chrono::seconds(60));
  };
  ASSERT_TRUE(registry.requestThumbnails(paletteA));
  std::shared_ptr<const GraphicsTemplateRegistry::Publication> first, second;
  ASSERT_TRUE(settled(1, first)) << "every declarative thumbnail renders under palette revision 1";
  EXPECT_EQ(Json(paletteA).stringify(), first->thumbnailPalette.get("palette")->stringify());
  // The same palette again is not a recolour.
  ASSERT_TRUE(registry.requestThumbnails(paletteA));
  EXPECT_EQ(1, registry.poll()->thumbnailPalette.getNumber("revision"));
  ASSERT_TRUE(registry.requestThumbnails(paletteB));
  ASSERT_TRUE(settled(2, second)) << "a new palette re-renders every declarative thumbnail";
  // Compatibility presets ignore the palette: no paletteRevision, never re-rendered.
  for (const auto& row : second->catalog.asArray())
    if (row.getString("runtime") == "compatibility") EXPECT_EQ(nullptr, row.get("thumbnail")->get("paletteRevision"));
#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && (COREVIDEO_WITH_D3D11 || COREVIDEO_WITH_METAL)
  const auto* a = catalogRow(first->catalog, "news-signal-anchor");
  const auto* b = catalogRow(second->catalog, "news-signal-anchor");
  ASSERT_TRUE(a && b);
  ASSERT_EQ("ready", a->get("thumbnail")->getString("state")) << a->get("thumbnail")->getString("reason");
  ASSERT_EQ("ready", b->get("thumbnail")->getString("state")) << b->get("thumbnail")->getString("reason");
  EXPECT_NE(a->get("thumbnail")->getString("pngBase64"), b->get("thumbnail")->getString("pngBase64"))
      << "the card must show the palette, not static art";
#endif
}

TEST(GraphicsBuiltInPacks, TemplateListCarriesThePaletteAndObservesPacksAndThumbnailPalette) {
  GraphicsInstances state{"test-core"};
  const auto palette = Json::Object{{"brand.primary", "#0B1F3AFF"}, {"brand.accent", "#E63946FF"}};
  ASSERT_TRUE(state.apply(request("graphics-template-list", {{"instanceId", "lister"}, {"includeThumbnails", true}, {"palette", palette}})));
  ASSERT_TRUE(waitFor([&] { state.observe({}); return state.snapshot().get("templateThumbnailPalette")->getNumber("revision") == 1; }));
  EXPECT_EQ(Json(palette).stringify(), state.snapshot().get("templateThumbnailPalette")->get("palette")->stringify());
  ASSERT_NE(nullptr, state.snapshot().get("templatePacks"));
  EXPECT_TRUE(state.snapshot().get("templatePacks")->asArray().empty()) << "tests opt in to built-in packs";
  // A malformed palette is refused like everywhere else.
  EXPECT_FALSE(state.apply(request("graphics-template-list", {{"instanceId", "lister"}, {"includeThumbnails", true}, {"palette", Json::Object{{"brand.primary", "red"}}}})));
  EXPECT_EQ("invalid-graphics-palette", state.snapshot().get("lastResult")->getString("reason"));
}

TEST(GraphicsBuiltInPacks, AnimationsSpeedIsValidatedObservedAndOnlyReportedWhenNotOne) {
  GraphicsInstances state{"test-core"};
  const auto create = [&](const std::string& id, Json animations) {
    return state.apply(request("graphics-instance-create", {{"instanceId", id}, {"templateId", "corporate-lower-third"}, {"templateVersion", 1},
        {"scope", "global"}, {"fields", Json::Object{{"name", "Ada"}, {"title", "Host"}}}, {"animations", std::move(animations)}}));
  };
  ASSERT_TRUE(create("fast", Json::Object{{"speed", 2}}));
  ASSERT_TRUE(create("authored", Json::Object{{"in", Json::Object{{"type", "fade"}, {"durationMs", 300}}}}));
  EXPECT_FALSE(create("too-fast", Json::Object{{"speed", 10}}));
  EXPECT_EQ("invalid-animations", state.snapshot().get("lastResult")->getString("reason"));
  EXPECT_FALSE(create("stopped", Json::Object{{"speed", 0}}));
  EXPECT_FALSE(create("text", Json::Object{{"speed", "2"}}));
  for (const auto& row : state.snapshot().get("instances")->asArray()) {
    const auto* animations = row.get("animations");
    ASSERT_NE(animations, nullptr);
    if (row.getString("instanceId") == "fast") EXPECT_EQ(2, animations->getNumber("speed"));
    else EXPECT_EQ(nullptr, animations->get("speed")) << "speed 1 keeps every pre-speed observation unchanged";
  }
  // An update that changes only the speed is a new revision (not a no-op).
  ASSERT_TRUE(state.apply(request("graphics-instance-update", {{"instanceId", "fast"}, {"target", "cue"}, {"expectedRevision", 1}, {"animations", Json::Object{{"speed", 0.5}}}})));
  for (const auto& row : state.snapshot().get("instances")->asArray())
    if (row.getString("instanceId") == "fast") EXPECT_EQ(0.5, row.get("animations")->getNumber("speed"));
}

TEST(GraphicsBuiltInPacks, SpeedScalesTheLayerClockAndWholeGraphicTransitions) {
  GraphicsLayerClock clock;
  clock.configure(400, 200, 2);
  clock.show(1000);
  // 100 ms of wall time is 200 ms of authored motion at 2x.
  EXPECT_EQ(GraphicsMotionPhase::In, clock.state(1100).phase);
  EXPECT_NEAR(200, clock.state(1100).elapsedMs, 1e-9);
  EXPECT_EQ("on-air", clock.visibility(1200)) << "a 400 ms entrance at 2x settles after 200 ms";
  EXPECT_NEAR(150, clock.state(1150).loopMs, 1e-9) << "loops keep wall-clock time";
  clock.hide(2000);
  EXPECT_EQ("hidden", clock.visibility(2100 + 1)) << "a 200 ms exit at 2x ends after 100 ms";
  GraphicsLayerClock slow;
  slow.configure(400, 200, 0.5);
  slow.show(0);
  EXPECT_EQ("entering", slow.visibility(700));
  EXPECT_EQ("on-air", slow.visibility(800));
  GraphicsTransition fade;
  fade.type = "fade"; fade.durationMs = 300;
  EXPECT_NEAR(150, fade.scaled(2).durationMs, 1e-9);
  EXPECT_NEAR(600, fade.scaled(0.5).durationMs, 1e-9);
  EXPECT_NEAR(300, fade.scaled(1).durationMs, 1e-9);
}
