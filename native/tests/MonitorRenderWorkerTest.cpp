#include "modules/MonitorRenderWorker.h"
#include "modules/MonitorFrameAdmission.h"
#include "modules/MonitorInputEvidence.h"
#include <gtest/gtest.h>
#include <future>
#include <atomic>

using namespace corevideo::modules;

TEST(MonitorInputEvidence, UnobservedAndTruncatedResultsAreExplicit) {
  auto absent = monitorInputEvidence({});
  ASSERT_TRUE(absent.get("observed") != nullptr); EXPECT_FALSE(absent.get("observed")->asBool());
  auto result = std::make_shared<MonitorRenderResult>(); result->sequence = 17;
  for (int i = 0; i < 65; ++i) result->inputs.push_back({std::to_string(i), "held", "input-unavailable", 3, 4, 99, 123});
  auto evidence = monitorInputEvidence(result);
  EXPECT_EQ(evidence.getNumber("sequence"), 17); EXPECT_EQ(evidence.getNumber("omitted"), 1);
  ASSERT_TRUE(evidence.get("sources") != nullptr);
  ASSERT_EQ(evidence.get("sources")->asArray().size(), 64u);
  const auto& source = evidence.get("sources")->asArray().front();
  EXPECT_EQ(source.getString("state"), "held"); EXPECT_EQ(source.getNumber("sourceEpoch"), 3);
  EXPECT_EQ(source.getNumber("requestedEpoch"), 4); EXPECT_EQ(source.getNumber("frameId"), 99);
}

TEST(MonitorFrameAdmission, RejectsProductionAliasAndWrongSizedPrivateCopyPerSource) {
  for (bool alias : {false, true}) {
    MonitorRenderRequest request; request.previewActive = true;
    VideoFrame frame; frame.participantId = "source";
    auto production = std::make_shared<GpuVideoFrame>(); production->width = production->height = 64;
    auto optional = std::make_shared<GpuVideoFrame>(); optional->width = optional->height = 32;
    optional->monitorPrivate = true;
    frame.gpuPixels = production; frame.monitorGpuPixels = alias ? production : optional;
    request.frames.push_back(frame);
    EXPECT_TRUE(prepareMonitorFrames(request));
    EXPECT_FALSE(request.frames.front().hasGpuPixels());
    EXPECT_EQ(request.unavailableInputs.size(), 1u);
  }
}

TEST(MonitorInputCache, HoldsOriginalIdentityAcrossEpochChangeAndRetiresWithoutDemand) {
  MonitorInputCache cache;
  MonitorRenderRequest request; request.previewActive = true;
  VideoFrame frame; frame.participantId = "source"; frame.sourceEpoch = 7; frame.frameId = 11;
  frame.captureTimestamp100ns = 123; frame.width = frame.height = frame.pixelWidth = frame.pixelHeight = 2;
  frame.pixelStride = 8; frame.pixels = std::make_shared<std::vector<uint8_t>>(16, 255);
  request.frames = {frame}; prepareMonitorFrames(request);
  MonitorRenderResult first; cache.prepare(request, first);
  EXPECT_EQ(first.retainedInputBytes, 16u);
  auto payload = request.frames.front().pixels;
  request.frames.front().pixels.reset(); request.frames.front().sourceEpoch = 8;
  auto production = std::make_shared<GpuVideoFrame>(); production->width = production->height = 2;
  request.frames.front().gpuPixels = production; prepareMonitorFrames(request);
  MonitorRenderResult held; cache.prepare(request, held);
  ASSERT_EQ(held.inputs.size(), 1u);
  EXPECT_EQ(held.heldInputs, 1u); EXPECT_EQ(held.inputs.front().requestedEpoch, 8u);
  EXPECT_EQ(request.frames.front().sourceEpoch, 7u);
  EXPECT_EQ(request.frames.front().frameId, 11); EXPECT_EQ(request.frames.front().captureTimestamp100ns, 123);
  EXPECT_EQ(request.frames.front().pixels, payload);
  MonitorRenderRequest empty; MonitorRenderResult retired; cache.prepare(empty, retired);
  EXPECT_EQ(retired.retainedInputs, 0u); EXPECT_EQ(retired.retainedInputBytes, 0u);
}

TEST(MonitorInputCache, BoundedRetentionNeverRefusesReadyInputs) {
  MonitorInputCache cache; MonitorRenderRequest request; request.previewActive = true;
  for (int i = 0; i < 65; ++i) {
    VideoFrame frame; frame.participantId = std::to_string(i);
    auto image = std::make_shared<GpuVideoFrame>(); image->width = image->height = 64; image->monitorPrivate = true;
    frame.monitorGpuPixels = image; request.frames.push_back(frame);
  }
  prepareMonitorFrames(request); MonitorRenderResult result; cache.prepare(request, result);
  EXPECT_EQ(result.readyInputs, 65u); EXPECT_EQ(result.retainedInputs, 64u);
  EXPECT_EQ(result.retentionRefusals, 1u);
  MonitorRenderRequest large; large.previewActive = true;
  VideoFrame frame; frame.participantId = "large";
  auto image = std::make_shared<GpuVideoFrame>(); image->width = image->height = 16384; image->monitorPrivate = true;
  frame.monitorGpuPixels = image; large.frames.push_back(frame);
  prepareMonitorFrames(large); MonitorRenderResult refused; cache.prepare(large, refused);
  EXPECT_EQ(refused.readyInputs, 1u); EXPECT_EQ(refused.retentionRefusals, 2u);
  EXPECT_LE(refused.retainedInputBytes, MonitorInputCache::ByteBudget);
}

