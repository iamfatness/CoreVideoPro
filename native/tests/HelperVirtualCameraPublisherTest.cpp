// HelperVirtualCameraPublisher: the core side of the macOS virtual camera when
// the CoreMediaIO sink runs in a helper process (plan 2026-11-16-mac-vcam-
// provider-arrival-fix, Task 3 design B).
//
// The defect this exists for is modelled directly. `World` is the Mac: is the
// camera extension's PROVIDER registered? `ProcessView` is one process's
// CoreMediaIO view of it, FIXED at that process's first touch, which is what
// was measured on macOS 26.3 (2026-11-16): the core polled
// CAMERA_EXTENSION_NOT_FOUND 210+ times while every newborn process saw the
// camera. The fakes below honour that rule; nothing in them can "refresh".
#include "modules/HelperVirtualCameraPublisher.h"
#include "modules/SinkVirtualCameraPublisher.h"
#include "modules/VirtualCameraFrame.h"

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

using namespace corevideo::modules;

namespace {

constexpr const char* kNotFound = "CAMERA_EXTENSION_NOT_FOUND: CoreVideo Pro Camera is not installed, not approved, or restarting.";
constexpr const char* kLost = "CAMERA_EXTENSION_LOST: CoreVideo Pro Camera restarted; reconnecting.";

struct World {
  bool providerRegistered = false;
};

// A process's view of the camera: taken once, at its first CoreMediaIO touch.
struct ProcessView {
  bool seesCamera;
  explicit ProcessView(const World& world) : seesCamera(world.providerRegistered) {}
};

// The pre-fix arrangement: the sink lives in the long-running core, so every
// connect() consults the SAME view.
class InProcessSink final : public IVirtualCameraSink {
 public:
  explicit InProcessSink(const World& world) : view_(world) {}
  std::string connect() override { return view_.seesCamera ? "" : kNotFound; }
  SinkEnqueueResult enqueueNv12(const uint8_t*, int, int, int64_t) override { return SinkEnqueueResult::Ok; }
  void disconnect() override {}

 private:
  ProcessView view_;
};

struct HelperScript {
  enum class Behaviour { AnswerFromView, DieSilently, NeverAnswer } behaviour = Behaviour::AnswerFromView;
  int dieExitCode = -11;
};

struct FakeHelper {  // survives its channel so a test can inspect it afterwards
  bool exited = false;
  int exitCode = 0;
  bool stopRequested = false;
  bool killed = false;
  int wakes = 0;
  std::int64_t pid = 0;
};

class FakeChannel final : public IVcamHelperChannel {
 public:
  FakeChannel(std::shared_ptr<FakeHelper> helper, int width, int height, int fps) : helper_(std::move(helper)) {
    const auto slot = static_cast<std::uint32_t>(nv12FrameSize(width, height));
    bytes_ = vcamHelperRegionBytes(kVcamHelperSlots, slot);
    raw_ = ::operator new(bytes_, std::align_val_t(4096));
    std::memset(raw_, 0, bytes_);
    formatVcamHelperRegion(shared(), width, height, fps, slot);
  }
  ~FakeChannel() override {
    helper_->exited = true;  // the real channel never leaves a process behind
    ::operator delete(raw_, std::align_val_t(4096));
  }
  VcamHelperShared* shared() override { return static_cast<VcamHelperShared*>(raw_); }
  void wake() override { ++helper_->wakes; }
  bool exited(int* exitCode) override {
    if (helper_->exited && exitCode != nullptr) *exitCode = helper_->exitCode;
    return helper_->exited;
  }
  void requestStop() override {
    helper_->stopRequested = true;
    if (obeysStop) helper_->exited = true;
  }
  void kill() override { helper_->killed = true; helper_->exited = true; helper_->exitCode = -9; }
  std::int64_t pid() const override { return helper_->pid; }
  bool obeysStop = true;

 private:
  std::shared_ptr<FakeHelper> helper_;
  void* raw_ = nullptr;
  std::size_t bytes_ = 0;
};

struct Rig;

class FakeLauncher final : public IVcamHelperLauncher {
 public:
  explicit FakeLauncher(Rig* rig) : rig_(rig) {}
  std::unique_ptr<IVcamHelperChannel> launch(int width, int height, int fps, std::string* error) override;

