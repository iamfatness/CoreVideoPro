// Tests of the COMPARATOR (GraphicsRasterParity), not parity evidence.
//
// Every image here is synthetic: it exists to prove each tolerance in
// GraphicsRasterParity.h triggers at its stated bound and not before. No
// value here describes what either raster produces (plan rule: expected values
// come from real output only; these test the judge).

#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "GraphicsRasterParity.h"
#include "graphics/GraphicsTemplate.h"

namespace {
using namespace corevideo::graphics::parity;
using corevideo::rpc::Json;

ParityImage blank(int w, int h) {
  ParityImage image;
  image.width = w;
  image.height = h;
  image.bgraPremultiplied.assign(size_t(w) * h * 4, 0);
  return image;
}
void fill(ParityImage& image, int x0, int y0, int x1, int y1, uint8_t b, uint8_t g, uint8_t r, uint8_t a) {
  for (int y = y0; y < y1; ++y)
    for (int x = x0; x < x1; ++x) {
      auto* p = image.bgraPremultiplied.data() + (size_t(y) * image.width + x) * 4;
      p[0] = b; p[1] = g; p[2] = r; p[3] = a;
    }
}
// A white "glyph" pattern: a few vertical stems and a bar, on transparent.
void glyphs(ParityImage& image, int dx, int dy, uint8_t alpha = 255) {
  for (int stem = 0; stem < 4; ++stem) fill(image, 20 + dx + stem * 12, 20 + dy, 24 + dx + stem * 12, 52 + dy, alpha, alpha, alpha, alpha);
  fill(image, 20 + dx, 48 + dy, 64 + dx, 52 + dy, alpha, alpha, alpha, alpha);
}
ParityRegion textRegion() {
  ParityRegion r;
  r.id = "t";
  r.text = true;
  r.x = 16; r.y = 16; r.width = 64; r.height = 40;
  r.linePitch = 48; r.maxLines = 1; r.colorArgb = 0xffffffffu; r.grow = 6;
  return r;
}
ParityRegion rectRegion() {
  ParityRegion r;
  r.id = "r";
  r.x = 20; r.y = 20; r.width = 40; r.height = 30;
  r.grow = 2;
  return r;
}
bool mentions(const Verdict& v, const std::string& text) {
  for (const auto& f : v.failures) if (f.find(text) != std::string::npos) return true;
  return false;
}
}  // namespace

TEST(GraphicsRasterParity, IdenticalImagesPassBothTiers) {
  auto image = blank(100, 80);
  fill(image, 20, 20, 60, 50, 40, 80, 120, 255);
  glyphs(image, 0, 0);
  const std::vector<ParityRegion> regions{rectRegion(), textRegion()};
  EXPECT_TRUE(judgeGeometry(image, image, regions).pass);
  EXPECT_TRUE(judgeText(image, image, regions, 1).pass);
}

TEST(GraphicsRasterParity, ATwoPixelShiftFailsTheInkBox) {
  auto reference = blank(100, 80), candidate = blank(100, 80);
  fill(reference, 20, 20, 60, 50, 40, 80, 120, 255);
  fill(candidate, 22, 20, 62, 50, 40, 80, 120, 255);  // shifted 2 px right
  const auto v = judgeGeometry(reference, candidate, {rectRegion()});
  EXPECT_FALSE(v.pass);
  EXPECT_TRUE(mentions(v, "stray ink")) << (v.failures.empty() ? "" : v.failures.front());
  // Text tier: an ink edge 3 px off fails, 2 px passes (tolerance 2 px).
  auto text = blank(100, 80), two = blank(100, 80), three = blank(100, 80);
  glyphs(text, 0, 0); glyphs(two, 2, 0); glyphs(three, 3, 0);
  EXPECT_FALSE(mentions(judgeText(text, two, {textRegion()}, 1), "ink left"));
  EXPECT_TRUE(mentions(judgeText(text, three, {textRegion()}, 1), "ink left"));
}

