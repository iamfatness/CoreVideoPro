#include "core/GradePreviewController.h"
#include "core/SourceGradeState.h"
#include "modules/MonitorInputCache.h"
#include <gtest/gtest.h>
#include <set>
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

TEST(GradePreviewControl, FullEventQueueRefusesBeforeReplacementAndCurrentSurfaceRetriesAfterDrain) {
  core::GradePreviewController control; PreviewCompositor compositor;
  const auto now = core::GradePreviewController::Clock::now();
  ASSERT_TRUE(control.configure(demand("a", 3), {}, true, now));
  compositor.result = std::make_shared<modules::MonitorRenderResult>();
  modules::GradePreviewSurface surface; surface.demand = {"a", "p1", 3, {}};
  surface.sourceEpoch = 7; surface.status = "ready";
  compositor.result->gradePreviews.push_back(surface);
  for (int i = 0; i < 20; ++i) {
    compositor.result->gradePreviews[0].sourceFrameId = i;
    control.tick({}, compositor, i, now + std::chrono::milliseconds(100 * i));
  }
  EXPECT_EQ(control.eventsRefused(), 4u);
  EXPECT_LE(control.pendingEventBytes(), core::GradePreviewController::EventByteBudget);
  auto events = control.drain(); ASSERT_EQ(events.size(), 16u);
  for (size_t i = 0; i < events.size(); ++i) EXPECT_EQ(events[i].getNumber("sourceFrameId"), double(i));
  std::weak_ptr<const rpc::Json::Object> lifetime = std::get<rpc::Json::ObjectValue>(events[0].value());
  core::SourceGradeState grades;
  auto captured = grades.captureAnnotations(std::move(events));
  EXPECT_FALSE(lifetime.expired()); EXPECT_EQ(control.pendingEventBytes(), 0u);
  auto assembled = std::move(captured).assemble();
  EXPECT_TRUE(lifetime.expired()); // original root retires during off-owner assembly, not during drain
  assembled.clear();
  EXPECT_TRUE(lifetime.expired());
  compositor.result->gradePreviews[0].sourceFrameId = 99;
  control.tick({}, compositor, 99, now + std::chrono::milliseconds(2100));
  events = control.drain(); ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].getNumber("sourceFrameId"), 99);
}

