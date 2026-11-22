// The process half of the macOS virtual-camera sink helper (plan 2026-11-16-
// mac-vcam-provider-arrival-fix, Task 3 design B): the real helper loop over a
// real shared region and wake pipe with a fake sink, and the real POSIX
// launcher against ordinary system executables. Built only with
// COREVIDEO_WITH_MAC_VIRTUALCAM. Nothing in THIS process touches CoreMediaIO;
// the wiring tests at the end run the built core in its two process modes, and
// the device-probe mode reads the camera's HAL properties in that child (the
// read the shell makes; it starts no stream).
#include "modules/HelperVirtualCameraPublisher.h"
#include "modules/SinkVirtualCameraPublisher.h"
#include "modules/VcamSinkHelper.h"
#include "modules/VirtualCameraFrame.h"
#include "modules/VirtualCameraPublisher.h"

#include <gtest/gtest.h>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <future>
#include <memory>
#include <new>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace corevideo::modules;

extern char** environ;

namespace {

struct SinkLog {
  std::string connectResult;
  std::atomic<int> connects{0}, disconnects{0}, enqueued{0};
  std::atomic<bool> loseNext{false};
  std::atomic<std::uint8_t> lastFirstByte{0}, lastLastByte{0};
};

class LoggedSink final : public IVirtualCameraSink {
 public:
  explicit LoggedSink(std::shared_ptr<SinkLog> log) : log_(std::move(log)) {}
  std::string connect() override { ++log_->connects; return log_->connectResult; }
  SinkEnqueueResult enqueueNv12(const uint8_t* nv12, int width, int height, int64_t) override {
    if (log_->loseNext.exchange(false)) return SinkEnqueueResult::Lost;
    log_->lastFirstByte = nv12[0];
    log_->lastLastByte = nv12[nv12FrameSize(width, height) - 1];
    ++log_->enqueued;
    return SinkEnqueueResult::Ok;
  }
  void disconnect() override { ++log_->disconnects; }

 private:
  std::shared_ptr<SinkLog> log_;
};

// The helper loop on a thread, over a real pipe and a heap region.
struct LoopRig {
  static constexpr int kW = 320, kH = 180;
  void* raw = nullptr;
  std::size_t bytes = 0;
  VcamHelperShared* shared = nullptr;
  int wake[2] = {-1, -1};
  std::shared_ptr<SinkLog> log = std::make_shared<SinkLog>();
  std::future<int> exit;
  std::int64_t clock = 1'000'000;