TEST(GraphicsRasterParity, AlphaScaledBy0_85FailsSummedAlpha) {
  auto reference = blank(100, 80), candidate = blank(100, 80);
  glyphs(reference, 0, 0, 255);
  glyphs(candidate, 0, 0, uint8_t(255 * 0.85));
  const auto v = judgeText(reference, candidate, {textRegion()}, 1);
  EXPECT_TRUE(mentions(v, "summed alpha")) << (v.failures.empty() ? "passed" : v.failures.front());
  // Scaled by 0.95 (5%) is inside the 8% tolerance.
  auto close = blank(100, 80);
  glyphs(close, 0, 0, uint8_t(255 * 0.95));
  EXPECT_FALSE(mentions(judgeText(reference, close, {textRegion()}, 1), "summed alpha"));
}

TEST(GraphicsRasterParity, ATextRegionIsMeasuredOnlyInsideItsClip) {
  // Ink in the grown band past the clip belongs to a neighbour (a table's next
  // row): neither raster can draw this primitive there.
  auto reference = blank(100, 80), candidate = blank(100, 80);
  glyphs(reference, 0, 0);
  glyphs(candidate, 0, 0);
  fill(candidate, 16, 57, 80, 61, 255, 255, 255, 255);
  auto unclipped = textRegion();
  EXPECT_TRUE(mentions(judgeText(reference, candidate, {unclipped}, 1), "summed alpha"))
      << "without a clip the grown band is measured";
  auto clipped = textRegion();
  clipped.clipped = true;
  clipped.clipX = 16; clipped.clipY = 16; clipped.clipWidth = 64; clipped.clipHeight = 40;
  const auto v = judgeText(reference, candidate, {clipped}, 1);
  EXPECT_TRUE(v.pass) << (v.failures.empty() ? "" : v.failures.front());
  // Inside the clip the judge is unchanged: dimmer glyphs still fail.
  auto dim = blank(100, 80);
  glyphs(dim, 0, 0, uint8_t(255 * 0.85));
  EXPECT_TRUE(mentions(judgeText(reference, dim, {clipped}, 1), "summed alpha"));
}

TEST(GraphicsRasterParity, AOnePixelBaselineMoveFailsTheBaseline) {
  auto reference = blank(100, 80), one = blank(100, 80), two = blank(100, 80);
  glyphs(reference, 0, 0); glyphs(one, 0, 1); glyphs(two, 0, 2);
  EXPECT_FALSE(mentions(judgeText(reference, one, {textRegion()}, 1), "baseline")) << "a 1 px move is inside tolerance";
  EXPECT_TRUE(mentions(judgeText(reference, two, {textRegion()}, 1), "baseline"));
}

TEST(GraphicsRasterParity, InteriorChannelOff3Fails) {
  auto reference = blank(100, 80), off2 = blank(100, 80), off3 = blank(100, 80);
  fill(reference, 20, 20, 60, 50, 40, 80, 120, 255);
  fill(off2, 20, 20, 60, 50, 42, 80, 120, 255);
  fill(off3, 20, 20, 60, 50, 43, 80, 120, 255);
  EXPECT_FALSE(mentions(judgeGeometry(reference, off2, {rectRegion()}), "interior"));
  EXPECT_TRUE(mentions(judgeGeometry(reference, off3, {rectRegion()}), "interior"));
}

TEST(GraphicsRasterParity, StrayInkOutsideTheGrownBoxFails) {
  auto reference = blank(100, 80);
  fill(reference, 20, 20, 60, 50, 40, 80, 120, 255);
  auto nine = reference, eight = reference;
  // 2 px outside the reference ink box (x 60 is the first column past it; +1 grown).
  fill(nine, 61, 30, 62, 31, 9, 9, 9, 9);
  fill(eight, 61, 30, 62, 31, 8, 8, 8, 8);
  EXPECT_TRUE(mentions(judgeGeometry(reference, nine, {rectRegion()}), "stray ink"));
  EXPECT_FALSE(mentions(judgeGeometry(reference, eight, {rectRegion()}), "stray ink"));
}

