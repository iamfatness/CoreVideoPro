#include "modules/ZoomEngineRuntime.h"

#include "modules/ZoomEngineProcess.h"
#include "modules/ZoomSubscriptionResolutionPolicy.h"

#include "engine-resolution-policy.h"

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
#include <tuple>
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

namespace {

const corevideo::rpc::Json* findChurnSource(const corevideo::rpc::Json& churn,
                                            const std::string& sourceUuid) {
  const auto* sources = churn.get("sources");
  if (!sources || !sources->isArray()) return nullptr;
  for (const auto& source : sources->asArray()) {
    if (source.getString("sourceUuid") == sourceUuid) return &source;
  }
  return nullptr;
}

}  // namespace


TEST(ZoomEngineRuntime, SubscriptionChurnNamesResolutionChangesAndTeardowns) {
  setEnv("COREVIDEO_ZOOM_ENGINE_PATH", "C:/fake/corevideo-zoom-engine.exe");
  auto fake = std::make_shared<FakeZoomEngineProcessClient>();
  {
    corevideo::modules::ZoomEngineRuntime runtime;
    runtime.installEngineProcessForTest(fake);

    // 301 is on the wall (720P); 302 is the fixed Program route (1080P).
    (void)runtime.syncSpine(spinePayload(corevideo::rpc::Json::Array{
                                subscriptionRequest("301", "participant-video", "multiview"),
                                subscriptionRequest("302", "participant-video", "program"),
                            }),
                            10.0);
    {
      const auto churn = runtime.subscriptionChurnState();
      EXPECT_TRUE(churn.get("engine")->asBool(false));
      EXPECT_EQ(churn.getNumber("totalChurn"), 0);  // starting is not churning
      const auto* wall = findChurnSource(churn, "participant-video-301-camera");
      ASSERT_NE(wall, nullptr);
      EXPECT_EQ(wall->getNumber("generation"), 1);
      EXPECT_EQ(wall->getNumber("churn"), 0);
      EXPECT_EQ(wall->getString("lastReason"), "initial");
      EXPECT_EQ(wall->getNumber("resolution"), 1);  // 720P
      EXPECT_TRUE(wall->get("subscribed")->asBool(false));
      const auto* program = findChurnSource(churn, "participant-video-302-camera");
      ASSERT_NE(program, nullptr);
      EXPECT_EQ(program->getNumber("resolution"), 2);  // 1080P
    }

    // 301 is cued to Preview: the one resolution change a guest can see, on a cue,
    // raising its renderer to the 1080P bus tier. Named as the teardown it is.
    (void)runtime.syncSpine(spinePayload(corevideo::rpc::Json::Array{
                                subscriptionRequest("301", "participant-video", "preview"),
                                subscriptionRequest("302", "participant-video", "program"),
                            }),
                            20.0);
    {
      const auto churn = runtime.subscriptionChurnState();
      EXPECT_EQ(churn.getNumber("totalChurn"), 1);
      EXPECT_EQ(churn.getNumber("lastResolutionChanges"), 1);
      const auto* cued = findChurnSource(churn, "participant-video-301-camera");
      ASSERT_NE(cued, nullptr);
      EXPECT_EQ(cued->getNumber("generation"), 2);
      EXPECT_EQ(cued->getNumber("churn"), 1);
      EXPECT_EQ(cued->getString("lastReason"), "resolution-change");
      EXPECT_EQ(cued->getNumber("resolution"), 2);
      EXPECT_EQ(cued->getNumber("lastChangeMs"), 20.0);
    }

    // Uncued back to the wall: NO RATCHET (#478 R4). It goes back to 720P, the
    // engine now honours the downgrade, and the ledger names it.
    (void)runtime.syncSpine(spinePayload(corevideo::rpc::Json::Array{
                                subscriptionRequest("301", "participant-video", "multiview"),
                                subscriptionRequest("302", "participant-video", "program"),
                            }),
                            25.0);
    {
      const auto churn = runtime.subscriptionChurnState();
      EXPECT_EQ(churn.getNumber("totalChurn"), 2);
      const auto* uncued = findChurnSource(churn, "participant-video-301-camera");
      ASSERT_NE(uncued, nullptr);
      EXPECT_EQ(uncued->getNumber("generation"), 3);
      EXPECT_EQ(uncued->getNumber("resolution"), 1);
      EXPECT_EQ(uncued->getString("lastReason"), "resolution-change");
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
      EXPECT_EQ(dropped->getNumber("generation"), 2);
      EXPECT_EQ(dropped->getNumber("churn"), 1);
      // No roster in this harness, so the retire reads as a departure; the
      // cap-eviction/unrouted/departure split is pinned by the policy test and
      // ARetireIsACapEvictionOnlyWhenTheShellNamesItAShortfall.
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
      EXPECT_EQ(back->getNumber("generation"), 3);
      EXPECT_EQ(back->getString("lastReason"), "resubscribe");
    }
  }
  unsetEnv("COREVIDEO_ZOOM_ENGINE_PATH");
}

