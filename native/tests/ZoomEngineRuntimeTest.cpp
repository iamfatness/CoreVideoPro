#include "modules/ZoomEngineRuntime.h"

#include "modules/ZoomEngineProcess.h"

#include "engine-ipc.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace {

void setEnv(const char* name, const char* value) {
#if defined(_WIN32)
  _putenv_s(name, value);
#else
  setenv(name, value, 1);
#endif
}

void unsetEnv(const char* name) {
#if defined(_WIN32)
  _putenv_s(name, "");
#else
  unsetenv(name);
#endif
}

// Fake engine process client for the phase 2 increment 3 sender-thread tests:
// records every line handed to sendLine (in order), can block sends on a latch
// (simulating a wedged engine pipe), and can report the process as dead.
class FakeZoomEngineProcessClient : public corevideo::modules::ZoomEngineProcessClient {
 public:
  explicit FakeZoomEngineProcessClient(bool blockSends = false) : blockSends_(blockSends) {}
  ~FakeZoomEngineProcessClient() override { releaseBlockedSends(); }

  bool sendLine(const std::string& line) override {
    std::unique_lock<std::mutex> lock(mx_);
    ++enteredSends_;
    cv_.notify_all();
    cv_.wait(lock, [&] { return !blockSends_ || released_; });
    lines_.push_back(line);
    cv_.notify_all();
    return true;
  }

  [[nodiscard]] bool running() const override { return running_.load(); }

  void stop() override {
    // Mirrors the real client: process termination breaks the pipe, which
    // unblocks any in-flight write.
    running_.store(false);
    releaseBlockedSends();
  }

  [[nodiscard]] std::string lastError() const override { return "fake engine pipe error"; }

  void setRunning(bool value) { running_.store(value); }

  void releaseBlockedSends() {
    {
      std::lock_guard<std::mutex> lock(mx_);
      released_ = true;
    }
    cv_.notify_all();
  }

  // Waits until `count` sendLine calls have STARTED (they may still be blocked).
  bool waitForEnteredSends(std::size_t count, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mx_);
    return cv_.wait_for(lock, timeout, [&] { return enteredSends_ >= count; });
  }

  // Waits until `count` lines have been fully recorded.
  bool waitForSentLines(std::size_t count, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mx_);
    return cv_.wait_for(lock, timeout, [&] { return lines_.size() >= count; });
  }

  [[nodiscard]] std::vector<std::string> sentLines() const {
    std::lock_guard<std::mutex> lock(mx_);
    return lines_;
  }

 private:
  mutable std::mutex mx_;
  std::condition_variable cv_;
  std::vector<std::string> lines_;
  std::size_t enteredSends_ = 0;
  bool blockSends_ = false;
  bool released_ = false;
  std::atomic<bool> running_{true};
};

corevideo::rpc::Json subscriptionRequest(const char* participantId, const char* kind, const char* purpose) {
  return corevideo::rpc::Json::Object{
      {"participantId", participantId},
      {"kind", kind},
      {"purpose", purpose},
  };
}

corevideo::rpc::Json spinePayload(corevideo::rpc::Json::Array subscriptions) {
  return corevideo::rpc::Json::Object{
      {"startCapture", false},
      {"subscriptions", std::move(subscriptions)},
  };
}

}  // namespace

TEST(ZoomEngineRuntime, IsDisabledWhenNoEnginePathIsConfigured) {
  unsetEnv("COREVIDEO_ZOOM_ENGINE_PATH");

  corevideo::modules::ZoomEngineRuntime runtime;

  EXPECT_FALSE(runtime.configured());
  EXPECT_TRUE(runtime.join(corevideo::rpc::Json::Object{}).isNull());
  EXPECT_TRUE(runtime.leave().isNull());
  EXPECT_TRUE(runtime.snapshot().isNull());
  EXPECT_TRUE(runtime.drainFrameEvents().empty());
}

TEST(ZoomEngineRuntime, RejectsJoinWithoutNumericMeetingIdBeforeLaunch) {
  setEnv("COREVIDEO_ZOOM_ENGINE_PATH", "C:/missing/corevideo-zoom-engine.exe");

  corevideo::modules::ZoomEngineRuntime runtime;
  const auto snapshot = runtime.join(corevideo::rpc::Json::Object{
      {"meetingUrl", "https://zoom.us/j/not-a-meeting"},
      {"displayName", "Operator"},
      {"webinar", false},
  });

  EXPECT_TRUE(runtime.configured());
  EXPECT_EQ(snapshot.getString("meetingState"), "error");
  ASSERT_NE(snapshot.get("warnings"), nullptr);
  EXPECT_FALSE(snapshot.get("warnings")->asArray().empty());

  unsetEnv("COREVIDEO_ZOOM_ENGINE_PATH");
}

// ---------------------------------------------------------------------------
// Phase 2 increment 3: outbound engine-command queue + dedicated sender thread.
// Engine pipe I/O must never happen on the caller's thread (syncSpine runs under
// coreMutex via MediaCore); ordering must be preserved; subscription dedup must
// stay correct with asynchronous sends; queued lines for a dead/replaced process
// are dropped; shutdown must never hang on a wedged pipe.
// ---------------------------------------------------------------------------

