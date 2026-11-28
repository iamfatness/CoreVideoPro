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

// A crawl sets its own weight (slice 10 ticker skins); 400 when absent.
TEST(GraphicsStylePrimitives, CrawlTakesATextWeight) {
  const auto crawl = [](Json weight) {
    std::string error;
    M node{{"id", "c"}, {"type", "crawl"}, {"x", 0}, {"y", 0}, {"width", 700}, {"height", 50},
           {"items", M{{"field", "items"}}}, {"font", "Arial"}, {"fontSize", 30}, {"color", "#FFFFFF"}};
    if (!weight.isNull()) node["fontWeight"] = weight;
    auto doc = Json::parse(document(Json::Array{Json(node)}))->asObject();
    doc["fields"] = Json::Array{M{{"id", "items"}, {"type", "string"}, {"default", "[\"One\",\"Two\"]"}, {"maxLength", 4096}}};
    const auto definition = GraphicsTemplate::load(Json(doc).stringify(), error);
    GraphicsDrawing drawing;
    if (!definition || !definition->resolve(definition->defaultFields(), M{}, drawing, error)) return std::make_pair(error, 0);
    return std::make_pair(std::string(), drawing.primitives.front().fontWeight);
  };
  EXPECT_EQ(400, crawl(Json()).second);
  EXPECT_EQ(600, crawl(Json(600)).second);
  EXPECT_EQ("template-font-weight", crawl(Json(450)).first);
}

// ── P3: polygon and skewed plates ────────────────────────────────────────────

M plate(M extra) {
  M node = rect("#FFFFFF");
  for (auto& [key, value] : extra) node[key] = value;
  return node;
}

TEST(GraphicsStylePrimitives, PolygonPointsAreBoxUnitsResolvedToTemplateUnits) {
  GraphicsDrawing drawing;
  ASSERT_EQ("", resolveOne(plate({{"shape", "polygon"},
                                  {"points", Json::Array{Json::Array{0, 0}, Json::Array{1, 0}, Json::Array{0.5, 1}}}}),
                           drawing));
  const auto& p = drawing.primitives[0];
  EXPECT_EQ("rect", p.type);
  ASSERT_EQ(3u, p.polygon.size());
  EXPECT_EQ(100.0, p.polygon[0].x); EXPECT_EQ(20.0, p.polygon[0].y);
  EXPECT_EQ(500.0, p.polygon[1].x); EXPECT_EQ(20.0, p.polygon[1].y);
  EXPECT_EQ(300.0, p.polygon[2].x); EXPECT_EQ(120.0, p.polygon[2].y);
  // A plain rect has no outline.
  ASSERT_EQ("", resolveOne(plate({}), drawing));
  EXPECT_TRUE(drawing.primitives[0].polygon.empty());
}

TEST(GraphicsStylePrimitives, SkewXIsAParallelogramInsideTheBox) {
  GraphicsDrawing drawing;
  ASSERT_EQ("", resolveOne(plate({{"skewX", 45}}), drawing));  // tan 45 * 100 = 100 px lean
  auto points = drawing.primitives[0].polygon;
  ASSERT_EQ(4u, points.size());
  EXPECT_NEAR(200, points[0].x, 1e-9); EXPECT_EQ(20.0, points[0].y);   // top-left leans right
  EXPECT_NEAR(500, points[1].x, 1e-9);
  EXPECT_NEAR(400, points[2].x, 1e-9); EXPECT_EQ(120.0, points[2].y);  // bottom-right pulls in
  EXPECT_NEAR(100, points[3].x, 1e-9);
  ASSERT_EQ("", resolveOne(plate({{"skewX", -45}}), drawing));
  points = drawing.primitives[0].polygon;
  EXPECT_NEAR(100, points[0].x, 1e-9);
  EXPECT_NEAR(400, points[1].x, 1e-9);
  EXPECT_NEAR(500, points[2].x, 1e-9);
  EXPECT_NEAR(200, points[3].x, 1e-9);
  ASSERT_EQ("", resolveOne(plate({{"skewX", 0}}), drawing));
  EXPECT_TRUE(drawing.primitives[0].polygon.empty());
}