// ---------------------------------------------------------------------------
// #478 (live 2026-09-11): an active-speaker flip rebuilt two engine renderers,
// because resolution was `purpose == "active-speaker" ? 1080P : 720P` and
// resolution is part of the subscription key. The tier is now stable.
// ---------------------------------------------------------------------------

TEST(ZoomSubscriptionResolutionPolicyRules, ResolutionIsAStableTierNotWhoIsTalking) {
  using Policy = corevideo::modules::ZoomSubscriptionResolutionPolicy;
  // Fixed bus routes and screen share: full resolution.
  EXPECT_EQ(Policy::requestedResolution("participant-video", "program"), Policy::k1080P);
  EXPECT_EQ(Policy::requestedResolution("participant-video", "preview"), Policy::k1080P);
  EXPECT_EQ(Policy::requestedResolution("screen-share", "program"), Policy::k1080P);
  EXPECT_EQ(Policy::requestedResolution("screen-share", ""), Policy::k1080P);
  // Everything else, INCLUDING active-speaker (a follow-speaker route): 720P, so
  // a change of speaker can never move a key.
  EXPECT_EQ(Policy::requestedResolution("participant-video", "active-speaker"), Policy::k720P);
  EXPECT_EQ(Policy::requestedResolution("participant-video", "multiview"), Policy::k720P);
  EXPECT_EQ(Policy::requestedResolution("participant-video", "program-tiles"), Policy::k720P);
  EXPECT_EQ(Policy::requestedResolution("participant-video", "preview-tiles"), Policy::k720P);
  EXPECT_EQ(Policy::requestedResolution("participant-video", "iso"), Policy::k720P);
  // The macOS shell's kind "video"/purpose "program" for every assigned guest is
  // NOT promoted to N x 1080P.
  EXPECT_EQ(Policy::requestedResolution("video", "program"), Policy::k720P);

  // Tiles members are 720P (R4): they share the wall with everyone else.
  EXPECT_FALSE(Policy::wantsFullResolution("participant-video", "program-tiles"));
}

TEST(ZoomSubscriptionResolutionPolicyRules, FullResolutionIsCappedInPayloadOrder) {
  using Policy = corevideo::modules::ZoomSubscriptionResolutionPolicy;
  Policy::Budget budget;
  // The shell orders Program routes before Preview routes, so the cap spends 1080P on
  // Program first.
  for (int i = 0; i < Policy::kMaxConcurrentFullResolutionCameras; ++i) {
    EXPECT_EQ(budget.resolve("participant-video", i % 2 == 0 ? "program" : "preview"), Policy::k1080P);
  }
  EXPECT_EQ(budget.resolve("participant-video", "preview"), Policy::k720P);
  EXPECT_EQ(budget.resolve("participant-video", "multiview"), Policy::k720P);
  // A screen share is never demoted and never spends the camera budget.
  EXPECT_EQ(budget.resolve("screen-share", "program"), Policy::k1080P);
  EXPECT_EQ(budget.granted(), Policy::kMaxConcurrentFullResolutionCameras);
  EXPECT_EQ(budget.demoted(), 1);
}