  LoopRig() {
    const auto slot = static_cast<std::uint32_t>(nv12FrameSize(kW, kH));
    bytes = vcamHelperRegionBytes(kVcamHelperSlots, slot);
    raw = ::operator new(bytes, std::align_val_t(4096));
    std::memset(raw, 0, bytes);
    shared = static_cast<VcamHelperShared*>(raw);
    formatVcamHelperRegion(shared, kW, kH, 60, slot);
    EXPECT_EQ(::pipe(wake), 0);
  }
  ~LoopRig() {
    closeWake();
    if (exit.valid()) exit.wait();
    ::close(wake[0]);
    ::operator delete(raw, std::align_val_t(4096));
  }
  void run() {
    exit = std::async(std::launch::async, [this] {
      return runVcamSinkHelper(shared, bytes, wake[0], std::make_unique<LoggedSink>(log),
                               [] {
                                 return std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            std::chrono::steady_clock::now().time_since_epoch()).count();
                               });
    });
  }
  void closeWake() { if (wake[1] >= 0) { ::close(wake[1]); wake[1] = -1; } }
  bool waitFor(const std::function<bool()>& condition, int ms = 3000) {
    for (int i = 0; i < ms; ++i) {
      if (condition()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return condition();
  }
  void send(std::uint8_t fill, std::int64_t sequence) {
    std::vector<std::uint8_t> frame(nv12FrameSize(kW, kH), fill);
    ASSERT_TRUE(vcamRingTryWrite(shared, frame.data(), frame.size(), sequence, 0));
    ASSERT_EQ(::write(wake[1], "f", 1), 1);
  }
};

}  // namespace

TEST(VcamSinkHelperLoop, ACameraItCannotSeeIsReportedOnceAndTheHelperLeaves) {
  LoopRig rig;
  rig.log->connectResult = "CAMERA_EXTENSION_NOT_FOUND: CoreVideo Pro Camera is not installed, not approved, or restarting.";
  rig.run();
  EXPECT_EQ(rig.exit.get(), kVcamHelperExitUnreachable);
  EXPECT_EQ(rig.log->connects, 1) << "one attempt per process: a retry is a new process";
  EXPECT_EQ(vcamHelperReadState(rig.shared), VcamHelperState::Failed);
  EXPECT_EQ(vcamHelperReadWarning(rig.shared), rig.log->connectResult);
  EXPECT_EQ(rig.shared->deviceSeen.load(), static_cast<std::uint32_t>(VcamDeviceSeen::No));
}

TEST(VcamSinkHelperLoop, ARefusedProducerStillSawTheDevice) {
  LoopRig rig;
  rig.log->connectResult = sinkStartStreamWarning(-4);
  rig.run();
  EXPECT_EQ(rig.exit.get(), kVcamHelperExitUnreachable);
  EXPECT_EQ(rig.shared->deviceSeen.load(), static_cast<std::uint32_t>(VcamDeviceSeen::Yes));
}

TEST(VcamSinkHelperLoop, FramesHandedOffAreEnqueuedInOrderAndCounted) {
  LoopRig rig;
  rig.run();
  ASSERT_TRUE(rig.waitFor([&] { return vcamHelperReadState(rig.shared) == VcamHelperState::Live; }));
  for (int i = 1; i <= 20; ++i) {
    rig.send(static_cast<std::uint8_t>(i), i);
    ASSERT_TRUE(rig.waitFor([&] { return rig.shared->framesPublished.load() == static_cast<std::uint64_t>(i); })) << i;
    EXPECT_EQ(rig.log->lastFirstByte, i);
    EXPECT_EQ(rig.log->lastLastByte, i);
  }
  EXPECT_EQ(rig.shared->framesReceived.load(), 20u);
  EXPECT_EQ(rig.shared->sequenceGaps.load(), 0u);
  EXPECT_EQ(vcamRingDepth(rig.shared), 0u);
}

TEST(VcamSinkHelperLoop, AProgramSequenceGapIsCountedByTheHelper) {
  LoopRig rig;
  rig.run();
  ASSERT_TRUE(rig.waitFor([&] { return vcamHelperReadState(rig.shared) == VcamHelperState::Live; }));
  rig.send(1, 10);
  rig.send(2, 13);  // 11 and 12 never arrived
  ASSERT_TRUE(rig.waitFor([&] { return rig.shared->framesPublished.load() == 2; }));
  EXPECT_EQ(rig.shared->sequenceGaps.load(), 2u);
}

// The core closing the pipe (camera turned off, or the core died) stops the
// stream cleanly: the camera then shows its slate at once.
TEST(VcamSinkHelperLoop, EndOfFileOnTheWakePipeDisconnectsAndExitsCleanly) {
  LoopRig rig;
  rig.run();
  ASSERT_TRUE(rig.waitFor([&] { return vcamHelperReadState(rig.shared) == VcamHelperState::Live; }));
  rig.send(7, 1);
  rig.closeWake();
  EXPECT_EQ(rig.exit.get(), kVcamHelperExitStopped);
  EXPECT_EQ(rig.log->disconnects, 1);
  EXPECT_EQ(rig.log->enqueued, 1) << "a frame handed off before the stop is still delivered";
  EXPECT_EQ(vcamHelperReadState(rig.shared), VcamHelperState::Stopped);
}

TEST(VcamSinkHelperLoop, ALostCameraEndsTheHelperWithTheReason) {
  LoopRig rig;
  rig.run();
  ASSERT_TRUE(rig.waitFor([&] { return vcamHelperReadState(rig.shared) == VcamHelperState::Live; }));
  rig.send(1, 1);
  ASSERT_TRUE(rig.waitFor([&] { return rig.shared->framesPublished.load() == 1; }));
  rig.log->loseNext = true;
  rig.send(2, 2);
  EXPECT_EQ(rig.exit.get(), kVcamHelperExitLost);
  EXPECT_EQ(vcamHelperReadState(rig.shared), VcamHelperState::Failed);
  EXPECT_EQ(vcamHelperReadWarning(rig.shared).rfind("CAMERA_EXTENSION_LOST", 0), 0u);
  EXPECT_EQ(rig.shared->framesPublished.load(), 1u);
  EXPECT_EQ(rig.log->connects, 1) << "the helper never reconnects by itself";
}

TEST(VcamSinkHelperLoop, ARegionItWasNotHandedByThisBuildIsRefused) {
  LoopRig rig;
  rig.shared->version = kVcamHelperVersion + 1;
  rig.run();
  EXPECT_EQ(rig.exit.get(), kVcamHelperExitBadInvocation);
  EXPECT_EQ(rig.log->connects, 0);
}

// The real launcher and channel, against executables every Mac has.
namespace {
HelperVirtualCameraPublisher realPublisher(const std::string& executable, std::vector<std::string> arguments) {
  return HelperVirtualCameraPublisher(createPosixVcamHelperLauncher(executable, std::move(arguments)),
                                      defaultHelperVirtualCameraClocks([] { return std::int64_t{0}; }));
}
}  // namespace

TEST(VcamSinkHelperProcess, AProcessThatExitsWithoutAnsweringIsReapedAndNamed) {
  auto camera = realPublisher("/usr/bin/false", {});
  EXPECT_FALSE(camera.start(1920, 1080, 60));
  const auto status = camera.status();
  EXPECT_EQ(status.warning.rfind("CAMERA_HELPER_FAILED", 0), 0u);
  EXPECT_EQ(status.sinkHelperLastExit, "exit 1");
  EXPECT_EQ(status.sinkHelperPid, 0);
  EXPECT_EQ(status.sinkAttempts, 1u);
}

TEST(VcamSinkHelperProcess, AProcessThatNeverAnswersIsKilledWithinTheBoundedWait) {
  auto camera = realPublisher("/bin/sleep", {"30"});
  const auto started = std::chrono::steady_clock::now();
  EXPECT_FALSE(camera.start(1920, 1080, 60));
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
  EXPECT_GE(ms, kVcamHelperAnswerWaitMs - 50);
  EXPECT_LT(ms, kVcamHelperAnswerWaitMs + 1500);
  EXPECT_EQ(camera.status().sinkHelperLastExit, "no answer");
  EXPECT_EQ(camera.status().sinkHelperPid, 0) << "no process is left behind";
}

TEST(VcamSinkHelperProcess, AMissingExecutableSaysSo) {
  auto camera = realPublisher("/nonexistent/corevideo-native", {});
  EXPECT_FALSE(camera.start(1920, 1080, 60));
  EXPECT_NE(camera.status().warning.find("posix_spawn"), std::string::npos);
  EXPECT_EQ(camera.status().sinkHelperLastExit, "not started");
}

// A binary that never opted in (every test and tool) has no helper path at all.
TEST(VcamSinkHelperProcess, WithoutTheCoresOptInNothingIsExecuted) {
  auto camera = realPublisher("", {kVcamSinkHelperArgument});
  EXPECT_FALSE(camera.start(1920, 1080, 60));
  EXPECT_NE(camera.status().warning.find("own path is unknown"), std::string::npos);
}

TEST(VcamSinkHelperProcess, ThisProcessCanNameItsOwnExecutableAndItsParents) {
  EXPECT_FALSE(currentExecutablePath().empty());
  EXPECT_EQ(executablePathOfProcess(::getpid()), currentExecutablePath());
  EXPECT_FALSE(vcamHelperParentAllowed(currentExecutablePath(), executablePathOfProcess(::getppid())))
      << "the test runner is not this executable";
}

// ---------------------------------------------------------------------------
// Wiring (review finding 4, 2026-11-16). Everything above tests the parts; a
// build that put the in-process sink back in the factory, dropped the process-
// mode dispatch from main, or dropped the helper's parent check passed all of
// it. These three fail on those regressions.

// The product's factory, as every MediaCore gets it. This binary never called
// enableVirtualCameraSinkHelper(), so the helper publisher has no executable
// to start and says so; any other backend (the in-process CoreMediaIO sink
// included) starts no helper and reports none.
TEST(VcamWiring, TheFactoryBuildsTheHelperPublisherNotAnInProcessSink) {
  auto camera = createVirtualCameraPublisher();
  ASSERT_NE(camera, nullptr);
  camera->start(1920, 1080, 60);  // applied on the publisher's own worker
  VirtualCameraStatus status;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  do {
    status = camera->status();
    if (status.sinkAttempts > 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  } while (std::chrono::steady_clock::now() < deadline);
  camera->stop();
  EXPECT_GE(status.sinkAttempts, 1u) << "no sink helper was attempted: state '" << status.state << "', warning '"
                                     << status.warning << "'";
  EXPECT_EQ(status.sinkHelperLastExit, "not started");
  EXPECT_NE(status.warning.find("own path is unknown"), std::string::npos) << status.warning;
  EXPECT_EQ(status.deviceVisibleToSystem, -1) << "nothing in this process looked at the camera";
}

namespace {

struct CoreRun {
  bool started = false;
  bool timedOut = false;
  int exitCode = -1;  // -signal when a signal ended it
  std::string out, err, error;
};

// Runs the built core (COREVIDEO_NATIVE_CORE_PATH) with one argument, as a
// child of THIS process, with stdin at /dev/null and nothing inherited above
// stderr (so no descriptor can pass for the helper's shared region).
CoreRun runBuiltCore(const char* argument, std::chrono::milliseconds limit) {
  CoreRun run;
  const std::string core = COREVIDEO_NATIVE_CORE_PATH;
  int outPipe[2] = {-1, -1}, errPipe[2] = {-1, -1};
  if (::pipe(outPipe) != 0 || ::pipe(errPipe) != 0) { run.error = "pipe failed"; return run; }
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&actions, outPipe[1], 1);
  posix_spawn_file_actions_adddup2(&actions, errPipe[1], 2);
  posix_spawnattr_t attributes;
  posix_spawnattr_init(&attributes);
  posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);
  std::string program = core, arg = argument;
  char* argv[] = {program.data(), arg.data(), nullptr};
  pid_t pid = 0;
  const int spawned = ::posix_spawn(&pid, core.c_str(), &actions, &attributes, argv, environ);
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attributes);
  ::close(outPipe[1]);
  ::close(errPipe[1]);
  if (spawned != 0) {
    ::close(outPipe[0]);
    ::close(errPipe[0]);
    run.error = "posix_spawn " + core + ": " + std::strerror(spawned);
    return run;
  }
  run.started = true;
  const auto deadline = std::chrono::steady_clock::now() + limit;
  struct pollfd fds[2] = {{outPipe[0], POLLIN, 0}, {errPipe[0], POLLIN, 0}};
  std::string* sinks[2] = {&run.out, &run.err};
  int open = 2;
  while (open > 0) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    if (left.count() <= 0) { run.timedOut = true; break; }
    const int ready = ::poll(fds, 2, static_cast<int>(left.count()));
    if (ready < 0 && errno == EINTR) continue;
    if (ready <= 0) { run.timedOut = ready == 0; break; }
    for (int i = 0; i < 2; ++i) {
      if (fds[i].fd < 0 || (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) == 0) continue;
      char buffer[4096];
      const ssize_t got = ::read(fds[i].fd, buffer, sizeof(buffer));
      if (got > 0) { sinks[i]->append(buffer, static_cast<std::size_t>(got)); continue; }
      if (got < 0 && errno == EINTR) continue;
      fds[i].fd = -1;  // end of file
      --open;
    }
  }
  if (run.timedOut) ::kill(pid, SIGKILL);
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
  ::close(outPipe[0]);
  ::close(errPipe[0]);
  run.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status);
  return run;
}

}  // namespace

