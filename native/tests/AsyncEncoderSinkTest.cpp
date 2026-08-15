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
using corevideo::modules::IsoSourceAudio;
using corevideo::modules::IsoSourceVideoFrame;
using corevideo::modules::OutputSession;
using corevideo::modules::ProgramFrame;
using corevideo::modules::RecordingSessionRequest;

// A wrapped encoder whose blocking behavior the test controls: submit() and
// stopRecording() spin while their "block" flag is set, so we can prove the
// AsyncEncoderSink layer keeps the caller (the audio worker) non-blocking and
// that its finalize/teardown are grace-bounded.
class ControllableEncoder final : public IEncoderSink {
 public:
  std::shared_ptr<std::atomic<bool>> blockSubmit = std::make_shared<std::atomic<bool>>(false);
  std::shared_ptr<std::atomic<bool>> blockStop = std::make_shared<std::atomic<bool>>(false);
  std::shared_ptr<std::atomic<bool>> blockIsoSubmit = std::make_shared<std::atomic<bool>>(false);
  std::shared_ptr<std::atomic<bool>> destroyed = std::make_shared<std::atomic<bool>>(false);
  std::atomic<bool> submitEntered{false};
  std::atomic<int> submitCount{0};
  std::atomic<int> lastFrameNumber{-1};
  std::atomic<int> audioCount{0};
  std::atomic<int> isoAudioCount{0};
  std::atomic<int> isoVideoCount{0};
  std::atomic<int> programCountAtSecondIso{0};
  std::atomic<bool> isoSubmitEntered{false};
  std::atomic<int64_t> lastProgramTimelineTimestamp100ns{0};
  std::atomic<int64_t> lastIsoVideoTimelineTimestamp100ns{0};
  std::atomic<int64_t> lastIsoTimelineTimestamp100ns{0};
  std::atomic<int> stopCount{0};

