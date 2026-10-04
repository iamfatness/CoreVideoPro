#include "modules/CaptureFrameWorker.h"
#include "modules/SourceVideoDemand.h"
#include <gtest/gtest.h>
#include <future>
#include <stdexcept>
#include <vector>

TEST(CaptureFrameWorker, KeepsLatestPendingWithoutBlockingCapture) {
  using Worker = corevideo::modules::CaptureFrameWorker<int>;
  std::promise<void> active, release, finished;
  auto gate = release.get_future().share();
  auto started = active.get_future();
  auto done = finished.get_future();
  std::vector<int> received;
  Worker worker([&](const int& frame) {
    received.push_back(frame);
    if (frame == 1) { active.set_value(); gate.wait(); }
    if (frame == 3) finished.set_value();
  });
  worker.submit(1);
  const bool running = started.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
  worker.submit(2); worker.submit(3);
  const auto queued = worker.stats();
  release.set_value(); // release before any assertion can return
  const bool completed = done.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
  worker.stop();
  ASSERT_TRUE(running); ASSERT_TRUE(completed);
  EXPECT_EQ(queued.superseded, 1u);
  EXPECT_EQ(received, (std::vector<int>{1, 3}));
}

TEST(CaptureFrameWorker, RecordingDemandPreservesAdmittedOrderAndCountsRefusal) {
  using Worker = corevideo::modules::CaptureFrameWorker<int>;
  std::promise<void> active, release, finished;
  auto gate = release.get_future().share();
  auto started = active.get_future(); auto done = finished.get_future();
  std::vector<int> received;
  Worker worker([&](const int& frame) {
    received.push_back(frame);
    if (frame == 1) { active.set_value(); gate.wait(); }
    if (frame == 2) finished.set_value();
  });
  worker.submit(1, true);
  const bool running = started.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
  worker.submit(2, true); worker.submit(3, true);
  const auto queued = worker.stats();
  release.set_value();
  const bool completed = done.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
  worker.stop();
  ASSERT_TRUE(running); ASSERT_TRUE(completed);
  EXPECT_EQ(queued.superseded, 0u); EXPECT_EQ(queued.refused, 1u);
  EXPECT_EQ(received, (std::vector<int>{1, 2}));
}

TEST(SourceVideoDemand, CpuRepresentationBelongsToItsConsumerAndSource) {
  using namespace corevideo::modules;
  std::vector<SourceVideoDemand> demands{
    {"capture:a", SourceVideoConsumer::Program, "program", SourceVideoRepresentation::Gpu},
    {"capture:b", SourceVideoConsumer::Iso, "recording", SourceVideoRepresentation::Cpu},
    {"capture:b", SourceVideoConsumer::SceneEditor, "editor", SourceVideoRepresentation::Cpu}};
  EXPECT_FALSE(sourceNeedsCpuVideo(demands, "capture:a"));
  EXPECT_TRUE(sourceNeedsCpuVideo(demands, "capture:b"));
  demands.erase(demands.begin() + 1);
  EXPECT_TRUE(sourceNeedsCpuVideo(demands, "capture:b"));
  demands.pop_back();
  EXPECT_FALSE(sourceNeedsCpuVideo(demands, "capture:b"));
}