TEST(ZoomEngineRuntime, SenderThreadDeliversSpineCommandsInOrderWithDedup) {
  setEnv("COREVIDEO_ZOOM_ENGINE_PATH", "C:/fake/corevideo-zoom-engine.exe");
  auto fake = std::make_shared<FakeZoomEngineProcessClient>();
  {
    corevideo::modules::ZoomEngineRuntime runtime;
    runtime.installEngineProcessForTest(fake);

    const auto payload = spinePayload(corevideo::rpc::Json::Array{
        subscriptionRequest("101", "participant-video", "active-speaker"),
        subscriptionRequest("102", "participant-video", "multiview"),
        subscriptionRequest("103", "participant-audio", "mix"),
    });
    const auto snapshot = runtime.syncSpine(payload, 0.0);
    EXPECT_FALSE(snapshot.isNull());

    // The sender thread delivers all three subscribe commands, preserving the
    // payload order even though the sends are asynchronous.
    ASSERT_TRUE(fake->waitForSentLines(3, std::chrono::milliseconds(5000)));
    auto lines = fake->sentLines();
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_NE(lines[0].find("participant-video-101-camera"), std::string::npos);
    EXPECT_NE(lines[0].find("\"subscribe\""), std::string::npos);
    EXPECT_NE(lines[1].find("participant-video-102-camera"), std::string::npos);
    EXPECT_NE(lines[2].find("participant-audio-103-mix"), std::string::npos);
    EXPECT_NE(lines[2].find("subscribe_audio"), std::string::npos);

    // Dedup with async sends: the same subscription set enqueues nothing new.
    const auto repeat = runtime.syncSpine(payload, 0.0);
    EXPECT_FALSE(repeat.isNull());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(fake->sentLines().size(), 3u);

    // Dropping a subscription emits exactly one unsubscribe for its source uuid.
    const auto shrunk = runtime.syncSpine(spinePayload(corevideo::rpc::Json::Array{
                                              subscriptionRequest("101", "participant-video", "active-speaker"),
                                              subscriptionRequest("103", "participant-audio", "mix"),
                                          }),
                                          0.0);
    EXPECT_FALSE(shrunk.isNull());
    ASSERT_TRUE(fake->waitForSentLines(4, std::chrono::milliseconds(5000)));
    lines = fake->sentLines();
    ASSERT_EQ(lines.size(), 4u);
    EXPECT_NE(lines[3].find("unsubscribe"), std::string::npos);
    EXPECT_NE(lines[3].find("participant-video-102-camera"), std::string::npos);
  }
  unsetEnv("COREVIDEO_ZOOM_ENGINE_PATH");
}

TEST(ZoomEngineRuntime, PreviewToProgramKeepsStableVideoSubscriptionIdentity) {
  setEnv("COREVIDEO_ZOOM_ENGINE_PATH", "C:/fake/corevideo-zoom-engine.exe");
  auto fake = std::make_shared<FakeZoomEngineProcessClient>();
  {
    corevideo::modules::ZoomEngineRuntime runtime;
    runtime.installEngineProcessForTest(fake);

    EXPECT_FALSE(runtime.syncSpine(
        spinePayload(corevideo::rpc::Json::Array{
            subscriptionRequest("104", "participant-video", "preview"),
        }), 0.0).isNull());
    ASSERT_TRUE(fake->waitForSentLines(1, std::chrono::milliseconds(5000)));
    auto lines = fake->sentLines();
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_NE(lines[0].find("participant-video-104-camera"), std::string::npos);

    // Preview and Program use the same 720p tier. A Take only changes purpose,
    // so the warmed subscription and its latest SHM frame stay in place.
    EXPECT_FALSE(runtime.syncSpine(
        spinePayload(corevideo::rpc::Json::Array{
            subscriptionRequest("104", "participant-video", "program"),
        }), 0.0).isNull());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(fake->sentLines().size(), 1u);
  }
  unsetEnv("COREVIDEO_ZOOM_ENGINE_PATH");
}

TEST(ZoomEngineRuntime, MeetingMixUsesDedicatedNonIsolatedAudioSubscription) {
  setEnv("COREVIDEO_ZOOM_ENGINE_PATH", "C:/fake/corevideo-zoom-engine.exe");
  auto fake = std::make_shared<FakeZoomEngineProcessClient>();
  {
    corevideo::modules::ZoomEngineRuntime runtime;
    runtime.installEngineProcessForTest(fake);

    const auto payload = spinePayload(corevideo::rpc::Json::Array{
        subscriptionRequest("101", "meeting-audio", "program"),
    });
    EXPECT_FALSE(runtime.syncSpine(payload, 0.0).isNull());

    ASSERT_TRUE(fake->waitForSentLines(1, std::chrono::milliseconds(5000)));
    const auto lines = fake->sentLines();
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_NE(lines[0].find("meeting-audio-101-program"), std::string::npos);
    EXPECT_NE(lines[0].find("subscribe_audio"), std::string::npos);
    EXPECT_EQ(lines[0].find("isolate_audio"), std::string::npos);
  }
  unsetEnv("COREVIDEO_ZOOM_ENGINE_PATH");
}

TEST(ZoomEngineRuntime, SyncSpineNeverBlocksOnAWedgedEnginePipe) {
  setEnv("COREVIDEO_ZOOM_ENGINE_PATH", "C:/fake/corevideo-zoom-engine.exe");
  auto fake = std::make_shared<FakeZoomEngineProcessClient>(/*blockSends=*/true);
  {
    corevideo::modules::ZoomEngineRuntime runtime;
    runtime.installEngineProcessForTest(fake);

    // First sync: the subscribe line is handed to the sender, which blocks
    // inside sendLine — the wedged-pipe scenario.
    const auto t0 = std::chrono::steady_clock::now();
    const auto first = runtime.syncSpine(spinePayload(corevideo::rpc::Json::Array{
                                             subscriptionRequest("201", "participant-video", "multiview"),
                                         }),
                                         0.0);
    const auto firstMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    EXPECT_FALSE(first.isNull());
    EXPECT_LT(firstMs, 1000);  // returned a snapshot without waiting on the pipe
    ASSERT_TRUE(fake->waitForEnteredSends(1, std::chrono::milliseconds(5000)));

    // With the pipe still wedged (send blocked), the runtime stays responsive:
    // another spine sync and a snapshot both complete without pipe I/O.
    const auto t1 = std::chrono::steady_clock::now();
    const auto second = runtime.syncSpine(spinePayload(corevideo::rpc::Json::Array{
                                              subscriptionRequest("201", "participant-video", "multiview"),
                                              subscriptionRequest("202", "participant-video", "multiview"),
                                          }),
                                          0.0);
    const auto snapshot = runtime.snapshot();
    const auto othersMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t1).count();
    EXPECT_FALSE(second.isNull());
    EXPECT_FALSE(snapshot.isNull());
    EXPECT_LT(othersMs, 1000);
    EXPECT_TRUE(fake->sentLines().empty());  // still blocked mid-first-write

    // Unwedge: everything queued flows out, in order.
    fake->releaseBlockedSends();
    ASSERT_TRUE(fake->waitForSentLines(2, std::chrono::milliseconds(5000)));
    const auto lines = fake->sentLines();
    EXPECT_NE(lines[0].find("participant-video-201-camera"), std::string::npos);
    EXPECT_NE(lines[1].find("participant-video-202-camera"), std::string::npos);
  }
  unsetEnv("COREVIDEO_ZOOM_ENGINE_PATH");
}

