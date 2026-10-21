#include "core/TestPatternSource.h"
#include <set>
#include <chrono>
#include "core/MediaCore.h"
#include "modules/Interfaces.h"
#include <gtest/gtest.h>
#include <memory>
#include <atomic>
#include <future>
#include <latch>
#include <thread>
#include <functional>
#include "rpc/JsonRpcServer.h"

namespace {
// Observe a real snapshot dependency rather than adding production-only counters.
class CountingEncoder final : public corevideo::modules::IEncoderSink {
 public:
  void configureRecording(const corevideo::modules::RecordingSessionRequest&) override {}
  corevideo::modules::OutputSession start(const std::vector<std::string>&,
                                         const std::vector<std::string>&) override { return {}; }
  void submit(const corevideo::modules::ProgramFrame&) override {}
  corevideo::modules::OutputSession session() const override { ++reads; return {}; }
  mutable int reads = 0;
};
}

namespace {
class StalledStatusEncoder final : public corevideo::modules::IEncoderSink {
 public:
  void configureRecording(const corevideo::modules::RecordingSessionRequest&) override {}
  corevideo::modules::OutputSession start(const std::vector<std::string>&,
      const std::vector<std::string>&) override { return {}; }
  void submit(const corevideo::modules::ProgramFrame&) override {}
  corevideo::modules::OutputSession session() const override {
    if (stall.exchange(false)) { entered.count_down(); release.wait(); }
    corevideo::modules::OutputSession result;
    result.encoderName = "owned-observation";
    return result;
  }
  mutable std::atomic<bool> stall{false};
  mutable std::latch entered{1}, release{1};
};
}

TEST(MediaCoreBatchSnapshot, StatusAndRenderContinueWhileAudioOwnerQueryIsStalled) {
  auto modules = corevideo::modules::createStubModules();
  auto encoder = std::make_unique<StalledStatusEncoder>();
  auto* stalled = encoder.get(); modules.encoder = std::move(encoder);
  corevideo::core::MediaCore core(std::move(modules));
  core.enableAudioOutputWorker(); std::mutex coreMutex;
  core.renderAudioOutputTick(coreMutex); // establish real owner observation
  stalled->stall = true;
  std::thread owner([&] { core.renderAudioOutputTick(coreMutex); });
  stalled->entered.wait();
  auto reader = std::async(std::launch::async, [&] {
    std::lock_guard<std::mutex> lock(coreMutex);
    corevideo::rpc::JsonRpcServer server(core);
    const auto before = core.sessionState();
    const auto health = server.handle(corevideo::rpc::Json::Object{{"type", "health"}});
    const auto profile = server.handle(corevideo::rpc::Json::Object{{"type", "handshake"}});
    core.renderDisplayTick();
    const auto after = core.sessionState();
    return std::make_pair(before.getString("encoder"), after.getNumber("programFrameCount"));
  });
  const auto ready = reader.wait_for(std::chrono::seconds(2));
  // Always release/join, even if the old nested-lock dependency regresses.
  stalled->release.count_down(); owner.join();
  ASSERT_EQ(ready, std::future_status::ready);
  const auto result = reader.get();
  EXPECT_EQ(result.first, "owned-observation");
  EXPECT_GT(result.second, 0);
  // Stub progress proves lock separation, not installed GPU/pixel acceptance.
}

