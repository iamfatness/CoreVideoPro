#include "modules/MonitorRenderWorker.h"
#include "modules/MonitorFrameAdmission.h"
#include <gtest/gtest.h>
#include <future>
#include <atomic>

using namespace corevideo::modules;

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
  selected.monitorGpuPixels = gpu;
  unused.participantId = "unused";
  unused.gpuPixels = gpu;
  request.frames = {selected, unused};
  EXPECT_TRUE(prepareMonitorFrames(request));
  ASSERT_EQ(request.frames.size(), 1u);
  EXPECT_EQ(request.frames.front().participantId, "selected");
}

TEST(MonitorFrameAdmission, MissingPrivateCopyReleasesProductionLeaseAndRefusesJob) {
  MonitorRenderRequest request;
  request.previewActive = true;
  auto gpu = std::make_shared<GpuVideoFrame>();
  gpu->width = gpu->height = 64;
  std::weak_ptr<const GpuVideoFrame> production = gpu;
  VideoFrame frame;
  frame.participantId = "selected";
  frame.gpuPixels = std::move(gpu);
  request.frames.push_back(std::move(frame));
  EXPECT_FALSE(prepareMonitorFrames(request));
  EXPECT_TRUE(production.expired());
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
