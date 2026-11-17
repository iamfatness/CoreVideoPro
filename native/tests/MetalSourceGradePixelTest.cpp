// Grading plan 2026-11-16, Task 3 (G1 pixel proof), end to end:
// request -> SourceGradeState -> applyTo -> Metal -> Program pixels.
// Fixture invariants: the two route rects do not overlap and the sample points sit at their
// centres; both sources show the SAME patch, so any difference on Program is the grade;
// applyCommands({}) renders exactly ONE full tick in a test process (no audio worker), and a
// full tick reads the Metal preview back.
#if COREVIDEO_WITH_METAL
#include <gtest/gtest.h>
#include <cstdio>
#include "SourceGradeTestSupport.h"
#include "core/MediaCore.h"
#include "rpc/JsonRpcServer.h"

using namespace corevideo;

namespace {
std::unique_ptr<modules::ICompositor> metalOrNull(std::string& why) {
  auto metal = modules::createMetalCompositor();
  if (!metal) { why = "createMetalCompositor returned null"; return nullptr; }
  modules::CompositorRenderPlan probe; probe.renderPlanId = "probe";
  for (const auto& w : metal->render(probe, {}).warnings)
    if (w.rfind("Metal compositor unavailable", 0) == 0) { why = w; return nullptr; }
  return metal;
}
// Program pixel at (fx, fy), B, G, R. Read from lastProgramFrameForTest().preview, the same
// pixels a program-frame-preview event carries: the event is throttled to one per 33 ms
// (MediaCore::renderTick), so back-to-back applyCommands({}) ticks would not each emit one.
std::array<int, 3> programPixel(core::MediaCore& core, float fx, float fy) {
  const auto& preview = core.lastProgramFrameForTest().preview;
  const int w = preview.width, h = preview.height;
  if (w <= 0 || h <= 0) return {-1, -1, -1};
  const size_t at = (size_t(int(fy * h)) * size_t(w) + size_t(int(fx * w))) * 4;
  if (at + 3 >= preview.bgra.size()) return {-1, -1, -1};
  return {preview.bgra[at], preview.bgra[at + 1], preview.bgra[at + 2]};
}
}  // namespace

TEST(MetalSourceGrade, AGradedSourceChangesOnProgramAndItsNeighbourDoesNot) {
  std::string why;
  auto metal = metalOrNull(why);
  if (!metal) {
    std::fprintf(stderr, "[metal-test] unavailable: %s\n", why.c_str());
    ASSERT_NE(why.find("no Metal device"), std::string::npos);
    return;
  }
  auto modules = modules::createStubModules();
  modules.compositor = std::move(metal);
  core::MediaCore core(std::move(modules));
  core.useZoomSourcesForTest({});
  auto left = std::make_shared<gradetest::EpochSource>("capture:cam-a", 192, 96, 64, 7);
  auto right = std::make_shared<gradetest::EpochSource>("capture:cam-b", 192, 96, 64, 11);
  core.addSourceForTest(left); core.addSourceForTest(right);
  (void)core.applyCommands(rpc::Json::Array{rpc::Json::Object{{"type", "load-scene-graph"}, {"sceneId", "pair"},
      {"routes", rpc::Json::Array{
          rpc::Json::Object{{"routeId", "a"}, {"mode", "capture-input"}, {"captureDeviceId", "cam-a"}, {"fitMode", "fill"},
                            {"rect", rpc::Json::Object{{"x", 0}, {"y", 0}, {"width", 0.5}, {"height", 1}}}},
          rpc::Json::Object{{"routeId", "b"}, {"mode", "capture-input"}, {"captureDeviceId", "cam-b"}, {"fitMode", "fill"},
                            {"rect", rpc::Json::Object{{"x", 0.5}, {"y", 0}, {"width", 0.5}, {"height", 1}}}}}}}});
  rpc::JsonRpcServer server(core);

  (void)core.applyCommands({});
  auto l = programPixel(core, .25f, .5f), r = programPixel(core, .75f, .5f);
  ASSERT_GE(l[0], 0) << "precondition: Program preview is read back on a full tick";
  EXPECT_NEAR(l[2], 192, 2); EXPECT_NEAR(r[2], 192, 2);   // precondition: both ungraded

  const auto q = server.handle(rpc::Json::Object{{"id", "q"}, {"type", "query-source-grade"}, {"sourceId", "capture:cam-a"}});
  ASSERT_TRUE(q.get("present")->asBool());
  ASSERT_EQ(q.getNumber("sourceEpoch"), 7);
  const auto set = server.handle(rpc::Json::Object{{"id", "s"}, {"type", "set-source-grade"}, {"sourceId", "capture:cam-a"},
      {"sourceEpoch", q.getNumber("sourceEpoch")}, {"expectedRevision", q.getNumber("revision")},
      {"grade", rpc::Json::Object{{"lut", "none"}, {"exposure", 2}}}});
  ASSERT_TRUE(set.get("accepted")->asBool()) << set.stringify();

  (void)core.applyCommands({});
  l = programPixel(core, .25f, .5f); r = programPixel(core, .75f, .5f);
  EXPECT_NEAR(l[2], 243, 2); EXPECT_NEAR(l[1], 147, 2); EXPECT_NEAR(l[0], 115, 2);
  EXPECT_NEAR(r[2], 192, 2); EXPECT_NEAR(r[1], 96, 2);  EXPECT_NEAR(r[0], 64, 2);

  // The identity fence is a pixel claim too: a new epoch paints the grade neutral.
  left->setEpoch(8);
  (void)core.applyCommands({});
  l = programPixel(core, .25f, .5f);
  EXPECT_NEAR(l[2], 192, 2) << "an epoch change must not leave the old grade on air";
  const auto state = core.sessionState();
  const auto* rows = state.get("sourceGrades");
  ASSERT_TRUE(rows && rows->isArray() && rows->asArray().size() == 1);
  EXPECT_FALSE(rows->asArray()[0].get("active")->asBool());
  EXPECT_EQ(rows->asArray()[0].getNumber("liveEpoch"), 8);

  // Re-asserting with the new epoch (what the Mac store does, D4) restores it.
  const auto again = server.handle(rpc::Json::Object{{"id", "s2"}, {"type", "set-source-grade"}, {"sourceId", "capture:cam-a"},
      {"sourceEpoch", 8}, {"expectedRevision", 1}, {"grade", rpc::Json::Object{{"lut", "none"}, {"exposure", 2}}}});
  ASSERT_TRUE(again.get("accepted")->asBool());
  (void)core.applyCommands({});
  l = programPixel(core, .25f, .5f); r = programPixel(core, .75f, .5f);
  EXPECT_NEAR(l[2], 243, 2);
  EXPECT_NEAR(r[2], 192, 2) << "the neighbour never changed";
}
#endif  // COREVIDEO_WITH_METAL
