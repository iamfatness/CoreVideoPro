#pragma once

#include "modules/Interfaces.h"
#include "modules/ZoomEngineClient.h"
#include "modules/ZoomEngineProcess.h"
#include "modules/ZoomEngineState.h"
#include "rpc/Json.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace corevideo::modules {

class ZoomEngineRuntime {
 public:
  ZoomEngineRuntime();
  ~ZoomEngineRuntime();

  ZoomEngineRuntime(const ZoomEngineRuntime&) = delete;
  ZoomEngineRuntime& operator=(const ZoomEngineRuntime&) = delete;

  [[nodiscard]] bool configured() const;
  [[nodiscard]] rpc::Json join(const rpc::Json& payload);
  [[nodiscard]] rpc::Json leave();
  // Capture-off: stop raw media in the engine WITHOUT leaving the meeting.
  // Enqueues the engine `stop_media` command (engine command loop runs
  // stop_raw_media: StopRawRecording — which clears Zoom's participant-facing
  // recording indicator — plus unsubscribe_all across video/share/audio),
  // clears the local media-started + subscription-dedup state so a later
  // capture-on re-arms from scratch, and returns the current snapshot.
  // Non-blocking: pipe I/O rides the dedicated sender thread.
  [[nodiscard]] rpc::Json stopCapture();
  [[nodiscard]] rpc::Json snapshot();
  [[nodiscard]] rpc::Json syncSpine(const rpc::Json& payload, double elapsedMs);
  [[nodiscard]] std::vector<rpc::Json> drainFrameEvents();
  // Returns the latest decoded BGRA frame per participant, carrying real pixels,
  // WITHOUT consuming the pending stdout/event queue (drainFrameEvents) that
  // feeds the WinUI multiview tiles. Used by the compositor tick to ingest real
  // pixels into RealZoomCaptureSource.
  [[nodiscard]] std::vector<VideoFrame> latestDecodedVideoFrames(int64_t timestampMs);
  [[nodiscard]] std::vector<VideoFrame> pollCompositorVideoFrames(int64_t timestampMs);
  [[nodiscard]] std::vector<AudioFrame> pollCompositorAudioFrames(int64_t timestampMs);

  // TEST SEAM (phase 2 increment 3): installs a (fake) engine process client and
  // starts the outbound sender thread WITHOUT launching a real subprocess or the
  // reader thread. Mirrors ensureStarted's restart semantics: bumps the
  // process generation, purges lines queued for the previous process, and clears
  // the subscription dedup so the new process is subscribed from scratch.
  void installEngineProcessForTest(std::shared_ptr<ZoomEngineProcessClient> process);
  // TEST SEAM: lines queued but not yet handed to the sender thread.
  [[nodiscard]] std::size_t pendingEngineSendCountForTest();
  // TEST SEAM: cumulative lines dropped (dead/replaced process, purge, shutdown).
  [[nodiscard]] std::uint64_t droppedEngineSendCountForTest() const;
  // TEST SEAM: feeds one parsed engine event through the exact path the reader
  // thread uses (state apply + frame/audio shared-memory ingest), so tests can
  // exercise the SHM audio ingest without a live engine subprocess.
  void applyEngineEventForTest(const ZoomEngineEvent& event);

 private:
  struct Config {
    std::string executablePath;
    std::string sdkJwt;
    std::string publicAppKey;
    std::string passcode;
    std::string onBehalfToken;
    std::string userZak;
    std::string appPrivilegeToken;
    int connectTimeoutMs = 30000;
    int joinWaitMs = 7000;
  };

  // One JSON line bound for the engine subprocess stdin, queued for the dedicated
  // sender thread (phase 2 increment 3: engine pipe I/O must never run under
  // coreMutex/mutex_ — a slow or wedged pipe previously stalled the whole command
  // path). `generation` pins the line to the process instance it was built for so
  // a restart drops stale lines instead of replaying them into the new process.
  struct PendingEngineSend {
    std::string line;
    std::string context;  // error-report stage: "init", "join", "subscribe", ...
    std::uint64_t generation = 0;
  };

