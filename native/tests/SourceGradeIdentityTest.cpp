// Per-source grading G0 (docs/superpowers/plans/2026-11-16-mac-grading-phase1.md, Task 1):
// SourceGradeState answers "what is this source's epoch and revision" and lists every
// accepted grade with whether applyTo is painting it.
#include "core/SourceGradeState.h"
#include <gtest/gtest.h>

using namespace corevideo;

namespace {
modules::VideoFrame frame(const std::string& id, uint64_t epoch) {
  modules::VideoFrame f; f.participantId = id; f.sourceEpoch = epoch; return f;
}
rpc::Json applyRequest(const std::string& id, double epoch, double expected) {
  return rpc::Json::Object{{"sourceId", id}, {"sourceEpoch", epoch}, {"expectedRevision", expected}};
}
modules::CompositorColorGrade exposure(float v) { modules::CompositorColorGrade g; g.exposure = v; return g; }
}  // namespace

TEST(SourceGradeIdentity, QueryReportsPresenceEpochAndRevisionFromTheLastObservation) {
  core::SourceGradeState state;
  auto q = state.query("16778240");
  EXPECT_FALSE(q.present); EXPECT_EQ(q.sourceEpoch, 0u); EXPECT_EQ(q.revision, 0);

  state.observe({frame("16778240", 7), frame("capture:cam", 0)});
  q = state.query("16778240");
  EXPECT_TRUE(q.present); EXPECT_EQ(q.sourceEpoch, 7u); EXPECT_EQ(q.revision, 0);

  ASSERT_TRUE(state.apply(applyRequest("16778240", 7, 0), exposure(3)).get("accepted")->asBool());
  EXPECT_EQ(state.query("16778240").revision, 1);

  // observe() REPLACES the epoch map: absent from this tick means not present.
  state.observe({frame("capture:cam", 0)});
  q = state.query("16778240");
  EXPECT_FALSE(q.present); EXPECT_EQ(q.sourceEpoch, 0u);
  EXPECT_EQ(q.revision, 1) << "the accepted revision survives a missing frame";

  // The answer is exactly what the next apply needs (the G0 contract).
  state.observe({frame("16778240", 8)});
  q = state.query("16778240");
  const auto accepted = state.apply(applyRequest("16778240", double(q.sourceEpoch), double(q.revision)), exposure(4));
  EXPECT_TRUE(accepted.get("accepted")->asBool());
  EXPECT_EQ(accepted.getNumber("revision"), 2);
}

TEST(SourceGradeIdentity, EpochZeroIsPresentNotAbsent) {
  core::SourceGradeState state;
  state.observe({frame("capture:cam", 0)});
  const auto q = state.query("capture:cam");
  EXPECT_TRUE(q.present) << "Mac capture frames carry epoch 0 (AvfCaptureDeviceAdapter sets none)";
  EXPECT_EQ(q.sourceEpoch, 0u);
}

TEST(SourceGradeIdentity, StatusesListOnlyAcceptedGradesAndReportActiveFromTheLiveEpoch) {
  core::SourceGradeState state;
  // Fixture invariants: three sources observed; grades accepted for "capture:b" FIRST and
  // "16778240" second, so insertion order differs from id order ('1' < 'c' in std::string order).
  state.observe({frame("16778240", 7), frame("capture:b", 0), frame("capture:ungraded", 0)});
  ASSERT_TRUE(state.apply(applyRequest("capture:b", 0, 0), exposure(1)).get("accepted")->asBool());
  ASSERT_TRUE(state.apply(applyRequest("16778240", 7, 0), exposure(2)).get("accepted")->asBool());

  auto s = state.statuses();
  ASSERT_EQ(s.size(), 2u) << "an observed but ungraded source is never listed";
  EXPECT_EQ(s[0].sourceId, "16778240");
  EXPECT_EQ(s[1].sourceId, "capture:b");
  EXPECT_EQ(s[0].revision, 1);
  EXPECT_EQ(s[0].appliedEpoch, 7u); EXPECT_EQ(s[0].liveEpoch, 7u);
  EXPECT_TRUE(s[0].livePresent); EXPECT_TRUE(s[0].active);

  // A Zoom size change: same participant, new epoch -> painted neutral -> inactive.
  state.observe({frame("16778240", 8), frame("capture:b", 0)});
  s = state.statuses();
  EXPECT_EQ(s[0].liveEpoch, 8u); EXPECT_TRUE(s[0].livePresent); EXPECT_FALSE(s[0].active);
  EXPECT_TRUE(s[1].active);

  // capture:b absent this tick: an applied epoch of 0 must not read active against a missing 0.
  state.observe({frame("16778240", 8)});
  s = state.statuses();
  EXPECT_FALSE(s[1].livePresent); EXPECT_EQ(s[1].liveEpoch, 0u); EXPECT_FALSE(s[1].active);
}

