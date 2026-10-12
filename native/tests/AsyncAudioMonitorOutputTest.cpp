#include "modules/AsyncAudioMonitorOutput.h"
#include "core/MediaCore.h"
#include <gtest/gtest.h>
#include <atomic>
#include <future>
#include <latch>
#include <mutex>

namespace {
struct DeviceGate {
  std::atomic<bool> blockStart{false}, blockRender{false};
  std::latch entered{1}, release{1};
  std::mutex mutex;
  std::vector<float> accepted;
};
class GatedMonitor final : public corevideo::modules::IAudioMonitorOutput {
 public:
  explicit GatedMonitor(std::shared_ptr<DeviceGate> gate) : gate_(std::move(gate)) {}
  bool start(const std::string& id, int, int) override {
    if (gate_->blockStart.exchange(false)) { gate_->entered.count_down(); gate_->release.wait(); }
    id_ = id; active_ = true; return true;
  }
  void stop() override { active_ = false; }
  bool render(const float* pcm, int frames, int, double) override {
    if (gate_->blockRender.exchange(false)) { gate_->entered.count_down(); gate_->release.wait(); }
    if (!active_) return false;
    std::lock_guard<std::mutex> lock(gate_->mutex);
    gate_->accepted.push_back(pcm[0]); return frames > 0;
  }
  bool active() const override { return active_; }
  std::string deviceName() const override { return id_; }
  std::string resolvedEndpointId() const override { return id_; }
  std::vector<std::string> warnings() const override { return {}; }
 private:
  std::shared_ptr<DeviceGate> gate_;
  std::string id_;
  bool active_ = false;
};
template<typename Predicate> bool waitUntil(Predicate ready) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!ready() && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  return ready();
}
}

TEST(AsyncAudioMonitorOutput, ActivationDoesNotBlockControlAndLateOldDeviceCannotBecomeReady) {
  auto gate = std::make_shared<DeviceGate>(); gate->blockStart = true;
  corevideo::modules::AsyncAudioMonitorOutput monitor(std::make_unique<GatedMonitor>(gate));
  EXPECT_FALSE(monitor.start("old", 48000, 2)); gate->entered.wait();
  auto control = std::async(std::launch::async, [&] {
    monitor.stop(); monitor.start("new", 48000, 2);
    return monitor.preparing() && !monitor.active() && monitor.resolvedEndpointId().empty();
  });
  const auto ready = control.wait_for(std::chrono::seconds(2));
  gate->release.count_down();
  ASSERT_EQ(ready, std::future_status::ready);
  EXPECT_TRUE(control.get());
  ASSERT_TRUE(waitUntil([&] { return monitor.active(); }));
  EXPECT_EQ(monitor.resolvedEndpointId(), "new");
}
TEST(AsyncAudioMonitorOutput, BoundedPressurePreservesSurvivingPcmOrderAndCountsActualAcceptance) {
  auto gate = std::make_shared<DeviceGate>();
  corevideo::modules::AsyncAudioMonitorOutput monitor(std::make_unique<GatedMonitor>(gate));
  monitor.start("device", 48000, 2); ASSERT_TRUE(waitUntil([&] { return monitor.active(); }));
  gate->blockRender = true;
  float first[2]{.1f,.1f}; const bool firstAdmitted = monitor.render(first, 1, 2, 1); gate->entered.wait();
  const auto before = monitor.acceptedFrameCount();
  bool admitted = firstAdmitted;
  for (int i=2;i<=6;++i) { float pcm[2]{i*.1f,i*.1f}; admitted = monitor.render(pcm, 1, 2, 1) && admitted; }
  const bool deliveryObserved = monitor.deviceDeliveryObserved();
  const auto dropped = monitor.droppedFrameCount();
  gate->release.count_down();
  EXPECT_TRUE(admitted); EXPECT_EQ(before, 0); EXPECT_FALSE(deliveryObserved);
  EXPECT_EQ(dropped, 1);
  ASSERT_TRUE(waitUntil([&] { return monitor.acceptedFrameCount() == 5; }));
  std::lock_guard<std::mutex> lock(gate->mutex);
  ASSERT_EQ(gate->accepted.size(), 5u);
  EXPECT_EQ(gate->accepted[0], .1f); EXPECT_EQ(gate->accepted[1], 3*.1f);
  EXPECT_EQ(gate->accepted[4], 6*.1f);
}
TEST(AsyncAudioMonitorOutput, StalledDeviceActivationCannotHoldProgramOrAudioOwner) {
  auto gate = std::make_shared<DeviceGate>(); gate->blockStart = true;
  auto modules = corevideo::modules::createStubModules();
  modules.monitorOutput = std::make_unique<GatedMonitor>(gate);
  corevideo::core::MediaCore core(std::move(modules)); core.enableAudioOutputWorker();
  std::mutex coreMutex;
  auto command = std::async(std::launch::async, [&] {
    std::lock_guard<std::mutex> lock(coreMutex);
    return core.applyCommand(corevideo::rpc::Json::Object{{"type","sync-audio-monitor"},{"enabled",true},{"deviceId","device"},{"volume",1}});
  });
  gate->entered.wait();
  auto work = std::async(std::launch::async, [&] {
    core.renderAudioOutputTick(coreMutex);
    std::lock_guard<std::mutex> lock(coreMutex);
    core.renderDisplayTick(); return core.sessionState();
  });
  const auto ready = work.wait_for(std::chrono::seconds(2));
  gate->release.count_down(); command.get(); ASSERT_EQ(ready, std::future_status::ready);
  const auto snapshot = work.get(); EXPECT_GT(snapshot.getNumber("programFrameCount"), 0);
  EXPECT_EQ(snapshot.get("audioMixSession")->getString("monitorStatus"), "preparing");
}