  [[nodiscard]] static Config loadConfig();
  // Ensures a running engine process. Takes mutex_ ITSELF, in phases: the
  // blocking CreateProcess + IPC connect runs UNLOCKED (a generation guard
  // discards a superseded spawn) so per-tick frame polls never stall behind it.
  [[nodiscard]] bool ensureStarted();
  void startReaderLocked();
  void readerLoop();
  void applyEvent(const ZoomEngineEvent& event);
  void stopReader();
  void startSenderLocked();
  void senderLoop();
  // Signals the sender thread to stop and drops (with a log) anything still
  // queued. Join happens in the destructor AFTER stopReader() has terminated the
  // engine process — process death breaks the pipe and unblocks an in-flight send.
  void signalSenderStopAndDropQueue();
  // Requires mutex_ held. Queues one line for the sender thread (lock order:
  // mutex_ OUTER → sendMutex_ INNER; the reverse nesting never happens).
  void enqueueEngineSendLocked(std::string context, std::string line);
  // Requires mutex_ held. Drops queued lines that target a previous process
  // generation (engine restart / test process swap).
  void purgeQueuedEngineSendsLocked(const char* reason);
  void noteDroppedEngineSends(std::size_t count, const char* reason);
  [[nodiscard]] rpc::Json rawCaptureSnapshotLocked();
  [[nodiscard]] rpc::Json spineSnapshotLocked(const rpc::Json& payload, double elapsedMs);
  void enqueueFrameEventLocked(const ZoomEngineEvent& event);
  void ingestAudioEventLocked(const ZoomEngineEvent& event);
  bool ensureMediaStartedLocked();
  void applyJoinCredentialsFromPayload(const rpc::Json& payload);
  [[nodiscard]] double runtimeElapsedMs() const;

  Config config_;
  // shared_ptr so the sender thread can hold the client alive across a blocking
  // sendLine while ensureStarted replaces process_ after a crash — the old
  // client is destroyed only when the last reference drops.
  std::shared_ptr<ZoomEngineProcessClient> process_;
  ZoomEngineRuntimeState state_;
  mutable std::mutex mutex_;
  std::thread reader_;
  bool readerRunning_ = false;
  bool initialized_ = false;
  bool mediaStarted_ = false;
  // Monotonic engine-process instance counter (guarded by mutex_). Bumped every
  // time a new process client is installed; queued sends carry the generation
  // they were built for and are dropped if the process has been replaced.
  std::uint64_t processGeneration_ = 0;
  // Per-instance IPC token of the current engine process (read back from the
  // process client after start). Used to derive SHM region names that match what
  // the engine creates. Guarded by mutex_.
  std::string instanceToken_;

  // Outbound engine-command queue + dedicated sender thread (increment 3). All
  // engine stdin writes happen ONLY on sender_, so no caller ever blocks on the
  // engine pipe while holding coreMutex or mutex_. FIFO drain by a single thread
  // preserves command ordering. Lock order: mutex_ → sendMutex_ (enqueue/purge);
  // the sender takes them strictly sequentially, never nested the other way.
  mutable std::mutex sendMutex_;
  std::condition_variable sendCv_;
  std::deque<PendingEngineSend> sendQueue_;
  std::thread sender_;
  bool senderRunning_ = false;  // guarded by sendMutex_
  std::atomic<std::uint64_t> droppedEngineSends_{0};
  // Operator opted in to raw capture (Studio "Engine On"). Raw recording /
  // recording-rights request only starts once this is set, so it no longer
  // fires automatically on meeting join.
  bool captureRequested_ = false;
  // Last subscription set sent to the engine (sourceUuid -> resolution), so the
  // per-tick spine sync only sends a subscribe/unsubscribe command when the set
  // actually CHANGES. Previously it re-sent every subscription on every spine
  // tick, flooding the engine command pipe and timing out the 4s spine round-trip
  // — which jammed the whole bridge (roster + multiview layout couldn't get
  // through). Dedup is keyed at ENQUEUE time (guarded by mutex_): the sender
  // thread delivers asynchronously but strictly in FIFO order, so "marked sent"
  // means "queued exactly once, will reach the engine in order". Cleared on
  // leave/rejoin AND whenever a new engine process is installed
  // (ensureStarted), so a restarted engine is re-subscribed from scratch.
  std::map<std::string, int> sentSubscriptions_;
  int fallbackTick_ = 0;
  std::chrono::steady_clock::time_point startedAt_;
  std::vector<rpc::Json> pendingFrameEvents_;