TEST(MonitorInputCache, FirstMissingInputIsUnavailableAndAbsentDemandedInputKeepsItsIdentity) {
  MonitorInputCache cache; MonitorRenderRequest request; request.previewActive = true;
  VideoFrame frame; frame.participantId = "source";
  auto production = std::make_shared<GpuVideoFrame>(); production->width = production->height = 2;
  frame.gpuPixels = production; request.frames = {frame}; prepareMonitorFrames(request);
  MonitorRenderResult first; cache.prepare(request, first);
  EXPECT_EQ(first.unavailableInputs, 1u); EXPECT_EQ(first.heldInputs, 0u);
  EXPECT_EQ(first.retainedInputs, 0u); EXPECT_FALSE(request.frames.front().hasContent());
  auto optional = std::make_shared<GpuVideoFrame>(); optional->width = optional->height = 2; optional->monitorPrivate = true;
  frame.monitorGpuPixels = optional; frame.sourceEpoch = 3; frame.frameId = 99;
  request.frames = {frame}; prepareMonitorFrames(request); MonitorRenderResult ready; cache.prepare(request, ready);
  request.frames.clear(); request.unavailableInputs.clear(); MonitorRenderResult absent; cache.prepare(request, absent);
  ASSERT_EQ(request.frames.size(), 1u); EXPECT_EQ(absent.heldInputs, 1u);
  EXPECT_EQ(request.frames.front().sourceEpoch, 3u); EXPECT_EQ(request.frames.front().frameId, 99);
  EXPECT_EQ(absent.inputs.front().requestedEpoch, 0u);
}

