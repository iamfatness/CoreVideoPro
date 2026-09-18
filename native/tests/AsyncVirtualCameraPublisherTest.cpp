#include "modules/AsyncVirtualCameraPublisher.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <future>
using namespace corevideo::modules;
using namespace std::chrono_literals;
namespace {
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

