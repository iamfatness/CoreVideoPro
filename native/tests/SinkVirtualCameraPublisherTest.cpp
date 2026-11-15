// SinkVirtualCameraPublisher: the portable half of the macOS virtual camera
// (docs/virtual-camera-macos-spec.md §2, roadmap M9.2). Driven through a FakeSink
// that honours the IVirtualCameraSink contract: it copies the bytes on enqueue
// (never retains the caller's pointer), refuses with QueueFull once its scripted
// capacity is reached (and stops counting), returns scripted connect() results in
// order, and records disconnect() calls.
#include "modules/SinkVirtualCameraPublisher.h"
#include "modules/VirtualCameraFrame.h"

#include <gtest/gtest.h>

#include <deque>
#include <memory>
#include <string>
#include <vector>

using namespace corevideo::modules;

namespace {

struct FakeSinkState {
  std::deque<std::string> connectResults;  // empty deque => "" (success)
  std::size_t capacity = 64;
  bool loseOnNextEnqueue = false;
  int connects = 0;
  int disconnects = 0;
  int enqueueCalls = 0;
  std::vector<std::vector<uint8_t>> held;  // copies, in enqueue order
  std::vector<int64_t> hostTimes;
};

class FakeSink final : public IVirtualCameraSink {
 public:
  explicit FakeSink(std::shared_ptr<FakeSinkState> state) : s_(std::move(state)) {}
  std::string connect() override {
    ++s_->connects;
    if (s_->connectResults.empty()) return "";
    auto result = s_->connectResults.front();
    s_->connectResults.pop_front();
    return result;
  }
  SinkEnqueueResult enqueueNv12(const uint8_t* nv12, int width, int height, int64_t hostTimeNs) override {
    ++s_->enqueueCalls;
    if (s_->loseOnNextEnqueue) { s_->loseOnNextEnqueue = false; return SinkEnqueueResult::Lost; }
    if (s_->held.size() >= s_->capacity) return SinkEnqueueResult::QueueFull;
    s_->held.emplace_back(nv12, nv12 + nv12FrameSize(width, height));
    s_->hostTimes.push_back(hostTimeNs);
    return SinkEnqueueResult::Ok;
  }
  void disconnect() override { ++s_->disconnects; }