TEST(GradePreviewControl, ExhaustedOneShotUnavailableIsRejectedAndCanBeRetried) {
  core::GradePreviewController control;
  for (int i = 0; i < 16; ++i) ASSERT_TRUE(control.configure(demand("unavailable", i), {}, false));
  EXPECT_FALSE(control.configure(demand("unavailable", 16), {}, false));
  EXPECT_EQ(control.eventsRefused(), 1u);
  auto events = control.drain(); ASSERT_EQ(events.size(), 16u);
  for (size_t i = 0; i < events.size(); ++i) EXPECT_EQ(events[i].getNumber("revision"), double(i));
  EXPECT_TRUE(control.configure(demand("unavailable", 16), {}, false));
  events = control.drain(); ASSERT_EQ(events.size(), 1u); EXPECT_EQ(events[0].getNumber("revision"), 16);
  core::SourceGradeState grades;
  bool rejected = false;
  try { (void)grades.captureAnnotations(std::vector<rpc::Json>(17)); }
  catch (const std::out_of_range&) { rejected = true; }
  EXPECT_TRUE(rejected);
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
  auto circle=fields; circle["scopeRoi"]=rpc::Json::Object{{"enabled",true},{"x",.25},{"y",.25},{"width",.5},{"height",.5},{"revision",1},{"shape","circle"}};
  EXPECT_FALSE(control.configure(circle,{},true)); // Same revision cannot silently change geometry/mask.
  circle["revision"]=2;EXPECT_TRUE(control.configure(circle,{},true));
  auto invalid=circle; invalid["revision"]=3;
  invalid["scopeRoi"]=rpc::Json::Object{{"shape","triangle"}};EXPECT_FALSE(control.configure(invalid,{},true));
  compositor::ScopeRoi mask;mask.enabled=true;mask.shape="circle";
  EXPECT_EQ(mask.sampleCount(),28960);mask.enabled=false;EXPECT_EQ(mask.sampleCount(),36864);
  fields["revision"]=3;ASSERT_TRUE(control.configure(fields,{},true));
  auto changed=fields; changed["scopeRoi"]=rpc::Json::Object{{"enabled",true},{"width",.25},{"height",.5},{"revision",2}};
  EXPECT_FALSE(control.configure(changed,{},true));
  changed["revision"]=4;EXPECT_TRUE(control.configure(changed,{},true));
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

TEST(GradePreviewControl, AuthoredStillCanvasDoesNotBecomeAStalledLiveSource) {
  core::GradePreviewController control; PreviewCompositor compositor;
  const auto now = core::GradePreviewController::Clock::now();
  const rpc::Json request = rpc::Json::Object{{"instanceId", "canvas"}, {"sourceId", "preview:lower-third"}, {"revision", 1},
      {"lowerThirdPreview", rpc::Json::Object{{"appearance", rpc::Json{}}, {"name", "Example"}}}};
  ASSERT_TRUE(control.configure(request, {}, true, now));
  compositor.result = std::make_shared<modules::MonitorRenderResult>();
  modules::GradePreviewSurface surface; surface.demand = {"canvas", "preview:lower-third", 1, {}};
  surface.sourceEpoch = 1; surface.sourceFrameId = 1; surface.status = "ready";
  compositor.result->gradePreviews.push_back(surface);
  control.tick({}, compositor, 1, now); EXPECT_EQ("ready", control.drain()[0].getString("status"));
  control.tick({}, compositor, 2, now + std::chrono::milliseconds(1500));
  EXPECT_EQ("ready", control.drain()[0].getString("status"));
  EXPECT_EQ(0, compositor.renders);
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

TEST(GradePreviewControl, PlayingDemandUsesTheTickClockAndThirtyHertzCadence) {
  using namespace std::chrono_literals;
  using J = rpc::Json;
  core::GradePreviewController control;
  PreviewCompositor compositor;
  const auto t0 = core::GradePreviewController::Clock::time_point{} + 1s;
  const auto canvas = [](int revision, const J& play) {
    return J::Object{{"instanceId","designer"},{"sourceId","preview:lower-third"},{"revision",revision},
        {"lowerThirdPreview",J::Object{{"appearance",J{}},{"graphics",J::Object{{"play",play}}}}}};
  };
  const J play = J::Object{{"mode","in"},{"token",1}};
  const auto prepare = [](const J&) { return modules::CompositorOverlayContent{}; };
  ASSERT_TRUE(control.configure(canvas(1, play), {}, true, t0, prepare));
  for (int i = 1; i <= 30; ++i) {
    control.tick({}, compositor, i, t0 + 34ms * i);
    EXPECT_EQ(1u, control.drain().size());
  }
  EXPECT_EQ(30, compositor.submissions);
  EXPECT_EQ(0u, control.eventsRefused());
  ASSERT_EQ(1u, compositor.request.gradePreviews.size());
  EXPECT_EQ("in", compositor.request.gradePreviews[0].playMode);
  EXPECT_EQ(1020., compositor.request.gradePreviews[0].playElapsedMs);
  ASSERT_TRUE(control.configure(canvas(2, play), {}, true, t0 + 1100ms, prepare));
  control.tick({}, compositor, 31, t0 + 1200ms);
  EXPECT_EQ(100., compositor.request.gradePreviews[0].playElapsedMs);
  // A still canvas (no play) returns to the idle cadence once its revision has
  // settled. Revision 3 arrives after revision 2's in-flight bound, so it is
  // admitted at once; its ready frame settles it on the first tick.
  ASSERT_TRUE(control.configure(canvas(3, J{}), {}, true, t0 + 1700ms, prepare));
  compositor.result = std::make_shared<modules::MonitorRenderResult>();
  modules::GradePreviewSurface still; still.demand = {"designer", "preview:lower-third", 3, {}};
  still.status = "ready"; still.texture.sharedHandleHex = "0x10"; still.texture.width = 1920; still.texture.height = 1080;
  compositor.result->gradePreviews.push_back(still);
  for (int i = 1; i <= 9; ++i) {
    control.tick({}, compositor, 31 + i, t0 + 1700ms + 34ms * i);
    control.drain();
  }
  EXPECT_EQ(34, compositor.submissions);
}

TEST(GradePreviewControl, InvalidPrivateEnvelopeCannotStartDraftPreparation) {
  using J = rpc::Json;
  core::GradePreviewController control;
  int preparations = 0;
  const auto prepare = [&](const J&) {
    ++preparations; return modules::CompositorOverlayContent{};
  };
  const auto now = core::GradePreviewController::Clock::now();
  J::Object preview{{"graphics", J::Object{{"templateId", "centered-title"}}}};
  J::Object request{{"instanceId", "designer"}, {"sourceId", "preview:lower-third"},
      {"revision", 1}, {"lowerThirdPreview", preview}};
  EXPECT_FALSE(control.configure(request, {}, true, now, prepare));
  EXPECT_EQ(preparations, 0);
  EXPECT_EQ(control.size(), 0u);
  preview["appearance"] = 42; request["lowerThirdPreview"] = preview;
  EXPECT_FALSE(control.configure(request, {}, true, now, prepare));
  EXPECT_EQ(preparations, 0);
  preview["appearance"] = J{}; request["lowerThirdPreview"] = preview;
  request["revision"] = -1;
  EXPECT_FALSE(control.configure(request, {}, true, now, prepare));
  EXPECT_EQ(preparations, 0);
  request["revision"] = 1;
  request["sourceId"] = "wrong-source";
  EXPECT_FALSE(control.configure(request, {}, true, now, prepare));
  EXPECT_EQ(preparations, 0);
  request["sourceId"] = "preview:lower-third";
  request["histogramMode"] = 42;
  EXPECT_FALSE(control.configure(request, {}, true, now, prepare));
  EXPECT_EQ(preparations, 0);
  request.erase("histogramMode");
  EXPECT_TRUE(control.configure(request, {}, true, now, prepare));
  EXPECT_EQ(preparations, 1);
  EXPECT_EQ(control.size(), 1u);
  EXPECT_TRUE(control.configure(request, {}, true, now, prepare)); // unchanged lease
  EXPECT_EQ(preparations, 1);
  preview["graphics"] = J::Object{{"templateId", "changed-without-revision"}};
  request["lowerThirdPreview"] = preview;
  EXPECT_FALSE(control.configure(request, {}, true, now, prepare));
  EXPECT_EQ(preparations, 1);
  request["instanceId"] = "other-designer";
  EXPECT_FALSE(control.configure(request, {}, true, now, prepare));
  EXPECT_EQ(preparations, 1);
  request["instanceId"] = "designer";
  request["revision"] = 2;
  request["enabled"] = false;
  EXPECT_TRUE(control.configure(request, {}, true, now, prepare));
  EXPECT_EQ(preparations, 1);
  EXPECT_EQ(control.size(), 0u);
}

TEST(GradePreviewControl, DraftPreparationRefusalPreservesTheAcceptedPreview) {
  using J = rpc::Json;
  core::GradePreviewController control;
  PreviewCompositor compositor;
  const auto now = core::GradePreviewController::Clock::now();
  J::Object request{{"instanceId", "designer"}, {"sourceId", "preview:lower-third"}, {"revision", 1},
      {"lowerThirdPreview", J::Object{{"appearance", J{}}, {"graphics", J::Object{{"templateId", "centered-title"}}}}}};
  int preparations = 0;
  const auto prepare = [&](const J&) -> std::optional<modules::CompositorOverlayContent> {
    if (++preparations > 1) return {};
    modules::CompositorOverlayContent overlay; overlay.text = "Accepted title"; return overlay;
  };
  ASSERT_TRUE(control.configure(request, {}, true, now, prepare));
  // Revision 1 is in flight: revision 2 waits unprepared, then is refused when
  // admitted past the in-flight bound. Its own revision reports unavailable.
  request["revision"] = 2;
  EXPECT_TRUE(control.configure(request, {}, true, now, prepare));
  EXPECT_EQ(preparations, 1);
  EXPECT_EQ(0u, control.promote(now + core::GradePreviewController::InFlightBound,
      [&](const J& draft, bool&) { return prepare(draft); }));
  EXPECT_EQ(preparations, 2);
  EXPECT_FALSE(control.waiting());
  auto refused = control.drain(); ASSERT_EQ(refused.size(), 1u);
  EXPECT_EQ(refused[0].getNumber("revision"), 2); EXPECT_EQ(refused[0].getString("status"), "unavailable");
  EXPECT_EQ(refused[0].getString("reason"), "graphics-draft-refused");
  control.tick({}, compositor, 1, now);
  ASSERT_EQ(compositor.request.gradePreviews.size(), 1u);
  const auto& accepted = compositor.request.gradePreviews.front();
  EXPECT_EQ(accepted.revision, 1);
  ASSERT_TRUE(accepted.lowerThirdPreview.has_value());
  EXPECT_EQ(accepted.lowerThirdPreview->text, "Accepted title");
}

// Grading Phase 2, Task 5 (shared, reaches Windows): the grade-preview texture objects carry the
// macOS IOSurface id ONLY when one is set, so a Windows event keeps exactly its old key set
// (the ProgramFramePreview.cpp program-shared-texture rule).
TEST(GradePreviewControl, TextureObjectsCarryAnIOSurfaceIdOnlyWhenOneIsSet) {
  core::GradePreviewController control; PreviewCompositor compositor;
  const auto now = core::GradePreviewController::Clock::now();
  ASSERT_TRUE(control.configure(demand("a", 1), {}, true, now));
  compositor.result = std::make_shared<modules::MonitorRenderResult>();
  modules::GradePreviewSurface surface; surface.demand = {"a", "p1", 1, {}};
  surface.status = "ready"; surface.sourceEpoch = 3;
  surface.texture.sharedHandleHex = "0x10"; surface.texture.width = 64; surface.texture.height = 36;
  surface.scopes.texture.sharedHandleHex = "0x20"; surface.scopes.texture.width = 1536; surface.scopes.texture.height = 512;
  compositor.result->gradePreviews.push_back(surface);
  control.tick({}, compositor, 1, now);
  auto events = control.drain(); ASSERT_EQ(events.size(), 1u);
  const std::set<std::string> windowsKeys{"sharedHandleHex", "width", "height", "format", "frameNumber"};
  auto keys = [](const rpc::Json& object) { std::set<std::string> out; for (const auto& [k, v] : object.asObject()) out.insert(k); return out; };
  EXPECT_EQ(keys(*events[0].get("texture")), windowsKeys) << "a Windows preview texture is unchanged";
  EXPECT_EQ(keys(*events[0].get("scopes")->get("texture")), windowsKeys) << "a Windows scope texture is unchanged";

  compositor.result->gradePreviews[0].texture = {}; compositor.result->gradePreviews[0].texture.iosurfaceId = 4242;
  compositor.result->gradePreviews[0].texture.width = 64; compositor.result->gradePreviews[0].texture.height = 36;
  compositor.result->gradePreviews[0].scopes.texture = {}; compositor.result->gradePreviews[0].scopes.texture.iosurfaceId = 4343;
  compositor.result->gradePreviews[0].scopes.texture.width = 1536; compositor.result->gradePreviews[0].scopes.texture.height = 512;
  compositor.result->gradePreviews[0].sourceFrameId = 2;
  control.tick({}, compositor, 2, now + std::chrono::milliseconds(150));
  events = control.drain(); ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].get("texture")->getNumber("iosurfaceId"), 4242.0);
  EXPECT_EQ(events[0].get("scopes")->get("texture")->getNumber("iosurfaceId"), 4343.0);
  EXPECT_EQ(events[0].get("scopes")->getNumber("sampleWidth"), 256.0) << "a Mac scope surface is a measurement";
}