// main's process-mode dispatch and runSinkHelper's real parent check, in the
// built core: started by anything but the core itself, the helper refuses
// BEFORE it looks at a descriptor or the camera, and says so. A main without
// the dispatch runs an ordinary core here (no refusal line); a helper without
// the check fails on its missing region instead (same exit code, no refusal).
TEST(VcamWiring, TheBuiltCoreRefusesToBeASinkHelperForAnyParentButItself) {
  const auto run = runBuiltCore(kVcamSinkHelperArgument, std::chrono::seconds(20));
  ASSERT_TRUE(run.started) << run.error;
  EXPECT_FALSE(run.timedOut) << "the core did not leave: it did not take the helper mode";
  EXPECT_EQ(run.exitCode, kVcamHelperExitBadInvocation) << run.err;
  EXPECT_NE(run.err.find("sink helper refused"), std::string::npos) << "stderr: " << run.err;
  EXPECT_TRUE(run.out.empty()) << "a helper never writes the core's RPC channel: " << run.out;
}

// The other process mode the shell depends on: one line of JSON and out.
TEST(VcamWiring, TheBuiltCoreAnswersTheDeviceProbeAndExits) {
  const auto run = runBuiltCore(kVcamDeviceProbeArgument, std::chrono::seconds(20));
  ASSERT_TRUE(run.started) << run.error;
  EXPECT_FALSE(run.timedOut) << "the core did not leave: it did not take the probe mode";
  EXPECT_EQ(run.exitCode, 0) << run.err;
  EXPECT_EQ(run.out.rfind("{\"registered\":", 0), 0u) << "stdout: " << run.out;
  EXPECT_NE(run.out.find("\"runningSomewhere\":"), std::string::npos) << run.out;
  EXPECT_NE(run.out.find("\"modelUid\":"), std::string::npos) << run.out;
  EXPECT_EQ(std::count(run.out.begin(), run.out.end(), '\n'), 1) << "exactly one line: " << run.out;
}