  struct DecodedFrame {
    // Full-resolution I420 planes (Y + U + V tightly packed). The compositor
    // uploads these to the GPU and converts to RGB in-shader.
    std::shared_ptr<const std::vector<std::uint8_t>> i420;
    int width = 0;   // luma width
    int height = 0;  // luma height
    std::int64_t frameId = 0;
    // When the ingest thread first OBSERVED this frame in shared memory. The
    // render thread fetches decoded frames on its own 16.7ms cadence, so this is
    // what makes the ingest->render handoff latency measurable. Cross-process
    // stamping would need a wire change to ShmFrameHeader, which would break an
    // already-built engine binary — so the unmeasured head of the path is bounded
    // by the ingest poll interval instead (see kVideoIngestPollMs).
    std::chrono::steady_clock::time_point observedAt{};
  };
  // Latest decoded I420 frame per participantId, tapped alongside the stdout
  // event queue so the compositor can read real pixels without draining the
  // multiview event path.
  std::map<std::string, DecodedFrame> latestDecodedFrames_;

  // Pending real PCM per participantId. The reader thread appends a chunk on
  // every engine audio event (snapshot-read from the per-source audio SHM
  // region); the audio worker drains the WHOLE buffer once per tick via
  // pollCompositorAudioFrames as ONE coalesced AudioFrame per source, so
  // multiple 10ms Zoom packets per ~20ms tick reach the mixer as contiguous
  // samples instead of overlap-summing. Bounded (~1s per source, drop-oldest).
  // Guarded by mutex_.
  std::map<std::string, ZoomEnginePendingAudio> pendingAudio_;
  // Z2b: persistently-open audio ring regions, drained on the 50Hz poll (the
  // per-pipe-event open/drain/close cycle lost hundreds of packets/source at
  // 500 events/s - the soak harness ring telemetry caught it). Keyed by the
  // stream source uuid; pendingKey routes chunks to the right mixer channel.
  // Region held opaquely (engine-ipc.h with its windows.h stays out of this
  // public header); the .cpp reinterprets to ShmRegion.
  struct AudioStreamRef {
    std::string pendingKey;
    void* regionOpaque = nullptr;  // owned ShmRegion*, destroyed on close
  };
  std::map<std::string, AudioStreamRef> audioStreams_;
  std::string mixStreamUuid_;  // the ONE live mixed-audio stream (see ingest)
  void drainAudioStreamLocked(const std::string& uuid, AudioStreamRef& ref);
  void closeAudioStreamsLocked();
  // Video-beacon fix (soak-measured queue drowning: per-frame events at 30fps
  // x N sources each did a full I420 copy + CPU thumbnail under the lock).
  // Frame events now REGISTER streams; frames are read on the render poll,
  // gated by a header-sequence peek (unchanged frame = 16-byte read).
  struct VideoStreamRef {
    std::uint32_t participantId = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t lastSequence = 0;
    // Thumbnail-event pacing (2026-07-19 system-audio clicking): every ingested
    // frame used to emit a 640x360 base64 event; the zoom pump stringified
    // ~30/s/participant (~1.2MB each) and latest-wins dropped most of them —
    // a full core of allocator/memory churn that glitched OTHER apps' audio.
    // -1 = nothing emitted yet (first frame emits immediately so first-frame
    // validation stays fast); thereafter ~2/s.
    std::int64_t lastThumbnailEmitMs = -1;
    // shared_ptr so the UNLOCKED snapshot phase can hold the mapping alive
    // while leave/reset paths release their reference under the lock.
    std::shared_ptr<void> regionOpaque;
  };
  std::map<std::string, VideoStreamRef> videoStreams_;
  // Three-phase video drain (audio-starvation fix: frame copies + thumbnail
  // conversion must NEVER run under mutex_ - they blocked the audio poll and
  // halved the worker tick rate). Peek/publish lock briefly; snapshot runs
  // unlocked on seqlock-protected regions.
  void drainVideoStreamsThreePhase();
  // Dedicated ingest thread: runs the three phases on its OWN ~120Hz cadence
  // so neither the render tick nor the audio worker ever carries pixel work
  // (run-15: phase 2 inside the render tick collapsed the audio worker to
  // 8 ticks/s via coreMutex contention).
  std::thread videoIngestThread_;
  std::atomic<bool> videoIngestRun_{false};
  void videoIngestLoop();
  void ensureVideoIngestThreadLocked();
  // `i420` is the decoded plane set, already owned by a shared buffer built in
  // the UNLOCKED snapshot phase — publishing must never copy pixels under mutex_
  // (the render thread waits on it while holding coreMutex).
  void publishVideoFrameLocked(const std::string& uuid, VideoStreamRef& ref,
                               const ZoomEngineRgbaFrame& frame,
                               std::shared_ptr<const std::vector<std::uint8_t>> i420,
                               std::chrono::steady_clock::time_point observedAt);
  void closeVideoStreamsLocked();
};

}  // namespace corevideo::modules