TEST(GraphicsRasterParity, LayoutWidthOff1_6PercentFails) {
  const auto summary = [](double width) {
    return Json(Json::Object{{"primitives", Json::Array{Json::Object{{"id", "t"}, {"type", "text"},
        {"box", Json::Array{0.0, 0.0, width, 40.0}}, {"fontSize", 40.0}}}}});
  };
  EXPECT_TRUE(judgeLayout(summary(1000), summary(1014)).pass) << "1.4% is inside 1.5%";
  const auto v = judgeLayout(summary(1000), summary(1016));
  EXPECT_FALSE(v.pass);
  EXPECT_TRUE(mentions(v, "width"));
}

TEST(GraphicsRasterParity, IndexIsWellFormed) {
  const std::string dir = std::string(COREVIDEO_REPO_ROOT) + "/native/tests/fixtures/graphics-parity";
  std::string error;
  const auto fixtures = loadParityIndex(dir + "/index.json", error);
  ASSERT_TRUE(error.empty()) << error;
  ASSERT_FALSE(fixtures.empty());
  std::set<std::string> names, curated;
  for (const auto& fixture : fixtures) {
    EXPECT_TRUE(names.insert(fixture.name).second) << "duplicate fixture name " << fixture.name;
    if (fixture.source.rfind("curated:", 0) == 0) curated.insert(fixture.source.substr(8));
    ParityInput input;
    std::string inputError;
    EXPECT_TRUE(parityInput(fixture, dir, input, inputError)) << fixture.name << ": " << inputError;
    if (!input.templated) continue;
    // Every field the index sets exists in the manifest.
    const auto defaults = input.overlay.graphicsTemplate->definition->defaultFields();
    if (fixture.fields.isObject())
      for (const auto& [key, _] : fixture.fields.asObject())
        EXPECT_TRUE(defaults.get(key) != nullptr) << fixture.name << ": unknown field " << key;
    EXPECT_GT(input.width, 0);
    EXPECT_LE(input.width, 4096) << fixture.name;
    EXPECT_LE(input.height, 4096) << fixture.name;
  }
  // The closed list includes every curated look (spec 7.3).
  for (const auto& definition : corevideo::graphics::GraphicsTemplate::curated())
    EXPECT_TRUE(curated.count(definition->id())) << "curated look missing from the index: " << definition->id();
}

// Review additions: the bounds the first set did not reach.

TEST(GraphicsRasterParity, EdgeBandAllows48AndFails49) {
  // A rect edge pixel (its reference neighbourhood spans the edge) off by 48 passes, 49 fails.
  auto reference = blank(100, 80);
  fill(reference, 20, 20, 60, 50, 40, 80, 120, 255);
  auto ok = reference, bad = reference;
  fill(ok, 59, 30, 60, 31, 40, 80, 120 - 48, 255);
  fill(bad, 59, 30, 60, 31, 40, 80, 120 - 49, 255);
  EXPECT_FALSE(mentions(judgeGeometry(reference, ok, {rectRegion()}), "edge band"));
  EXPECT_TRUE(mentions(judgeGeometry(reference, bad, {rectRegion()}), "edge band"));
}

TEST(GraphicsRasterParity, FixtureSummedAlphaAllowsHalfAPercent) {
  // A large rect at alpha 200 vs 199 (0.5%) passes; vs 198 (1%) fails, with
  // every pixel interior-legal (off by 1-2) so only the summed alpha speaks.
  auto reference = blank(100, 80), off05 = blank(100, 80), off1 = blank(100, 80);
  fill(reference, 10, 10, 90, 70, 100, 100, 100, 200);
  fill(off05, 10, 10, 90, 70, 100, 100, 100, 199);
  fill(off1, 10, 10, 90, 70, 100, 100, 100, 198);
  ParityRegion big = rectRegion();
  big.x = 10; big.y = 10; big.width = 80; big.height = 60;
  EXPECT_FALSE(mentions(judgeGeometry(reference, off05, {big}), "summed alpha"));
  EXPECT_TRUE(mentions(judgeGeometry(reference, off1, {big}), "summed alpha"));
}