TEST(ZoomEngineRuntime, ReplacingTheEngineProcessDropsQueuedLinesAndResubscribes) {
  setEnv("COREVIDEO_ZOOM_ENGINE_PATH", "C:/fake/corevideo-zoom-engine.exe");
  auto fakeA = std::make_shared<FakeZoomEngineProcessClient>(/*blockSends=*/true);
  auto fakeB = std::make_shared<FakeZoomEngineProcessClient>();
  {
    corevideo::modules::ZoomEngineRuntime runtime;
    runtime.installEngineProcessForTest(fakeA);

    const auto payload = spinePayload(corevideo::rpc::Json::Array{
        subscriptionRequest("301", "participant-video", "multiview"),
        subscriptionRequest("302", "participant-video", "multiview"),
    });
    const auto first = runtime.syncSpine(payload, 0.0);
    EXPECT_FALSE(first.isNull());
    // The sender is now blocked writing 301; 302 is still queued.
    ASSERT_TRUE(fakeA->waitForEnteredSends(1, std::chrono::milliseconds(5000)));
    EXPECT_EQ(runtime.pendingEngineSendCountForTest(), 1u);

    // Engine "restart": the queued 302 line targets the dead process and must be
    // dropped (never replayed into the new pipe); dedup resets so the new
    // process is re-subscribed from scratch.
    runtime.installEngineProcessForTest(fakeB);
    EXPECT_GE(runtime.droppedEngineSendCountForTest(), 1u);
    EXPECT_EQ(runtime.pendingEngineSendCountForTest(), 0u);
    fakeA->releaseBlockedSends();  // the in-flight 301 write completes harmlessly

    const auto second = runtime.syncSpine(payload, 0.0);
    EXPECT_FALSE(second.isNull());
    ASSERT_TRUE(fakeB->waitForSentLines(2, std::chrono::milliseconds(5000)));
    const auto lines = fakeB->sentLines();
    EXPECT_NE(lines[0].find("participant-video-301-camera"), std::string::npos);
    EXPECT_NE(lines[1].find("participant-video-302-camera"), std::string::npos);
  }
  unsetEnv("COREVIDEO_ZOOM_ENGINE_PATH");
}

TEST(ZoomEngineRuntime, DestructorDropsQueuedSendsAndJoinsWithoutHanging) {
  setEnv("COREVIDEO_ZOOM_ENGINE_PATH", "C:/fake/corevideo-zoom-engine.exe");
  auto fake = std::make_shared<FakeZoomEngineProcessClient>(/*blockSends=*/true);
  {
    corevideo::modules::ZoomEngineRuntime runtime;
    runtime.installEngineProcessForTest(fake);
    const auto snapshot = runtime.syncSpine(spinePayload(corevideo::rpc::Json::Array{
                                                subscriptionRequest("401", "participant-video", "multiview"),
                                                subscriptionRequest("402", "participant-video", "multiview"),
                                            }),
                                            0.0);
    EXPECT_FALSE(snapshot.isNull());
    ASSERT_TRUE(fake->waitForEnteredSends(1, std::chrono::milliseconds(5000)));
    // Destructor: drops the still-queued 402 line, stops the process (which
    // unblocks the in-flight 401 write), and joins the sender. Completing this
    // scope without hanging IS the assertion.
  }
  EXPECT_EQ(fake->sentLines().size(), 1u);
  unsetEnv("COREVIDEO_ZOOM_ENGINE_PATH");
}

// TSan fodder (runs under ThreadSanitizer in the native-stub-tsan CI job):
// hammer the enqueue path from one thread while the sender drains concurrently
// and a reader thread takes snapshots — the increment 3 handoff must be race-free.
TEST(ZoomEngineRuntime, ConcurrentSpineChurnSnapshotsAndSenderDrainAreRaceFree) {
  setEnv("COREVIDEO_ZOOM_ENGINE_PATH", "C:/fake/corevideo-zoom-engine.exe");
  auto fake = std::make_shared<FakeZoomEngineProcessClient>();
  {
    corevideo::modules::ZoomEngineRuntime runtime;
    runtime.installEngineProcessForTest(fake);

    const auto setA = spinePayload(corevideo::rpc::Json::Array{
        subscriptionRequest("501", "participant-video", "multiview"),
    });
    const auto setB = spinePayload(corevideo::rpc::Json::Array{
        subscriptionRequest("502", "participant-video", "multiview"),
        subscriptionRequest("503", "participant-audio", "mix"),
    });

    // NOTE: no assertions inside the worker threads — the vendored gtest shim
    // reports failures by throwing, which would std::terminate off-thread.
    std::atomic<int> nullSnapshots{0};
    std::thread churn([&] {
      for (int i = 0; i < 100; ++i) {
        if (runtime.syncSpine((i % 2 == 0) ? setA : setB, 0.0).isNull()) {
          ++nullSnapshots;
        }
      }
    });
    std::thread reads([&] {
      for (int i = 0; i < 100; ++i) {
        if (runtime.snapshot().isNull()) {
          ++nullSnapshots;
        }
        const auto events = runtime.drainFrameEvents();
        (void)events;
      }
    });
    churn.join();
    reads.join();
    EXPECT_EQ(nullSnapshots.load(), 0);

    // Every set flip emits subscribe/unsubscribe traffic; make sure the sender
    // actually drained a meaningful amount of it.
    EXPECT_TRUE(fake->waitForSentLines(10, std::chrono::milliseconds(5000)));
  }
  unsetEnv("COREVIDEO_ZOOM_ENGINE_PATH");
}

