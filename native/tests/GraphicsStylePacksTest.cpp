// Wave 1 style packs (graphics styles spec 6.3, slice 10). The pack lint
// (GraphicsPack.ShippedPacksPassTheLint) checks each pack's format, fonts and
// colours; this checks the launch set itself: all seven packs, the spec 6.3
// deliverables, 2-3 palettes each, and every template resolving under every
// one of its palettes. Pixels are judged by the parity fixtures.

#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>

#include "graphics/GraphicsCompatibility.h"
#include "graphics/GraphicsTemplate.h"
#include "gtest/gtest.h"

namespace {
namespace fs = std::filesystem;
using corevideo::graphics::GraphicsDrawing;
using corevideo::graphics::GraphicsTemplate;
using corevideo::rpc::Json;

const fs::path kPacks = fs::path(COREVIDEO_REPO_ROOT) / "native" / "resources" / "graphics-packs";

std::string readText(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file), {});
}

const char* const kWaveOne[] = {"news-signal", "sports-kinetic", "worship-grace", "corporate-clarity",
                                "esports-neon", "talk-studio", "minimal-line"};

TEST(GraphicsStylePacks, AllSevenPacksShipTheSpecDeliverables) {
  for (const char* id : kWaveOne) {
    const auto manifest = Json::parse(readText(kPacks / id / "pack.json"));
    ASSERT_TRUE(manifest && manifest->isObject()) << id;
    const auto& palettes = manifest->get("palettes")->asArray();
    EXPECT_GE(palettes.size(), 2u) << id;
    EXPECT_LE(palettes.size(), 3u) << id;
    std::set<std::string> families;
    for (const auto& file : manifest->get("templates")->asArray()) {
      const auto document = Json::parse(readText(kPacks / id / file.asString()));
      ASSERT_TRUE(document) << id << ": " << file.asString();
      families.insert(document->getString("family"));
    }
    // Spec 6.3: lower thirds, a full-width title and a corner bug in every pack.
    for (const char* family : {"lower-third", "title", "bug"})
      EXPECT_TRUE(families.count(family)) << id << " has no " << family << " template";
    EXPECT_GE(manifest->get("templates")->asArray().size(), 6u) << id;
  }
}

TEST(GraphicsStylePacks, EveryTemplateResolvesUnderEveryPaletteOfItsPack) {
  for (const char* id : kWaveOne) {
    const auto manifest = Json::parse(readText(kPacks / id / "pack.json"));
    ASSERT_TRUE(manifest) << id;
    std::vector<Json> brands;
    for (const auto& file : manifest->get("palettes")->asArray()) {
      const auto colors = Json::parse(readText(kPacks / id / file.asString()));
      ASSERT_TRUE(colors && colors->isObject()) << id << ": " << file.asString();
      EXPECT_TRUE(corevideo::graphics::graphicsPaletteValid(*colors)) << id << ": " << file.asString();
      const auto appearance = corevideo::graphics::defaultAppearance("compact-solid");
      brands.push_back(corevideo::graphics::graphicsBrand(appearance, colors->getString("brand.primary"), false, *colors));
    }
    for (const auto& file : manifest->get("templates")->asArray()) {
      const auto where = std::string(id) + "/" + file.asString();
      std::string error;
      const auto definition = GraphicsTemplate::load(readText(kPacks / id / file.asString()), error);
      ASSERT_TRUE(definition) << where << ": " << error;
      EXPECT_EQ(0u, definition->id().rfind(std::string(id) + "-", 0)) << where << ": template id " << definition->id();
      // Defaults, with a sample in any required text a template leaves empty.
      auto fields = definition->defaultFields().asObject();
      for (const auto& field : definition->document().get("fields")->asArray())
        if (field.get("required") && field.get("required")->asBool() && field.getString("type") == "string" &&
            fields[field.getString("id")].asString().empty())
          fields[field.getString("id")] = "Sample";
      for (const auto& brand : brands) {
        GraphicsDrawing drawing;
        EXPECT_TRUE(definition->resolve(Json(fields), brand, drawing, error)) << where << ": " << error;
        EXPECT_FALSE(drawing.primitives.empty()) << where;
      }
    }
  }
}

}  // namespace
