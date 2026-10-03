#include "modules/MonitorRenderWorker.h"
#include <gtest/gtest.h>
#include <future>
#include <atomic>

using namespace corevideo::modules;

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