// Capture-off must actually reach the engine: stopCapture enqueues the
// stop_media command for the sender thread (never a direct pipe write), resets
// the media-started latch, and clears the subscription dedup so a fresh
// capture-on re-subscribes every source from scratch (the engine's
// stop_raw_media unsubscribed them all).
TEST(ZoomEngineRuntime, StopCaptureSendsStopMediaAndRearmsSubscriptionsFromScratch) {
  setEnv("COREVIDEO_ZOOM_ENGINE_PATH", "C:/fake/corevideo-zoom-engine.exe");
  auto fake = std::make_shared<FakeZoomEngineProcessClient>();
  {
    corevideo::modules::ZoomEngineRuntime runtime;
    runtime.installEngineProcessForTest(fake);

    const auto payload = spinePayload(corevideo::rpc::Json::Array{
        subscriptionRequest("601", "participant-video", "active-speaker"),
    });
    EXPECT_FALSE(runtime.syncSpine(payload, 0.0).isNull());
    ASSERT_TRUE(fake->waitForSentLines(1, std::chrono::milliseconds(5000)));

    // The engine reported raw media running (raw_media_status active:true).
    corevideo::modules::ZoomEngineEvent statusOn;
    statusOn.kind = corevideo::modules::ZoomEngineEventKind::RawMediaStatus;
    statusOn.command = "raw_media_status";
    statusOn.rawMediaActive = true;
    runtime.applyEngineEventForTest(statusOn);
    EXPECT_TRUE(runtime.snapshot().get("rawMediaActive")->asBool());

    // Capture off: exactly one stop_media line rides the sender thread.
    const auto stopSnapshot = runtime.stopCapture();
    EXPECT_FALSE(stopSnapshot.isNull());
    ASSERT_NE(stopSnapshot.get("rawMediaActive"), nullptr);
    ASSERT_TRUE(fake->waitForSentLines(2, std::chrono::milliseconds(5000)));
    auto lines = fake->sentLines();
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_NE(lines[1].find("\"stop_media\""), std::string::npos);

    // Engine ack flips the reported state to stopped.
    corevideo::modules::ZoomEngineEvent statusOff;
    statusOff.kind = corevideo::modules::ZoomEngineEventKind::RawMediaStatus;
    statusOff.command = "raw_media_status";
    statusOff.rawMediaActive = false;
    runtime.applyEngineEventForTest(statusOff);
    EXPECT_FALSE(runtime.snapshot().get("rawMediaActive")->asBool());

    // Capture on again: the SAME subscription payload must re-send the
    // subscribe (dedup was cleared — the engine forgot every source on stop).
    EXPECT_FALSE(runtime.syncSpine(payload, 0.0).isNull());
    ASSERT_TRUE(fake->waitForSentLines(3, std::chrono::milliseconds(5000)));
    lines = fake->sentLines();
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_NE(lines[2].find("participant-video-601-camera"), std::string::npos);
    EXPECT_NE(lines[2].find("\"subscribe\""), std::string::npos);
  }
  unsetEnv("COREVIDEO_ZOOM_ENGINE_PATH");
}

// End-to-end SHM audio ingest: create the per-source audio region the way the
// engine does, feed the matching pipe event through the reader path, and the
// next compositor audio poll must return REAL coalesced PCM for that
// participant (replacing the metadata-only placeholder).
TEST(ZoomEngineRuntime, IngestsIsoAudioPcmFromSharedMemoryIntoCompositorPoll) {
  unsetEnv("COREVIDEO_ZOOM_ENGINE_PATH");
  corevideo::modules::ZoomEngineRuntime runtime;

  // Unique region name per test process so parallel/leaked runs never alias.
  const std::string sourceUuid =
      "participant-audio-4242-mix-cvp-test-" + std::to_string(
#if defined(_WIN32)
          static_cast<unsigned long>(::GetCurrentProcessId())
#else
          static_cast<unsigned long>(::getpid())
#endif
      );

  const std::vector<std::int16_t> samples{0, 16384, -16384, 8192};
  const auto byteLength = static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));
  ShmRegion region{};
  ASSERT_TRUE(shm_region_create(
      region,
      corevideo::modules::zoomEngineAudioSharedMemoryName(sourceUuid),
      corevideo::modules::zoomEngineAudioRingByteSize()));
  // Ring layout (garbled-Zoom fix): header + sequenced slots.
  auto* header = static_cast<ShmAudioRingHeader*>(region.ptr);
  header->magic = kAudioRingMagic;
  header->slot_count = kAudioRingSlots;
  header->slot_payload = kAudioRingSlotPayload;
  const auto writePacket = [&](std::uint32_t counter) {
    auto* slotBase = static_cast<char*>(region.ptr) + sizeof(ShmAudioRingHeader) +
                     static_cast<size_t>(counter % kAudioRingSlots) * audio_ring_slot_stride();
    auto* slot = reinterpret_cast<ShmAudioRingSlot*>(slotBase);
    slot->sample_rate = 48000;
    slot->channels = 1;
    slot->byte_len = byteLength;
    std::memcpy(slotBase + sizeof(ShmAudioRingSlot), samples.data(), byteLength);
    slot->seq = 2u * counter + 2u;
    header->write_counter = counter + 1u;
  };
  writePacket(0);

  corevideo::modules::ZoomEngineEvent event;
  event.kind = corevideo::modules::ZoomEngineEventKind::Audio;
  event.command = "audio";
  event.sourceUuid = sourceUuid;
  event.participantId = 4242;
  event.byteLength = byteLength;
  runtime.applyEngineEventForTest(event);

  auto frames = runtime.pollCompositorAudioFrames(100);
  const auto found = std::find_if(frames.begin(), frames.end(), [](const corevideo::modules::AudioFrame& frame) {
    return frame.participantId == "4242";
  });
  ASSERT_TRUE(found != frames.end());
  EXPECT_EQ(found->sampleRate, 48000);
  EXPECT_EQ(found->channels, 1);
  EXPECT_EQ(found->sampleCount, 4);
  EXPECT_TRUE(found->requiresSteadyFeedPriming);
  ASSERT_TRUE(found->pcm.size() == 4u);
  EXPECT_EQ(found->pcm[1], 0.5f);
  EXPECT_EQ(found->pcm[2], -0.5f);

  // The poll drained the pending buffer: with no new packet the next poll falls
  // back to the metadata placeholder (no stale PCM replay).
  auto second = runtime.pollCompositorAudioFrames(120);
  const auto foundAgain = std::find_if(second.begin(), second.end(), [](const corevideo::modules::AudioFrame& frame) {
    return frame.participantId == "4242";
  });
  ASSERT_TRUE(foundAgain != second.end());
  EXPECT_TRUE(foundAgain->pcm.empty());

  // The engine rewrote the region (new even sequence): the next event ingests
  // the new chunk ONCE — the duplicate event for the same sequence is skipped,
  // so the samples are not doubled.
  writePacket(1);
  runtime.applyEngineEventForTest(event);
  runtime.applyEngineEventForTest(event);
  auto third = runtime.pollCompositorAudioFrames(140);
  const auto foundThird = std::find_if(third.begin(), third.end(), [](const corevideo::modules::AudioFrame& frame) {
    return frame.participantId == "4242";
  });
  ASSERT_TRUE(foundThird != third.end());
  EXPECT_EQ(foundThird->pcm.size(), 4u);

  shm_region_destroy(region);
}

