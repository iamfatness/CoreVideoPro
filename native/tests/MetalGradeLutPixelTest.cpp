// Grading Phase 2, Task 3 (G3 pixel proof): `.cube` TEXT, parsed by the same parser
// register-grade-document uses (compositor/CubeLut.h), changes Program pixels on Metal by the
// amount the LUT predicts. The existing MetalCompositor.AdvancedPrimariesCurvesCubeAndBypass...
// test feeds a hand-built GradeCubeLut; this one goes through the text and the shared fixture.
// Fixture invariants: `channel-swap-2` maps input (r,g,b) to (b,g,r) at every lattice point, so
// with linear 3D interpolation any colour maps to its red/blue swap; `identity-2` maps every colour
// to itself. The patch (B=51,G=102,R=153) is mid-range, so trilinear filtering is exact to ±3.
#if COREVIDEO_WITH_METAL
#include "MetalGradeTestSupport.h"
#include <chrono>
#include "compositor/CubeLut.h"
#include "rpc/Json.h"

using namespace metal_grade_test;
using corevideo::rpc::Json;

namespace {
std::string cubeFixture(const std::string& name) {
  const auto fixtures = Json::parse(readRepoFile("contracts/grade.fixtures.json"));
  if (!fixtures || !fixtures->get("cubes")) return {};
  for (const auto& entry : fixtures->get("cubes")->asArray())
    if (entry.getString("name") == name) return entry.getString("text");
  return {};
}

CompositorColorGrade gradeFromCubeText(const std::string& text, const std::string& content) {
  CompositorColorGrade grade;
  auto document = std::make_shared<AdvancedGradeDocument>();
  document->content = content;
  GradeOperation op; op.kind = "cube"; op.cube = parseGradeCube(text, sha256Hex(text));
  document->operations.push_back(op);
  grade.advanced = document;
  return grade;
}
}  // namespace

TEST(MetalGradeLut, ParsedCubeTextSwapsRedAndBlueOnProgram) {
  METAL_GRADE_OR_SKIP(compositor);
  const auto swapText = cubeFixture("channel-swap-2"), identityText = cubeFixture("identity-2");
  ASSERT_FALSE(swapText.empty()); ASSERT_FALSE(identityText.empty());
  ASSERT_TRUE(parseGradeCube(swapText, sha256Hex(swapText))) << "the fixture must parse";
  CompositorRenderPlan plan; plan.renderPlanId = "lut-text"; plan.width = 320; plan.height = 180;
  plan.layers.push_back(fullLayer("cam"));
  plan.layers[0].hasColorGrade = true;
  const auto frame = bgraFrame("cam", 320, 180, 51, 102, 153);

  plan.layers[0].colorGrade = gradeFromCubeText(identityText, "identity-text");
  auto pixel = programCentre(compositor->render(plan, {frame}));
  EXPECT_NEAR(pixel[0], 51, 3); EXPECT_NEAR(pixel[1], 102, 3); EXPECT_NEAR(pixel[2], 153, 3);

  plan.layers[0].colorGrade = gradeFromCubeText(swapText, "swap-text");
  pixel = programCentre(compositor->render(plan, {frame}));
  EXPECT_NEAR(pixel[0], 153, 3) << "blue now carries the input red";
  EXPECT_NEAR(pixel[1], 102, 3) << "green unchanged";
  EXPECT_NEAR(pixel[2], 51, 3) << "red now carries the input blue";
}

// Spec R4: a first-use 33-point LUT is uploaded inside the layer draw on the render thread.
// Diagnostic only (timing on a shared machine is not a gate): prints the cost of the first render
// that meets the LUT and of the next render that reuses it, and pins only that both render.
TEST(MetalGradeLut, FirstUseOf33PointLutCostIsReported) {
  METAL_GRADE_OR_SKIP(compositor);
  std::ostringstream text; text.imbue(std::locale::classic());
  text << "LUT_3D_SIZE 33\n";
  for (int b = 0; b < 33; ++b) for (int g = 0; g < 33; ++g) for (int r = 0; r < 33; ++r)
    text << r / 32.0 << ' ' << g / 32.0 << ' ' << b / 32.0 << '\n';
  CompositorRenderPlan plan; plan.renderPlanId = "lut-33"; plan.width = 1920; plan.height = 1080;
  plan.skipCpuReadback = true;
  plan.layers.push_back(fullLayer("cam"));
  plan.layers[0].hasColorGrade = true;
  plan.layers[0].colorGrade = gradeFromCubeText(text.str(), "lut-33");
  ASSERT_TRUE(plan.layers[0].colorGrade.advanced->operations[0].cube);
  const auto frame = bgraFrame("cam", 1920, 1080, 51, 102, 153);
  compositor->render(plan, {frame});  // warm the source texture; the LUT is uploaded here
  plan.layers[0].colorGrade = gradeFromCubeText(text.str() + "# second copy\n", "lut-33-b");
  const auto t0 = std::chrono::steady_clock::now();
  const auto first = compositor->render(plan, {frame});
  const auto t1 = std::chrono::steady_clock::now();
  const auto second = compositor->render(plan, {frame});
  const auto t2 = std::chrono::steady_clock::now();
  EXPECT_TRUE(first.gpuComposed); EXPECT_TRUE(second.gpuComposed);
  std::fprintf(stderr, "[lut-upload-r4] first-use 33^3 render submit=%.3f ms, cached render submit=%.3f ms (CPU submit, no GPU wait)\n",
      std::chrono::duration<double, std::milli>(t1 - t0).count(), std::chrono::duration<double, std::milli>(t2 - t1).count());
}
#endif
