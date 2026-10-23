#include "modules/CaptureFrameWorker.h"
#include "modules/DeferredCaptureGpuCopy.h"
#include "modules/SourceVideoDemand.h"
#include <gtest/gtest.h>
#include <future>
#include <stdexcept>
#include <vector>

TEST(DeferredCaptureGpuCopy, LateCompletionPublishesOriginalPixelsAndIdentityWithoutAnotherArrival) {
  using Deferred = corevideo::modules::DeferredCaptureGpuCopy;
  Deferred deferred;
  EXPECT_TRUE(deferred.defer({13473, 800000, 7, 1920, 1080, 2, 1}));
  auto completed = std::shared_ptr<const int>{};
  int published = 0;
  auto ready = [&](int slot) { EXPECT_EQ(slot, 2); return completed; };
  auto publish = [&](const Deferred::Copy& copy, auto image) {
    EXPECT_EQ(copy.sequence, 13473); EXPECT_EQ(copy.epoch, 7u);
    EXPECT_EQ(copy.capture100ns, 800000); EXPECT_EQ(copy.monitorSlot, 1);
    EXPECT_EQ(*image, 213); ++published;
  };
  // The initial bounded wait has expired. Unready work must remain unpublished
  // and a later arrival cannot overwrite the outstanding slot/identity.
  EXPECT_FALSE(deferred.poll(ready, publish));
  EXPECT_FALSE(deferred.defer({13474, 900000, 8, 2560, 1440, 0, 0}));
  EXPECT_EQ(published, 0); EXPECT_TRUE(deferred.pending());
  completed = std::make_shared<const int>(213);
  EXPECT_TRUE(deferred.poll(ready, publish)); // idle owner, no new OS frame
  EXPECT_FALSE(deferred.pending());
  EXPECT_FALSE(deferred.poll(ready, publish));
  EXPECT_EQ(published, 1);
}

TEST(DeferredCaptureGpuCopy, CancellationCannotPublishRetiredSessionIntoReplacement) {
  using Deferred = corevideo::modules::DeferredCaptureGpuCopy;
  Deferred deferred; int published = 0;
  EXPECT_TRUE(deferred.defer({1, 100, 1, 1920, 1080, 0, -1}));
  deferred.cancel();
  auto ready = [](int) { return std::make_shared<const int>(77); };
  EXPECT_FALSE(deferred.poll(ready, [&](const auto&, auto) { ++published; }));
  EXPECT_TRUE(deferred.defer({2, 200, 2, 2560, 1440, 1, -1}));
  EXPECT_TRUE(deferred.poll(ready, [&](const auto& copy, auto image) {
    EXPECT_EQ(copy.epoch, 2u); EXPECT_EQ(copy.width, 2560);
    EXPECT_EQ(copy.sequence, 2); EXPECT_EQ(*image, 77); ++published;
  }));
  EXPECT_EQ(published, 1);
}

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