TEST(ZoomEngineRuntime, IngestsDedicatedMeetingMixPcmAsZoomMix) {
  unsetEnv("COREVIDEO_ZOOM_ENGINE_PATH");
  corevideo::modules::ZoomEngineRuntime runtime;

  const std::string sourceUuid =
      "meeting-audio-4242-program-cvp-test-" + std::to_string(
#if defined(_WIN32)
          static_cast<unsigned long>(::GetCurrentProcessId())
#else
          static_cast<unsigned long>(::getpid())
#endif
      );
  const std::vector<std::int16_t> samples{8192, -8192, 16384, -16384};
  const auto byteLength = static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));
  ShmRegion region{};
  ASSERT_TRUE(shm_region_create(
      region,
      corevideo::modules::zoomEngineAudioSharedMemoryName(sourceUuid),
      corevideo::modules::zoomEngineAudioRingByteSize()));

  auto* header = static_cast<ShmAudioRingHeader*>(region.ptr);
  header->magic = kAudioRingMagic;
  header->slot_count = kAudioRingSlots;
  header->slot_payload = kAudioRingSlotPayload;
  auto* slotBase = static_cast<char*>(region.ptr) + sizeof(ShmAudioRingHeader);
  auto* slot = reinterpret_cast<ShmAudioRingSlot*>(slotBase);
  slot->sample_rate = 48000;
  slot->channels = 1;
  slot->byte_len = byteLength;
  std::memcpy(slotBase + sizeof(ShmAudioRingSlot), samples.data(), byteLength);
  slot->seq = 2u;
  header->write_counter = 1u;

  corevideo::modules::ZoomEngineEvent event;
  event.kind = corevideo::modules::ZoomEngineEventKind::Audio;
  event.command = "audio";
  event.sourceUuid = sourceUuid;
  event.participantId = 4242;
  event.byteLength = byteLength;
  runtime.applyEngineEventForTest(event);

  const auto frames = runtime.pollCompositorAudioFrames(100);
  const auto found = std::find_if(frames.begin(), frames.end(), [](const corevideo::modules::AudioFrame& frame) {
    return frame.participantId == "zoom-mix";
  });
  ASSERT_TRUE(found != frames.end());
  EXPECT_EQ(found->sampleRate, 48000);
  EXPECT_EQ(found->channels, 1);
  ASSERT_EQ(found->pcm.size(), 4u);
  EXPECT_EQ(found->pcm[0], 0.25f);
  EXPECT_EQ(found->pcm[1], -0.25f);
  EXPECT_EQ(found->pcm[2], 0.5f);
  EXPECT_EQ(found->pcm[3], -0.5f);

  shm_region_destroy(region);
}

TEST(ZoomEngineRuntime, CancellationInterruptsAuthWaitAndLeaveIgnoresLateJoined) {
  setEnv("COREVIDEO_ZOOM_ENGINE_PATH", "C:/fake/corevideo-zoom-engine.exe");
  setEnv("COREVIDEO_ZOOM_JOIN_WAIT_MS", "30000");
  auto fake = std::make_shared<FakeZoomEngineProcessClient>();
  {
    corevideo::modules::ZoomEngineRuntime runtime;
    runtime.installEngineProcessForTest(fake);
    std::atomic<bool> cancelled{false};
    corevideo::rpc::Json result;
    std::thread worker([&] {
      result = runtime.join(corevideo::rpc::Json::Object{{"meetingNumber", "123456789"}},
          [&] { return cancelled.load(); });
    });
    EXPECT_TRUE(fake->waitForSentLines(1, std::chrono::seconds(3)));
    const auto start = std::chrono::steady_clock::now();
    cancelled.store(true);
    (void)runtime.leave();
    worker.join();
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(1));
    EXPECT_TRUE(result.isNull());
    runtime.applyEngineEventForTest({corevideo::modules::ZoomEngineEventKind::Joined});
    EXPECT_NE(runtime.snapshot().getString("meetingState"), "in_meeting");
    // The next join must retire the old process even though it still reports
    // running; a missing replacement executable cannot reuse its late Joined.
    const auto rejoin = runtime.join(corevideo::rpc::Json::Object{{"meetingNumber", "987654321"}});
    EXPECT_FALSE(fake->running());
    EXPECT_NE(rejoin.getString("meetingState"), "in_meeting");
  }
  unsetEnv("COREVIDEO_ZOOM_ENGINE_PATH");
  unsetEnv("COREVIDEO_ZOOM_JOIN_WAIT_MS");
}