// Structural (rule 10): drives applyTo, the thing `active` claims to describe.
TEST(SourceGradeIdentity, ActiveAgreesWithWhatApplyToPaints) {
  core::SourceGradeState state;
  const auto live = frame("16778240", 7);
  state.observe({live});
  ASSERT_TRUE(state.apply(applyRequest("16778240", 7, 0), exposure(5)).get("accepted")->asBool());
  modules::CompositorRenderPlanLayer layer; layer.participantId = "16778240";
  modules::CompositorRenderPlan plan; plan.layers.push_back(layer);
  state.applyTo(plan, {live});
  EXPECT_TRUE(state.statuses()[0].active);
  EXPECT_EQ(plan.layers[0].colorGrade.exposure, 5.f);

  const auto moved = frame("16778240", 8);
  state.observe({moved});
  plan.layers[0] = layer;
  state.applyTo(plan, {moved});
  EXPECT_FALSE(state.statuses()[0].active);
  EXPECT_EQ(plan.layers[0].colorGrade.exposure, 0.f) << "inactive == painted neutral";
}

// ---------------------------------------------------------------------------
// Task 2 (G0): query-source-grade and the sourceGrades snapshot node on the wire.
// Fixture invariants: sources are kind "test" (SourceGradeTestSupport.h: a "capture"-kind
// bus source would be removed by syncCaptureSources); the routes use capture-input so
// RouteSourcePolicy binds participantId = "capture:cam-a" / "capture:cam-b", the frame keys;
// both route rects are disjoint halves of the canvas. These run on the stub and Metal alike.
#include "core/MediaCore.h"
#include "rpc/JsonRpcServer.h"
#include "SourceGradeTestSupport.h"

namespace {
struct Rig {
  modules::ModuleSet modules = modules::createStubModules();
  gradetest::GradeTestCompositor* compositor = nullptr;
  std::unique_ptr<core::MediaCore> core;
  std::unique_ptr<rpc::JsonRpcServer> server;
  Rig() {
    auto wrapped = std::make_unique<gradetest::GradeTestCompositor>(std::move(modules.compositor));
    compositor = wrapped.get();
    modules.compositor = std::move(wrapped);
    core = std::make_unique<core::MediaCore>(std::move(modules));
    core->useZoomSourcesForTest({});
    server = std::make_unique<rpc::JsonRpcServer>(*core);
  }
  rpc::Json query(const std::string& id) {
    return server->handle(rpc::Json::Object{{"id", "q"}, {"type", "query-source-grade"}, {"sourceId", id}});
  }
  rpc::Json set(const std::string& id, double epoch, double expected, double exposure) {
    return server->handle(rpc::Json::Object{{"id", "s"}, {"type", "set-source-grade"}, {"sourceId", id},
        {"sourceEpoch", epoch}, {"expectedRevision", expected},
        {"grade", rpc::Json::Object{{"lut", "none"}, {"exposure", exposure}}}});
  }
  void routeSideBySide() {
    (void)core->applyCommands(rpc::Json::Array{rpc::Json::Object{{"type", "load-scene-graph"}, {"sceneId", "pair"},
        {"routes", rpc::Json::Array{
            rpc::Json::Object{{"routeId", "a"}, {"mode", "capture-input"}, {"captureDeviceId", "cam-a"}, {"fitMode", "fill"},
                              {"rect", rpc::Json::Object{{"x", 0}, {"y", 0}, {"width", 0.5}, {"height", 1}}}},
            rpc::Json::Object{{"routeId", "b"}, {"mode", "capture-input"}, {"captureDeviceId", "cam-b"}, {"fitMode", "fill"},
                              {"rect", rpc::Json::Object{{"x", 0.5}, {"y", 0}, {"width", 0.5}, {"height", 1}}}}}}}});
  }
  const modules::CompositorRenderPlanLayer* layer(const std::string& participantId) const {
    for (const auto& l : compositor->lastPlan.layers) if (l.participantId == participantId) return &l;
    return nullptr;
  }
};
const rpc::Json* gradeRow(const rpc::Json& state, const std::string& id) {
  const auto* rows = state.get("sourceGrades");
  if (!rows || !rows->isArray()) return nullptr;
  for (const auto& row : rows->asArray()) if (row.getString("sourceId") == id) return &row;
  return nullptr;
}
}  // namespace

