#include "modules/AsyncEncoderSink.h"
#include "modules/Interfaces.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

namespace {

using corevideo::modules::AsyncEncoderSink;
using corevideo::modules::IEncoderSink;
using corevideo::modules::OutputSession;
using corevideo::modules::ProgramFrame;
using corevideo::modules::RecordingSessionRequest;

// A wrapped encoder whose blocking behavior the test controls: submit() and
// stopRecording() spin while their "block" flag is set, so we can prove the
// AsyncEncoderSink layer keeps the caller (the audio worker) non-blocking and
// that its finalize/teardown are grace-bounded.
class ControllableEncoder final : public IEncoderSink {
 public:
  std::atomic<bool> blockSubmit{false};
  std::atomic<bool> blockStop{false};
  std::atomic<int> submitCount{0};
  std::atomic<int> lastFrameNumber{-1};
  std::atomic<int> audioCount{0};
  std::atomic<int> stopCount{0};

  void configureRecording(const RecordingSessionRequest& request) override {
    std::lock_guard<std::mutex> lock(mutex_);
    session_.recordingSessionId = request.sessionId;
  }

  OutputSession start(const std::vector<std::string>& destinations,
                      const std::vector<std::string>& /*isoParticipantIds*/) override {
    std::lock_guard<std::mutex> lock(mutex_);
    session_.active = true;
    session_.destinations = destinations;
    session_.recordingStatus = "recording";
    return session_;
  }

  void submit(const ProgramFrame& frame) override {
    while (blockSubmit.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    lastFrameNumber.store(static_cast<int>(frame.frameNumber));
    const int count = ++submitCount;
    std::lock_guard<std::mutex> lock(mutex_);
    session_.encodedFrameCount = count;
    session_.recordingVideoFrameCount = count;
    session_.recordingLastFrameNumber = frame.frameNumber;
  }

  void submitAudio(const float* /*interleaved*/, int frameCount, int /*channels*/, int /*sampleRate*/) override {
    if (frameCount <= 0) {
      return;
    }
    const int count = ++audioCount;
    std::lock_guard<std::mutex> lock(mutex_);
    session_.recordingAudioPacketCount = count;
  }

  void stopRecording() override {
    while (blockStop.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ++stopCount;
    std::lock_guard<std::mutex> lock(mutex_);
    session_.recordingStatus = "stopped";
  }

  OutputSession session() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return session_;
  }

 private:
  mutable std::mutex mutex_;
  OutputSession session_;
};

ProgramFrame videoFrame(int64_t number) {
  ProgramFrame frame;
  frame.frameNumber = number;
  frame.width = 1920;
  frame.height = 1080;
  return frame;
}

}  // namespace

TEST(AsyncEncoderSink, IdleSubmitBeforeStartIsDroppedWithoutTouchingInner) {
  auto inner = std::make_unique<ControllableEncoder>();
  auto* raw = inner.get();
  AsyncEncoderSink sink(std::move(inner));

  // No start() yet: the wrapped submit is a no-op, so nothing should be enqueued.
  sink.submit(videoFrame(1));
  sink.submit(videoFrame(2));
  ASSERT_TRUE(sink.drainForTest(std::chrono::seconds(1)));
  EXPECT_EQ(raw->submitCount.load(), 0);
}

TEST(AsyncEncoderSink, PassesFramesAndAudioThroughInOrderWhenNotOverloaded) {
  auto inner = std::make_unique<ControllableEncoder>();
  auto* raw = inner.get();
  AsyncEncoderSink sink(std::move(inner));

  RecordingSessionRequest request;
  request.sessionId = "async-show";
  sink.configureRecording(request);
  const auto started = sink.start({"recording"}, {});
  EXPECT_TRUE(started.active);

  for (int i = 1; i <= 5; ++i) {
    sink.submit(videoFrame(i));
  }
  const float pcm[4] = {0.1f, -0.1f, 0.2f, -0.2f};
  sink.submitAudio(pcm, 2, 2, 48000);

  ASSERT_TRUE(sink.drainForTest(std::chrono::seconds(2)));
  EXPECT_EQ(raw->submitCount.load(), 5);
  EXPECT_EQ(raw->lastFrameNumber.load(), 5);
  EXPECT_EQ(raw->audioCount.load(), 1);

  const auto session = sink.session();
  EXPECT_EQ(session.recordingVideoFrameCount, 5);
  EXPECT_EQ(session.recordingLastFrameNumber, 5);
  EXPECT_EQ(session.recordingAudioPacketCount, 1);
  EXPECT_EQ(sink.droppedVideoFrames(), 0u);
}

TEST(AsyncEncoderSink, SubmitIsNonBlockingAndDropsToLatestUnderBacklog) {
  AsyncEncoderSink::Options options;
  options.maxVideoQueue = 4;
  auto inner = std::make_unique<ControllableEncoder>();
  auto* raw = inner.get();
  raw->blockSubmit.store(true);  // wedge the wrapped writer like a disk stall
  AsyncEncoderSink sink(std::move(inner), options);
  sink.start({"recording"}, {});

  // Fire far more frames than the queue can hold. Each submit must return fast
  // even though the wrapped encoder is blocked — that is the whole point.
  const int total = 200;
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 1; i <= total; ++i) {
    sink.submit(videoFrame(i));
  }
  const auto elapsedMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  EXPECT_LT(elapsedMs, 500) << "submit() blocked behind the stalled encoder";

  // Backlog is bounded and older frames were dropped (drop-to-latest).
  EXPECT_GT(sink.droppedVideoFrames(), 0u);

  // Let the writer catch up; the freshest frame must be the last one submitted.
  raw->blockSubmit.store(false);
  ASSERT_TRUE(sink.drainForTest(std::chrono::seconds(2)));
  EXPECT_EQ(raw->lastFrameNumber.load(), total);
  EXPECT_LT(raw->submitCount.load(), total) << "no frames were dropped despite the backlog";
}

TEST(AsyncEncoderSink, StopRecordingIsNonBlockingEvenWhenWriterIsStuck) {
  // The caller holds coreMutex, so stop-recording must return instantly and let
  // the finalize happen on the writer thread — never stall the operator/render.
  auto inner = std::make_unique<ControllableEncoder>();
  auto* raw = inner.get();
  raw->blockStop.store(true);  // finalize would block if it ran on the caller
  AsyncEncoderSink sink(std::move(inner));
  sink.start({"recording"}, {});

  const auto t0 = std::chrono::steady_clock::now();
  sink.stopRecording();
  const auto elapsedMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  EXPECT_LT(elapsedMs, 500) << "stop-recording blocked the caller on the stuck finalize";

  raw->blockStop.store(false);  // release so the writer can finalize + exit cleanly
}

TEST(AsyncEncoderSink, TeardownIsBoundedWhenWriterIsStuck) {
  AsyncEncoderSink::Options options;
  options.finalizeGrace = std::chrono::milliseconds(150);
  auto inner = std::make_unique<ControllableEncoder>();
  auto* raw = inner.get();
  raw->blockSubmit.store(true);  // wedge the writer inside a submit
  auto sink = std::make_unique<AsyncEncoderSink>(std::move(inner), options);
  sink->start({"recording"}, {});
  sink->submit(videoFrame(1));  // this one wedges the writer

  const auto t0 = std::chrono::steady_clock::now();
  sink.reset();  // destructor must not hang on the stuck writer
  const auto elapsedMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  EXPECT_LT(elapsedMs, 1500) << "teardown hung on the stuck finalize";

  raw->blockSubmit.store(false);  // let the detached writer complete + free State
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
}