TEST(EngineResolutionPolicy, ALowerRequestRebuildsOnlyWhenNoOtherTargetNeedsTheHigherOne) {
  // Raising always rebuilt. Lowering used to be a no-op forever (the ratchet that put
  // every rotated guest at 1080P); now it rebuilds when this is the renderer's only target.
  EXPECT_TRUE(video_resolution_needs_rebuild(2, 1, 0));
  EXPECT_TRUE(video_resolution_needs_rebuild(2, 1, 3));
  EXPECT_TRUE(video_resolution_needs_rebuild(1, 2, 0));
  EXPECT_FALSE(video_resolution_needs_rebuild(1, 2, 1));
  EXPECT_FALSE(video_resolution_needs_rebuild(1, 1, 0));
  EXPECT_FALSE(video_resolution_needs_rebuild(2, 2, 0));
}

TEST(ZoomEngineRuntime, AnActiveSpeakerFlipCausesNoTeardown) {
  setEnv("COREVIDEO_ZOOM_ENGINE_PATH", "C:/fake/corevideo-zoom-engine.exe");
  auto fake = std::make_shared<FakeZoomEngineProcessClient>();
  {
    corevideo::modules::ZoomEngineRuntime runtime;
    runtime.installEngineProcessForTest(fake);

    // The exact shape the pre-#478 shell sent: the talker first as
    // "active-speaker", everyone else on the wall.
    (void)runtime.syncSpine(spinePayload(corevideo::rpc::Json::Array{
                                subscriptionRequest("401", "participant-video", "active-speaker"),
                                subscriptionRequest("402", "participant-video", "multiview"),
                            }),
                            10.0);
    ASSERT_TRUE(fake->waitForSentLines(2, std::chrono::milliseconds(5000)));

    // 402 starts talking.
    (void)runtime.syncSpine(spinePayload(corevideo::rpc::Json::Array{
                                subscriptionRequest("402", "participant-video", "active-speaker"),
                                subscriptionRequest("401", "participant-video", "multiview"),
                            }),
                            20.0);
    // And back again.
    (void)runtime.syncSpine(spinePayload(corevideo::rpc::Json::Array{
                                subscriptionRequest("401", "participant-video", "active-speaker"),
                                subscriptionRequest("402", "participant-video", "multiview"),
                            }),
                            30.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Nothing re-sent, nothing torn down, no generation moved.
    EXPECT_EQ(fake->sentLines().size(), 2u);
    const auto churn = runtime.subscriptionChurnState();
    EXPECT_EQ(churn.getNumber("totalChurn"), 0);
    EXPECT_EQ(churn.getNumber("lastResolutionChanges"), 0);
    for (const char* uuid : {"participant-video-401-camera", "participant-video-402-camera"}) {
      const auto* source = findChurnSource(churn, uuid);
      ASSERT_NE(source, nullptr) << uuid;
      EXPECT_EQ(source->getNumber("generation"), 1) << uuid;
      EXPECT_EQ(source->getNumber("resolution"), 1) << uuid;
      EXPECT_EQ(source->getString("lastReason"), "initial") << uuid;
    }
  }
  unsetEnv("COREVIDEO_ZOOM_ENGINE_PATH");
}

TEST(ZoomEngineRuntime, ARetireIsACapEvictionOnlyWhenTheShellNamesItAShortfall) {
  setEnv("COREVIDEO_ZOOM_ENGINE_PATH", "C:/fake/corevideo-zoom-engine.exe");
  auto fake = std::make_shared<FakeZoomEngineProcessClient>();
  {
    corevideo::modules::ZoomEngineRuntime runtime;
    runtime.installEngineProcessForTest(fake);

    corevideo::modules::ZoomEngineEvent roster;
    roster.kind = corevideo::modules::ZoomEngineEventKind::Participants;
    for (const std::uint32_t id : {501u, 502u}) {
      corevideo::modules::ZoomEngineParticipant participant;
      participant.id = id;
      participant.displayName = "Guest " + std::to_string(id);
      participant.hasVideo = true;
      roster.participants.push_back(participant);
    }
    runtime.applyEngineEventForTest(roster);

    (void)runtime.syncSpine(spinePayload(corevideo::rpc::Json::Array{
                                subscriptionRequest("501", "participant-video", "multiview"),
                                subscriptionRequest("502", "participant-video", "multiview"),
                                subscriptionRequest("502", "participant-audio", "mix"),
                            }),
                            10.0);

    // 501 is pushed out by the budget (the shell names it); 502 is un-routed
    // by the operator — its video AND its audio go, and neither is the cap.
    const corevideo::rpc::Json payload = corevideo::rpc::Json::Object{
        {"startCapture", false},
        {"subscriptions", corevideo::rpc::Json::Array{}},
        {"videoSubscriptionShortfall",
         corevideo::rpc::Json::Array{
             corevideo::rpc::Json::Object{{"participantId", "501"}, {"purpose", "multiview"}},
         }},
    };
    (void)runtime.syncSpine(payload, 20.0);

    const auto churn = runtime.subscriptionChurnState();
    const auto* evicted = findChurnSource(churn, "participant-video-501-camera");
    ASSERT_NE(evicted, nullptr);
    EXPECT_EQ(evicted->getString("lastReason"), "cap-eviction");
    const auto* unroutedVideo = findChurnSource(churn, "participant-video-502-camera");
    ASSERT_NE(unroutedVideo, nullptr);
    EXPECT_EQ(unroutedVideo->getString("lastReason"), "unrouted");
    const auto* unroutedAudio = findChurnSource(churn, "participant-audio-502-mix");
    ASSERT_NE(unroutedAudio, nullptr);
    EXPECT_EQ(unroutedAudio->getString("lastReason"), "unrouted");
    EXPECT_EQ(churn.getNumber("lastCapEvictions"), 1);
    EXPECT_EQ(churn.getNumber("lastUnrouted"), 2);
  }
  unsetEnv("COREVIDEO_ZOOM_ENGINE_PATH");
}

// ---------------------------------------------------------------------------
// #478 fix round 1.
// ---------------------------------------------------------------------------

namespace {

double sentResolution(const std::string& line) {
  const auto parsed = corevideo::rpc::Json::parse(line);
  if (!parsed || !parsed->get("resolution")) return -1;
  return parsed->get("resolution")->asNumber();
}

corevideo::modules::ZoomEngineEvent rosterEvent(
    std::initializer_list<std::tuple<std::uint32_t, bool, bool>> people) {
  corevideo::modules::ZoomEngineEvent roster;
  roster.kind = corevideo::modules::ZoomEngineEventKind::Participants;
  for (const auto& [id, hasVideo, talking] : people) {
    corevideo::modules::ZoomEngineParticipant participant;
    participant.id = id;
    participant.displayName = "Guest " + std::to_string(id);
    participant.hasVideo = hasVideo;
    participant.isTalking = talking;
    roster.participants.push_back(participant);
  }
  return roster;
}

}  // namespace

TEST(ZoomEngineRuntime, ACueRaisesOnceTheTakeSendsNothingAndLeavingTheBusDropsBack) {
  // R4: Tiles 720P -> cued to Preview 1080P (raise, off air) -> taken to Program
  // (same tier: nothing) -> back on the wall only (drop back to 720P: no ratchet).
  setEnv("COREVIDEO_ZOOM_ENGINE_PATH", "C:/fake/corevideo-zoom-engine.exe");
  auto fake = std::make_shared<FakeZoomEngineProcessClient>();
  {
    corevideo::modules::ZoomEngineRuntime runtime;
    runtime.installEngineProcessForTest(fake);
    const char* purposes[] = {"program-tiles", "preview", "program", "multiview"};
    double elapsed = 10.0;
    for (const char* purpose : purposes) {
      (void)runtime.syncSpine(spinePayload(corevideo::rpc::Json::Array{
                                  subscriptionRequest("701", "participant-video", purpose)}),
                              elapsed);
      elapsed += 10.0;
    }
    ASSERT_TRUE(fake->waitForSentLines(3, std::chrono::milliseconds(5000)));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto lines = fake->sentLines();
    ASSERT_EQ(lines.size(), 3u);  // initial, raise, drop — the Take sent nothing
    EXPECT_EQ(sentResolution(lines[0]), 1);
    EXPECT_EQ(sentResolution(lines[1]), 2);
    EXPECT_EQ(sentResolution(lines[2]), 1);
    const auto churn = runtime.subscriptionChurnState();
    EXPECT_EQ(churn.getNumber("lastResolutionChanges"), 1);
    EXPECT_EQ(churn.getNumber("totalChurn"), 2);
  }
  unsetEnv("COREVIDEO_ZOOM_ENGINE_PATH");
}

TEST(ZoomEngineRuntime, TheFullResolutionCapDemotesTheRoutesPastItAndSaysSo) {
  setEnv("COREVIDEO_ZOOM_ENGINE_PATH", "C:/fake/corevideo-zoom-engine.exe");
  auto fake = std::make_shared<FakeZoomEngineProcessClient>();
  {
    corevideo::modules::ZoomEngineRuntime runtime;
    runtime.installEngineProcessForTest(fake);
    corevideo::rpc::Json::Array requests;
    const int routes = corevideo::modules::ZoomSubscriptionResolutionPolicy::kMaxConcurrentFullResolutionCameras + 1;
    for (int i = 0; i < routes; ++i) {
      requests.push_back(subscriptionRequest(std::to_string(800 + i).c_str(), "participant-video",
                                             i % 2 == 0 ? "program" : "preview"));
    }
    (void)runtime.syncSpine(spinePayload(std::move(requests)), 10.0);
    ASSERT_TRUE(fake->waitForSentLines(static_cast<std::size_t>(routes), std::chrono::milliseconds(5000)));
    const auto lines = fake->sentLines();
    for (int i = 0; i < routes - 1; ++i) {
      EXPECT_EQ(sentResolution(lines[static_cast<std::size_t>(i)]), 2) << i;
    }
    EXPECT_EQ(sentResolution(lines.back()), 1);
    const auto churn = runtime.subscriptionChurnState();
    EXPECT_EQ(churn.getNumber("fullResolutionDemoted"), 1);
    EXPECT_EQ(churn.getNumber("fullResolutionCap"),
              corevideo::modules::ZoomSubscriptionResolutionPolicy::kMaxConcurrentFullResolutionCameras);
  }
  unsetEnv("COREVIDEO_ZOOM_ENGINE_PATH");
}

TEST(ZoomEngineRuntime, AResolutionReSubscribeKeepsTheSourcesLastFrameForTheCompositor) {
  using namespace corevideo::modules;
  // R4 "hide the re-subscribe": the core keeps the last decoded frame of a source
  // across a resolution change (only a RETIRE erases it), so the compositor keeps
  // drawing it until the rebuilt renderer delivers. Contrast
  // UnsubscribeRetiresHeldFrameAndFrameSyncQueue above.
  setEnv("COREVIDEO_ZOOM_ENGINE_PATH", "C:/fake/corevideo-zoom-engine.exe");
  auto fake = std::make_shared<FakeZoomEngineProcessClient>();
  {
    corevideo::modules::ZoomEngineRuntime runtime;
    runtime.installEngineProcessForTest(fake);
    const std::string sourceUuid = "participant-video-42-camera";
    ZoomEngineRuntimeTestAccess::seedSubscribedVideoCaches(runtime, sourceUuid, 42);  // at 720P

    (void)runtime.syncSpine(spinePayload(corevideo::rpc::Json::Array{
                                subscriptionRequest("42", "participant-video", "program")}),
                            10.0);
    ASSERT_TRUE(fake->waitForSentLines(1, std::chrono::milliseconds(5000)));
    EXPECT_EQ(sentResolution(fake->sentLines().front()), 2);  // re-subscribed at 1080P
    EXPECT_TRUE(ZoomEngineRuntimeTestAccess::hasVideoCaches(runtime, sourceUuid, 42));
    EXPECT_EQ(ZoomEngineRuntimeTestAccess::decodedCount(runtime), 1u);
    const auto churn = runtime.subscriptionChurnState();
    const auto* source = findChurnSource(churn, sourceUuid);
    ASSERT_NE(source, nullptr);
    EXPECT_EQ(source->getString("lastReason"), "resolution-change");
  }
  unsetEnv("COREVIDEO_ZOOM_ENGINE_PATH");
}

TEST(ZoomEngineRuntime, ANonSourceWhoTalksFirstIsReleasedAndNeverDirected) {
  // Review finding 1 (Critical) / R1: under "sources only" a non-source never has a
  // subscription, so the director's fresh-frame gate could never promote anyone past
  // the first talker it filled the vacancy with. The director now follows only the
  // sources the shell names.
  setEnv("COREVIDEO_ZOOM_ENGINE_PATH", "C:/fake/corevideo-zoom-engine.exe");
  auto fake = std::make_shared<FakeZoomEngineProcessClient>();
  {
    corevideo::modules::ZoomEngineRuntime runtime;
    runtime.installEngineProcessForTest(fake);
    // 901 (the unrouted host) talks first; 902 is on the wall.
    runtime.applyEngineEventForTest(rosterEvent({{901, true, true}, {902, true, false}}));
    ASSERT_EQ(runtime.directedSpeakerId(), "901");  // no filter yet: the old behaviour

    const corevideo::rpc::Json payload = corevideo::rpc::Json::Object{
        {"startCapture", false},
        {"subscriptions", corevideo::rpc::Json::Array{
            subscriptionRequest("902", "participant-video", "multiview")}},
        {"sourceParticipantIds", corevideo::rpc::Json::Array{corevideo::rpc::Json{"902"}}},
    };
    (void)runtime.syncSpine(payload, 10.0);
    EXPECT_EQ(runtime.directedSpeakerId(), "");  // released: 901 is not a source

    // 901 keeps talking: still never directed.
    runtime.applyEngineEventForTest(rosterEvent({{901, true, true}, {902, true, false}}));
    (void)runtime.syncSpine(payload, 20.0);
    EXPECT_NE(runtime.directedSpeakerId(), "901");
  }
  unsetEnv("COREVIDEO_ZOOM_ENGINE_PATH");
}

TEST(ZoomEngineRuntime, ACameraTurningOffIsLedgeredAsVideoOffNotUnrouted) {
  setEnv("COREVIDEO_ZOOM_ENGINE_PATH", "C:/fake/corevideo-zoom-engine.exe");
  auto fake = std::make_shared<FakeZoomEngineProcessClient>();
  {
    corevideo::modules::ZoomEngineRuntime runtime;
    runtime.installEngineProcessForTest(fake);
    runtime.applyEngineEventForTest(rosterEvent({{601, true, false}}));
    (void)runtime.syncSpine(spinePayload(corevideo::rpc::Json::Array{
                                subscriptionRequest("601", "participant-video", "multiview"),
                                subscriptionRequest("601", "participant-audio", "mix")}),
                            10.0);

    // The camera goes off: the shell drops the video (keeps the audio: still a source).
    runtime.applyEngineEventForTest(rosterEvent({{601, false, false}}));
    (void)runtime.syncSpine(spinePayload(corevideo::rpc::Json::Array{
                                subscriptionRequest("601", "participant-audio", "mix")}),
                            20.0);

    const auto churn = runtime.subscriptionChurnState();
    const auto* video = findChurnSource(churn, "participant-video-601-camera");
    ASSERT_NE(video, nullptr);
    EXPECT_EQ(video->getString("lastReason"), "video-off");
    EXPECT_EQ(churn.getNumber("lastVideoOff"), 1);
    EXPECT_EQ(churn.getNumber("lastUnrouted"), 0);
    const auto* audio = findChurnSource(churn, "participant-audio-601-mix");
    ASSERT_NE(audio, nullptr);
    EXPECT_TRUE(audio->get("subscribed")->asBool(false));
  }
  unsetEnv("COREVIDEO_ZOOM_ENGINE_PATH");
}