TEST(MediaCoreBatchSnapshot, CapturesOnceAfterOrderedMutations) {
  auto modules = corevideo::modules::createStubModules();
  auto encoder = std::make_unique<CountingEncoder>();
  auto* counts = encoder.get();
  modules.encoder = std::move(encoder);
  corevideo::core::MediaCore core(std::move(modules));
  core.enableAudioOutputWorker();
  counts->reads = 0;
  const auto snapshot = core.sessionState();
  const int snapshotReads = counts->reads;
  // Live status reads consume the owner publication, including before the
  // first audio tick; they must not query the encoder to fill missing facts.
  ASSERT_EQ(snapshotReads, 0);
  counts->reads = 0;
  const auto result = core.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{{"type", "load-scene-graph"}, {"sceneId", "first"}},
      corevideo::rpc::Json::Object{{"type", "set-overlay-asset"}, {"overlayId", "key"}, {"text", "Name"}},
      corevideo::rpc::Json::Object{{"type", "load-scene-graph"}, {"sceneId", "last"}},
      corevideo::rpc::Json::Object{{"type", "set-overlay-asset"}, {"overlayId", "key"},
          {"enabled", false}, {"keyPhase", "hidden"}}});
  EXPECT_EQ(counts->reads, snapshotReads);
  EXPECT_EQ(result.getString("sceneId"), "last");
  EXPECT_EQ(result.getNumber("overlayCount"), 0);
  EXPECT_EQ(result.getNumber("programFrameCount"), snapshot.getNumber("programFrameCount"));

  counts->reads = 0;
  const auto direct = core.applyCommand(corevideo::rpc::Json::Object{
      {"type", "load-scene-graph"}, {"sceneId", "direct"}});
  EXPECT_EQ(counts->reads, snapshotReads);
  EXPECT_EQ(direct.getString("sceneId"), "direct");
}

namespace {
// Observe the actual request assembled by MediaCore, including its Preview gate.
// This does not create a D3D device, run a meeting, or open capture/shared memory.
class SourcePreviewRequestProbe final : public corevideo::modules::ICompositor {
 public:
  explicit SourcePreviewRequestProbe(std::unique_ptr<corevideo::modules::ICompositor> inner)
      : inner_(std::move(inner)) {}
  std::string rendererName() const override { return inner_->rendererName(); }
  corevideo::modules::ProgramFrame render(const corevideo::modules::CompositorRenderPlan& plan,
      const std::vector<corevideo::modules::VideoFrame>& frames) override {
    if (beforeRender) beforeRender(plan, frames);
    return inner_->render(plan, frames);
  }
  bool hasIsolatedMonitors() const override { return true; }
  bool supportsGradePreview() const override { return true; }
  void submitMonitors(corevideo::modules::MonitorRenderRequest request) override { monitors = std::move(request); ++monitorCalls; }
  void submitGradePreviews(corevideo::modules::MonitorRenderRequest request) override { grade = std::move(request); }
  corevideo::modules::MonitorRenderRequest monitors, grade;
  int monitorCalls = 0;
  std::function<void(const corevideo::modules::CompositorRenderPlan&,
      const std::vector<corevideo::modules::VideoFrame>&)> beforeRender;
 private:
  std::unique_ptr<corevideo::modules::ICompositor> inner_;
};

class SourcePreviewEpochedSource final : public corevideo::core::ISource {
 public:
  explicit SourcePreviewEpochedSource(std::string id) : inner_(std::move(id), 16, 16) {}
  const corevideo::core::SourceDescriptor& descriptor() const override { return inner_.descriptor(); }
  corevideo::core::SourceTick poll(int64_t now) override {
    auto tick = inner_.poll(now);
    for (auto& frame : tick.video) frame.sourceEpoch = 123;
    return tick;
  }
  corevideo::core::SourceIngestCounters counters() const override { return inner_.counters(); }
 private:
  corevideo::core::TestPatternSource inner_;
};

struct SourcePreviewRequestRig {
  SourcePreviewRequestRig() {
    auto modules = corevideo::modules::createStubModules();
    auto probe = std::make_unique<SourcePreviewRequestProbe>(std::move(modules.compositor));
    observed = probe.get(); modules.compositor = std::move(probe);
    core = std::make_unique<corevideo::core::MediaCore>(std::move(modules));
    std::vector<std::shared_ptr<corevideo::core::ISource>> sources;
    for (int i = 0; i < 12; ++i)
      sources.push_back(std::make_shared<SourcePreviewEpochedSource>("speaker-" + std::to_string(i)));
    core->useZoomSourcesForTest(std::move(sources));
  }
  void assign(bool duplicate = false) {
    corevideo::rpc::Json::Array rows;
    for (int i = 0; i < 12; ++i) {
      const auto id = "speaker-" + std::to_string(duplicate && i == 1 ? 0 : i);
      rows.emplace_back(corevideo::rpc::Json::Object{{"sourceId", "zoom:" + id},
          {"kind", "participant"}, {"participantId", id}, {"slot", i}});
    }
    (void)core->applyCommand(corevideo::rpc::Json::Object{{"type", "set-multiview-layout"}, {"sources", rows}});
  }
  void compositePreview() {
    (void)core->applyCommand(corevideo::rpc::Json::Object{{"type", "set-preview-scene"}, {"sceneId", "preview"},
        {"routes", corevideo::rpc::Json::Array{corevideo::rpc::Json::Object{
            {"routeId", "guest"}, {"mode", "fixed"}, {"participantId", "speaker-0"}}}}});
  }
  bool interest(bool enabled) {
    corevideo::rpc::JsonRpcServer rpc(*core);
    const auto response = rpc.handle(corevideo::rpc::Json::Object{{"id", "interest"},
        {"type", "set-source-preview-demand"}, {"enabled", enabled}});
    return response.get("ok") && response.get("ok")->asBool();
  }
  std::unique_ptr<corevideo::core::MediaCore> core;
  SourcePreviewRequestProbe* observed = nullptr;
};

std::set<std::string> sourcePreviewDemandIds(const corevideo::modules::MonitorRenderRequest& request,
    corevideo::modules::SourceMonitorConsumer consumer) {
  std::set<std::string> result;
  for (const auto& source : request.sourceExports) if (source.consumer == consumer) result.insert(source.sourceId);
  return result;
}
}  // namespace

