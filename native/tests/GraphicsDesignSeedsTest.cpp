#include "graphics/GraphicsTemplate.h"
#include "gtest/gtest.h"
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {
std::string readSeed(const std::string& kind) {
  const auto path = std::filesystem::path(COREVIDEO_REPO_ROOT) / "contracts" / "graphics-seeds" / (kind + ".json");
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text; text << in.rdbuf();
  return text.str();
}
}

TEST(GraphicsDesignSeeds, EverySeedLoadsAsADeclarativeTemplate) {
  const char* const families[][2] = {{"lower-third", "lower-third"}, {"title", "title"}, {"bug", "bug"}, {"full-screen", "title"}};
  for (const auto& entry : families) {
    const auto parsed = corevideo::rpc::Json::parse(readSeed(entry[0]));
    ASSERT_TRUE(parsed.has_value()) << entry[0];
    const auto& manifest = *parsed;
    ASSERT_TRUE(manifest.isObject());
    auto object = manifest.asObject();
    object["templateId"] = "design-seed-test-000000";
    std::string error;
    const auto loaded = corevideo::graphics::GraphicsTemplate::load(corevideo::rpc::Json(std::move(object)).stringify(), error);
    ASSERT_TRUE(loaded) << entry[0] << ": " << error;
    EXPECT_EQ(manifest.getString("family"), entry[1]);
  }
}