// perf/designer-live-preview step 2: the authored canvas keeps one revision in
// the render pipeline and a single latest-value waiting slot.
namespace {
using J = rpc::Json;
J authored(int revision, double x = 0) {
  return J::Object{{"instanceId","designer"},{"sourceId","preview:lower-third"},{"revision",revision},
      {"lowerThirdPreview",J::Object{{"appearance",J{}},{"graphics",J::Object{{"templateId","centered-title"},{"x",x}}}}}};
}
std::shared_ptr<modules::MonitorRenderResult> readyResult(int revision) {
  auto result = std::make_shared<modules::MonitorRenderResult>();
  modules::GradePreviewSurface surface; surface.demand = {"designer", "preview:lower-third", revision, {}};
  surface.status = "ready"; surface.texture.sharedHandleHex = "0x10"; surface.texture.width = 1920; surface.texture.height = 1080;
  result->gradePreviews.push_back(surface); result->workMs = 2.5;
  return result;
}
}

TEST(GradePreviewControl, NewestDraftWaitsWhileOneIsInFlightAndNoStaleRevisionIsAdmitted) {
  using namespace std::chrono_literals;
  core::GradePreviewController control; PreviewCompositor compositor;
  const auto t0 = core::GradePreviewController::Clock::time_point{} + 1s;
  std::vector<double> prepared;
  const auto prepare = [&](const J& draft) -> std::optional<modules::CompositorOverlayContent> {
    prepared.push_back(draft.getNumber("x")); return modules::CompositorOverlayContent{};
  };
  const core::GradePreviewController::PrepareDraft admit = [&](const J& draft, bool&) { return prepare(draft); };
  ASSERT_TRUE(control.configure(authored(1, 1), {}, true, t0, prepare));
  for (int revision = 2; revision <= 4; ++revision)
    ASSERT_TRUE(control.configure(authored(revision, revision), {}, true, t0 + 5ms * revision, prepare));
  EXPECT_EQ(prepared, std::vector<double>{1});           // waiting drafts are not prepared
  EXPECT_TRUE(control.configure(authored(3, 3), {}, true, t0 + 25ms, prepare)); // older than waiting: ignored
  EXPECT_FALSE(control.configure(authored(4, 99), {}, true, t0 + 25ms, prepare)); // same revision, new content
  EXPECT_TRUE(control.renew(authored(4, 4), t0 + 25ms));  // the waiting revision's lease renews
  EXPECT_EQ(0u, control.promote(t0 + 30ms, admit));       // revision 1 has not settled
  EXPECT_EQ(2u, control.stats().coalesced);
  compositor.result = readyResult(1);
  control.tick({}, compositor, 1, t0 + 40ms);             // revision 1 settles
  EXPECT_EQ(1u, control.promote(t0 + 41ms, admit));
  EXPECT_EQ(prepared, (std::vector<double>{1, 4}));      // 2 and 3 were never prepared
  control.tick({}, compositor, 2, t0 + 42ms);
  ASSERT_EQ(compositor.request.gradePreviews.size(), 1u);
  EXPECT_EQ(compositor.request.gradePreviews[0].revision, 4);
  EXPECT_FALSE(control.waiting());
  const auto stats = control.stats();
  EXPECT_EQ(stats.admitted, 2u); EXPECT_EQ(stats.ready, 1u); EXPECT_EQ(stats.supersededBeforeReady, 0u);
  EXPECT_NEAR(stats.admitToReadyP50Ms, 40.0, 0.001);
}