TEST(SourcePreviewRequestIntegration, CompositePreviewRequiresLiveInspectorInterestAndBoundsAssignedDemand) {
  using corevideo::modules::SourceMonitorConsumer;
  SourcePreviewRequestRig rig; rig.assign(); rig.compositePreview();
  ASSERT_TRUE(rig.core->configureGradePreview(corevideo::rpc::Json::Object{{"instanceId", "grade-editor"},
      {"sourceId", "speaker-0"}, {"revision", 1}, {"enabled", true}}));
  rig.core->renderDisplayTick();
  ASSERT_GT(rig.observed->monitorCalls, 0);
  ASSERT_TRUE(rig.observed->monitors.previewActive);
  EXPECT_TRUE(rig.observed->monitors.sourceExports.empty());
  ASSERT_EQ(rig.observed->grade.gradePreviews.size(), 1u);
  EXPECT_EQ(rig.observed->grade.gradePreviews[0].instanceId, "grade-editor");
  ASSERT_TRUE(rig.interest(true)); rig.core->renderDisplayTick();
  auto active = sourcePreviewDemandIds(rig.observed->monitors, SourceMonitorConsumer::Inspector);
  ASSERT_EQ(active.size(), 10u); EXPECT_EQ(rig.observed->monitors.sourceExports.size(), 10u);
  EXPECT_TRUE(active.count("speaker-0")); EXPECT_TRUE(active.count("speaker-9"));
  EXPECT_FALSE(active.count("speaker-10")); EXPECT_FALSE(active.count("speaker-11"));
  EXPECT_TRUE(sourcePreviewDemandIds(rig.observed->monitors, SourceMonitorConsumer::PreviewFallback).empty());

  const auto before = rig.core->sessionState();
  const auto registry = rig.core->sourceRegistrySnapshotForTest();
  const auto planId = rig.observed->monitors.programPlan.renderPlanId;
  std::vector<std::pair<std::string, uint64_t>> sourceEpochs;
  for (const auto& source : rig.observed->monitors.frames) sourceEpochs.emplace_back(source.participantId, source.sourceEpoch);
  ASSERT_TRUE(std::any_of(sourceEpochs.begin(), sourceEpochs.end(), [](const auto& source) {
    return source.first == "speaker-0" && source.second == 123;
  }));
  // Real RPC renewals are lease updates only, not scene/source mutations.
  for (int i = 0; i < 5; ++i) ASSERT_TRUE(rig.interest(true));
  const auto after = rig.core->sessionState();
  EXPECT_EQ(after.getNumber("appliedControlRevision"), before.getNumber("appliedControlRevision"));
  EXPECT_EQ(after.get("realtimeEvidence")->get("render")->getNumber("generation"),
      before.get("realtimeEvidence")->get("render")->getNumber("generation"));
  EXPECT_EQ(after.get("realtimeEvidence")->get("capturePreparation")->stringify(),
      before.get("realtimeEvidence")->get("capturePreparation")->stringify());
  EXPECT_EQ(rig.core->sourceRegistrySnapshotForTest()->revision, registry->revision);
  EXPECT_EQ(rig.core->sourceRegistrySnapshotForTest()->decisionRevision, registry->decisionRevision);
  rig.core->renderDisplayTick();
  EXPECT_EQ(rig.observed->monitors.programPlan.renderPlanId, planId);
  EXPECT_EQ(sourcePreviewDemandIds(rig.observed->monitors, SourceMonitorConsumer::Inspector), active);
  std::vector<std::pair<std::string, uint64_t>> renewedEpochs;
  for (const auto& source : rig.observed->monitors.frames) renewedEpochs.emplace_back(source.participantId, source.sourceEpoch);
  EXPECT_EQ(renewedEpochs, sourceEpochs);
  ASSERT_EQ(rig.observed->grade.gradePreviews.size(), 1u);
  EXPECT_EQ(rig.observed->grade.gradePreviews[0].instanceId, "grade-editor");

  rig.assign(true); rig.core->renderDisplayTick();
  EXPECT_EQ(sourcePreviewDemandIds(rig.observed->monitors, SourceMonitorConsumer::Inspector).size(), 9u);
  EXPECT_EQ(rig.observed->monitors.sourceExports.size(), 9u);
  ASSERT_TRUE(rig.interest(false)); rig.core->renderDisplayTick();
  EXPECT_TRUE(rig.observed->monitors.sourceExports.empty());
  EXPECT_TRUE(rig.observed->monitors.previewActive);
  ASSERT_EQ(rig.observed->grade.gradePreviews.size(), 1u);
}