TEST(GraphicsRasterParity, TextPixelsAreNotHeldToTier1) {
  // A text-only pair 5% apart in alpha: tier 2's 8% allows it, so tier 1
  // (0.5%, interior 2/255) must not judge those pixels at all.
  auto reference = blank(100, 80), candidate = blank(100, 80);
  glyphs(reference, 0, 0, 255);
  glyphs(candidate, 0, 0, uint8_t(255 * 0.95));
  const std::vector<ParityRegion> regions{textRegion()};
  EXPECT_TRUE(judgeGeometry(reference, candidate, regions).pass);
  EXPECT_FALSE(mentions(judgeText(reference, candidate, regions, 1), "summed alpha"));
}

TEST(GraphicsRasterParity, CoverageMeasuresTextOnAnOpaquePlate) {
  // White glyphs on an opaque dark plate: alpha is 255 everywhere, so only
  // the colour projection can see the glyphs. Thinning them must fail summed
  // alpha; the identical image must pass.
  auto plate = [](int stems) {
    auto image = blank(100, 80);
    fill(image, 0, 0, 100, 80, 30, 20, 10, 255);
    for (int stem = 0; stem < stems; ++stem) fill(image, 20 + stem * 12, 20, 24 + stem * 12, 52, 255, 255, 255, 255);
    return image;
  };
  const auto four = plate(4), three = plate(3);
  EXPECT_TRUE(judgeText(four, four, {textRegion()}, 1).pass);
  EXPECT_TRUE(mentions(judgeText(four, three, {textRegion()}, 1), "summed alpha"));
  EXPECT_GT(measureRegion(four, textRegion()).summedAlpha, 0.0) << "coverage saw nothing on the plate";
}

TEST(GraphicsRasterParity, LineBandsAndStructureAreJudged) {
  ParityRegion two = textRegion();
  two.height = 60; two.linePitch = 24; two.maxLines = 2;
  auto oneLine = blank(100, 100), twoLines = blank(100, 100);
  fill(oneLine, 20, 20, 64, 36, 255, 255, 255, 255);
  fill(twoLines, 20, 20, 64, 36, 255, 255, 255, 255);
  fill(twoLines, 20, 46, 64, 60, 255, 255, 255, 255);
  EXPECT_TRUE(mentions(judgeText(twoLines, oneLine, {two}, 1), "line bands"));
  // Same summed alpha, different structure: stems vs one block of equal area.
  auto stems = blank(100, 80), block = blank(100, 80);
  glyphs(stems, 0, 0);
  fill(block, 20, 20, 52, 38, 255, 255, 255, 255);
  EXPECT_LT(alphaSsim(stems, block, textRegion()), 0.9);
  EXPECT_GT(alphaSsim(stems, stems, textRegion()), 0.999999);
}

TEST(GraphicsRasterParity, StructureFailsWhenOnlyTheGlyphShapeDiffers) {
  // Same ink box (x 20..64, y 20..52), same bar and baseline, one line band,
  // summed alpha within 8%: vertical stems against horizontal stripes. Only
  // the SSIM check can tell them apart.
  auto stems = blank(100, 80), stripes = blank(100, 80);
  glyphs(stems, 0, 0);
  fill(stripes, 20, 20, 22, 52, 255, 255, 255, 255);
  fill(stripes, 62, 20, 64, 52, 255, 255, 255, 255);
  fill(stripes, 20, 48, 64, 52, 255, 255, 255, 255);
  for (int y = 20; y < 44; y += 6) fill(stripes, 22, y, 62, y + 2, 255, 255, 255, 255);
  const auto v = judgeText(stems, stripes, {textRegion()}, 1);
  EXPECT_TRUE(mentions(v, "SSIM")) << (v.failures.empty() ? "passed" : v.failures.front());
  for (const auto& f : v.failures) EXPECT_TRUE(f.find("SSIM") != std::string::npos) << "fixture not isolated: " << f;
}