TEST(GradePreviewControl, InFlightRevisionIsPolledEveryTickAndASettledCanvasIdles) {
  using namespace std::chrono_literals;
  core::GradePreviewController control; PreviewCompositor compositor;
  const auto t0 = core::GradePreviewController::Clock::time_point{} + 1s;
  const auto prepare = [](const J&) { return std::optional<modules::CompositorOverlayContent>{modules::CompositorOverlayContent{}}; };
  ASSERT_TRUE(control.configure(authored(1), {}, true, t0, prepare));
  for (int i = 1; i <= 5; ++i) control.tick({}, compositor, i, t0 + 16ms * i);
  EXPECT_EQ(compositor.submissions, 5);                  // every render tick while in flight
  auto events = control.drain(); ASSERT_EQ(events.size(), 5u);
  EXPECT_EQ(events.back().getString("status"), "preparing");
  ASSERT_NE(events.back().get("timing"), nullptr);
  compositor.result = readyResult(1);
  control.tick({}, compositor, 6, t0 + 96ms);
  events = control.drain(); ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].getString("status"), "ready");
  EXPECT_NEAR(events[0].get("timing")->getNumber("admittedToReadyMs"), 96.0, 0.001);
  EXPECT_EQ(events[0].get("timing")->getNumber("workerMs"), 2.5);
  for (int i = 1; i <= 5; ++i) control.tick({}, compositor, 6 + i, t0 + 96ms + 16ms * i);
  EXPECT_EQ(compositor.submissions, 6);                  // settled: back to the 100 ms cadence
  control.tick({}, compositor, 12, t0 + 196ms);
  EXPECT_EQ(compositor.submissions, 7);
  // A revision that never settles is polled fast only for the in-flight bound.
  ASSERT_TRUE(control.configure(authored(2), {}, true, t0 + 200ms, prepare));
  compositor.result.reset();
  const int before = compositor.submissions;
  for (int i = 1; i <= 10; ++i)
    control.tick({}, compositor, 20 + i, t0 + 200ms + core::GradePreviewController::InFlightBound + 16ms * i);
  EXPECT_LE(compositor.submissions - before, 2);
}