TEST(SourcePreviewRequestIntegration, InactiveInspectorDoesNotRemoveIndependentPreviewFallback) {
  using corevideo::modules::SourceMonitorConsumer;
  SourcePreviewRequestRig rig; rig.assign(); rig.core->renderDisplayTick();
  ASSERT_FALSE(rig.observed->monitors.previewActive);
  const auto fallback = sourcePreviewDemandIds(rig.observed->monitors, SourceMonitorConsumer::PreviewFallback);
  ASSERT_GE(fallback.size(), 12u);
  EXPECT_TRUE(fallback.count("speaker-11"));
  ASSERT_TRUE(rig.interest(true)); rig.core->renderDisplayTick();
  EXPECT_EQ(sourcePreviewDemandIds(rig.observed->monitors, SourceMonitorConsumer::PreviewFallback), fallback);
  EXPECT_TRUE(sourcePreviewDemandIds(rig.observed->monitors, SourceMonitorConsumer::Inspector).empty());
  ASSERT_TRUE(rig.interest(false)); rig.core->renderDisplayTick();
  EXPECT_EQ(sourcePreviewDemandIds(rig.observed->monitors, SourceMonitorConsumer::PreviewFallback), fallback);
}

TEST(SourcePreviewRequestIntegration, ExpiredConsumerLeaseStopsInspectorWhileCompositeAndGradeRemainActive) {
  SourcePreviewRequestRig rig; rig.assign(); rig.compositePreview();
  ASSERT_TRUE(rig.interest(true)); rig.core->renderDisplayTick();
  ASSERT_EQ(rig.observed->monitors.sourceExports.size(), 10u);
  std::this_thread::sleep_for(std::chrono::milliseconds(3100));
  ASSERT_TRUE(rig.core->configureGradePreview(corevideo::rpc::Json::Object{{"instanceId", "still-open-grade"},
      {"sourceId", "speaker-0"}, {"revision", 1}, {"enabled", true}}));
  rig.core->renderDisplayTick();
  EXPECT_TRUE(rig.observed->monitors.previewActive);
  EXPECT_TRUE(rig.observed->monitors.sourceExports.empty());
  ASSERT_EQ(rig.observed->grade.gradePreviews.size(), 1u);
  EXPECT_EQ(rig.observed->grade.gradePreviews[0].instanceId, "still-open-grade");
}