  ~ControllableEncoder() override { destroyed->store(true); }

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
    submitEntered.store(true);
    while (blockSubmit->load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    lastFrameNumber.store(static_cast<int>(frame.frameNumber));
    lastProgramTimelineTimestamp100ns.store(frame.timelineTimestamp100ns);
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

  void submitIsoAudio(const std::vector<IsoSourceAudio>& sources) override {
    if (sources.empty()) {
      return;
    }
    lastIsoTimelineTimestamp100ns.store(sources.front().timelineTimestamp100ns);
    ++isoAudioCount;
  }

  void submitIsoVideo(const std::vector<IsoSourceVideoFrame>& sources) override {
    if (sources.empty()) {
      return;
    }
    lastIsoVideoTimelineTimestamp100ns.store(sources.front().timelineTimestamp100ns);
    isoSubmitEntered.store(true);
    while (blockIsoSubmit->load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const int count = ++isoVideoCount;
    if (count == 2) {
      programCountAtSecondIso.store(submitCount.load());
    }
  }

  void stopRecording() override {
    while (blockStop->load()) {
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

TEST(AsyncEncoderSink, PreservesIsoAudioCaptureTimestampAcrossWriterQueue) {
  auto inner = std::make_unique<ControllableEncoder>();
  auto* raw = inner.get();
  AsyncEncoderSink sink(std::move(inner));
  sink.start({"recording"}, {"zoom:A"});

  IsoSourceAudio stem;
  stem.sourceId = "zoom:A";
  stem.timelineTimestamp100ns = 123'456'789;
  sink.submitIsoAudio({stem});

  ASSERT_TRUE(sink.drainForTest(std::chrono::seconds(2)));
  EXPECT_EQ(raw->isoAudioCount.load(), 1);
  EXPECT_EQ(raw->lastIsoTimelineTimestamp100ns.load(), 123'456'789);
}

TEST(AsyncEncoderSink, PreservesProgramAndIsoVideoCaptureTimestampsAcrossWriterQueue) {
  auto inner = std::make_unique<ControllableEncoder>();
  auto* raw = inner.get();
  AsyncEncoderSink sink(std::move(inner));
  sink.start({"recording"}, {"zoom:A"});

  auto program = videoFrame(42);
  program.timelineTimestamp100ns = 222'333'444;
  sink.submit(program);

  IsoSourceVideoFrame iso;
  iso.sourceId = "zoom:A";
  iso.timelineTimestamp100ns = 222'333'555;
  sink.submitIsoVideo({iso});

  ASSERT_TRUE(sink.drainForTest(std::chrono::seconds(2)));
  EXPECT_EQ(raw->lastProgramTimelineTimestamp100ns.load(), 222'333'444);
  EXPECT_EQ(raw->lastIsoVideoTimelineTimestamp100ns.load(), 222'333'555);
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
  raw->blockSubmit->store(true);  // wedge the wrapped writer like a disk stall
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
  raw->blockSubmit->store(false);
  ASSERT_TRUE(sink.drainForTest(std::chrono::seconds(2)));
  EXPECT_EQ(raw->lastFrameNumber.load(), total);
  EXPECT_LT(raw->submitCount.load(), total) << "no frames were dropped despite the backlog";
  EXPECT_EQ(sink.session().encoderQueueDroppedVideoFrames,
            static_cast<int64_t>(sink.droppedVideoFrames()));
  EXPECT_GT(sink.session().encoderQueueDroppedVideoFrames, 0);
}

TEST(AsyncEncoderSink, StopRecordingIsNonBlockingEvenWhenWriterIsStuck) {
  // The caller holds coreMutex, so stop-recording must return instantly and let
  // the finalize happen on the writer thread — never stall the operator/render.
  auto inner = std::make_unique<ControllableEncoder>();
  auto* raw = inner.get();
  raw->blockStop->store(true);  // finalize would block if it ran on the caller
  AsyncEncoderSink sink(std::move(inner));
  sink.start({"recording"}, {});

  const auto t0 = std::chrono::steady_clock::now();
  sink.stopRecording();
  const auto elapsedMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  EXPECT_LT(elapsedMs, 500) << "stop-recording blocked the caller on the stuck finalize";

  raw->blockStop->store(false);  // release so the writer can finalize + exit cleanly
}

TEST(AsyncEncoderSink, ProgramMediaRunsBetweenIndividualIsoEncodes) {
  auto inner = std::make_unique<ControllableEncoder>();
  auto* raw = inner.get();
  raw->blockIsoSubmit->store(true);
  AsyncEncoderSink sink(std::move(inner));
  sink.start({"recording"}, {});
  ASSERT_TRUE(sink.drainForTest(std::chrono::seconds(2)));

  std::vector<IsoSourceVideoFrame> isoFrames;
  for (int source = 0; source < 8; ++source) {
    IsoSourceVideoFrame frame;
    frame.sourceId = "zoom:" + std::to_string(source + 1);
    isoFrames.push_back(std::move(frame));
  }
  sink.submitIsoVideo(isoFrames);
  const auto enteredDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!raw->isoSubmitEntered.load() && std::chrono::steady_clock::now() < enteredDeadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(raw->isoSubmitEntered.load());

  for (int number = 1; number <= 5; ++number) {
    sink.submit(videoFrame(number));
  }
  raw->blockIsoSubmit->store(false);
  ASSERT_TRUE(sink.drainForTest(std::chrono::seconds(2)));

  EXPECT_EQ(raw->isoVideoCount.load(), 8);
  EXPECT_GT(raw->programCountAtSecondIso.load(), 0)
      << "the writer encoded consecutive ISO sources while Program video was waiting";
  EXPECT_LE(raw->programCountAtSecondIso.load(), 4)
      << "continuous Program traffic starved the next ISO encode";
}

TEST(AsyncEncoderSink, HeldProgramAndIsoFramesDoNotFloodTheQueueOrCountAsDrops) {
  auto inner = std::make_unique<ControllableEncoder>();
  auto* raw = inner.get();
  AsyncEncoderSink sink(std::move(inner));
  sink.start({"recording"}, {"zoom:A"});

  auto program = videoFrame(42);
  IsoSourceVideoFrame iso;
  iso.sourceId = "zoom:A";
  iso.frame.frameId = 77;
  for (int tick = 0; tick < 500; ++tick) {
    sink.submit(program);
    sink.submitIsoVideo({iso});
  }

  ASSERT_TRUE(sink.drainForTest(std::chrono::seconds(2)));
  EXPECT_EQ(raw->submitCount.load(), 1);
  EXPECT_EQ(raw->isoVideoCount.load(), 1);
  EXPECT_EQ(sink.droppedVideoFrames(), 0u)
      << "held frames were reported as encoder capacity drops";
}

TEST(AsyncEncoderSink, StopDrainsAcceptedMediaBeforeFinalizingAndRejectsLateMedia) {
  auto inner = std::make_unique<ControllableEncoder>();
  auto* raw = inner.get();
  raw->blockSubmit->store(true);
  AsyncEncoderSink sink(std::move(inner));
  sink.start({"recording"}, {});

  // Pin the writer inside one media item, then build a representative backlog.
  sink.submit(videoFrame(1));
  const auto enteredDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!raw->submitEntered.load() && std::chrono::steady_clock::now() < enteredDeadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(raw->submitEntered.load());
  for (int frame = 2; frame <= 5; ++frame) {
    sink.submit(videoFrame(frame));
  }
  const float pcm[4] = {0.1f, -0.1f, 0.2f, -0.2f};
  for (int packet = 0; packet < 5; ++packet) {
    sink.submitAudio(pcm, 2, 2, 48000);
  }

  sink.stopRecording();
  // Stop closes the producer gate synchronously. These must not be accepted
  // behind the Finalize barrier or contaminate a rapid next take.
  sink.submit(videoFrame(99));
  sink.submitAudio(pcm, 2, 2, 48000);
  raw->blockSubmit->store(false);
  ASSERT_TRUE(sink.drainForTest(std::chrono::seconds(2)));

  EXPECT_EQ(raw->stopCount.load(), 1);
  EXPECT_EQ(raw->submitCount.load(), 5)
      << "accepted program frames were discarded from the recording tail";
  EXPECT_EQ(raw->lastFrameNumber.load(), 5)
      << "a post-Stop frame crossed the Finalize barrier";
  EXPECT_EQ(raw->audioCount.load(), 5)
      << "accepted audio packets were discarded from the recording tail";
  EXPECT_EQ(sink.session().recordingStatus, "stopped");
  EXPECT_FALSE(sink.session().active);
}

TEST(AsyncEncoderSink, TeardownIsBoundedWhenWriterIsStuck) {
  AsyncEncoderSink::Options options;
  options.finalizeGrace = std::chrono::milliseconds(150);
  auto inner = std::make_unique<ControllableEncoder>();
  const auto blockSubmit = inner->blockSubmit;
  const auto destroyed = inner->destroyed;
  blockSubmit->store(true);  // wedge the writer inside a submit
  auto sink = std::make_unique<AsyncEncoderSink>(std::move(inner), options);
  sink->start({"recording"}, {});
  sink->submit(videoFrame(1));  // this one wedges the writer

  const auto t0 = std::chrono::steady_clock::now();
  sink.reset();  // destructor must not hang on the stuck writer
  const auto elapsedMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  EXPECT_LT(elapsedMs, 1500) << "teardown hung on the stuck finalize";

  // Never touch the wrapped encoder through a raw pointer after detach: it may
  // destroy itself immediately after observing the release. Shared test gates
  // let us release and positively observe destruction without a lifetime race.
  blockSubmit->store(false);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!destroyed->load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_TRUE(destroyed->load());
}