namespace corevideo::modules {
struct ZoomEngineRuntimeTestAccess {
  static void installVideoRegion(ZoomEngineRuntime& runtime, std::shared_ptr<void> region,
                                 std::uint32_t width = 4, std::uint32_t height = 4) {
    std::lock_guard<std::mutex> lock(runtime.mutex_);
    auto& stream = runtime.videoStreams_["test-camera"];
    stream = {};
    stream.regionOpaque = std::move(region);
    stream.participantId = 42;
    stream.width = width;
    stream.height = height;
    stream.lumaRangeProbed = true;
    runtime.frameSyncEnabled_ = false;
  }
  static void drainVideo(ZoomEngineRuntime& runtime, const std::function<void()>& afterCapture = {},
                         const std::function<void()>& beforePublish = {}) {
    runtime.drainVideoStreamsThreePhase(afterCapture, beforePublish);
  }
  static std::uint32_t videoSequence(ZoomEngineRuntime& runtime) {
    std::lock_guard<std::mutex> lock(runtime.mutex_);
    return runtime.videoStreams_.at("test-camera").lastSequence;
  }
  static std::size_t decodedCount(ZoomEngineRuntime& runtime) {
    std::lock_guard<std::mutex> lock(runtime.mutex_);
    return runtime.latestDecodedFrames_.size();
  }
  static void seedSubscribedVideoCaches(ZoomEngineRuntime& runtime,
                                        const std::string& sourceUuid,
                                        std::uint32_t participantId) {
    std::lock_guard<std::mutex> lock(runtime.mutex_);
    runtime.sentSubscriptions_[sourceUuid] = 1;
    auto& stream = runtime.videoStreams_[sourceUuid];
    stream.participantId = participantId;
    auto& decoded = runtime.latestDecodedFrames_[std::to_string(participantId)];
    decoded.i420 = std::make_shared<const std::vector<std::uint8_t>>(24, 128);
    auto& sync = runtime.frameSync_[std::to_string(participantId)];
    sync.frames.push_back(decoded);
    sync.primed = true;
  }
  static bool hasVideoCaches(ZoomEngineRuntime& runtime,
                             const std::string& sourceUuid,
                             std::uint32_t participantId) {
    std::lock_guard<std::mutex> lock(runtime.mutex_);
    const auto id = std::to_string(participantId);
    return runtime.videoStreams_.contains(sourceUuid) ||
           runtime.latestDecodedFrames_.contains(id) || runtime.frameSync_.contains(id);
  }
  static std::uint64_t staleVideoCount(ZoomEngineRuntime& runtime) {
    std::lock_guard<std::mutex> lock(runtime.mutex_);
    return runtime.staleVideoPublications_;
  }
  static void invalidateVideo(ZoomEngineRuntime& runtime, int mutation, std::shared_ptr<void> replacement) {
    std::lock_guard<std::mutex> lock(runtime.mutex_);
    auto& stream = runtime.videoStreams_.at("test-camera");
    switch (mutation) {
      case 0: stream.regionOpaque = std::move(replacement); break;
      case 1: ++stream.participantId; break;
      case 2: ++stream.width; break;
      case 3: ++stream.height; break;
      case 4: ++runtime.processGeneration_; break;
      case 5: runtime.shuttingDown_ = true; break;
      case 6: runtime.restartBeforeJoin_ = true; break;
    }
  }
  static void markInitialized(ZoomEngineRuntime& runtime) {
    std::lock_guard<std::mutex> lock(runtime.mutex_);
    runtime.initialized_ = true;
  }
  static std::uint64_t generation(ZoomEngineRuntime& runtime) {
    std::lock_guard<std::mutex> lock(runtime.mutex_);
    return runtime.processGeneration_;
  }
  static void applyFromGeneration(ZoomEngineRuntime& runtime, const ZoomEngineEvent& event,
                                 std::uint64_t generation) {
    runtime.applyEvent(event, generation);
  }
  static void beginShutdown(ZoomEngineRuntime& runtime) { runtime.beginShutdown(); }
  static bool ingestRunning(ZoomEngineRuntime& runtime) {
    return runtime.videoIngestRun_.load(std::memory_order_acquire);
  }
  static bool ingestJoinable(ZoomEngineRuntime& runtime) {
    std::lock_guard<std::mutex> lock(runtime.mutex_);
    return runtime.videoIngestThread_.joinable();
  }
};
}  // namespace corevideo::modules

namespace {
// Owned in-memory SHM view exercises the real parser/copy/publication path
// without a subprocess, named region, sleeps, or background ingestion thread.
struct InMemoryVideoRegion {
  std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(sizeof(ShmFrameHeader) + 4 * 4 * 3 / 2, 128);
  ShmRegion region{};
  InMemoryVideoRegion() { region.ptr = bytes.data(); region.size = bytes.size(); setHeader(2, 4, 4); }
  void setHeader(std::uint32_t sequence, std::uint32_t width, std::uint32_t height) {
    const ShmFrameHeader header{sequence, width, height, width * height};
    std::memcpy(bytes.data(), &header, sizeof(header));
  }
  static std::shared_ptr<void> holder(const std::shared_ptr<InMemoryVideoRegion>& owner) {
    return std::shared_ptr<void>(owner, &owner->region);
  }
};
}

TEST(ZoomEngineRuntime, UnsubscribeRetiresHeldFrameAndFrameSyncQueue) {
  using namespace corevideo::modules;
  setEnv("COREVIDEO_ZOOM_ENGINE_PATH", "C:/fake/corevideo-zoom-engine.exe");
  auto fake = std::make_shared<FakeZoomEngineProcessClient>();
  {
    ZoomEngineRuntime runtime;
    runtime.installEngineProcessForTest(fake);
    const std::string sourceUuid = "participant-video-42-camera";
    ZoomEngineRuntimeTestAccess::seedSubscribedVideoCaches(runtime, sourceUuid, 42);
    ASSERT_TRUE(ZoomEngineRuntimeTestAccess::hasVideoCaches(runtime, sourceUuid, 42));

    EXPECT_FALSE(runtime.syncSpine(spinePayload(corevideo::rpc::Json::Array{}), 0.0).isNull());
    ASSERT_TRUE(fake->waitForSentLines(1, std::chrono::milliseconds(5000)));
    EXPECT_NE(fake->sentLines().front().find("unsubscribe"), std::string::npos);
    EXPECT_FALSE(ZoomEngineRuntimeTestAccess::hasVideoCaches(runtime, sourceUuid, 42));
  }
  unsetEnv("COREVIDEO_ZOOM_ENGINE_PATH");
}