TEST(SourceGradeWire, QueryAnswersFromTheLiveTickAndRefusesAMalformedId) {
  Rig rig;
  auto cam = std::make_shared<gradetest::EpochSource>("capture:cam-a", 192, 96, 64, 7);
  rig.core->addSourceForTest(cam);

  auto r = rig.query("capture:cam-a");
  ASSERT_TRUE(r.get("ok")->asBool());
  EXPECT_FALSE(r.get("present")->asBool()) << "no tick has observed the source yet";
  EXPECT_EQ(r.getNumber("sourceEpoch"), 0); EXPECT_EQ(r.getNumber("revision"), 0);

  rig.core->renderDisplayTick();
  r = rig.query("capture:cam-a");
  EXPECT_TRUE(r.get("present")->asBool()); EXPECT_EQ(r.getNumber("sourceEpoch"), 7);

  cam->setPresent(false);
  rig.core->renderDisplayTick();
  EXPECT_FALSE(rig.query("capture:cam-a").get("present")->asBool()) << "absent this tick";

  for (const auto& bad : {rpc::Json(rpc::Json::Object{{"id", "b"}, {"type", "query-source-grade"}}),
                          rpc::Json(rpc::Json::Object{{"id", "b"}, {"type", "query-source-grade"}, {"sourceId", ""}}),
                          rpc::Json(rpc::Json::Object{{"id", "b"}, {"type", "query-source-grade"}, {"sourceId", std::string(257, 'x')}}),
                          rpc::Json(rpc::Json::Object{{"id", "b"}, {"type", "query-source-grade"}, {"sourceId", 5}}),
                          // root-level contract: a payload wrapper is NOT read
                          rpc::Json(rpc::Json::Object{{"id", "b"}, {"type", "query-source-grade"},
                                                      {"payload", rpc::Json::Object{{"sourceId", "capture:cam-a"}}}})}) {
    const auto refused = rig.server->handle(bad);
    EXPECT_FALSE(refused.get("ok")->asBool());
    EXPECT_EQ(refused.get("error")->getString("code"), "protocol-error");
  }
  EXPECT_TRUE(rig.query(std::string(256, 'x')).get("ok")->asBool()) << "256 bytes is the inclusive bound";
}

TEST(SourceGradeWire, SnapshotNodeIsUnconditionalAndTracksActivity) {
  Rig rig;
  auto a = std::make_shared<gradetest::EpochSource>("capture:cam-a", 192, 96, 64, 7);
  auto b = std::make_shared<gradetest::EpochSource>("capture:cam-b", 192, 96, 64, 11);
  rig.core->addSourceForTest(a); rig.core->addSourceForTest(b);
  rig.routeSideBySide();

  auto state = rig.core->sessionState();
  ASSERT_NE(state.get("sourceGrades"), nullptr) << "published even with no grade";
  EXPECT_TRUE(state.get("sourceGrades")->isArray());
  EXPECT_TRUE(state.get("sourceGrades")->asArray().empty());

  rig.core->renderDisplayTick();
  // Fixture invariant: both route layers exist and carry the frame keys.
  ASSERT_NE(rig.layer("capture:cam-a"), nullptr);
  ASSERT_NE(rig.layer("capture:cam-b"), nullptr);

  const auto accepted = rig.set("capture:cam-a", 7, 0, 2);
  ASSERT_TRUE(accepted.get("accepted")->asBool()) << accepted.stringify();
  rig.core->renderDisplayTick();
  state = rig.core->sessionState();
  const auto* row = gradeRow(state, "capture:cam-a");
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(row->getNumber("revision"), 1);
  EXPECT_EQ(row->getNumber("appliedEpoch"), 7); EXPECT_EQ(row->getNumber("liveEpoch"), 7);
  EXPECT_TRUE(row->get("livePresent")->asBool()); EXPECT_TRUE(row->get("active")->asBool());
  EXPECT_EQ(gradeRow(state, "capture:cam-b"), nullptr) << "the ungraded neighbour is not listed";
  EXPECT_TRUE(rig.layer("capture:cam-a")->hasColorGrade);
  EXPECT_EQ(rig.layer("capture:cam-a")->colorGrade.exposure, 2.f);
  EXPECT_FALSE(rig.layer("capture:cam-b")->hasColorGrade) << "the neighbour keeps the Program look";

  a->setEpoch(8);
  rig.core->renderDisplayTick();
  state = rig.core->sessionState();
  row = gradeRow(state, "capture:cam-a");
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(row->getNumber("liveEpoch"), 8);
  EXPECT_FALSE(row->get("active")->asBool());
  EXPECT_EQ(rig.layer("capture:cam-a")->colorGrade.exposure, 0.f) << "inactive is painted neutral";

  a->setPresent(false);
  rig.core->renderDisplayTick();
  state = rig.core->sessionState();
  row = gradeRow(state, "capture:cam-a");
  ASSERT_NE(row, nullptr) << "an accepted grade stays listed while its source is absent";
  EXPECT_FALSE(row->get("livePresent")->asBool()); EXPECT_EQ(row->getNumber("liveEpoch"), 0);
}