TEST(SourcePreviewRequestIntegration, PublicRpcRejectsMissingOrNonBooleanInterestWithoutStartingInspector) {
  SourcePreviewRequestRig rig; rig.assign(); rig.compositePreview();
  corevideo::rpc::JsonRpcServer rpc(*rig.core);
  const auto missing = rpc.handle(corevideo::rpc::Json::Object{{"id", "missing"}, {"type", "set-source-preview-demand"}});
  ASSERT_NE(missing.get("ok"), nullptr); EXPECT_FALSE(missing.get("ok")->asBool());
  for (const auto& invalid : corevideo::rpc::Json::Array{nullptr, "true", 1, corevideo::rpc::Json::Array{}, corevideo::rpc::Json::Object{}}) {
    const auto response = rpc.handle(corevideo::rpc::Json::Object{{"id", "bad"},
        {"type", "set-source-preview-demand"}, {"enabled", invalid}});
    ASSERT_NE(response.get("ok"), nullptr); EXPECT_FALSE(response.get("ok")->asBool());
    rig.core->renderDisplayTick();
    EXPECT_TRUE(rig.observed->monitors.sourceExports.empty());
  }
  EXPECT_TRUE(rig.interest(true));
  const auto invalidWhileActive = rpc.handle(corevideo::rpc::Json::Object{{"id", "bad-active"},
      {"type", "set-source-preview-demand"}, {"enabled", "false"}});
  EXPECT_FALSE(invalidWhileActive.get("ok")->asBool());
  rig.core->renderDisplayTick();
  EXPECT_EQ(rig.observed->monitors.sourceExports.size(), 10u);
}

TEST(SourcePreviewRequestIntegration, CurrentMonitorPayloadDoesNotWaitForProgramRendering) {
  SourcePreviewRequestRig rig; rig.assign(); rig.compositePreview();
  ASSERT_TRUE(rig.interest(true));
  for (int tick = 0; tick < 2; ++tick) {
    const int previousCalls = rig.observed->monitorCalls;
    bool rendered = false;
    rig.observed->beforeRender = [&](const auto& plan, const auto& frames) {
      rendered = true;
      // A stalled Program render must already have handed this tick's immutable
      // payload to the independent monitor owner, not last tick's fallback.
      EXPECT_EQ(rig.observed->monitorCalls, previousCalls + 1);
      EXPECT_EQ(rig.observed->monitors.programPlan.renderPlanId, plan.renderPlanId);
      EXPECT_TRUE(rig.observed->monitors.multiviewActive);
      EXPECT_TRUE(rig.observed->monitors.previewActive);
      ASSERT_EQ(rig.observed->monitors.frames.size(), frames.size());
      for (size_t i = 0; i < frames.size(); ++i) {
        EXPECT_EQ(rig.observed->monitors.frames[i].participantId, frames[i].participantId);
        EXPECT_EQ(rig.observed->monitors.frames[i].frameId, frames[i].frameId);
        EXPECT_EQ(rig.observed->monitors.frames[i].sourceEpoch, frames[i].sourceEpoch);
      }
    };
    rig.core->renderDisplayTick();
    EXPECT_TRUE(rendered);
    EXPECT_EQ(rig.observed->monitorCalls, previousCalls + 1);
  }
  rig.observed->beforeRender = {};
}