TEST(GraphicsStylePrimitives, PolygonAndSkewLimitsAreEnforced) {
  GraphicsDrawing drawing;
  Json::Array sixteen, seventeen;
  for (int i = 0; i < 16; ++i) sixteen.push_back(Json::Array{double(i % 2), double(i) / 15});
  seventeen = sixteen;
  seventeen.push_back(Json::Array{0.5, 0.5});
  EXPECT_EQ("", resolveOne(plate({{"shape", "polygon"}, {"points", sixteen}}), drawing));
  EXPECT_EQ("template-polygon-points", resolveOne(plate({{"shape", "polygon"}, {"points", seventeen}}), drawing));
  EXPECT_EQ("template-polygon-points", resolveOne(plate({{"shape", "polygon"},
      {"points", Json::Array{Json::Array{0, 0}, Json::Array{1, 0}}}}), drawing));
  EXPECT_EQ("template-polygon-points", resolveOne(plate({{"shape", "polygon"},
      {"points", Json::Array{Json::Array{0, 0}, Json::Array{1, 0}, Json::Array{1.5, 1}}}}), drawing));
  EXPECT_EQ("template-polygon-points", resolveOne(plate({{"shape", "polygon"},
      {"points", Json::Array{Json::Array{0, 0}, Json::Array{1, 0}, Json::Array{1}}}}), drawing));
  EXPECT_EQ("template-polygon-points", resolveOne(plate({{"shape", "polygon"}}), drawing));
  EXPECT_EQ("template-polygon-points", resolveOne(plate({{"points", sixteen}}), drawing));  // points need the shape
  EXPECT_EQ("template-shape", resolveOne(plate({{"shape", "star"}}), drawing));
  EXPECT_EQ("template-shape-skew", resolveOne(plate({{"shape", "polygon"}, {"points", sixteen}, {"skewX", 10}}), drawing));
  EXPECT_EQ("template-geometry-or-number", resolveOne(plate({{"skewX", 61}}), drawing));
  // tan 60 * 100 = 173 px fits a 400 px box; a 150 px box does not.
  EXPECT_EQ("", resolveOne(plate({{"skewX", 60}}), drawing));
  EXPECT_EQ("template-shape-skew", resolveOne(plate({{"skewX", 60}, {"width", 150}}), drawing));
  EXPECT_EQ("template-shape-polygon-style", resolveOne(plate({{"skewX", 10}, {"cornerRadius", 4}}), drawing));
  EXPECT_EQ("template-shape-polygon-style",
            resolveOne(plate({{"skewX", 10}, {"stroke", "#000000"}, {"strokeWidth", 2}}), drawing));
  const M ellipse{{"id", "e"}, {"type", "ellipse"}, {"x", 0}, {"y", 0}, {"width", 50}, {"height", 50},
                  {"fill", "#FFFFFF"}, {"skewX", 10}};
  EXPECT_EQ("unsupported-template-property", resolveOne(ellipse, drawing));
}

// ── P4: shape drop shadow and glow ───────────────────────────────────────────

