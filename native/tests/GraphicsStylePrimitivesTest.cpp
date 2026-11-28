// Graphics styles renderer primitives (docs/reference/graphics-styles-color-lower-thirds-spec.md
// section 6.1): the shared model's schema, limits and resolved primitives.
// Pixels are judged by the parity fixtures (native/tests/fixtures/graphics-parity).

#include <array>
#include <cmath>
#include <string>

#include "graphics/GraphicsTemplate.h"
#include "gtest/gtest.h"

namespace {
using corevideo::graphics::GraphicsDrawing;
using corevideo::graphics::GraphicsTemplate;
using corevideo::rpc::Json;
using M = Json::Object;

// One template, bounds 800x200, with `layers`.
std::string document(Json::Array layers) {
  return Json(M{{"schemaVersion", 1}, {"templateId", "styles"}, {"templateVersion", 1}, {"name", "styles"},
                {"family", "title"}, {"canvas", M{{"width", 1920}, {"height", 1080}}},
                {"bounds", M{{"width", 800}, {"height", 200}}}, {"brandTokens", Json::Array{}},
                {"fields", Json::Array{}},
                {"animations", M{{"in", M{{"type", "none"}}}, {"out", M{{"type", "none"}}}}},
                {"layers", std::move(layers)}})
      .stringify();
}

M text(M extra) {
  M node{{"id", "t"}, {"type", "text"}, {"x", 10}, {"y", 10}, {"width", 700}, {"height", 60},
         {"text", "Stra\xC3\x9F" "e news"}, {"font", "Arial"}, {"fontSize", 40}, {"color", "#FFFFFF"}};
  for (auto& [key, value] : extra) node[key] = value;
  return node;
}

// Loads and resolves a one-layer template; returns the error ("" when it loaded).
std::string resolveOne(M layer, GraphicsDrawing& drawing) {
  std::string error;
  const auto definition = GraphicsTemplate::load(document(Json::Array{Json(std::move(layer))}), error);
  if (!definition) return error;
  if (!definition->resolve(definition->defaultFields(), M{}, drawing, error)) return error;
  return "";
}

// ── P1: letterSpacing, fontStyle, textTransform ──────────────────────────────

TEST(GraphicsStylePrimitives, TextTransformMapsCaseOneToOneOnEveryPlatform) {
  using corevideo::graphics::transformGraphicsText;
  EXPECT_EQ("BREAKING NEWS 24/7", transformGraphicsText("Breaking news 24/7", "uppercase"));
  EXPECT_EQ("breaking news", transformGraphicsText("BREAKING News", "lowercase"));
  // Latin-1 and Latin Extended-A; U+00DF has no one-character capital and stays.
  EXPECT_EQ("\xC3\x89T\xC3\x89 STRA\xC3\x9F" "E \xC5\x81\xC3\x93" "D\xC5\xBB", transformGraphicsText(
      "\xC3\xA9t\xC3\xA9 stra\xC3\x9F" "e \xC5\x82\xC3\xB3" "d\xC5\xBC", "uppercase"));
  EXPECT_EQ("\xC3\xA9t\xC3\xA9 \xC5\x82\xC3\xB3" "d\xC5\xBC", transformGraphicsText(
      "\xC3\x89T\xC3\x89 \xC5\x81\xC3\x93" "D\xC5\xBB", "lowercase"));
  // Greek (final sigma capitalises to SIGMA) and Cyrillic.
  EXPECT_EQ("\xCE\x9B\xCE\x9F\xCE\x93\xCE\x9F\xCE\xA3", transformGraphicsText("\xCE\xBB\xCE\xBF\xCE\xB3\xCE\xBF\xCF\x82", "uppercase"));
  EXPECT_EQ("\xD0\x9D\xD0\x9E\xD0\x92\xD0\x9E\xD0\xA1\xD0\xA2\xD0\x98 \xD0\x81", transformGraphicsText(
      "\xD0\xBD\xD0\xBE\xD0\xB2\xD0\xBE\xD1\x81\xD1\x82\xD0\xB8 \xD1\x91", "uppercase"));
  // Untouched: other scripts, "none", and bytes that are not UTF-8.
  EXPECT_EQ("\xE6\x97\xA5\xE6\x9C\xAC ABC", transformGraphicsText("\xE6\x97\xA5\xE6\x9C\xAC abc", "uppercase"));
  EXPECT_EQ("Mixed Case", transformGraphicsText("Mixed Case", "none"));
  EXPECT_EQ(std::string("A\xFF" "B"), transformGraphicsText(std::string("a\xFF" "b"), "uppercase"));
}

TEST(GraphicsStylePrimitives, TextStyleResolvesIntoThePrimitive) {
  GraphicsDrawing drawing;
  ASSERT_EQ("", resolveOne(text({{"letterSpacing", 0.1}, {"fontStyle", "italic"}, {"textTransform", "uppercase"}}),
                           drawing));
  ASSERT_EQ(1u, drawing.primitives.size());
  const auto& p = drawing.primitives[0];
  EXPECT_EQ("STRA\xC3\x9F" "E NEWS", p.text);
  EXPECT_TRUE(p.italic);
  EXPECT_NEAR(4.0, p.letterSpacing, 1e-9);  // 0.1 em of 40 px
  ASSERT_EQ("", resolveOne(text({}), drawing));
  EXPECT_FALSE(drawing.primitives[0].italic);
  EXPECT_EQ(0.0, drawing.primitives[0].letterSpacing);
  EXPECT_EQ("Stra\xC3\x9F" "e news", drawing.primitives[0].text);
}

TEST(GraphicsStylePrimitives, TextStyleLimitsAreEnforced) {
  GraphicsDrawing drawing;
  EXPECT_EQ("", resolveOne(text({{"letterSpacing", -0.25}}), drawing));
  EXPECT_EQ("", resolveOne(text({{"letterSpacing", 1}}), drawing));
  EXPECT_EQ("template-geometry-or-number", resolveOne(text({{"letterSpacing", -0.26}}), drawing));
  EXPECT_EQ("template-geometry-or-number", resolveOne(text({{"letterSpacing", 1.01}}), drawing));
  EXPECT_EQ("template-geometry-or-number", resolveOne(text({{"letterSpacing", "wide"}}), drawing));
  EXPECT_EQ("template-font-style", resolveOne(text({{"fontStyle", "oblique"}}), drawing));
  EXPECT_EQ("template-string-required", resolveOne(text({{"fontStyle", true}}), drawing));
  EXPECT_EQ("template-text-transform", resolveOne(text({{"textTransform", "capitalize"}}), drawing));
  // Not text properties anywhere else.
  EXPECT_EQ("unsupported-template-property",
            resolveOne(M{{"id", "r"}, {"type", "rect"}, {"x", 0}, {"y", 0}, {"width", 10}, {"height", 10},
                         {"fill", "#FFFFFF"}, {"letterSpacing", 0.1}}, drawing));
}

TEST(GraphicsStylePrimitives, LetterSpacingWidensMeasuredTextAndShrinksAFit) {
  // A fit to a narrow box: tracked text needs a smaller font for the same width.
  GraphicsDrawing plain, tracked;
  ASSERT_EQ("", resolveOne(text({{"width", 200}, {"minFontSize", 8}}), plain));
  ASSERT_EQ("", resolveOne(text({{"width", 200}, {"minFontSize", 8}, {"letterSpacing", 0.3}}), tracked));
  EXPECT_LT(tracked.primitives[0].fontSize, plain.primitives[0].fontSize);
  EXPECT_NEAR(0.3 * tracked.primitives[0].fontSize, tracked.primitives[0].letterSpacing, 1e-9);
#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && (COREVIDEO_WITH_D3D11 || COREVIDEO_WITH_METAL)
  // Uppercase is measured as drawn: capitals are wider, so the fit is smaller
  // still (real typography only; the portable estimate counts characters).
  GraphicsDrawing upper;
  ASSERT_EQ("", resolveOne(text({{"width", 200}, {"minFontSize", 8}, {"textTransform", "uppercase"}}), upper));
  EXPECT_LT(upper.primitives[0].fontSize, plain.primitives[0].fontSize);
#endif
}

// ── P2: angled, multi-stop and radial gradients ──────────────────────────────

M rect(Json fill) {
  return M{{"id", "r"}, {"type", "rect"}, {"x", 100}, {"y", 20}, {"width", 400}, {"height", 100}, {"fill", std::move(fill)}};
}

TEST(GraphicsStylePrimitives, TheV1TwoStopFormStillParsesAsNinetyAndOneEightyDegrees) {
  GraphicsDrawing drawing;
  ASSERT_EQ("", resolveOne(rect(M{{"stops", Json::Array{"#FF0000", "#0000FF80"}}}), drawing));
  const auto& p = drawing.primitives[0];
  ASSERT_TRUE(p.gradient);
  EXPECT_FALSE(p.gradient->radial);
  EXPECT_EQ(90.0, p.gradient->angle);
  ASSERT_EQ(2u, p.gradient->stops.size());
  EXPECT_EQ(0xffff0000u, p.color);  // the first stop
  EXPECT_EQ(0.0, p.gradient->stops[0].offset);
  EXPECT_EQ(1.0, p.gradient->stops[1].offset);
  EXPECT_EQ(0x800000ffu, p.gradient->stops[1].color);
  // V1 geometry, exactly: horizontal runs along the top edge, vertical down the left.
  const auto horizontal = corevideo::graphics::linearGradientLine(*p.gradient, p.box);
  EXPECT_EQ((std::array<double, 4>{100, 20, 500, 20}), horizontal);
  ASSERT_EQ("", resolveOne(rect(M{{"stops", Json::Array{"#FF0000", "#0000FF"}}, {"direction", "vertical"}}), drawing));
  EXPECT_EQ(180.0, drawing.primitives[0].gradient->angle);
  EXPECT_EQ((std::array<double, 4>{100, 20, 100, 120}),
            corevideo::graphics::linearGradientLine(*drawing.primitives[0].gradient, drawing.primitives[0].box));
  EXPECT_EQ("template-gradient-direction",
            resolveOne(rect(M{{"stops", Json::Array{"#FF0000", "#0000FF"}}, {"direction", "diagonal"}}), drawing));
}

TEST(GraphicsStylePrimitives, AngledGradientLineCoversTheBoxCornersLikeCss) {
  corevideo::graphics::GraphicsGradient g;
  const corevideo::graphics::GraphicsBox box{0, 0, 400, 100};
  g.angle = 0;  // to top
  auto line = corevideo::graphics::linearGradientLine(g, box);
  EXPECT_NEAR(200, line[0], 1e-9); EXPECT_NEAR(100, line[1], 1e-9);
  EXPECT_NEAR(200, line[2], 1e-9); EXPECT_NEAR(0, line[3], 1e-9);
  g.angle = 45;
  line = corevideo::graphics::linearGradientLine(g, box);
  // Both far corners project onto the ends: (0,100) -> 0 and (400,0) -> 1.
  const double dx = line[2] - line[0], dy = line[3] - line[1], length2 = dx * dx + dy * dy;
  EXPECT_NEAR(0, ((0 - line[0]) * dx + (100 - line[1]) * dy) / length2, 1e-9);
  EXPECT_NEAR(1, ((400 - line[0]) * dx + (0 - line[1]) * dy) / length2, 1e-9);
}

TEST(GraphicsStylePrimitives, MultiStopAndRadialGradientsResolve) {
  GraphicsDrawing drawing;
  ASSERT_EQ("", resolveOne(rect(M{{"type", "linear"}, {"angle", 135},
                                  {"stops", Json::Array{"#FF0000", "#00FF00", "#0000FF", "#FFFFFF"}}}), drawing));
  const auto& linear = *drawing.primitives[0].gradient;
  EXPECT_EQ(135.0, linear.angle);
  ASSERT_EQ(4u, linear.stops.size());
  EXPECT_NEAR(1.0 / 3, linear.stops[1].offset, 1e-12);
  EXPECT_NEAR(2.0 / 3, linear.stops[2].offset, 1e-12);
  ASSERT_EQ("", resolveOne(rect(M{{"type", "linear"}, {"stops", Json::Array{"#FF0000", "#0000FF"}}}), drawing));
  EXPECT_EQ(180.0, drawing.primitives[0].gradient->angle);  // CSS default: to bottom
  ASSERT_EQ("", resolveOne(rect(M{{"type", "radial"}, {"center", Json::Array{0.25, 1}},
                                  {"stops", Json::Array{M{{"color", "#FFFFFF"}, {"offset", 0}},
                                                        M{{"color", "#FFFFFF80"}, {"offset", 0.4}},
                                                        M{{"color", "#00000000"}, {"offset", 1}}}}}), drawing));
  const auto& radial = *drawing.primitives[0].gradient;
  EXPECT_TRUE(radial.radial);
  EXPECT_EQ(0.25, radial.centerX);
  EXPECT_EQ(1.0, radial.centerY);
  EXPECT_EQ(0.4, radial.stops[1].offset);
  EXPECT_EQ(0x80ffffffu, radial.stops[1].color);
  // Ellipses take gradients too.
  const M ellipse{{"id", "e"}, {"type", "ellipse"}, {"x", 0}, {"y", 0}, {"width", 50}, {"height", 50},
                  {"fill", M{{"type", "radial"}, {"stops", Json::Array{"#FFFFFF", "#000000"}}}}};
  ASSERT_EQ("", resolveOne(ellipse, drawing));
  EXPECT_TRUE(drawing.primitives[0].gradient->radial);
}

TEST(GraphicsStylePrimitives, GradientLimitsAreEnforced) {
  GraphicsDrawing drawing;
  Json::Array eight, nine;
  for (int i = 0; i < 8; ++i) eight.push_back("#FFFFFF");
  nine = eight;
  nine.push_back("#000000");
  EXPECT_EQ("", resolveOne(rect(M{{"type", "linear"}, {"stops", eight}}), drawing));
  EXPECT_EQ("template-gradient-stops", resolveOne(rect(M{{"type", "linear"}, {"stops", nine}}), drawing));
  EXPECT_EQ("template-gradient-stops", resolveOne(rect(M{{"stops", Json::Array{"#FFFFFF"}}}), drawing));
  EXPECT_EQ("template-gradient-stops", resolveOne(rect(M{{"type", "linear"}}), drawing));
  EXPECT_EQ("template-gradient-type",
            resolveOne(rect(M{{"type", "conic"}, {"stops", Json::Array{"#FFFFFF", "#000000"}}}), drawing));
  EXPECT_EQ("template-geometry-or-number",
            resolveOne(rect(M{{"angle", 361}, {"stops", Json::Array{"#FFFFFF", "#000000"}}}), drawing));
  EXPECT_EQ("template-gradient-angle",
            resolveOne(rect(M{{"type", "radial"}, {"angle", 45}, {"stops", Json::Array{"#FFFFFF", "#000000"}}}), drawing));
  EXPECT_EQ("template-gradient-center",
            resolveOne(rect(M{{"type", "linear"}, {"center", Json::Array{0.5, 0.5}}, {"stops", Json::Array{"#FFFFFF", "#000000"}}}), drawing));
  EXPECT_EQ("template-gradient-center",
            resolveOne(rect(M{{"type", "radial"}, {"center", Json::Array{0.5, 1.5}}, {"stops", Json::Array{"#FFFFFF", "#000000"}}}), drawing));
  EXPECT_EQ("template-gradient-direction",
            resolveOne(rect(M{{"type", "linear"}, {"direction", "vertical"}, {"stops", Json::Array{"#FFFFFF", "#000000"}}}), drawing));
  EXPECT_EQ("unsupported-template-property",
            resolveOne(rect(M{{"type", "linear"}, {"repeat", true}, {"stops", Json::Array{"#FFFFFF", "#000000"}}}), drawing));
  // Offsets: all or none, in 0..1, never decreasing.
  EXPECT_EQ("template-gradient-offset", resolveOne(rect(M{{"type", "linear"}, {"stops", Json::Array{
      M{{"color", "#FFFFFF"}, {"offset", 0}}, "#000000"}}}), drawing));
  EXPECT_EQ("template-gradient-offset", resolveOne(rect(M{{"type", "linear"}, {"stops", Json::Array{
      M{{"color", "#FFFFFF"}, {"offset", 0.6}}, M{{"color", "#000000"}, {"offset", 0.5}}}}}), drawing));
  EXPECT_EQ("template-geometry-or-number", resolveOne(rect(M{{"type", "linear"}, {"stops", Json::Array{
      M{{"color", "#FFFFFF"}, {"offset", 0}}, M{{"color", "#000000"}, {"offset", 1.5}}}}}), drawing));
  EXPECT_EQ("template-color-required", resolveOne(rect(M{{"type", "linear"}, {"stops", Json::Array{"red", "#000000"}}}), drawing));
}

}  // namespace