TEST(ZoomEngineRuntime, VideoPublicationRejectsReplacedMappingIdentityAndRetiredGenerations) {
  using namespace corevideo::modules;
  for (int mutation = 0; mutation != 7; ++mutation) {
    ZoomEngineRuntime runtime;
    auto original = std::make_shared<InMemoryVideoRegion>();
    auto replacement = std::make_shared<InMemoryVideoRegion>();
    ZoomEngineRuntimeTestAccess::installVideoRegion(runtime, InMemoryVideoRegion::holder(original));
    ZoomEngineRuntimeTestAccess::drainVideo(runtime, {}, [&] {
      ZoomEngineRuntimeTestAccess::invalidateVideo(runtime, mutation, InMemoryVideoRegion::holder(replacement));
    });
    EXPECT_EQ(ZoomEngineRuntimeTestAccess::videoSequence(runtime), 0u);
    EXPECT_EQ(ZoomEngineRuntimeTestAccess::decodedCount(runtime), 0u);
    EXPECT_EQ(ZoomEngineRuntimeTestAccess::staleVideoCount(runtime), 1u);
  }
}

TEST(ZoomEngineRuntime, VideoPublicationUsesCopiedSequenceAndRequiresMatchingHeaderDimensions) {
  using namespace corevideo::modules;
  ZoomEngineRuntime runtime;
  auto region = std::make_shared<InMemoryVideoRegion>();
  ZoomEngineRuntimeTestAccess::installVideoRegion(runtime, InMemoryVideoRegion::holder(region));
  ZoomEngineRuntimeTestAccess::drainVideo(runtime, [&] { region->setHeader(4, 2, 4); });
  EXPECT_EQ(ZoomEngineRuntimeTestAccess::videoSequence(runtime), 0u);
  EXPECT_EQ(ZoomEngineRuntimeTestAccess::decodedCount(runtime), 0u);
  // After matching dimensions are announced, a newer complete frame can arrive
  // between the phase-1 peek and phase-2 copy. Its own sequence is consumed.
  ZoomEngineRuntimeTestAccess::installVideoRegion(runtime, InMemoryVideoRegion::holder(region), 2, 4);
  ZoomEngineRuntimeTestAccess::drainVideo(runtime, [&] { region->setHeader(6, 2, 4); });
  EXPECT_EQ(ZoomEngineRuntimeTestAccess::videoSequence(runtime), 6u);
  EXPECT_EQ(ZoomEngineRuntimeTestAccess::decodedCount(runtime), 1u);
  EXPECT_EQ(ZoomEngineRuntimeTestAccess::staleVideoCount(runtime), 1u);
}

TEST(ZoomEngineRuntime, AuthAndJoinTimeoutsRetireHelperAndRejectLateEvents) {
  using namespace corevideo::modules;
  setEnv("COREVIDEO_ZOOM_ENGINE_PATH", "C:/fake/corevideo-zoom-engine.exe");
  setEnv("COREVIDEO_ZOOM_JOIN_WAIT_MS", "0");
  for (const bool alreadyAuthenticated : {false, true}) {
    ZoomEngineRuntime runtime;
    auto fake = std::make_shared<FakeZoomEngineProcessClient>();
    runtime.installEngineProcessForTest(fake);
    if (alreadyAuthenticated) ZoomEngineRuntimeTestAccess::markInitialized(runtime);
    const auto oldGeneration = ZoomEngineRuntimeTestAccess::generation(runtime);
    const auto result = runtime.join(corevideo::rpc::Json::Object{{"meetingNumber", "123456789"}});
    EXPECT_EQ(result.getString("meetingState"), "error");
    EXPECT_GT(ZoomEngineRuntimeTestAccess::generation(runtime), oldGeneration);
    // Both an already-read callback and a newly read callback from the retired
    // helper must preserve the timeout; callbacks have no SDK operation ID.
    ZoomEngineRuntimeTestAccess::applyFromGeneration(runtime, {ZoomEngineEventKind::Joined}, oldGeneration);
    runtime.applyEngineEventForTest({ZoomEngineEventKind::Joined});
    EXPECT_EQ(runtime.snapshot().getString("meetingState"), "error");
    // The retry may fail to launch our intentionally missing executable, but it
    // must first stop the still-running fake rather than reinitialize its SDK.
    const auto retry = runtime.join(corevideo::rpc::Json::Object{{"meetingNumber", "987654321"}});
    EXPECT_FALSE(fake->running());
    EXPECT_EQ(retry.getString("meetingState"), "error");
    ZoomEngineRuntimeTestAccess::applyFromGeneration(runtime, {ZoomEngineEventKind::Joined}, oldGeneration);
    EXPECT_EQ(runtime.snapshot().getString("meetingState"), "error");
  }
  unsetEnv("COREVIDEO_ZOOM_ENGINE_PATH");
  unsetEnv("COREVIDEO_ZOOM_JOIN_WAIT_MS");
}

TEST(ZoomEngineRuntime, LateFrameDuringShutdownCannotReplaceJoinableIngestThread) {
  using namespace corevideo::modules;
  ZoomEngineRuntime runtime;
  ZoomEngineEvent frame;
  frame.kind = ZoomEngineEventKind::Frame;
  frame.sourceUuid = "shutdown-regression-no-shared-memory";
  frame.participantId = 4242;
  frame.width = 2;
  frame.height = 2;
  runtime.applyEngineEventForTest(frame);
  ASSERT_TRUE(ZoomEngineRuntimeTestAccess::ingestRunning(runtime));
  ASSERT_TRUE(ZoomEngineRuntimeTestAccess::ingestJoinable(runtime));

  // Exercise the precise destructor window: stop is published, but the old
  // std::thread has not been joined. A late reader event previously assigned a
  // new thread over this joinable owner and called std::terminate.
  ZoomEngineRuntimeTestAccess::beginShutdown(runtime);
  runtime.applyEngineEventForTest(frame);
  EXPECT_FALSE(ZoomEngineRuntimeTestAccess::ingestRunning(runtime));
  EXPECT_TRUE(ZoomEngineRuntimeTestAccess::ingestJoinable(runtime));
  // Destruction must join the existing owner outside the event mutex.
}