 private:
  Rig* rig_;
};

struct Rig {
  World world;
  HelperScript script;
  bool launchFails = false;
  bool coreSeesCamera = false;  // the long-running core's own (stale) view
  std::int64_t nowMs = 10'000;
  std::int64_t hostNs = 5'000'000;
  std::vector<std::shared_ptr<FakeHelper>> helpers;
  FakeChannel* live = nullptr;  // the newest channel, while the publisher holds it
  std::unique_ptr<HelperVirtualCameraPublisher> camera;

  Rig() {
    HelperVirtualCameraClocks clocks;
    clocks.steadyMs = [this] { return nowMs; };
    clocks.hostNs = [this] { return hostNs += 1000; };
    clocks.unixMs = [this] { return 1'700'000'000'000 + nowMs; };
    clocks.sleepMs = [this](std::int64_t ms) { nowMs += ms; };
    camera = std::make_unique<HelperVirtualCameraPublisher>(std::make_unique<FakeLauncher>(this), std::move(clocks),
                                                            [this] { return coreSeesCamera ? 1 : 0; });
  }

  // What AsyncVirtualCameraPublisher's worker does on its 1 s retry.
  bool retry() {
    camera->stop();
    return camera->start(1920, 1080, 60);
  }
  // Retries once a second (the worker's cadence) for `seconds`; true if one connected.
  bool retryFor(int seconds) {
    for (int i = 0; i < seconds; ++i) {
      nowMs += 1000;
      if (retry()) return true;
    }
    return false;
  }
  // Act as the helper: take every frame off the ring and "enqueue" it.
  int helperTakesFrames() {
    int taken = 0;
    VcamRingFrame frame;
    while (vcamRingPeek(live->shared(), frame)) {
      vcamRingPop(live->shared());
      live->shared()->framesReceived.fetch_add(1);
      live->shared()->framesPublished.fetch_add(1);
      ++taken;
    }
    return taken;
  }
};

std::unique_ptr<IVcamHelperChannel> FakeLauncher::launch(int width, int height, int fps, std::string* error) {
  if (rig_->launchFails) {
    if (error != nullptr) *error = "posix_spawn: No such file or directory";
    return nullptr;
  }
  auto helper = std::make_shared<FakeHelper>();
  helper->pid = 4000 + static_cast<std::int64_t>(rig_->helpers.size());
  rig_->helpers.push_back(helper);
  auto channel = std::make_unique<FakeChannel>(helper, width, height, fps);
  rig_->live = channel.get();
  // A NEWBORN process: its view of the Mac is taken now.
  const ProcessView view(rig_->world);
  switch (rig_->script.behaviour) {
    case HelperScript::Behaviour::AnswerFromView:
      if (view.seesCamera) {
        vcamHelperPublishState(channel->shared(), VcamHelperState::Live, "");
      } else {
        vcamHelperPublishState(channel->shared(), VcamHelperState::Failed, kNotFound);
        helper->exited = true;
        helper->exitCode = 3;
      }
      break;
    case HelperScript::Behaviour::DieSilently:
      helper->exited = true;
      helper->exitCode = rig_->script.dieExitCode;
      break;
    case HelperScript::Behaviour::NeverAnswer:
      channel->obeysStop = false;
      break;
  }
  return channel;
}

std::shared_ptr<const std::vector<uint8_t>> frame(int w = 1920, int h = 1080, uint8_t fill = 100) {
  return std::make_shared<const std::vector<uint8_t>>(nv12FrameSize(w, h), fill);
}

}  // namespace

// A check of THIS FILE'S FIXTURE, not of macOS and not of product code: it
// passes by construction, because InProcessSink is a fake whose ProcessView
// never refreshes. The platform behaviour it stands in for (a process never
// sees a camera-extension provider that registers after its first CoreMediaIO
// touch) was MEASURED on macOS 26.3 on 2026-11-16 and no unit test proves it;
// see the plan's evidence and its owner gate. What this keeps honest is the
// premise the next test depends on: with the fake's rule, retrying inside one
// process can never succeed, so a connect there can only come from a new one.
TEST(VcamProviderArrival, FixturePremiseAProcessViewInTheseFakesIsFixedAtFirstTouch) {
  World world;
  SinkVirtualCameraPublisher inProcess(std::make_unique<InProcessSink>(world), [] { return int64_t{0}; },
                                       /*logConnectFailures=*/false);
  EXPECT_FALSE(inProcess.start(1920, 1080, 60));
  world.providerRegistered = true;  // first install approved / update finished
  for (int second = 0; second < 300; ++second) {
    inProcess.stop();
    ASSERT_FALSE(inProcess.start(1920, 1080, 60)) << "second " << second;
  }
  EXPECT_EQ(inProcess.status().warning, kNotFound);
}