TEST(GradePreviewControl, ObserveSettlesBetweenTicksSoTheWaitingDraftIsAdmitted) {
  using namespace std::chrono_literals;
  core::GradePreviewController control; PreviewCompositor compositor;
  const auto t0 = core::GradePreviewController::Clock::time_point{} + 1s;
  const auto prepare = [](const J&) { return std::optional<modules::CompositorOverlayContent>{modules::CompositorOverlayContent{}}; };
  const core::GradePreviewController::PrepareDraft admit = [&](const J& draft, bool&) { return prepare(draft); };
  ASSERT_TRUE(control.configure(authored(1), {}, true, t0, prepare));
  ASSERT_TRUE(control.configure(authored(2), {}, true, t0 + 2ms, prepare));
  EXPECT_EQ(0u, control.observe(compositor, t0 + 4ms));    // no worker result yet
  compositor.result = readyResult(2);                       // a different revision does not settle 1
  EXPECT_EQ(0u, control.observe(compositor, t0 + 5ms));
  compositor.result = readyResult(1);
  EXPECT_EQ(1u, control.observe(compositor, t0 + 6ms));
  auto events = control.drain(); ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].getNumber("revision"), 1); EXPECT_EQ(events[0].getString("status"), "ready");
  EXPECT_EQ(0u, control.observe(compositor, t0 + 7ms));    // settled once
  EXPECT_EQ(1u, control.promote(t0 + 7ms, admit));
  EXPECT_EQ(compositor.submissions, 0);                     // observe never submits work
}