TEST(GraphicsStylePrimitives, ShapeShadowResolvesWithItsClipOutsideTheBox) {
  GraphicsDrawing drawing;
  ASSERT_EQ("", resolveOne(plate({{"shadow", M{{"color", "#00000080"}, {"dx", 4}, {"dy", 6}, {"blur", 8}, {"spread", 2}}}}),
                           drawing));
  const auto& p = drawing.primitives[0];
  ASSERT_TRUE(p.shapeShadow);
  EXPECT_EQ(0x80000000u, p.shapeShadow->color);
  EXPECT_EQ(4.0, p.shapeShadow->dx);
  EXPECT_EQ(6.0, p.shapeShadow->dy);
  EXPECT_EQ(8.0, p.shapeShadow->blur);
  EXPECT_EQ(2.0, p.shapeShadow->spread);
  // Extent = spread + 2 * blur + 1 = 19 around the shifted box, inside the 800x200 root.
  EXPECT_EQ(100.0 + 4 - 19, p.shadowClip.x);
  EXPECT_EQ(20.0 + 6 - 19, p.shadowClip.y);
  EXPECT_EQ(400.0 + 38, p.shadowClip.width);
  EXPECT_EQ(100.0 + 38, p.shadowClip.height);
  // The shape's own clip is unchanged.
  EXPECT_EQ(100.0, p.clip.x);
  EXPECT_EQ(400.0, p.clip.width);
  // Glow: a shadow with no offset, here on an ellipse; defaults are 0.
  const M glow{{"id", "e"}, {"type", "ellipse"}, {"x", 0}, {"y", 0}, {"width", 50}, {"height", 50},
               {"fill", "#FFFFFF"}, {"shadow", M{{"color", "#44C1A1"}, {"blur", 12}}}};
  ASSERT_EQ("", resolveOne(glow, drawing));
  const auto& e = drawing.primitives[0];
  EXPECT_EQ(0.0, e.shapeShadow->dx);
  EXPECT_EQ(0.0, e.shapeShadow->spread);
  EXPECT_EQ(0.0, e.shadowClip.x);  // clamped to the root
}

TEST(GraphicsStylePrimitives, AnAtlasSliceHoldsTheShadowNotJustTheBox) {
  GraphicsDrawing drawing;
  ASSERT_EQ("", resolveOne(plate({{"shadow", M{{"color", "#000000"}, {"dy", 10}, {"blur", 8}}},
                                  {"animations", M{{"in", M{{"type", "fade"}, {"durationMs", 200}}}}}}), drawing));
  const auto runs = corevideo::graphics::planRuns(drawing);
  ASSERT_EQ(1u, runs.size());
  const auto& clip = drawing.primitives[0].shadowClip;
  EXPECT_EQ(clip.x, runs[0].source.x);
  EXPECT_EQ(clip.y, runs[0].source.y);
  EXPECT_EQ(clip.width, runs[0].source.width);
  EXPECT_EQ(clip.height, runs[0].source.height);
  EXPECT_LT(runs[0].source.x, drawing.primitives[0].box.x);
}

TEST(GraphicsStylePrimitives, ShapeShadowLimitsAreEnforced) {
  GraphicsDrawing drawing;
  const auto with = [&](M shadow) { return resolveOne(plate({{"shadow", std::move(shadow)}}), drawing); };
  EXPECT_EQ("", with(M{{"color", "#000000"}, {"dx", -256}, {"dy", 256}, {"blur", 64}, {"spread", 64}}));
  EXPECT_EQ("template-geometry-or-number", with(M{{"color", "#000000"}, {"dx", 257}}));
  EXPECT_EQ("template-geometry-or-number", with(M{{"color", "#000000"}, {"blur", 64.5}}));
  EXPECT_EQ("template-geometry-or-number", with(M{{"color", "#000000"}, {"blur", -1}}));
  EXPECT_EQ("template-geometry-or-number", with(M{{"color", "#000000"}, {"spread", -1}}));
  EXPECT_EQ("template-geometry-or-number", with(M{{"color", "#000000"}, {"spread", 65}}));
  EXPECT_EQ("template-property-required", with(M{{"blur", 4}}));
  EXPECT_EQ("unsupported-template-property", with(M{{"color", "#000000"}, {"x", 4}}));
}

}  // namespace

// ── ShapeShadowRaster: the shared shadow pixels ──────────────────────────────

#include "compositor/ShapeShadowRaster.h"