// THE FIX, under that modelled rule: same world, same rule for every process's
// view; the only change is that each attempt is a new process.
TEST(VcamProviderArrival, AHelperStartedAfterTheProviderArrivesConnectsWithNoToggleAndNoCoreRestart) {
  Rig rig;
  EXPECT_FALSE(rig.camera->start(1920, 1080, 60));
  auto before = rig.camera->status();
  EXPECT_EQ(before.state, "failed");
  EXPECT_EQ(before.warning.rfind("CAMERA_EXTENSION_NOT_FOUND", 0), 0u) << "the worker retries only CAMERA_ failures";
  EXPECT_EQ(before.deviceVisibleToSystem, 0);
  EXPECT_EQ(before.lastConnectedAtUnixMs, 0);
  EXPECT_FALSE(rig.retryFor(120)) << "nothing registered yet";

  rig.world.providerRegistered = true;  // the core's own view (coreSeesCamera) stays false
  ASSERT_TRUE(rig.retryFor(6)) << "connected within the slowest retry step";
  const auto after = rig.camera->status();
  EXPECT_EQ(after.state, "live");
  EXPECT_TRUE(after.enabled);
  EXPECT_TRUE(after.warning.empty());
  EXPECT_EQ(after.deviceVisibleToSystem, 1);
  EXPECT_EQ(after.deviceVisibleInProcess, 0) << "the blind spot is reported, not hidden";
  EXPECT_GT(after.lastConnectedAtUnixMs, 0);
  EXPECT_GT(after.sinkAttempts, 1u);
  EXPECT_EQ(after.sinkHelperPid, rig.helpers.back()->pid);
}

TEST(HelperVirtualCameraPublisher, EveryAttemptIsANewProcessAndTheOldOneIsGone) {
  Rig rig;
  rig.camera->start(1920, 1080, 60);
  ASSERT_FALSE(rig.retryFor(5));
  ASSERT_GE(rig.helpers.size(), 3u);
  for (std::size_t i = 0; i + 1 < rig.helpers.size(); ++i) EXPECT_TRUE(rig.helpers[i]->exited) << i;
  EXPECT_EQ(rig.camera->status().sinkAttempts, rig.helpers.size());
  EXPECT_EQ(rig.camera->status().sinkHelperGeneration, rig.helpers.size());
}

// The worker calls start() every second; the policy, not the worker, decides
// when a process is actually started.
TEST(HelperVirtualCameraPublisher, ASettledAbsenceStartsFewerProcessesThanRetries) {
  Rig rig;
  rig.camera->start(1920, 1080, 60);
  ASSERT_FALSE(rig.retryFor(600));
  // 10 fast (1 s) + 20 settled (2 s) covers 50 s; the remaining 550 s at 5 s = 110.
  EXPECT_LE(rig.helpers.size(), 1u + 10u + 20u + 111u);
  EXPECT_GE(rig.helpers.size(), 100u);
  const auto status = rig.camera->status();
  EXPECT_EQ(status.state, "failed");
  EXPECT_EQ(status.warning.rfind("CAMERA_", 0), 0u) << "still retrying, never given up";
  EXPECT_FALSE(status.sinkHelperGaveUp);
}

// Honest reporting: a hand-off is a submission. framesPublished moves only
// when the helper reports the camera's queue accepted the frame.
TEST(HelperVirtualCameraPublisher, FramesPublishedIsWhatTheHelperAcceptedNotWhatWasHandedOff) {
  Rig rig;
  rig.world.providerRegistered = true;
  ASSERT_TRUE(rig.camera->start(1920, 1080, 60));
  rig.camera->publishNv12IdentifiedOnWorker(frame(), 1920, 1080, 1, 0);
  rig.camera->publishNv12IdentifiedOnWorker(frame(), 1920, 1080, 2, 0);
  EXPECT_EQ(rig.camera->status().framesPublished, 0u) << "handed off, not yet accepted";
  EXPECT_EQ(rig.helpers.back()->wakes, 2);
  EXPECT_EQ(rig.helperTakesFrames(), 2);
  EXPECT_EQ(rig.camera->status().framesPublished, 2u);
}

