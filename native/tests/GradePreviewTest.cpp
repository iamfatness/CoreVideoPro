#include "core/GradePreviewController.h"
#include "modules/MonitorInputCache.h"
#include <gtest/gtest.h>
using namespace corevideo;
namespace {
struct PreviewCompositor : modules::ICompositor {
  modules::MonitorRenderRequest request;
  int submissions = 0, renders = 0;
  std::shared_ptr<modules::MonitorRenderResult> result;
  std::string rendererName() const override { return "test"; }
  modules::ProgramFrame render(const modules::CompositorRenderPlan&, const std::vector<modules::VideoFrame>&) override { ++renders; return {}; }
  void submitGradePreviews(modules::MonitorRenderRequest value) override { request = std::move(value); ++submissions; }
  std::shared_ptr<const modules::MonitorRenderResult> latestGradePreviews() const override { return result; }
};
rpc::Json demand(const std::string& id, int revision = 0, bool enabled = true) {
  return rpc::Json::Object{{"instanceId", id}, {"sourceId", "p1"}, {"revision", revision}, {"enabled", enabled}};
}
}
TEST(GradePreviewControl, CapacityLeaseAndCloseRetireDemandWithoutRenderingProgram) {
  core::GradePreviewController control; PreviewCompositor compositor;
  auto now = core::GradePreviewController::Clock::now();
  for (auto id : {"a", "b", "c", "d"}) ASSERT_TRUE(control.configure(demand(id), {}, true, now));
  EXPECT_EQ(control.size(), 3u);
  auto events = control.drain(); ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].getString("reason"), "grade-preview-capacity");
  modules::VideoFrame source; source.participantId = "p1";
  source.pixels = std::make_shared<std::vector<uint8_t>>(16, 127);
  modules::VideoFrame other; other.participantId = "unrelated";
  control.tick({source, other}, compositor, 1, now);
  ASSERT_EQ(compositor.request.frames.size(), 1u);
  EXPECT_TRUE(compositor.request.frames[0].pixels == source.pixels);
  ASSERT_TRUE(control.configure(demand("a", 0, false), {}, true, now));
  EXPECT_EQ(control.size(), 2u);
  control.tick({}, compositor, 2, now + core::GradePreviewController::Lease);
  EXPECT_EQ(control.size(), 0u); EXPECT_TRUE(compositor.request.gradePreviews.empty());
  EXPECT_EQ(compositor.submissions, 2); EXPECT_EQ(compositor.renders, 0);
  control.tick({}, compositor, 3, now + std::chrono::seconds(4));
  EXPECT_EQ(compositor.submissions, 2);
}
TEST(GradePreviewControl, RejectsInvalidAndAmbiguousRevisionAndReportsUnsupported) {
  core::GradePreviewController control;
  EXPECT_FALSE(control.configure(demand(""), {}, true));
  EXPECT_FALSE(control.configure(demand("a", -1), {}, true));
  EXPECT_TRUE(control.configure(demand("a", 2), {}, true));
  modules::CompositorColorGrade warm; warm.lut = "warm-film";
  EXPECT_FALSE(control.configure(demand("a", 2), warm, true));
  EXPECT_TRUE(control.configure(demand("a", 1), warm, true));
  EXPECT_TRUE(control.configure(demand("stub"), warm, false));
  auto events = control.drain(); ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].getString("status"), "unavailable");
  EXPECT_EQ(events[0].getString("reason"), "native-grade-preview-not-built");
}
TEST(GradePreviewControl, RoiValidationAndRevisionProtectMeasurements) {
  core::GradePreviewController control;
  auto d=demand("roi",1);
  auto fields=d.asObject(); fields["scopeRoi"]=rpc::Json::Object{{"enabled",true},{"x",.25},{"y",.25},{"width",.5},{"height",.5},{"revision",1}};
  ASSERT_TRUE(control.configure(fields,{},true));
  auto changed=fields; changed["scopeRoi"]=rpc::Json::Object{{"enabled",true},{"width",.25},{"height",.5},{"revision",2}};
  EXPECT_FALSE(control.configure(changed,{},true));
  changed["revision"]=2;EXPECT_TRUE(control.configure(changed,{},true));
  changed["scopeRoi"]=rpc::Json::Object{{"enabled",true},{"x",.9},{"width",.2}};
  EXPECT_FALSE(control.configure(changed,{},true));
  compositor::ScopeRoi r; r.enabled=true;r.x=r.y=.999;r.width=r.height=.001;
  const auto p=r.pixels(64,64);EXPECT_EQ(p.x,63);EXPECT_EQ(p.y,63);EXPECT_EQ(p.width,1);EXPECT_EQ(p.height,1);
}
TEST(GradePreviewControl, MatchesRevisionAndSourceIdentityAndDetectsHeldFrameRatherThanJobCadence) {
  core::GradePreviewController control; PreviewCompositor compositor;
  auto now = core::GradePreviewController::Clock::now();
  ASSERT_TRUE(control.configure(demand("a", 1), {}, true, now));
  compositor.result = std::make_shared<modules::MonitorRenderResult>();
  modules::GradePreviewSurface surface; surface.demand = {"a", "p1", 0, {}};
  surface.status = "ready"; surface.sourceFrameId = 5; surface.sourceEpoch = 2;
  compositor.result->gradePreviews.push_back(surface);
  control.tick({}, compositor, 1, now);
  EXPECT_EQ(control.drain()[0].getString("status"), "preparing");
  compositor.result->gradePreviews[0].demand.revision = 1;
  control.tick({}, compositor, 2, now + std::chrono::milliseconds(100));
  EXPECT_EQ(control.drain()[0].getString("status"), "ready");
  control.tick({}, compositor, 99, now + std::chrono::milliseconds(1200));
  EXPECT_EQ(control.drain()[0].getString("status"), "stale");
  compositor.result->gradePreviews[0].sourceEpoch = 3;
  control.tick({}, compositor, 100, now + std::chrono::milliseconds(1300));
  EXPECT_EQ(control.drain()[0].getString("status"), "ready");
  compositor.result->gradePreviews[0].demand.sourceId = "wrong";
  control.tick({}, compositor, 101, now + std::chrono::milliseconds(1400));
  EXPECT_EQ(control.drain()[0].getString("status"), "preparing");
}
TEST(GradePreviewControl, InputCacheHoldsTrueIdentityAndReleasesOnClose) {
  modules::MonitorInputCache cache; modules::MonitorRenderRequest request;
  request.gradePreviews.push_back({"a", "p1", 1, {}});
  modules::VideoFrame source; source.participantId = "p1"; source.sourceEpoch = 8;
  source.frameId = 77; source.width = source.height = source.pixelWidth = source.pixelHeight = 2; source.pixelStride = 8;
  source.pixels = std::make_shared<std::vector<uint8_t>>(16, 127);
  request.frames.push_back(source); modules::MonitorRenderResult result;
  cache.prepare(request, result); EXPECT_EQ(result.retainedInputs, 1u);
  request.frames.clear(); result = {}; cache.prepare(request, result);
  EXPECT_EQ(result.heldInputs, 1u); ASSERT_EQ(request.frames.size(), 1u);
  EXPECT_EQ(request.frames[0].sourceEpoch, 8u); EXPECT_EQ(request.frames[0].frameId, 77);
  request = {}; result = {}; cache.prepare(request, result);
  EXPECT_EQ(result.retainedInputs, 0u);
}
