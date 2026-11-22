// Core Text measurement for template layout (Metal graphics backend G2).
//
// On a Metal build GraphicsTemplate's measureText must use Core Text, not the
// portable `characters * size * 0.55` estimate, so auto-width plates, crawl
// item widths and minFontSize fits are laid out from the same shaped widths the
// raster draws. The discriminating fixture: ten "M"s and ten "i"s have the same
// character count, so the estimate gives them EQUAL widths; real typography
// makes the M run several times wider.

#if defined(__APPLE__)

#include <gtest/gtest.h>

#include <string>

#include "graphics/GraphicsTemplate.h"
#include "graphics/GraphicsTextCoreText.h"

namespace {
using namespace corevideo::graphics;
using corevideo::rpc::Json;

TEST(GraphicsTextMeasure, ShapedWidthsNotTheCharacterEstimate) {
  const auto wide = measureGraphicsText("MMMMMMMMMM", "Arial", 40, 400, 8192, false);
  const auto narrow = measureGraphicsText("iiiiiiiiii", "Arial", 40, 400, 8192, false);
  EXPECT_EQ(wide.lines, 1u);
  EXPECT_GT(wide.width, narrow.width * 2.5) << wide.width << " vs " << narrow.width;
  // Trailing whitespace is part of the width (DirectWrite's
  // widthIncludingTrailingWhitespace), so "ab " is wider than "ab".
  EXPECT_GT(measureGraphicsText("ab ", "Arial", 40, 400, 8192, false).width,
            measureGraphicsText("ab", "Arial", 40, 400, 8192, false).width);
}

TEST(GraphicsTextMeasure, WrappedTextCountsLinesThatEachFit) {
  const std::string sentence = "The quick brown fox jumps over the lazy dog again and again";
  const auto single = measureGraphicsText(sentence, "Arial", 30, 400, 8192, false);
  const auto wrapped = measureGraphicsText(sentence, "Arial", 30, 400, 300, true);
  ASSERT_GT(single.width, 600.0);
  EXPECT_GE(wrapped.lines, 3u);
  // Widest line may exceed the wrap width only by its trailing space.
  EXPECT_LE(wrapped.width, 300.0 + 30.0 * 0.5);
}

#if COREVIDEO_WITH_METAL && !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS
TEST(GraphicsTextMeasure, TemplateLayoutUsesCoreTextOnMetal) {
  using M = Json::Object;
  auto document = GraphicsTemplate::corporateLowerThird()->document().asObject();
  document["fields"] = Json::Array{M{{"id", "crawl-items"}, {"type", "string"}, {"default", "[\"Sample\"]"}, {"maxLength", 4096}}};
  document["layers"] = Json::Array{M{{"id", "crawl"}, {"type", "crawl"}, {"x", 30}, {"y", 4}, {"width", 140}, {"height", 40},
      {"items", M{{"field", "crawl-items"}}}, {"font", "Arial"}, {"fontSize", 20}, {"color", "#FFFFFF"}}};
  std::string error;
  const auto definition = GraphicsTemplate::load(Json(document).stringify(), error);
  ASSERT_TRUE(definition) << error;
  GraphicsDrawing drawing;
  ASSERT_TRUE(definition->resolve(M{{"crawl-items", "[\"MMMMMMMMMM\",\"iiiiiiiiii\"]"}},
      Json::Object{{"brand.primary", "#10203080"}, {"brand.onPrimary", "#FFFFFF"}, {"brand.onPrimaryMuted", "#C0C8D0"},
                   {"brand.fontBody", "Arial"}, {"brand.accent", "#44C1A1"}}, drawing, error)) << error;
  ASSERT_EQ(drawing.primitives.size(), 2u);
  const double m = drawing.primitives[0].box.width, i = drawing.primitives[1].box.width;
  EXPECT_GT(m, i * 2.5) << "crawl item widths came from the 0.55 estimate (" << m << " vs " << i << ")";
  EXPECT_NE(m, 110.0) << "10 chars * 20 px * 0.55";
}
#endif

}  // namespace

#endif  // __APPLE__