TEST(HelperVirtualCameraPublisher, TheFrameReachesTheHelperByteForByteWithItsProgramSequence) {
  Rig rig;
  rig.world.providerRegistered = true;
  ASSERT_TRUE(rig.camera->start(1920, 1080, 60));
  auto bytes = std::make_shared<std::vector<uint8_t>>(nv12FrameSize(1920, 1080));
  for (std::size_t i = 0; i < bytes->size(); ++i) (*bytes)[i] = static_cast<uint8_t>(i * 31 + 7);
  rig.camera->publishNv12IdentifiedOnWorker(bytes, 1920, 1080, 4242, 0);
  VcamRingFrame got;
  ASSERT_TRUE(vcamRingPeek(rig.live->shared(), got));
  EXPECT_EQ(got.meta.programSequence, 4242);
  EXPECT_GT(got.meta.writtenAtNs, 0);
  EXPECT_EQ(std::memcmp(got.nv12, bytes->data(), bytes->size()), 0);
}

// Spec section 4 law 1 across the process boundary.
TEST(HelperVirtualCameraPublisher, AFullHandOffDropsTheNewFrameCountsItAndNeverBlocks) {
  Rig rig;
  rig.world.providerRegistered = true;
  ASSERT_TRUE(rig.camera->start(1920, 1080, 60));
  for (std::uint32_t i = 0; i < kVcamHelperSlots + 3; ++i)
    rig.camera->publishNv12IdentifiedOnWorker(frame(1920, 1080, static_cast<uint8_t>(i + 1)), 1920, 1080, i + 1, 0);
  EXPECT_EQ(rig.camera->status().transportDrops, 3u);
  EXPECT_EQ(rig.camera->status().state, "live") << "a slow helper is back-pressure, not a failure";
  VcamRingFrame oldest;
  ASSERT_TRUE(vcamRingPeek(rig.live->shared(), oldest));
  EXPECT_EQ(oldest.nv12[0], 1) << "held frames are never overwritten";
  EXPECT_EQ(rig.helperTakesFrames(), static_cast<int>(kVcamHelperSlots));
}

TEST(HelperVirtualCameraPublisher, AnotherSizeIsANoOp) {
  Rig rig;
  rig.world.providerRegistered = true;
  ASSERT_TRUE(rig.camera->start(1920, 1080, 60));
  rig.camera->publishNv12IdentifiedOnWorker(frame(1280, 720), 1280, 720, 1, 0);
  EXPECT_EQ(vcamRingDepth(rig.live->shared()), 0u);
  EXPECT_EQ(rig.camera->status().transportDrops, 0u);
}

// An extension update mid-stream: the helper says the camera was lost and
// exits; the next attempt is a new process, and the count carries on.
TEST(HelperVirtualCameraPublisher, ALostCameraIsReconnectedByANewHelperWithoutAToggle) {
  Rig rig;
  rig.world.providerRegistered = true;
  ASSERT_TRUE(rig.camera->start(1920, 1080, 60));
  rig.camera->publishNv12IdentifiedOnWorker(frame(), 1920, 1080, 1, 0);
  rig.helperTakesFrames();
  const auto firstPid = rig.camera->status().sinkHelperPid;

  vcamHelperPublishState(rig.live->shared(), VcamHelperState::Failed, kLost);
  rig.helpers.back()->exited = true;
  rig.helpers.back()->exitCode = 4;
  rig.camera->publishNv12IdentifiedOnWorker(frame(), 1920, 1080, 2, 0);
  auto lost = rig.camera->status();
  EXPECT_EQ(lost.state, "failed");
  EXPECT_EQ(lost.warning, kLost);
  EXPECT_EQ(lost.framesPublished, 1u) << "the ended helper's count is kept";
  EXPECT_EQ(lost.sinkHelperPid, 0);

  ASSERT_TRUE(rig.retryFor(2)) << "back within the fast retry";
  rig.camera->publishNv12IdentifiedOnWorker(frame(), 1920, 1080, 3, 0);
  rig.helperTakesFrames();
  const auto back = rig.camera->status();
  EXPECT_EQ(back.state, "live");
  EXPECT_EQ(back.framesPublished, 2u);
  EXPECT_NE(back.sinkHelperPid, firstPid);
}