// ---------------------------------------------------------------------------
// D6 override (grading plan "Defaults taken"): an additive `clear: true` on set-source-grade
// forgets the source's own grade so its layer returns to the Program look. The same identity
// and revision fences apply, and the revision keeps counting (a tombstone), so an old editor's
// expectedRevision is still refused after a clear.
TEST(SourceGradeIdentity, ClearForgetsTheGradeButKeepsTheRevisionFence) {
  core::SourceGradeState state;
  const auto live = frame("capture:cam", 0);
  state.observe({live});
  ASSERT_TRUE(state.apply(applyRequest("capture:cam", 0, 0), exposure(5)).get("accepted")->asBool());
  auto clear = [](const std::string& id, double epoch, double expected) {
    return rpc::Json::Object{{"sourceId", id}, {"sourceEpoch", epoch}, {"expectedRevision", expected}, {"clear", true}};
  };
  EXPECT_EQ(state.apply(clear("capture:cam", 3, 1), {}).getString("reason"), "source-identity-changed")
      << "a clear is fenced by identity like any apply";
  EXPECT_EQ(state.apply(clear("capture:cam", 0, 0), {}).getString("reason"), "grade-revision-conflict")
      << "and by revision";
  const auto cleared = state.apply(clear("capture:cam", 0, 1), {});
  ASSERT_TRUE(cleared.get("accepted")->asBool());
  EXPECT_EQ(cleared.getNumber("revision"), 2) << "a clear is a new revision";
  EXPECT_TRUE(state.statuses().empty()) << "a cleared source has no grade to report";
  EXPECT_EQ(state.query("capture:cam").revision, 2);

  modules::CompositorRenderPlanLayer layer; layer.participantId = "capture:cam";
  modules::CompositorRenderPlan plan; plan.layers.push_back(layer);
  state.applyTo(plan, {live});
  EXPECT_FALSE(plan.layers[0].hasColorGrade) << "the layer takes the Program look again";
  EXPECT_TRUE(plan.sourceGrades.empty());

  EXPECT_EQ(state.apply(applyRequest("capture:cam", 0, 1), exposure(6)).getString("reason"), "grade-revision-conflict")
      << "an editor that missed the clear is refused";
  ASSERT_TRUE(state.apply(applyRequest("capture:cam", 0, 2), exposure(6)).get("accepted")->asBool());
  plan.layers[0] = layer;
  state.applyTo(plan, {live});
  EXPECT_TRUE(plan.layers[0].hasColorGrade);
  EXPECT_EQ(plan.layers[0].colorGrade.exposure, 6.f);

  state.observe({frame("16778240", 4)});
  const auto fresh = state.apply(clear("16778240", 4, 0), {});
  ASSERT_TRUE(fresh.get("accepted")->asBool()) << "clearing a never-graded source is accepted";
  EXPECT_EQ(state.statuses().size(), 1u) << "and lists nothing for it (capture:cam is still listed)";
}

TEST(SourceGradeWire, AClearRequestReturnsTheLayerToTheProgramLook) {
  Rig rig;
  auto a = std::make_shared<gradetest::EpochSource>("capture:cam-a", 192, 96, 64, 7);
  rig.core->addSourceForTest(a);
  rig.routeSideBySide();
  rig.core->renderDisplayTick();
  ASSERT_TRUE(rig.set("capture:cam-a", 7, 0, 2).get("accepted")->asBool());
  rig.core->renderDisplayTick();
  ASSERT_NE(rig.layer("capture:cam-a"), nullptr);
  ASSERT_TRUE(rig.layer("capture:cam-a")->hasColorGrade);

  const auto cleared = rig.server->handle(rpc::Json::Object{{"id", "c"}, {"type", "set-source-grade"},
      {"sourceId", "capture:cam-a"}, {"sourceEpoch", 7}, {"expectedRevision", 1}, {"clear", true},
      {"grade", rpc::Json::Object{{"lut", "none"}}}});
  ASSERT_TRUE(cleared.get("accepted")->asBool()) << cleared.stringify();
  rig.core->renderDisplayTick();
  EXPECT_FALSE(rig.layer("capture:cam-a")->hasColorGrade);
  EXPECT_TRUE(rig.core->sessionState().get("sourceGrades")->asArray().empty());
  EXPECT_EQ(rig.query("capture:cam-a").getNumber("revision"), 2);
}
