#include "modules/AsyncAudioMonitorOutput.h"
#include "core/MediaCore.h"
#include <gtest/gtest.h>
#include <atomic>
#include <future>
#include <latch>
#include <mutex>
#include <stdexcept>

namespace {
struct DeviceGate {
  std::atomic<bool> blockStart{false}, blockRender{false}, blockStop{false}, failStart{false};
  std::latch entered{1}, release{1};
  std::mutex mutex;
  std::vector<float> accepted;
  std::vector<std::string> started;
  std::atomic<bool> released{false};
  std::atomic<int64_t> rawUnderruns{0};
  std::atomic<bool> failEvidence{false}, classifyEvidence{false}, intentionalSilence{false};
  std::atomic<int> evidenceReads{0};
  corevideo::modules::MonitorDeliveryRecorder delivery;
  void unblock() { if (!released.exchange(true)) release.count_down(); }
};
struct ReleaseGateOnExit {
  DeviceGate& gate;
  ~ReleaseGateOnExit() { gate.unblock(); }
};
class GatedMonitor final : public corevideo::modules::IAudioMonitorOutput {
 public:
  explicit GatedMonitor(std::shared_ptr<DeviceGate> gate) : gate_(std::move(gate)) {}
  bool start(const std::string& id, int, int) override {
    if (gate_->blockStart.exchange(false)) { gate_->entered.count_down(); gate_->release.wait(); }
    { std::lock_guard<std::mutex> lock(gate_->mutex); gate_->started.push_back(id); }
    if (gate_->failStart.exchange(false)) return false;
    id_ = id; active_ = true; gate_->delivery.beginEndpoint(); return true;
  }
  void stop() override {
    if (gate_->blockStop.exchange(false)) { gate_->entered.count_down(); gate_->release.wait(); }
    active_ = false; gate_->delivery.endEndpoint();
  }
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
  int64_t underrunCount() const override { return gate_->rawUnderruns.load(); }
  void setIntentionalSilence(bool value) override {
    gate_->intentionalSilence = value; gate_->delivery.setSuppressed(value);
  }
  corevideo::modules::MonitorDeliveryEvidence deliveryEvidence() const override {
    ++gate_->evidenceReads;
    if (gate_->failEvidence) throw std::runtime_error("controlled evidence failure");
    if (!gate_->classifyEvidence) return IAudioMonitorOutput::deliveryEvidence();
    auto value = gate_->delivery.snapshot(corevideo::modules::monitorEvidenceNow100ns());
    value.endpointId = id_;
    return value;
  }
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

TEST(AsyncAudioMonitorOutput, IdleEvidenceRefreshDoesNotPostponeFailedActivationRetry) {
  auto gate = std::make_shared<DeviceGate>(); gate->failStart = true;
  corevideo::modules::AsyncAudioMonitorOutput monitor(std::make_unique<GatedMonitor>(gate));
  monitor.start("device", 48000, 2);
  ASSERT_TRUE(waitUntil([&] { return !monitor.preparing(); }));
  EXPECT_FALSE(monitor.active());
  ASSERT_TRUE(waitUntil([&] {
    monitor.start("device", 48000, 2);
    return monitor.active();
  }));
  std::lock_guard<std::mutex> lock(gate->mutex);
  EXPECT_EQ(gate->started.size(), 2u);
}

TEST(AsyncAudioMonitorOutput, ActivationDoesNotBlockControlAndLateOldDeviceCannotBecomeReady) {
  auto gate = std::make_shared<DeviceGate>(); gate->blockStart = true;
  ReleaseGateOnExit release{*gate};
  corevideo::modules::AsyncAudioMonitorOutput monitor(std::make_unique<GatedMonitor>(gate));
  EXPECT_FALSE(monitor.start("old", 48000, 2));
  ASSERT_TRUE(waitUntil([&] { return gate->entered.try_wait(); }));
  auto control = std::async(std::launch::async, [&] {
    monitor.stop(); monitor.start("new", 48000, 2);
    return monitor.preparing() && !monitor.active() && monitor.resolvedEndpointId().empty();
  });
  const auto ready = control.wait_for(std::chrono::seconds(2));
  gate->unblock();
  ASSERT_EQ(ready, std::future_status::ready);
  EXPECT_TRUE(control.get());
  ASSERT_TRUE(waitUntil([&] { return monitor.active(); }));
  EXPECT_EQ(monitor.resolvedEndpointId(), "new");
}
TEST(AsyncAudioMonitorOutput, BoundedPressurePreservesSurvivingPcmOrderAndCountsActualAcceptance) {
  auto gate = std::make_shared<DeviceGate>();
  ReleaseGateOnExit release{*gate};
  corevideo::modules::AsyncAudioMonitorOutput monitor(std::make_unique<GatedMonitor>(gate));
  monitor.start("device", 48000, 2); ASSERT_TRUE(waitUntil([&] { return monitor.active(); }));
  gate->blockRender = true;
  float first[2]{.1f,.1f}; const bool firstAdmitted = monitor.render(first, 1, 2, 1);
  ASSERT_TRUE(waitUntil([&] { return gate->entered.try_wait(); }));
  const auto before = monitor.acceptedFrameCount();
  bool admitted = firstAdmitted;
  for (int i=2;i<=6;++i) { float pcm[2]{i*.1f,i*.1f}; admitted = monitor.render(pcm, 1, 2, 1) && admitted; }
  const bool deliveryObserved = monitor.deviceDeliveryObserved();
  const auto dropped = monitor.droppedFrameCount();
  gate->unblock();
  EXPECT_TRUE(admitted); EXPECT_EQ(before, 0); EXPECT_FALSE(deliveryObserved);
  EXPECT_EQ(dropped, 1);
  ASSERT_TRUE(waitUntil([&] { return monitor.acceptedFrameCount() == 5; }));
  std::lock_guard<std::mutex> lock(gate->mutex);
  ASSERT_EQ(gate->accepted.size(), 5u);
  EXPECT_EQ(gate->accepted[0], .1f); EXPECT_EQ(gate->accepted[1], 3*.1f);
  EXPECT_EQ(gate->accepted[4], 6*.1f);
}
TEST(AsyncAudioMonitorOutput, StalledDeviceActivationCannotHoldProgramOrAudioOwner) {
  for (const int stallMs : {20, 40, 100, 500}) {
    auto gate = std::make_shared<DeviceGate>(); gate->blockStart = true;
    ReleaseGateOnExit release{*gate};
    auto modules = corevideo::modules::createStubModules();
    modules.monitorOutput = std::make_unique<GatedMonitor>(gate);
    corevideo::core::MediaCore core(std::move(modules)); core.enableAudioOutputWorker();
    std::mutex coreMutex;
    auto command = std::async(std::launch::async, [&] {
      std::lock_guard<std::mutex> lock(coreMutex);
      return core.applyCommand(corevideo::rpc::Json::Object{{"type","sync-audio-monitor"},{"enabled",true},{"deviceId","device"},{"volume",1}});
    });
    ASSERT_TRUE(waitUntil([&] { return gate->entered.try_wait(); }));
    auto work = std::async(std::launch::async, [&] {
      const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(stallMs);
      do {
        core.renderAudioOutputTick(coreMutex);
        { std::lock_guard<std::mutex> lock(coreMutex); core.renderDisplayTick(); (void)core.sessionState(); }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      } while (std::chrono::steady_clock::now() < until);
      std::lock_guard<std::mutex> lock(coreMutex); return core.sessionState();
    });
    const auto ready = work.wait_for(std::chrono::seconds(2));
    gate->unblock(); command.get(); ASSERT_EQ(ready, std::future_status::ready);
    const auto snapshot = work.get(); EXPECT_GT(snapshot.getNumber("programFrameCount"), 0);
    EXPECT_EQ(snapshot.get("audioMixSession")->getString("monitorStatus"), "preparing");
  }
}

TEST(AsyncAudioMonitorOutput, RetargetDuringStalledStopNeverOpensSupersededEndpoint) {
  auto gate = std::make_shared<DeviceGate>();
  ReleaseGateOnExit release{*gate};
  corevideo::modules::AsyncAudioMonitorOutput monitor(std::make_unique<GatedMonitor>(gate));
  monitor.start("initial", 48000, 2);
  ASSERT_TRUE(waitUntil([&] { return monitor.active(); }));
  gate->blockStop = true;
  monitor.start("superseded", 48000, 2);
  ASSERT_TRUE(waitUntil([&] { return gate->entered.try_wait(); }));
  auto control = std::async(std::launch::async, [&] {
    monitor.stop(); monitor.start("final", 48000, 2);
    return monitor.preparing() && !monitor.active() && monitor.resolvedEndpointId().empty();
  });
  const auto finished = control.wait_for(std::chrono::seconds(2));
  gate->unblock();
  ASSERT_EQ(finished, std::future_status::ready);
  EXPECT_TRUE(control.get());
  ASSERT_TRUE(waitUntil([&] { return monitor.active(); }));
  EXPECT_EQ(monitor.resolvedEndpointId(), "final");
  std::lock_guard<std::mutex> lock(gate->mutex);
  EXPECT_EQ(gate->started, (std::vector<std::string>{"initial", "final"}));
}

TEST(AsyncAudioMonitorOutput, MuteDuringStalledStopCannotRestartCapturedEndpoint) {
  auto gate = std::make_shared<DeviceGate>();
  ReleaseGateOnExit release{*gate};
  corevideo::modules::AsyncAudioMonitorOutput monitor(std::make_unique<GatedMonitor>(gate));
  monitor.start("initial", 48000, 2);
  ASSERT_TRUE(waitUntil([&] { return monitor.active(); }));
  gate->blockStop = true;
  monitor.start("superseded", 48000, 2);
  ASSERT_TRUE(waitUntil([&] { return gate->entered.try_wait(); }));
  auto control = std::async(std::launch::async, [&] {
    monitor.stop();
    return !monitor.active() && monitor.preparing();
  });
  const auto finished = control.wait_for(std::chrono::seconds(2));
  gate->unblock();
  ASSERT_EQ(finished, std::future_status::ready);
  EXPECT_TRUE(control.get());
  ASSERT_TRUE(waitUntil([&] { return !monitor.preparing(); }));
  EXPECT_FALSE(monitor.active());
  std::lock_guard<std::mutex> lock(gate->mutex);
  EXPECT_EQ(gate->started, (std::vector<std::string>{"initial"}));
}

TEST(AsyncAudioMonitorOutput, ShutdownDuringStalledStopRetiresOnOwnerWithoutReopening) {
  auto gate = std::make_shared<DeviceGate>();
  ReleaseGateOnExit release{*gate};
  auto monitor = std::make_unique<corevideo::modules::AsyncAudioMonitorOutput>(std::make_unique<GatedMonitor>(gate));
  monitor->start("initial", 48000, 2);
  ASSERT_TRUE(waitUntil([&] { return monitor->active(); }));
  gate->blockStop = true;
  monitor->start("superseded", 48000, 2);
  ASSERT_TRUE(waitUntil([&] { return gate->entered.try_wait(); }));
  // Keep the device blocked beyond the wrapper's bounded retirement wait.
  auto destroy = std::async(std::launch::async, [&] { monitor.reset(); });
  const auto finished = destroy.wait_for(std::chrono::seconds(2));
  gate->unblock();
  ASSERT_EQ(finished, std::future_status::ready);
  destroy.get();
  // The detached owner still owns the backend, so wait for its retirement
  // rather than sampling a possibly unfinished start immediately after release.
  ASSERT_TRUE(waitUntil([&] { return gate.use_count() == 1; }));
  std::lock_guard<std::mutex> lock(gate->mutex);
  EXPECT_EQ(gate->started, (std::vector<std::string>{"initial"}));
}

TEST(MonitorDeliveryEvidence, RawHistoryAndEventClassificationSurviveIntentAndEndpointChanges) {
  using namespace corevideo::modules;
  MonitorDeliveryRecorder recorder;
  recorder.beginEndpoint();
  recorder.observe(480, 0, false, 100);
  auto first = recorder.snapshot(150);
  EXPECT_EQ(first.phase, MonitorDeliveryPhase::Priming);
  EXPECT_EQ(first.primingSilenceFrames, 480);
  EXPECT_EQ(first.rawEvents, 0);
  recorder.observe(480, 0, true, 200);
  recorder.setSuppressed(true);
  recorder.observe(480, 0, false, 300);
  auto muted = recorder.snapshot(350);
  EXPECT_EQ(muted.rawEvents, 1);
  EXPECT_EQ(muted.unexpectedEvents, 1);
  EXPECT_EQ(muted.lastEvent, MonitorDeliveryPhase::Starved);
  EXPECT_EQ(muted.lastEventAt100ns, 200);
  EXPECT_EQ(muted.phase, MonitorDeliveryPhase::Suppressed);
  recorder.observe(480, 0, true, 400);
  recorder.endEndpoint(); recorder.beginEndpoint();
  auto restarted = recorder.snapshot(500);
  EXPECT_EQ(restarted.backendEpoch, first.backendEpoch);
  EXPECT_EQ(restarted.lifecycleEpoch, first.lifecycleEpoch + 1);
  EXPECT_EQ(restarted.lastEventLifecycleEpoch, first.lifecycleEpoch);
  EXPECT_EQ(restarted.rawEvents, 2);
  EXPECT_EQ(restarted.rawMissingFrames, 960);
  EXPECT_EQ(restarted.suppressedEvents, 1);
  EXPECT_EQ(restarted.unexpectedEvents, 1);
  EXPECT_EQ(restarted.suppressedSilenceFrames, 960);
  MonitorDeliveryRecorder replacement;
  EXPECT_NE(replacement.snapshot(600).backendEpoch, first.backendEpoch);
}

TEST(MonitorDeliveryEvidence, ConcurrentReadsNeverPublishMixedCategoryTotals) {
  corevideo::modules::MonitorDeliveryRecorder recorder;
  recorder.beginEndpoint();
  std::atomic<bool> done{false};
  std::thread writer([&] {
    for (int i = 1; i <= 20000; ++i) {
      recorder.setSuppressed(i % 2 == 0);
      recorder.observe(480, 0, true, i);
    }
    done = true;
  });
  bool coherentTotals = true;
  while (!done) {
    const auto value = recorder.snapshot(30000);
    if (value.coherent) coherentTotals &= value.rawEvents == value.suppressedEvents + value.unexpectedEvents;
  }
  writer.join();
  EXPECT_TRUE(coherentTotals);
  const auto value = recorder.snapshot(30001);
  EXPECT_TRUE(value.coherent);
  EXPECT_EQ(value.rawEvents, 20000);
  EXPECT_EQ(value.suppressedEvents, 10000);
  EXPECT_EQ(value.unexpectedEvents, 10000);
}

TEST(AsyncAudioMonitorOutput, IdleDeviceEvidenceRefreshesAndRetainsRawCountsAcrossMuteAndRestart) {
  auto gate = std::make_shared<DeviceGate>(); gate->classifyEvidence = true;
  corevideo::modules::AsyncAudioMonitorOutput monitor(std::make_unique<GatedMonitor>(gate));
  monitor.start("device", 48000, 2);
  ASSERT_TRUE(waitUntil([&] { return monitor.active(); }));
  const auto initial = monitor.deliveryEvidence();
  gate->delivery.observe(480, 0, true, corevideo::modules::monitorEvidenceNow100ns());
  ASSERT_TRUE(waitUntil([&] { return monitor.deliveryEvidence().unexpectedEvents == 1; }));
  const auto beforeMute = monitor.deliveryEvidence();
  EXPECT_GT(beforeMute.observedAt100ns, initial.observedAt100ns);
  monitor.setIntentionalSilence(true);
  ASSERT_TRUE(waitUntil([&] { return monitor.deliveryEvidence().suppressionApplied; }));
  gate->delivery.observe(480, 0, true, corevideo::modules::monitorEvidenceNow100ns());
  ASSERT_TRUE(waitUntil([&] { return monitor.deliveryEvidence().suppressedEvents == 1; }));
  EXPECT_EQ(monitor.underrunCount(), 2);
  monitor.stop();
  ASSERT_TRUE(waitUntil([&] { return !monitor.preparing(); }));
  EXPECT_EQ(monitor.underrunCount(), 2);
  monitor.start("other-device", 48000, 2);
  ASSERT_TRUE(waitUntil([&] { return monitor.active(); }));
  const auto restarted = monitor.deliveryEvidence();
  EXPECT_EQ(restarted.rawEvents, 2);
  EXPECT_EQ(restarted.backendEpoch, initial.backendEpoch);
  EXPECT_GT(restarted.lifecycleEpoch, initial.lifecycleEpoch);
  EXPECT_EQ(restarted.endpointId, "other-device");
  EXPECT_EQ(restarted.lastEventLifecycleEpoch, initial.lifecycleEpoch);
  EXPECT_EQ(monitor.acceptedFrameCount(), 0) << "idle observation must not invent PCM acceptance";
  EXPECT_EQ(monitor.droppedFrameCount(), 0);
}

TEST(AsyncAudioMonitorOutput, FailedEvidenceReadPreservesThePriorValueAndMeasurementTime) {
  auto gate = std::make_shared<DeviceGate>(); gate->rawUnderruns = 7;
  corevideo::modules::AsyncAudioMonitorOutput monitor(std::make_unique<GatedMonitor>(gate));
  monitor.start("device", 48000, 2);
  ASSERT_TRUE(waitUntil([&] { return monitor.active() && monitor.underrunCount() == 7; }));
  gate->failEvidence = true;
  const int reads = gate->evidenceReads;
  ASSERT_TRUE(waitUntil([&] { return gate->evidenceReads > reads + 1; }));
  const auto stale = monitor.deliveryEvidence();
  const auto stableAt = stale.observedAt100ns;
  const int laterReads = gate->evidenceReads;
  ASSERT_TRUE(waitUntil([&] { return gate->evidenceReads > laterReads + 1; }));
  EXPECT_EQ(monitor.underrunCount(), 7);
  EXPECT_EQ(monitor.deliveryEvidence().observedAt100ns, stableAt);
  EXPECT_FALSE(stale.supported) << "legacy counters must not invent classification";
  gate->rawUnderruns = 8; gate->failEvidence = false;
  ASSERT_TRUE(waitUntil([&] { return monitor.underrunCount() == 8; }));
  EXPECT_GT(monitor.deliveryEvidence().observedAt100ns, stableAt);
}

TEST(AsyncAudioMonitorOutput, CoreSnapshotNeverSubstitutesZeroForMutedDeviceHistory) {
  auto gate = std::make_shared<DeviceGate>(); gate->rawUnderruns = 1;
  auto modules = corevideo::modules::createStubModules();
  modules.monitorOutput = std::make_unique<GatedMonitor>(gate);
  corevideo::core::MediaCore core(std::move(modules));
  std::mutex coreMutex;
  for (const auto [enabled, volume] : {std::pair{true, 1.0}, std::pair{true, 0.0},
                                      std::pair{false, 0.0}, std::pair{true, 1.0}}) {
    (void)core.applyCommand(corevideo::rpc::Json::Object{{"type", "sync-audio-monitor"},
        {"enabled", enabled}, {"deviceId", "device"}, {"volume", volume}});
    core.renderAudioOutputTick(coreMutex);
    const auto snapshot = core.sessionState();
    const auto* mix = snapshot.get("audioMixSession");
    ASSERT_NE(mix, nullptr);
    EXPECT_EQ(mix->getNumber("monitorUnderruns", -1), 1.0);
    const auto* evidence = mix->get("monitorDeliveryEvidence");
    ASSERT_NE(evidence, nullptr);
    EXPECT_EQ(evidence->getNumber("rawEvents", -1), 1.0);
    EXPECT_FALSE(evidence->get("classificationSupported")->asBool());
  }
}
