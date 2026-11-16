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