TEST(GradePreviewControl, BusyCompilerKeepsTheNewestDraftWaiting) {
  using namespace std::chrono_literals;
  core::GradePreviewController control; PreviewCompositor compositor;
  const auto t0 = core::GradePreviewController::Clock::time_point{} + 1s;
  const auto prepare = [](const J&) { return std::optional<modules::CompositorOverlayContent>{modules::CompositorOverlayContent{}}; };
  ASSERT_TRUE(control.configure(authored(1), {}, true, t0, prepare));
  ASSERT_TRUE(control.configure(authored(2), {}, true, t0 + 1ms, prepare));
  compositor.result = readyResult(1);
  control.tick({}, compositor, 1, t0 + 2ms);
  control.drain();
  int attempts = 0;
  const core::GradePreviewController::PrepareDraft busy = [&](const J&, bool& isBusy) -> std::optional<modules::CompositorOverlayContent> {
    ++attempts; isBusy = true; return std::nullopt;
  };
  EXPECT_EQ(0u, control.promote(t0 + 3ms, busy));
  EXPECT_TRUE(control.waiting());
  EXPECT_TRUE(control.drain().empty());                  // busy is not a refusal
  const core::GradePreviewController::PrepareDraft ready = [&](const J& draft, bool&) { ++attempts; return prepare(draft); };
  EXPECT_EQ(1u, control.promote(t0 + 11ms, ready));
  EXPECT_EQ(attempts, 2);
  control.tick({}, compositor, 2, t0 + 12ms);
  ASSERT_EQ(compositor.request.gradePreviews.size(), 1u);
  EXPECT_EQ(compositor.request.gradePreviews[0].revision, 2);
}