// Review finding 3 (2026-11-16): the extension accepts the stream, then stops
// draining it, so every helper connects and reports the camera lost about 3 s
// later. That must slow down and must be readable in the status the snapshot
// publishes; before, it was a new process every 3 s forever with nothing to
// tell it from a healthy reconnect.
TEST(HelperVirtualCameraPublisher, ACameraThatConnectsAndIsLostEachTimeBacksOffAndSaysSo) {
  Rig rig;
  rig.world.providerRegistered = true;
  ASSERT_TRUE(rig.camera->start(1920, 1080, 60));
  auto loseAfter = [&](std::int64_t runMs) {
    rig.nowMs += runMs;
    vcamHelperPublishState(rig.live->shared(), VcamHelperState::Failed, kLost);
    rig.helpers.back()->exited = true;
    rig.helpers.back()->exitCode = 4;
    rig.camera->publishNv12IdentifiedOnWorker(frame(), 1920, 1080, 1, 0);
  };
  for (int cycle = 1; cycle <= kVcamHelperSettledRetries; ++cycle) {
    loseAfter(3'000);
    const auto lost = rig.camera->status();
    ASSERT_EQ(lost.state, "failed");
    ASSERT_EQ(lost.sinkConsecutiveShortRuns, cycle);
    ASSERT_EQ(lost.sinkConsecutiveUnreachable, cycle);
    ASSERT_EQ(lost.sinkLastRunMs, 3'000);
    ASSERT_TRUE(rig.retryFor(3)) << "cycle " << cycle;
    ASSERT_EQ(rig.camera->status().sinkConsecutiveShortRuns, cycle) << "connecting again is not yet a healthy run";
  }
  // Settled: the slow rung. Still retrying, never given up, one process per 5 s wait.
  const auto helpersBefore = rig.helpers.size();
  loseAfter(3'000);
  auto slow = rig.camera->status();
  EXPECT_EQ(slow.sinkNextAttemptInMs, kVcamHelperSlowRetryMs);
  EXPECT_FALSE(slow.sinkHelperGaveUp);
  EXPECT_EQ(slow.warning.rfind("CAMERA_", 0), 0u) << "the worker keeps calling back";
  EXPECT_FALSE(rig.retryFor(4)) << "no new process inside the slow rung";
  EXPECT_EQ(rig.helpers.size(), helpersBefore);
  ASSERT_TRUE(rig.retryFor(2));

  // A run that lasts is healthy again: the next loss is retried fast.
  loseAfter(kVcamHelperHealthyRunMs);
  const auto healthy = rig.camera->status();
  EXPECT_EQ(healthy.sinkConsecutiveShortRuns, 0);
  EXPECT_EQ(healthy.sinkConsecutiveUnreachable, 1);
  EXPECT_EQ(healthy.sinkNextAttemptInMs, kVcamHelperFastRetryMs);
  EXPECT_TRUE(rig.retryFor(2));
}

TEST(HelperVirtualCameraPublisher, AHelperThatDiesMidStreamIsAHelperFailureAndIsReplaced) {
  Rig rig;
  rig.world.providerRegistered = true;
  ASSERT_TRUE(rig.camera->start(1920, 1080, 60));
  rig.helpers.back()->exited = true;
  rig.helpers.back()->exitCode = -11;
  rig.camera->publishNv12IdentifiedOnWorker(frame(), 1920, 1080, 1, 0);
  const auto status = rig.camera->status();
  EXPECT_EQ(status.state, "failed");
  EXPECT_EQ(status.warning.rfind("CAMERA_HELPER_FAILED", 0), 0u);
  EXPECT_EQ(status.sinkHelperLastExit, "signal 11");
  EXPECT_FALSE(rig.retryFor(4)) << "first rung of the house ladder is 5 s";
  EXPECT_TRUE(rig.retryFor(2));
}

TEST(HelperVirtualCameraPublisher, AHelperThatKeepsFailingIsGivenUpOnLoudlyUntilTheOperatorRearms) {
  Rig rig;
  rig.world.providerRegistered = true;
  rig.script.behaviour = HelperScript::Behaviour::DieSilently;
  rig.camera->start(1920, 1080, 60);
  EXPECT_FALSE(rig.retryFor(200));
  EXPECT_EQ(rig.helpers.size(), 5u) << "five consecutive helper failures, then no more processes";
  auto status = rig.camera->status();
  EXPECT_EQ(status.state, "failed");
  EXPECT_TRUE(status.sinkHelperGaveUp);
  EXPECT_NE(status.warning.rfind("CAMERA_", 0), 0u) << "no CAMERA_ prefix: the worker's 1 s retry stops";
  EXPECT_FALSE(status.warning.empty());

  // Off and on again (AsyncVirtualCameraPublisher calls rearmRetryBudget).
  rig.script.behaviour = HelperScript::Behaviour::AnswerFromView;
  rig.camera->stop();
  rig.camera->rearmRetryBudget();
  EXPECT_TRUE(rig.camera->start(1920, 1080, 60));
  EXPECT_FALSE(rig.camera->status().sinkHelperGaveUp);
}

TEST(HelperVirtualCameraPublisher, AHelperThatNeverAnswersIsKilledAfterTheBoundedWait) {
  Rig rig;
  rig.world.providerRegistered = true;
  rig.script.behaviour = HelperScript::Behaviour::NeverAnswer;
  const auto startedAt = rig.nowMs;
  EXPECT_FALSE(rig.camera->start(1920, 1080, 60));
  EXPECT_LE(rig.nowMs - startedAt, kVcamHelperAnswerWaitMs + 100);
  EXPECT_TRUE(rig.helpers.back()->killed);
  EXPECT_EQ(rig.camera->status().warning.rfind("CAMERA_HELPER_FAILED", 0), 0u);
}

TEST(HelperVirtualCameraPublisher, AHelperThatStopsTakingFramesForTwoSecondsIsKilledAndReplaced) {
  Rig rig;
  rig.world.providerRegistered = true;
  ASSERT_TRUE(rig.camera->start(1920, 1080, 60));
  for (int tick = 0; tick < 130; ++tick) {  // 60 Hz for a little over 2 s, nothing taken
    rig.camera->publishNv12IdentifiedOnWorker(frame(), 1920, 1080, tick + 1, 0);
    rig.nowMs += 17;
    if (rig.camera->status().state != "live") break;
  }
  EXPECT_TRUE(rig.helpers.back()->killed);
  const auto status = rig.camera->status();
  EXPECT_EQ(status.state, "failed");
  EXPECT_EQ(status.sinkHelperLastExit, "stalled");
  EXPECT_EQ(status.warning.rfind("CAMERA_HELPER_FAILED", 0), 0u);
}

TEST(HelperVirtualCameraPublisher, AHelperThatTakesFramesAgainRestartsTheStallClock) {
  Rig rig;
  rig.world.providerRegistered = true;
  ASSERT_TRUE(rig.camera->start(1920, 1080, 60));
  for (int tick = 0; tick < 600; ++tick) {
    rig.camera->publishNv12IdentifiedOnWorker(frame(), 1920, 1080, tick + 1, 0);
    rig.nowMs += 17;
    if (tick % 90 == 89) rig.helperTakesFrames();  // a 1.5 s hiccup, repeatedly
  }
  EXPECT_EQ(rig.camera->status().state, "live");
  EXPECT_FALSE(rig.helpers.back()->killed);
  EXPECT_GT(rig.camera->status().transportDrops, 0u);
}

TEST(HelperVirtualCameraPublisher, StopAsksTheHelperToStopAndLeavesNoProcess) {
  Rig rig;
  rig.world.providerRegistered = true;
  ASSERT_TRUE(rig.camera->start(1920, 1080, 60));
  rig.camera->stop();
  EXPECT_TRUE(rig.helpers.back()->stopRequested);
  EXPECT_TRUE(rig.helpers.back()->exited);
  EXPECT_FALSE(rig.helpers.back()->killed) << "a helper that obeys is not killed";
  const auto status = rig.camera->status();
  EXPECT_EQ(status.state, "off");
  EXPECT_FALSE(status.enabled);
  EXPECT_EQ(status.sinkHelperPid, 0);
  rig.camera->publishNv12IdentifiedOnWorker(frame(), 1920, 1080, 1, 0);  // ignored while off
}

TEST(HelperVirtualCameraPublisher, AProcessThatCannotBeStartedIsAHelperFailureThatSaysWhy) {
  Rig rig;
  rig.launchFails = true;
  EXPECT_FALSE(rig.camera->start(1920, 1080, 60));
  const auto status = rig.camera->status();
  EXPECT_EQ(status.warning.rfind("CAMERA_HELPER_FAILED", 0), 0u);
  EXPECT_NE(status.warning.find("No such file"), std::string::npos);
  EXPECT_EQ(status.deviceVisibleToSystem, -1) << "nothing looked: not observed, never false";
}

TEST(HelperVirtualCameraPublisher, CpuProgramFramesAreRefusedAndMirrorIsUpstream) {
  Rig rig;
  rig.world.providerRegistered = true;
  ASSERT_TRUE(rig.camera->start(1920, 1080, 60));
  corevideo::modules::ProgramFrame cpu;
  rig.camera->publish(cpu);
  EXPECT_EQ(vcamRingDepth(rig.live->shared()), 0u);
  EXPECT_TRUE(rig.camera->mirrorAppliedUpstream());
}
