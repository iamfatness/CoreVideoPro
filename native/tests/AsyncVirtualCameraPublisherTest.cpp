#include "modules/AsyncVirtualCameraPublisher.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <future>
using namespace corevideo::modules;
using namespace std::chrono_literals;
namespace {
#if !COREVIDEO_WITH_VIRTUALCAM
TEST(AsyncVirtualCameraPublisher, DisabledCameraFactoryReportsFramerIntentAsUnavailable) {
  auto camera=createVirtualCameraPublisher();
  EXPECT_FALSE(camera->status().framerEnabled);
  camera->setFramerEnabled(true);
  EXPECT_TRUE(camera->status().framerEnabled);
  EXPECT_EQ(camera->status().framerState,"unavailable");
  EXPECT_FALSE(camera->status().framerWarning.empty());
  camera->setFramerEnabled(false);
  EXPECT_FALSE(camera->status().framerEnabled);
  EXPECT_EQ(camera->status().framerState,"off");
  EXPECT_TRUE(camera->status().framerWarning.empty());
}
#endif
struct SlowCamera : IVirtualCameraPublisher {
  std::promise<void> entered, release;
  std::shared_future<void> gate = release.get_future().share();
  std::atomic<int> stopped{0};
  VirtualCameraStatus current;
  bool start(int, int, int) override { entered.set_value(); gate.wait(); current.enabled=true; current.state="live"; return true; }
  void stop() override { ++stopped; current.enabled=false; current.state="off"; }
  void publish(const ProgramFrame&) override {}
  VirtualCameraStatus status() const override { return current; }
};
TEST(AsyncVirtualCameraPublisher, SlowStartDoesNotBlockCommandsAndLateCompletionCannotUndoStop) {
  auto inner = std::make_unique<SlowCamera>(); auto* backend = inner.get();
  auto entered = backend->entered.get_future();
  AsyncVirtualCameraPublisher camera(std::move(inner));
  auto command = std::async(std::launch::async, [&] { return camera.start(1920,1080,60); });
  const bool enteredStart = entered.wait_for(2s) == std::future_status::ready;
  const bool commandReturned = command.wait_for(2s) == std::future_status::ready;
  auto stop = std::async(std::launch::async, [&] { camera.stop(); return camera.status(); });
  const bool stopReturned = stop.wait_for(2s) == std::future_status::ready;
  VirtualCameraStatus pending;
  if (stopReturned) pending = stop.get();
  backend->release.set_value(); // release even on regression so cleanup finishes
  EXPECT_TRUE(enteredStart); EXPECT_TRUE(commandReturned); EXPECT_TRUE(stopReturned);
  EXPECT_FALSE(pending.enabled); EXPECT_EQ(pending.state, "stopping");
  const auto deadline = std::chrono::steady_clock::now()+2s;
  while (camera.status().state != "off" && std::chrono::steady_clock::now()<deadline) std::this_thread::sleep_for(1ms);
  EXPECT_EQ(camera.status().state, "off"); EXPECT_FALSE(camera.status().enabled);
  EXPECT_TRUE(backend->stopped.load() >= 2);
}
struct FailedCamera : IVirtualCameraPublisher {
  bool start(int,int,int) override { return false; }
  void stop() override {}
  void publish(const ProgramFrame&) override {}
  VirtualCameraStatus status() const override { VirtualCameraStatus s; s.state="failed"; s.warning="OS rejected camera"; return s; }
};
struct CaptureCamera : IVirtualCameraPublisher {
  std::mutex captureMutex;
  std::condition_variable changed;
  std::shared_ptr<const std::vector<uint8_t>> last;
  int64_t sequence=0, deliveredAt=0;
  int lastWidth=0, lastHeight=0;
  VirtualCameraStatus current;
  bool start(int,int,int) override { current.enabled=true; current.state="live"; return true; }
  void stop() override { current.enabled=false; current.state="off"; }
  void publish(const ProgramFrame&) override {}
  void publishNv12IdentifiedOnWorker(std::shared_ptr<const std::vector<uint8_t>> bytes,
      int w,int h,int64_t seq,int64_t at) override {
    std::lock_guard<std::mutex> lock(captureMutex);
    last=std::move(bytes); sequence=seq; deliveredAt=at; lastWidth=w; lastHeight=h;
    ++current.framesPublished; changed.notify_all();
  }
  VirtualCameraStatus status() const override { return current; }
  bool await(int64_t expected) {
    std::unique_lock<std::mutex> lock(captureMutex);
    return changed.wait_for(lock,2s,[&]{ return sequence==expected; });
  }
};
TEST(AsyncVirtualCameraPublisher, FramerForkPreservesSourceAttributionAndOffIdentity) {
  auto inner=std::make_unique<CaptureCamera>(); auto* backend=inner.get();
  AsyncVirtualCameraPublisher camera(std::move(inner));
  camera.setFramerEnabled(true); camera.start(1920,1080,60);
  auto clean=std::make_shared<const std::vector<uint8_t>>(1920*1080*3/2,100);
  camera.publishNv12Identified(clean,1920,1080,401,9001);
  ASSERT_TRUE(backend->await(401));
  const auto readyDeadline=std::chrono::steady_clock::now()+2s;
  while (camera.status().framerState != "active" && camera.status().framerState != "unavailable"
         && std::chrono::steady_clock::now()<readyDeadline) {
    std::this_thread::sleep_for(2ms);
    camera.publishNv12Identified(clean,1920,1080,401,9001);
  }
  EXPECT_NE(camera.status().framerState,"waiting");
  std::shared_ptr<const std::vector<uint8_t>> decorated;
  { std::lock_guard<std::mutex> lock(backend->captureMutex);
    decorated=backend->last;
    EXPECT_EQ(backend->deliveredAt,9001);
  }
#if defined(_WIN32) && COREVIDEO_WITH_D3D11
  EXPECT_NE(decorated.get(),clean.get());
  EXPECT_EQ(decorated->at(950*1920+960),58);
#else
  EXPECT_EQ(decorated.get(),clean.get());
#endif
  EXPECT_EQ(clean->at(950*1920+960),100);
  camera.setFramerEnabled(false);
  camera.publishNv12Identified(clean,1920,1080,402,9002);
  ASSERT_TRUE(backend->await(402));
  { std::lock_guard<std::mutex> lock(backend->captureMutex);
    EXPECT_EQ(backend->last.get(),clean.get());
    EXPECT_EQ(backend->deliveredAt,9002);
  }
  EXPECT_FALSE(camera.status().framerEnabled);
  EXPECT_EQ(camera.status().framerState,"off");
  // A retained decorated image must remain immutable after another publication.
#if defined(_WIN32) && COREVIDEO_WITH_D3D11
  constexpr int expectedTint=58;
#else
  constexpr int expectedTint=100;
#endif
  EXPECT_EQ(decorated->at(950*1920+960),expectedTint);
}
TEST(AsyncVirtualCameraPublisher, FramerFallbackKeepsFixedCameraSizeAndIdentity) {
  auto inner=std::make_unique<CaptureCamera>(); auto* backend=inner.get();
  AsyncVirtualCameraPublisher camera(std::move(inner));
  camera.setFramerEnabled(true); camera.start(1920,1080,60);
  auto primer=std::make_shared<const std::vector<uint8_t>>(1920*1080*3/2,100);
  const auto readyDeadline=std::chrono::steady_clock::now()+2s;
  do {
    camera.publishNv12Identified(primer,1920,1080,400,9000);
    std::this_thread::sleep_for(2ms);
  } while (camera.status().framerState != "active" && camera.status().framerState != "unavailable"
           && std::chrono::steady_clock::now()<readyDeadline);
  EXPECT_NE(camera.status().framerState,"waiting");
  ProgramFrame frame;
  frame.frameNumber=403; frame.deliveredAt100ns=9003;
  frame.preview.width=8; frame.preview.height=8; frame.preview.bgra.assign(8*8*4,100);
  camera.publish(frame);
  ASSERT_TRUE(backend->await(403));
  { std::lock_guard<std::mutex> lock(backend->captureMutex);
    EXPECT_EQ(backend->lastWidth,1920); EXPECT_EQ(backend->lastHeight,1080);
    EXPECT_EQ(backend->last->size(),1920u*1080*3/2);
    EXPECT_EQ(backend->deliveredAt,9003);
  }
}
struct QueuedCamera : IVirtualCameraPublisher {
  std::promise<void> entered, release;
  std::shared_future<void> gate = release.get_future().share();
  std::atomic<int> sharedCalls{0}, workerCalls{0}, lastPixel{0};
  std::atomic<int64_t> lastProgramSequence{0};
  VirtualCameraStatus current;
  bool start(int,int,int) override { current.enabled = true; current.state = "live"; return true; }
  void stop() override { current.enabled = false; }
  void publish(const ProgramFrame&) override {}
  void publishNv12Shared(std::shared_ptr<const std::vector<uint8_t>>, int, int) override { ++sharedCalls; }
  void publishNv12OnWorker(std::shared_ptr<const std::vector<uint8_t>> bytes, int, int) override {
    if (++workerCalls == 1) { entered.set_value(); gate.wait(); }
    lastPixel.store(bytes->front());
    ++current.framesPublished;
  }
  void publishNv12IdentifiedOnWorker(std::shared_ptr<const std::vector<uint8_t>> bytes,
      int w, int h, int64_t sequence, int64_t) override {
    publishNv12OnWorker(std::move(bytes), w, h);
    lastProgramSequence.store(sequence);
  }
  VirtualCameraStatus status() const override { return current; }
};
TEST(AsyncVirtualCameraPublisher, OneLatestSlotCountsReplacementAndUsesDirectWorkerPublication) {
  auto inner = std::make_unique<QueuedCamera>();
  auto* backend = inner.get();
  auto entered = backend->entered.get_future();
  AsyncVirtualCameraPublisher camera(std::move(inner));
  camera.start(2, 2, 60);
  camera.publishNv12Identified(std::make_shared<const std::vector<uint8_t>>(6, 1), 2, 2, 101, 1000);
  const bool blocked = entered.wait_for(2s) == std::future_status::ready;
  if (!blocked) backend->release.set_value();
  ASSERT_TRUE(blocked);
  for (int i = 2; i <= 30; ++i)
    camera.publishNv12Identified(std::make_shared<const std::vector<uint8_t>>(6, static_cast<uint8_t>(i)), 2, 2, 100 + i, 1000 + i);
  const auto pending = camera.status();
  EXPECT_EQ(pending.framesAccepted, 30u);
  EXPECT_EQ(pending.pendingFramesReplaced, 28u);
  backend->release.set_value();
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (camera.status().framesPublished != 2 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  EXPECT_EQ(camera.status().framesPublished, 2u);
  EXPECT_EQ(backend->lastPixel.load(), 30);
  EXPECT_EQ(backend->lastProgramSequence.load(), 130);
  EXPECT_EQ(backend->sharedCalls.load(), 0);
  EXPECT_EQ(camera.status().publicationExceptions, 0u);
}
TEST(AsyncVirtualCameraPublisher, PublishesBackendStartFailure) {
  AsyncVirtualCameraPublisher camera(std::make_unique<FailedCamera>());
  EXPECT_TRUE(camera.start(1920,1080,60));
  const auto deadline = std::chrono::steady_clock::now()+2s;
  while (camera.status().state == "starting" && std::chrono::steady_clock::now()<deadline) std::this_thread::sleep_for(1ms);
  EXPECT_EQ(camera.status().state,"failed");
  EXPECT_EQ(camera.status().warning,"OS rejected camera");
  EXPECT_FALSE(camera.status().enabled);
}
}

