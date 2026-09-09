#pragma once

#include "modules/Interfaces.h"

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace corevideo::modules {

// Decorator that makes the per-frame encoder path NON-BLOCKING so a slow disk
// (Media Foundation WriteSample) or a stalled recording writer can never wedge
// the audio/output worker that calls submit()/submitAudio() every tick.
//
// WHY: the Phase 2 audio/output worker runs the mix, monitor render, loudness
// meter, encoder submit and output-sender sync under `audioOutputMutex_`. When
// the wrapped encoder's submit blocked on disk I/O under load, the whole worker
// collapsed (soak: 47 -> 0.6 ticks/s) AND — because stop-recording also needs
// `audioOutputMutex_` to finalize the container — the operator could not stop
// the recording for minutes. Draining the encoder onto its own writer thread
// keeps the worker's mutex hold bounded (a cheap enqueue), so it never collapses
// and stop-recording gets the lock promptly.
//
// POLICY:
//   - submit(video): enqueue and return. When the pending-video backlog exceeds
//     `maxVideoQueue`, the OLDEST pending frame is dropped (drop-to-latest) so
//     the recording degrades to a lower effective fps instead of the worker
//     stalling or memory growing without bound.
//     Caps cover ALL queued generations. If older stopped takes hold the entire
//     budget, new incoming media is dropped instead, preserving their tail.
//   - submitAudio: enqueue and return. Audio drops are audible, so the audio cap
//     is generous; beyond `maxAudioQueue` the oldest packet is dropped.
//   - configureRecording / start: NON-BLOCKING. They are re-emitted every sync
//     while recording, so a blocking wait would couple the command thread to the
//     writer's queue drain each tick. Ordering (container open ahead of frames)
//     is preserved by the single FIFO queue; start() returns starting until
//     the writer reports actual output progress.
//   - stopRecording: NON-BLOCKING. The caller holds coreMutex, so it closes the
//     producer gate and enqueues a FIFO barrier. Already-accepted media drains
//     before asynchronous Finalize, preserving the take's A/V tail. The bounded
//     finalize GRACE is enforced at teardown (destructor), where no lock is held.
//   - session(): returns a thread-safe snapshot the writer refreshes after every
//     applied item (eventually consistent within a few frames — fine for the
//     live app; unit tests that need exact synchronous counts use the wrapped
//     sink directly).
//
// The writer thread is the SOLE owner of the wrapped sink, so no external lock is
// needed around it. All writer-touched state lives in a shared control block so
// teardown can bound its wait: the destructor signals the writer and joins within
// `finalizeGrace`; if the writer is still stuck in a blocking Finalize past the
// grace it is DETACHED — it keeps the shared block (and the wrapped sink) alive
// via its own shared_ptr, so process shutdown never hangs and nothing dangles.
class AsyncEncoderSink final : public IEncoderSink {
 public:
  struct Options {
    // Max pending PROGRAM video frames before drop-to-latest kicks in.
    size_t maxVideoQueue = 6;
    // ISO video is enqueued one source per item and coalesced by sourceId. Eight
    // slots retain at most the latest frame for each supported Zoom ISO.
    size_t maxIsoVideoQueue = 8;
    // Max pending PROGRAM audio packets before the oldest is dropped.
    size_t maxAudioQueue = 96;
    // ISO audio is wall-clock anchored and silence-fills a dropped tick. A small
    // queue is therefore both safe and essential: one item fans out to every
    // armed ISO AAC writer.
    size_t maxIsoAudioQueue = 4;
    // Bounded wait for teardown's writer join (the finalize grace at shutdown).
    std::chrono::milliseconds finalizeGrace{4000};
  };

  explicit AsyncEncoderSink(std::unique_ptr<IEncoderSink> inner);
  AsyncEncoderSink(std::unique_ptr<IEncoderSink> inner, Options options);
  ~AsyncEncoderSink() override;

  AsyncEncoderSink(const AsyncEncoderSink&) = delete;
  AsyncEncoderSink& operator=(const AsyncEncoderSink&) = delete;

  void configureRecording(const RecordingSessionRequest& request) override;
  OutputSession start(const std::vector<std::string>& destinations,
                      const std::vector<std::string>& isoParticipantIds) override;
  void submit(const ProgramFrame& frame) override;
  void submitIsoVideo(const std::vector<IsoSourceVideoFrame>& sources) override;
  void submitIsoAudio(const std::vector<IsoSourceAudio>& sources) override;
  void submitAudio(const float* interleaved, int frameCount, int channels, int sampleRate) override;
  void submitAudioAt(const float* interleaved, int frameCount, int channels, int sampleRate,
                     int64_t timelineTimestamp100ns) override;
  // A3: forwarded straight to the inner sink — the interface contract makes
  // the implementation thread-safe (an atomic store), so no queue item.
  void setAudioContentLatencySamples(int latencySamples) override;
  void stopRecording() override;
  OutputSession session() const override;

  // Test/diagnostic accessors (not part of IEncoderSink).
  // Number of video / audio items dropped by the backlog policy in STEADY
  // STATE. Startup shedding is counted separately (see below) so a clean run
  // does not report steady-state loss it did not have.
  [[nodiscard]] uint64_t droppedVideoFrames() const;
  [[nodiscard]] uint64_t droppedAudioPackets() const;
  // Video items shed while the writer was still applying Start. The Media
  // Foundation open is synchronous and runs as a FIFO item on the writer thread
  // (measured 95-250ms), while the producer keeps submitting at 60Hz because
  // `recording.status` already reads "recording". Those 7-9 frames are a
  // CLIPPED HEAD, not steady-state loss: no frame is missing from the file
  // (muxed count equals submitted count, preroll zero), the first 95-250ms of
  // the show simply is not in it. Audio has had this distinction since
  // recordingStartupDroppedAudioPackets; video did not, so every startup drop
  // landed in droppedVideo and made a fail-closed judge report a false red.
  // Reported, never hidden.
  [[nodiscard]] uint64_t startupDroppedVideoFrames() const;
  // Block until every item enqueued so far has been applied by the writer, or
  // `timeout` elapses. Returns true if fully drained. Lets tests observe the
  // deterministic post-drain session() without sleeping on wall-clock guesses.
  bool drainForTest(std::chrono::milliseconds timeout);

  // Process-lifetime counters, explicitly distinct from media written. Array
  // order: configure, start, program-video, iso-video, program-audio, iso-audio,
  // stop. Completed calls include exceptions and may be no-ops; only writer
  // counters prove progress. Times are steady-clock milliseconds (not UTC),
  // zero means unobserved. Written counts are the inner sink's reported Program
  // counts, not durable bytes or ISO progress. No per-frame history is retained.
  // ISO-3 (fidelity): per-source ISO VIDEO accounting. `framesWritten` on an
  // ISO stream is an APPEND count and says nothing about how many DISTINCT
  // source pictures reached the file, which made any change to the ISO cadence
  // unverifiable. These are counted at the two places that can tell the
  // difference: the producer-side (sourceId, frameId) dedup, and the writer.
  struct IsoVideoSourceEvidence {
    uint64_t submitted = 0;          // distinct (sourceId, frameId) accepted into the queue
    uint64_t duplicateRejected = 0;  // re-submissions of a frame id already accepted
    uint64_t dropped = 0;            // shed by the backlog policy before the writer saw them
    uint64_t written = 0;            // items the writer applied for this source
  };

  struct Evidence {
    std::array<uint64_t, 7> enqueued{}, completedCalls{}, queuedByKind{};
    uint64_t generation = 0, operationGeneration = 0, operationSequence = 0;
    std::string operation = "idle";
    std::string lifecycleState = "unavailable";
    int64_t operationStartedMs = 0, operationAgeMs = 0;
    uint64_t queueDepth = 0;
    uint64_t droppedVideo = 0, droppedAudio = 0;
    // Startup shedding, kept OUT of droppedVideo (see startupDroppedVideoFrames).
    uint64_t startupDroppedVideo = 0;
    std::map<std::string, IsoVideoSourceEvidence> isoVideoBySource;
    int64_t oldestQueuedAgeMs = 0, lastWriterProgressMs = 0;
    int64_t programVideoWritten = 0, programAudioPacketsWritten = 0;
    uint64_t writtenGeneration = 0;
    uint64_t stopGeneration = 0;
    int64_t stopRequestedMs = 0, finalizeStartedMs = 0, finalizeFinishedMs = 0;
    std::string finalizeResult = "not-requested";
    std::string firstFailure;
    uint64_t firstFailureGeneration = 0;
    int64_t firstFailureMs = 0;
  };
  [[nodiscard]] Evidence evidence() const;

 private:
  enum class Kind { Configure, Start, Video, IsoVideo, Audio, IsoAudio, StopRecording };

  struct Item {
    Kind kind;
    uint64_t seq = 0;
    uint64_t generation = 0;
    int64_t enqueuedMs = 0;
    // Configure
    RecordingSessionRequest request;
    // Start
    std::vector<std::string> destinations;
    std::vector<std::string> isoParticipantIds;
    // Video
    ProgramFrame frame;
    // IsoVideo (zero-copy: each entry's payload is a shared_ptr)
    std::vector<IsoSourceVideoFrame> isoSources;
    // IsoAudio (ISO-2: per-source raw-stem PCM for this tick)
    std::vector<IsoSourceAudio> isoAudioSources;
    // Audio
    std::vector<float> audioPcm;
    int audioFrameCount = 0;
    int audioChannels = 0;
    int audioSampleRate = 0;
    int64_t audioTimelineTimestamp100ns = 0;
  };

  // All state the (possibly-detached) writer thread touches. Held by shared_ptr
  // so a stuck writer keeps it alive after the owning sink is destroyed.
  struct State {
    std::unique_ptr<IEncoderSink> inner;

    std::mutex queueMutex;
    std::condition_variable queueCv;    // writer waits for work
    std::condition_variable appliedCv;  // control ops wait for apply
    std::deque<Item> queue;
    uint64_t nextSeq = 1;
    uint64_t appliedSeq = 0;
    uint64_t generation = 0;
    Evidence evidence;
    std::string configuredSessionId = "recording";
    // Separate from active: a failed writer still needs one cleanup/finalize.
    bool stopRequested = true;
    std::string epoch = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    bool applying = false;
    bool stop = false;
    bool writerDone = false;

    // Producer-side held-frame suppression. MediaCore intentionally re-submits
    // the latest Program/ISO frame from the audio tick; letting those identical
    // frames enter the bounded queue made them look like real drops and could
    // keep the writer permanently busy with work the mux clock would discard.
    bool hasLastProgramFrameNumber = false;
    int64_t lastProgramFrameNumber = 0;
    std::map<std::string, int64_t> lastIsoFrameIdBySource;
    // ISO-3 fidelity counters, guarded by queueMutex.
    std::map<std::string, IsoVideoSourceEvidence> isoVideoBySource;

    // Weighted fairness for the single writer. Strict Program priority starved
    // every ISO whenever Program audio/video arrived continuously (the live
    // eight-source failure wrote one ISO frame, then never serviced ISO again).
    size_t consecutiveProgramItems = 0;

    // True from the moment a Start is enqueued until the writer has APPLIED it
    // (the synchronous Media Foundation open). Video shed inside that window is
    // a clipped head, not steady-state loss — see startupDroppedVideoFrames.
    // Guarded by queueMutex, like the rest of the queue bookkeeping.
    bool videoStartupPhase = false;

    std::atomic<bool> active{false};
    std::atomic<uint64_t> droppedVideo{0};
    std::atomic<uint64_t> droppedAudio{0};
    std::atomic<uint64_t> startupDroppedVideo{0};

    std::mutex snapshotMutex;
    OutputSession snapshot;

    size_t maxVideoQueue = 6;
    size_t maxIsoVideoQueue = 8;
    size_t maxAudioQueue = 96;
    size_t maxIsoAudioQueue = 4;
  };

  // Enqueue `item`, assigning it a seq. Applies the drop policy for Video/Audio.
  // Returns the assigned seq.
  uint64_t enqueue(Item&& item);
  // Wait until the writer has applied the item with seq >= `seq`, or `timeout`.
  bool waitApplied(uint64_t seq, std::chrono::milliseconds timeout);
  static void writerLoop(std::shared_ptr<State> state);

  Options options_;
  std::shared_ptr<State> state_;
  std::thread writer_;
};

}  // namespace corevideo::modules