namespace {
namespace shadows = corevideo::compositor::shapeshadow;

TEST(ShapeShadowRaster, CoverageIsExactAreaForAxisAlignedAndSlantedEdges) {
  // A 2.5 x 1.5 px rect at (0.25, 0.5): corner pixel areas are exact.
  const auto area = [](const std::vector<uint32_t>& values) {
    std::vector<double> out;
    for (const auto v : values) out.push_back(v / 65535.0);
    return out;
  };
  const auto c = area(shadows::polygonCoverage({{0.25, 0.5}, {2.75, 0.5}, {2.75, 2}, {0.25, 2}}, 0, 0, 4, 3));
  EXPECT_NEAR(0.75 * 0.5, c[0], 2e-5);
  EXPECT_NEAR(0.5, c[1], 2e-5);
  EXPECT_NEAR(0.75 * 0.5, c[2], 2e-5);
  EXPECT_NEAR(0.0, c[3], 2e-5);
  EXPECT_NEAR(0.75, c[4], 2e-5);
  EXPECT_NEAR(1.0, c[5], 2e-5);
  EXPECT_NEAR(0.0, c[8], 2e-5);
  // A right triangle across a 4x4 buffer: half of every diagonal pixel, and
  // the total equals the area (8), whichever way it winds.
  for (const bool clockwise : {true, false}) {
    std::vector<shadows::Point> triangle{{0, 0}, {4, 4}, {0, 4}};
    if (!clockwise) std::swap(triangle[0], triangle[2]);
    const auto t = area(shadows::polygonCoverage(triangle, 0, 0, 4, 4));
    double total = 0;
    for (const auto v : t) total += v;
    EXPECT_NEAR(8.0, total, 1e-3);
    EXPECT_NEAR(0.5, t[0], 2e-5);
    EXPECT_NEAR(1.0, t[4], 2e-5);
    EXPECT_NEAR(0.0, t[1], 2e-5);
  }
  // A buffer offset into the shape, with the shape running off every side.
  const auto inside = area(shadows::polygonCoverage({{-10, -10}, {10, -10}, {10, 10}, {-10, 10}}, 2, 3, 3, 3));
  for (const auto v : inside) EXPECT_NEAR(1.0, v, 2e-5);
}

TEST(ShapeShadowRaster, BoxRadiusFollowsTheCssBlurRadius) {
  EXPECT_EQ(0, shadows::boxRadius(0));
  EXPECT_EQ(0, shadows::boxRadius(1));    // w = 1.41
  EXPECT_EQ(4, shadows::boxRadius(8));    // w = 8.06
  EXPECT_EQ(12, shadows::boxRadius(24));  // w = 24.02
  EXPECT_EQ(32, shadows::boxRadius(64));
}

TEST(ShapeShadowRaster, BlurConservesCoverageAndGlowIsSymmetric) {
  shadows::Outline square;
  square.x = 30; square.y = 30; square.width = 40; square.height = 40;
  const auto glow = shadows::rasterize(square, 0xff000000u, 0, 0, 8, 0, 1, 0, 0, 100, 100, 1, 0, 0, 100, 100);
  ASSERT_EQ(100, glow.width);
  ASSERT_EQ(100, glow.height);
  double total = 0;
  for (size_t i = 3; i < glow.bgra.size(); i += 4) total += glow.bgra[i];
  EXPECT_NEAR(1600.0 * 255, total, 1600.0 * 255 * 0.01);  // the blur spreads, never adds or loses
  const auto at = [&](int x, int y) { return int(glow.bgra[(size_t(y) * 100 + size_t(x)) * 4 + 3]); };
  EXPECT_EQ(at(25, 50), at(74, 50));  // mirror images across the square's centre line
  EXPECT_EQ(at(50, 25), at(50, 74));
  EXPECT_EQ(255, at(50, 50));          // more than 3k = 12 px inside stays solid
  EXPECT_LT(at(30, 50), 255);          // the edge is soft
  EXPECT_GT(at(23, 50), 0);            // the blur reaches 3k = 12 px past the edge
  EXPECT_EQ(0, at(17, 50));
  // An offset moves the same picture by whole pixels.
  const auto moved = shadows::rasterize(square, 0xff000000u, 5, -3, 8, 0, 1, 0, 0, 100, 100, 1, 0, 0, 100, 100);
  EXPECT_EQ(at(25, 50), int(moved.bgra[(size_t(47) * 100 + 30) * 4 + 3]));
}

TEST(ShapeShadowRaster, ColourAlphaOpacityAndClipShapeTheComposite) {
  shadows::Outline square;
  square.x = 10; square.y = 10; square.width = 20; square.height = 20;
  // Premultiplied: half-alpha red at opacity 0.5 is alpha 64, red 64.
  const auto tinted = shadows::rasterize(square, 0x80ff0000u, 0, 0, 0, 0, 0.5, 0, 0, 40, 40, 1, 0, 0, 40, 40);
  const uint8_t* px = tinted.bgra.data() + (size_t(20) * 40 + 20) * 4;
  EXPECT_EQ(0, px[0]);
  EXPECT_EQ(0, px[1]);
  EXPECT_EQ(64, px[2]);
  EXPECT_EQ(64, px[3]);
  // The composite is the clip in device pixels (scale 2, offset 3), rounded out, on the target.
  const auto clipped = shadows::rasterize(square, 0xff000000u, 0, 0, 4, 2, 1, 5.25, 5, 10, 10, 2, 3, 0, 30, 100);
  EXPECT_EQ(13, clipped.x);  // floor(5.25 * 2 + 3)
  EXPECT_EQ(10, clipped.y);
  EXPECT_EQ(30 - 13, clipped.width);  // ceil(15.25 * 2 + 3) = 34, cut at the 30 px target
  EXPECT_EQ(20, clipped.height);
  // Nothing to draw: transparent colour, zero opacity or an empty clip.
  EXPECT_EQ(0, shadows::rasterize(square, 0x00ffffffu, 0, 0, 4, 0, 1, 0, 0, 40, 40, 1, 0, 0, 40, 40).width);
  EXPECT_EQ(0, shadows::rasterize(square, 0xff000000u, 0, 0, 4, 0, 0, 0, 0, 40, 40, 1, 0, 0, 40, 40).width);
  EXPECT_EQ(0, shadows::rasterize(square, 0xff000000u, 0, 0, 4, 0, 1, 0, 0, 0, 40, 1, 0, 0, 40, 40).width);
  // A buffer past 4096 x 4096 px is refused, never allocated.
  const auto huge = shadows::rasterize(square, 0xff000000u, 0, 0, 64, 0, 1, 0, 0, 8192, 8192, 1, 0, 0, 8192, 8192);
  EXPECT_TRUE(huge.refused);
  EXPECT_TRUE(huge.bgra.empty());
}

TEST(ShapeShadowRaster, SpreadGrowsEveryOutlineKind) {
  shadows::Outline rect;
  rect.x = 10; rect.y = 10; rect.width = 20; rect.height = 10;
  auto grown = shadows::shadowPolygon(rect, 0, 0, 3, 0.25);
  ASSERT_EQ(4u, grown.size());
  EXPECT_EQ(7.0, grown[0].x); EXPECT_EQ(7.0, grown[0].y);
  EXPECT_EQ(33.0, grown[2].x); EXPECT_EQ(23.0, grown[2].y);
  // A polygon's mitred offset: the square's corners move out diagonally by spread * sqrt 2.
  shadows::Outline square;
  square.kind = shadows::Outline::Kind::Polygon;
  square.points = {{0, 0}, {10, 0}, {10, 10}, {0, 10}};
  grown = shadows::shadowPolygon(square, 1, 2, 2, 0.25);
  ASSERT_EQ(4u, grown.size());
  EXPECT_NEAR(-1, grown[0].x, 1e-9); EXPECT_NEAR(0, grown[0].y, 1e-9);
  EXPECT_NEAR(13, grown[2].x, 1e-9); EXPECT_NEAR(14, grown[2].y, 1e-9);
  // Counter-clockwise gives the same outward offset.
  square.points = {{0, 0}, {0, 10}, {10, 10}, {10, 0}};
  grown = shadows::shadowPolygon(square, 0, 0, 2, 0.25);
  EXPECT_NEAR(-2, grown[0].x, 1e-9); EXPECT_NEAR(-2, grown[0].y, 1e-9);
  // A needle-sharp corner past the miter limit is bevelled, not spiked.
  shadows::Outline needle;
  needle.kind = shadows::Outline::Kind::Polygon;
  needle.points = {{0, 0}, {100, 1}, {0, 2}};
  grown = shadows::shadowPolygon(needle, 0, 0, 1, 0.25);
  EXPECT_EQ(4u, grown.size());
  for (const auto& p : grown) EXPECT_LT(p.x, 102.0);
}

}  // namespace