TEST(MonitorInputCache, DemandRetirementDestroysPrivateImageOnWorkerOwner) {
  struct Image : GpuVideoFrame {
    std::promise<std::thread::id>* destroyed;
    ~Image() override { destroyed->set_value(std::this_thread::get_id()); }
  };
  std::promise<std::thread::id> destroyed; auto completion = destroyed.get_future();
  MonitorRenderWorker worker([](const MonitorRenderRequest&) { return MonitorRenderResult{}; });
  MonitorRenderRequest request; request.previewActive = true; request.sequence = 1;
  auto image = std::make_shared<Image>(); image->width = image->height = 2; image->monitorPrivate = true; image->destroyed = &destroyed;
  VideoFrame frame; frame.participantId = "source"; frame.monitorGpuPixels = std::move(image);
  request.frames.push_back(std::move(frame)); prepareMonitorFrames(request); worker.submit(std::move(request));
  const auto readyBy = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (worker.diagnostics().completed == 0 && std::chrono::steady_clock::now() < readyBy)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  ASSERT_EQ(worker.diagnostics().retainedInputs, 1u);
  MonitorRenderRequest empty; empty.sequence = 2; worker.submit(std::move(empty));
  ASSERT_TRUE(completion.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  EXPECT_NE(completion.get(), std::this_thread::get_id());
}

TEST(MonitorRenderWorker, InitializationReportsStartingBeforeDeviceIsReady) {
  std::promise<void> entered, release;
  auto gate = release.get_future().share();
  MonitorRenderWorker worker([](const MonitorRenderRequest&) { return MonitorRenderResult{}; }, [&] {
    entered.set_value(); gate.wait();
  });
  auto started = entered.get_future().wait_for(std::chrono::seconds(2));
  auto evidence = worker.diagnostics();
  release.set_value(); // release before assertions so failure cannot hang teardown
  ASSERT_TRUE(started == std::future_status::ready);
  EXPECT_TRUE(evidence.enabled);
  EXPECT_EQ(evidence.readiness, "starting");
  EXPECT_EQ(evidence.completed, 0u);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (worker.diagnostics().readiness == "starting" && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  EXPECT_EQ(worker.diagnostics().readiness, "ready");
  EXPECT_EQ(worker.diagnostics().completed, 0u);
}

TEST(MonitorRenderWorker, InitializationFailureRemainsIsolatedAndSuccessfulWorkRecovers) {
  MonitorRenderWorker worker([](const MonitorRenderRequest& request) {
    if (request.sequence == 1) throw std::runtime_error("render failure");
    return MonitorRenderResult{};
  }, [] { throw std::runtime_error("init failure with private details"); });
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (worker.diagnostics().failed == 0 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  auto failed = worker.diagnostics();
  EXPECT_EQ(failed.failed, 1u);
  EXPECT_EQ(failed.effectiveMode, "isolated");
  EXPECT_EQ(failed.readiness, "degraded");
  EXPECT_EQ(failed.failureReason, "monitor-initialization");
  MonitorRenderRequest request;
  request.sequence = 1;
  worker.submit(request);
  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (worker.diagnostics().failed < 2 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  EXPECT_EQ(worker.diagnostics().failureReason, "monitor-render");
  EXPECT_TRUE(worker.latest() == nullptr);
  request.sequence = 2;
  worker.submit(request);
  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (worker.diagnostics().completed == 0 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  auto recovered = worker.diagnostics();
  EXPECT_EQ(recovered.readiness, "ready");
  EXPECT_TRUE(recovered.failureReason.empty());
  EXPECT_EQ(recovered.failed, 2u);
  EXPECT_EQ(recovered.lastSequence, 2);
  worker.refuse();
  EXPECT_EQ(worker.diagnostics().readiness, "degraded");
  EXPECT_EQ(worker.diagnostics().failureReason, "monitor-frame-admission");
}

TEST(MonitorFrameAdmission, UnusedGpuSourceCannotRefuseAnOtherwiseReadyMonitor) {
  MonitorRenderRequest request;
  request.previewActive = true;
  CompositorRenderPlanLayer layer;
  layer.participantId = "selected";
  request.previewPlan.layers.push_back(layer);
  auto gpu = std::make_shared<GpuVideoFrame>();
  gpu->width = gpu->height = 64;
  VideoFrame selected, unused;
  selected.participantId = "selected";
  selected.gpuPixels = gpu;
  auto optional = std::make_shared<GpuVideoFrame>();
  optional->width = optional->height = 64; optional->monitorPrivate = true;
  selected.monitorGpuPixels = optional;
  unused.participantId = "unused";
  unused.gpuPixels = gpu;
  request.frames = {selected, unused};
  EXPECT_TRUE(prepareMonitorFrames(request));
  ASSERT_EQ(request.frames.size(), 1u);
  EXPECT_EQ(request.frames.front().participantId, "selected");
}

TEST(MonitorFrameAdmission, MissingPrivateCopyReleasesProductionLeaseWithoutRefusingJob) {
  MonitorRenderRequest request;
  request.previewActive = true;
  auto gpu = std::make_shared<GpuVideoFrame>();
  gpu->width = gpu->height = 64;
  std::weak_ptr<const GpuVideoFrame> production = gpu;
  VideoFrame frame;
  frame.participantId = "selected";
  frame.gpuPixels = std::move(gpu);
  request.frames.push_back(std::move(frame));
  EXPECT_TRUE(prepareMonitorFrames(request));
  EXPECT_TRUE(production.expired());
  ASSERT_EQ(request.unavailableInputs.size(), 1u);
  EXPECT_EQ(request.unavailableInputs.front(), "selected");
}

TEST(MonitorRenderWorker, ReplacesPendingWorkWithoutWaitingForTheActiveMonitor) {
  std::promise<void> entered, release;
  auto released = release.get_future().share();
  std::atomic<int> calls{0};
  MonitorRenderWorker worker([&](const MonitorRenderRequest& request) {
    if (++calls == 1) { entered.set_value(); released.wait(); }
    MonitorRenderResult result;
    result.tiles = request.tiles;
    result.preview.width = request.previewPlan.width;
    return result;
  });
  MonitorRenderRequest first;
  first.sequence = 1;
  worker.submit(std::move(first));
  const auto started = entered.get_future().wait_for(std::chrono::seconds(2));
  if (started != std::future_status::ready) { release.set_value(); }
  ASSERT_TRUE(started == std::future_status::ready);
  for (int i = 2; i <= 30; ++i) {
    MonitorRenderRequest next;
    next.sequence = i;
    next.previewPlan.width = 100 + i;
    worker.submit(std::move(next));
  }
  EXPECT_EQ(calls.load(), 1);
  EXPECT_TRUE(worker.latest() == nullptr);
  release.set_value();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while ((!worker.latest() || worker.latest()->sequence != 30) && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  ASSERT_TRUE(worker.latest() != nullptr);
  EXPECT_EQ(worker.latest()->sequence, 30);
  EXPECT_EQ(worker.latest()->preview.width, 130);
  EXPECT_EQ(calls.load(), 2);
}

TEST(MonitorRenderWorker, FailedWorkDoesNotPublishSuccessfulMetadataAndLaterWorkRecovers) {
  std::promise<void> failed;
  MonitorRenderWorker worker([&](const MonitorRenderRequest& request) {
    if (request.sequence == 1) { failed.set_value(); throw std::runtime_error("injected"); }
    MonitorRenderResult result;
    result.preview.height = 720;
    return result;
  });
  MonitorRenderRequest request;
  request.sequence = 1;
  worker.submit(request);
  ASSERT_TRUE(failed.get_future().wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  EXPECT_TRUE(worker.latest() == nullptr);
  request.sequence = 2;
  worker.submit(request);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!worker.latest() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  ASSERT_TRUE(worker.latest() != nullptr);
  EXPECT_EQ(worker.latest()->sequence, 2);
  EXPECT_EQ(worker.latest()->preview.height, 720);
}