TEST(ZoomEngineRuntime, ShutdownBeforeFirstFrameNeverStartsIngest) {
  using namespace corevideo::modules;
  ZoomEngineRuntime runtime;
  ZoomEngineRuntimeTestAccess::beginShutdown(runtime);
  ZoomEngineEvent frame;
  frame.kind = ZoomEngineEventKind::Frame;
  frame.sourceUuid = "shutdown-before-first-frame";
  frame.participantId = 4242;
  frame.width = 2;
  frame.height = 2;
  runtime.applyEngineEventForTest(frame);
  EXPECT_FALSE(ZoomEngineRuntimeTestAccess::ingestRunning(runtime));
  EXPECT_FALSE(ZoomEngineRuntimeTestAccess::ingestJoinable(runtime));
}

TEST(ZoomEngineRuntime, SubscriptionChurnNamesResolutionChangesAndTeardowns) {
  setEnv("COREVIDEO_ZOOM_ENGINE_PATH", "C:/fake/corevideo-zoom-engine.exe");
  auto fake = std::make_shared<FakeZoomEngineProcessClient>();
  {
    corevideo::modules::ZoomEngineRuntime runtime;
    runtime.installEngineProcessForTest(fake);

    // 301 is the active speaker (1080P); 302 is an ordinary wall member (720P).
    (void)runtime.syncSpine(spinePayload(corevideo::rpc::Json::Array{
                                subscriptionRequest("301", "participant-video", "active-speaker"),
                                subscriptionRequest("302", "participant-video", "program"),
                            }),
                            10.0);
    {
      const auto churn = runtime.subscriptionChurnState();
      EXPECT_TRUE(churn.get("engine")->asBool(false));
      EXPECT_EQ(churn.getNumber("totalChurn"), 0);  // starting is not churning
      const auto* speaker = findChurnSource(churn, "participant-video-301-camera");
      ASSERT_NE(speaker, nullptr);
      EXPECT_EQ(speaker->getNumber("generation"), 1);
      EXPECT_EQ(speaker->getNumber("churn"), 0);
      EXPECT_EQ(speaker->getString("lastReason"), "initial");
      EXPECT_EQ(speaker->getNumber("resolution"), 2);  // 1080P
      EXPECT_TRUE(speaker->get("subscribed")->asBool(false));
      const auto* member = findChurnSource(churn, "participant-video-302-camera");
      ASSERT_NE(member, nullptr);
      EXPECT_EQ(member->getNumber("resolution"), 1);  // 720P
    }

    // The active speaker changes. 301's uuid is unchanged — the purpose is
    // deliberately not in it — but its RESOLUTION is, so it is re-subscribed.
    (void)runtime.syncSpine(spinePayload(corevideo::rpc::Json::Array{
                                subscriptionRequest("301", "participant-video", "program"),
                                subscriptionRequest("302", "participant-video", "active-speaker"),
                            }),
                            20.0);
    {
      const auto churn = runtime.subscriptionChurnState();
      EXPECT_EQ(churn.getNumber("totalChurn"), 2);
      EXPECT_EQ(churn.getNumber("lastResolutionChanges"), 2);
      const auto* speaker = findChurnSource(churn, "participant-video-301-camera");
      ASSERT_NE(speaker, nullptr);
      EXPECT_EQ(speaker->getNumber("generation"), 2);
      EXPECT_EQ(speaker->getNumber("churn"), 1);
      EXPECT_EQ(speaker->getString("lastReason"), "resolution-change");
      EXPECT_EQ(speaker->getNumber("resolution"), 1);
      EXPECT_EQ(speaker->getNumber("lastChangeMs"), 20.0);
    }

    // 302 falls out of the requested set entirely — the cap-reordering shape.
    // Its ledger SURVIVES the unsubscribe: a record erased with the subscription
    // could not answer the question it exists for.
    (void)runtime.syncSpine(spinePayload(corevideo::rpc::Json::Array{
                                subscriptionRequest("301", "participant-video", "program"),
                            }),
                            30.0);
    {
      const auto churn = runtime.subscriptionChurnState();
      const auto* dropped = findChurnSource(churn, "participant-video-302-camera");
      ASSERT_NE(dropped, nullptr);
      EXPECT_FALSE(dropped->get("subscribed")->asBool(true));
      EXPECT_EQ(dropped->getNumber("generation"), 3);
      EXPECT_EQ(dropped->getNumber("churn"), 2);
      // No roster in this harness, so the retire reads as a departure; the
      // cap-eviction/departure split itself is pinned by the policy test.
      EXPECT_EQ(dropped->getString("lastReason"), "departure");
      EXPECT_EQ(churn.getNumber("subscribedCount"), 1);
      EXPECT_EQ(churn.getNumber("sourceCount"), 2);
    }

    // Coming back is churn too, and the generation keeps advancing.
    (void)runtime.syncSpine(spinePayload(corevideo::rpc::Json::Array{
                                subscriptionRequest("301", "participant-video", "program"),
                                subscriptionRequest("302", "participant-video", "program"),
                            }),
                            40.0);
    {
      const auto churn = runtime.subscriptionChurnState();
      const auto* back = findChurnSource(churn, "participant-video-302-camera");
      ASSERT_NE(back, nullptr);
      EXPECT_TRUE(back->get("subscribed")->asBool(false));
      EXPECT_EQ(back->getNumber("generation"), 4);
      EXPECT_EQ(back->getString("lastReason"), "resubscribe");
    }
  }
  unsetEnv("COREVIDEO_ZOOM_ENGINE_PATH");
}