 private:
  std::shared_ptr<FakeSinkState> s_;
};

struct Rig {
  std::shared_ptr<FakeSinkState> state = std::make_shared<FakeSinkState>();
  int64_t clock = 1000;
  std::unique_ptr<SinkVirtualCameraPublisher> camera;
  Rig() {
    camera = std::make_unique<SinkVirtualCameraPublisher>(
        std::make_unique<FakeSink>(state), [this] { return clock += 16'666'667; });
  }
};

std::shared_ptr<const std::vector<uint8_t>> frame(int w, int h, uint8_t fill = 100) {
  return std::make_shared<const std::vector<uint8_t>>(nv12FrameSize(w, h), fill);
}

TEST(SinkVirtualCameraPublisher, ConnectFailureIsACameraRetryableFailure) {
  Rig rig;
  rig.state->connectResults.push_back("CAMERA_EXTENSION_NOT_FOUND: x");
  EXPECT_FALSE(rig.camera->start(1920, 1080, 60));
  const auto status = rig.camera->status();
  EXPECT_EQ(status.state, "failed");
  EXPECT_FALSE(status.enabled);
  EXPECT_EQ(status.warning.rfind("CAMERA_", 0), 0u) << status.warning;
  EXPECT_EQ(status.warning, "CAMERA_EXTENSION_NOT_FOUND: x");
  // Nothing is fed into a sink that never connected.
  rig.camera->publishNv12IdentifiedOnWorker(frame(1920, 1080), 1920, 1080, 1, 0);
  EXPECT_EQ(rig.state->enqueueCalls, 0);
}

TEST(SinkVirtualCameraPublisher, PublishesOnlyTheDeclaredSizeAndCountsAcceptedFrames) {
  Rig rig;
  ASSERT_TRUE(rig.camera->start(1920, 1080, 60));
  EXPECT_EQ(rig.camera->status().state, "live");
  EXPECT_TRUE(rig.camera->status().enabled);
  rig.camera->publishNv12IdentifiedOnWorker(frame(1280, 720), 1280, 720, 1, 0);
  EXPECT_EQ(rig.state->held.size(), 0u);
  EXPECT_EQ(rig.camera->status().framesPublished, 0u);
  rig.camera->publishNv12IdentifiedOnWorker(frame(1920, 1080), 1920, 1080, 2, 0);
  EXPECT_EQ(rig.state->held.size(), 1u);
  EXPECT_EQ(rig.camera->status().framesPublished, 1u);
  // The host clock stamps the enqueue (it becomes the sample's presentation time).
  ASSERT_EQ(rig.state->hostTimes.size(), 1u);
  EXPECT_EQ(rig.state->hostTimes[0], rig.clock);
}

TEST(SinkVirtualCameraPublisher, MirrorIsUpstreamAndBytesPassThroughUntouched) {
  Rig rig;
  ASSERT_TRUE(rig.camera->start(1920, 1080, 60));
  rig.camera->setMirror(true);
  EXPECT_TRUE(rig.camera->mirrorAppliedUpstream());
  // Y row ramp: column 0 = 0, last column = 255. A CPU mirror would swap them.
  auto bytes = std::make_shared<std::vector<uint8_t>>(nv12FrameSize(1920, 1080), 128);
  for (int row = 0; row < 1080; ++row)
    for (int col = 0; col < 1920; ++col)
      (*bytes)[static_cast<size_t>(row) * 1920 + col] = static_cast<uint8_t>((col * 255) / 1919);
  const std::vector<uint8_t> original = *bytes;
  rig.camera->publishNv12IdentifiedOnWorker(bytes, 1920, 1080, 1, 0);
  ASSERT_EQ(rig.state->held.size(), 1u);
  const auto& seen = rig.state->held[0];
  EXPECT_EQ(seen[0], 0);
  EXPECT_EQ(seen[1919], 255);
  EXPECT_TRUE(seen == original) << "the sink must receive the bytes exactly as published";
  EXPECT_TRUE(*bytes == original) << "the shared clean frame must never be modified";
}

TEST(SinkVirtualCameraPublisher, CpuProgramFramesAreRefused) {
  Rig rig;
  ASSERT_TRUE(rig.camera->start(1920, 1080, 60));
  ProgramFrame program;
  program.frameNumber = 7;
  program.programFullBgra.width = 1920;
  program.programFullBgra.height = 1080;
  program.programFullBgra.bgra.assign(static_cast<size_t>(1920) * 1080 * 4, 0);
  for (size_t i = 0; i < program.programFullBgra.bgra.size(); i += 4) {
    program.programFullBgra.bgra[i + 2] = 255;  // red
    program.programFullBgra.bgra[i + 3] = 255;
  }
  rig.camera->publish(program);
  rig.camera->publish(program);
  EXPECT_EQ(rig.state->enqueueCalls, 0);
  EXPECT_EQ(rig.camera->status().framesPublished, 0u);
  EXPECT_EQ(rig.camera->status().state, "live");
}

TEST(SinkVirtualCameraPublisher, CountsProgramSequenceGaps) {
  Rig rig;
  ASSERT_TRUE(rig.camera->start(1920, 1080, 60));
  for (int64_t sequence : {1, 2, 3, 6, 7})
    rig.camera->publishNv12IdentifiedOnWorker(frame(1920, 1080), 1920, 1080, sequence, 0);
  EXPECT_EQ(rig.camera->sequenceGapsForTest(), 2u);
  EXPECT_EQ(rig.camera->status().framesPublished, 5u);
}

TEST(SinkVirtualCameraPublisher, AFullQueueDropsTheNewFrameWithoutFailing) {
  Rig rig;
  rig.state->capacity = 2;
  ASSERT_TRUE(rig.camera->start(1920, 1080, 60));
  for (int i = 1; i <= 5; ++i)
    rig.camera->publishNv12IdentifiedOnWorker(frame(1920, 1080, static_cast<uint8_t>(i)), 1920, 1080, i, 0);
  EXPECT_EQ(rig.state->held.size(), 2u);
  // The dropped frames are the NEW ones; the held two are the first two.
  EXPECT_EQ(rig.state->held[0][0], 1);
  EXPECT_EQ(rig.state->held[1][0], 2);
  const auto status = rig.camera->status();
  EXPECT_EQ(status.framesPublished, 2u);
  EXPECT_EQ(status.state, "live");
  EXPECT_TRUE(status.enabled);
  EXPECT_TRUE(status.warning.empty()) << status.warning;
  EXPECT_EQ(rig.state->disconnects, 0);
}

TEST(SinkVirtualCameraPublisher, ALostSinkReconnectsWithoutAToggle) {
  Rig rig;
  ASSERT_TRUE(rig.camera->start(1920, 1080, 60));
  rig.state->loseOnNextEnqueue = true;
  rig.camera->publishNv12IdentifiedOnWorker(frame(1920, 1080), 1920, 1080, 1, 0);
  auto status = rig.camera->status();
  EXPECT_EQ(status.state, "failed");
  EXPECT_FALSE(status.enabled);
  EXPECT_EQ(status.warning.rfind("CAMERA_EXTENSION_LOST:", 0), 0u) << status.warning;
  EXPECT_EQ(rig.state->disconnects, 1);
  EXPECT_EQ(status.framesPublished, 0u);
  // Frames arriving while lost go nowhere.
  rig.camera->publishNv12IdentifiedOnWorker(frame(1920, 1080), 1920, 1080, 2, 0);
  EXPECT_EQ(rig.state->enqueueCalls, 1);
  // The owning worker's retry (stop + start, the Windows registration-retry shape).
  rig.camera->stop();
  EXPECT_EQ(rig.state->disconnects, 1) << "a sink already disconnected on loss is not disconnected twice";
  ASSERT_TRUE(rig.camera->start(1920, 1080, 60));
  EXPECT_EQ(rig.state->connects, 2);
  status = rig.camera->status();
  EXPECT_EQ(status.state, "live");
  EXPECT_TRUE(status.enabled);
  EXPECT_TRUE(status.warning.empty()) << status.warning;
  rig.camera->publishNv12IdentifiedOnWorker(frame(1920, 1080), 1920, 1080, 3, 0);
  EXPECT_EQ(rig.state->held.size(), 1u);
  EXPECT_EQ(rig.camera->status().framesPublished, 1u);
}

// Review Focus 1 (Task 7): a queue that stays full for ~2 s with no device change
// means the extension stopped draining. That is a loss: disconnect, report
// CAMERA_EXTENSION_LOST (the shell reads it as "connecting"; the async worker
// retries on its 1 s cadence). The Rig clock advances 16.67 ms per enqueue.
TEST(SinkVirtualCameraPublisher, AQueueFullForTwoSecondsIsALostSink) {
  Rig rig;
  rig.state->capacity = 1;
  ASSERT_TRUE(rig.camera->start(1920, 1080, 60));
  rig.camera->publishNv12IdentifiedOnWorker(frame(1920, 1080), 1920, 1080, 1, 0);  // Ok, fills the queue
  // 1.5 s of continuous QueueFull: still live, nothing disconnected.
  for (int i = 2; i <= 91; ++i)
    rig.camera->publishNv12IdentifiedOnWorker(frame(1920, 1080), 1920, 1080, i, 0);
  EXPECT_EQ(rig.camera->status().state, "live");
  EXPECT_EQ(rig.state->disconnects, 0);
  // Past 2 s of continuous QueueFull: lost.
  for (int i = 92; i <= 140; ++i)
    rig.camera->publishNv12IdentifiedOnWorker(frame(1920, 1080), 1920, 1080, i, 0);
  auto status = rig.camera->status();
  EXPECT_EQ(status.state, "failed");
  EXPECT_FALSE(status.enabled);
  EXPECT_EQ(status.warning.rfind("CAMERA_EXTENSION_LOST:", 0), 0u) << status.warning;
  EXPECT_EQ(rig.state->disconnects, 1);
  // The retry reconnects, and the stall clock starts over: one QueueFull right
  // after the reconnect is not a loss.
  rig.camera->stop();
  ASSERT_TRUE(rig.camera->start(1920, 1080, 60));
  rig.camera->publishNv12IdentifiedOnWorker(frame(1920, 1080), 1920, 1080, 141, 0);
  status = rig.camera->status();
  EXPECT_EQ(status.state, "live");
  EXPECT_EQ(rig.state->disconnects, 1);
}

TEST(SinkVirtualCameraPublisher, AnAcceptedFrameRestartsTheStallClock) {
  Rig rig;
  rig.state->capacity = 1;
  ASSERT_TRUE(rig.camera->start(1920, 1080, 60));
  rig.camera->publishNv12IdentifiedOnWorker(frame(1920, 1080), 1920, 1080, 1, 0);
  for (int i = 2; i <= 91; ++i)  // 1.5 s full
    rig.camera->publishNv12IdentifiedOnWorker(frame(1920, 1080), 1920, 1080, i, 0);
  rig.state->held.clear();  // the consumer drained once
  rig.camera->publishNv12IdentifiedOnWorker(frame(1920, 1080), 1920, 1080, 92, 0);  // Ok
  for (int i = 93; i <= 182; ++i)  // another 1.5 s full: 3 s of mostly-full, never 2 s continuous
    rig.camera->publishNv12IdentifiedOnWorker(frame(1920, 1080), 1920, 1080, i, 0);
  const auto status = rig.camera->status();
  EXPECT_EQ(status.state, "live");
  EXPECT_TRUE(status.warning.empty()) << status.warning;
  EXPECT_EQ(rig.state->disconnects, 0);
  EXPECT_EQ(status.framesPublished, 2u);
}

TEST(SinkVirtualCameraPublisher, StopDisconnectsAndReportsOff) {
  Rig rig;
  ASSERT_TRUE(rig.camera->start(1920, 1080, 60));
  rig.camera->stop();
  EXPECT_EQ(rig.state->disconnects, 1);
  EXPECT_EQ(rig.camera->status().state, "off");
  EXPECT_FALSE(rig.camera->status().enabled);
  rig.camera->publishNv12IdentifiedOnWorker(frame(1920, 1080), 1920, 1080, 1, 0);
  EXPECT_EQ(rig.state->enqueueCalls, 0);
}

}  // namespace

// Live 2026-11-15: the core retried every second but logged only the first
// failure, so "is the retry running?" had no answer in the log.
TEST(SinkVirtualCameraPublisher, RepeatedConnectFailuresAreCountedAndResetByAConnect) {
  Rig rig;
  for (int i = 0; i < 3; ++i) rig.state->connectResults.push_back("CAMERA_EXTENSION_NOT_FOUND: x");
  for (int i = 0; i < 3; ++i) {
    EXPECT_FALSE(rig.camera->start(1920, 1080, 60));
    rig.camera->stop();
  }
  EXPECT_EQ(rig.camera->consecutiveConnectFailuresForTest(), 3u);
  EXPECT_EQ(rig.state->connects, 3);
  EXPECT_TRUE(rig.camera->start(1920, 1080, 60));
  EXPECT_EQ(rig.camera->consecutiveConnectFailuresForTest(), 0u);
  EXPECT_EQ(rig.camera->status().state, "live");
}

TEST(SinkVirtualCameraPublisher, ARepeatedFailureIsLoggedAsAHeartbeatNotEverySecond) {
  EXPECT_FALSE(shouldLogRepeatedSinkConnectFailure(0));
  EXPECT_FALSE(shouldLogRepeatedSinkConnectFailure(1));
  EXPECT_FALSE(shouldLogRepeatedSinkConnectFailure(29));
  EXPECT_TRUE(shouldLogRepeatedSinkConnectFailure(30));
  EXPECT_FALSE(shouldLogRepeatedSinkConnectFailure(31));
  EXPECT_TRUE(shouldLogRepeatedSinkConnectFailure(60));
}
