#pragma once

#include "modules/Interfaces.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
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
//   - submitAudio: enqueue and return. Audio drops are audible, so the audio cap
//     is generous; beyond `maxAudioQueue` the oldest packet is dropped.
//   - configureRecording / start: NON-BLOCKING. They are re-emitted every sync
//     while recording, so a blocking wait would couple the command thread to the
//     writer's queue drain each tick. Ordering (container open ahead of frames)
//     is preserved by the single FIFO queue; start() returns an optimistic
//     active snapshot the writer reconciles with the wrapped session shortly.
//   - stopRecording: NON-BLOCKING. The caller holds coreMutex, so it only enqueues
//     the stop and returns instantly (the operator's stop must never stall under
//     load); the writer finalizes the container asynchronously. The bounded
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
    // Max pending VIDEO frames before drop-to-latest kicks in.
    size_t maxVideoQueue = 6;
    // Max pending AUDIO packets before the oldest is dropped.
    size_t maxAudioQueue = 96;
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
  // A3: forwarded straight to the inner sink — the interface contract makes
  // the implementation thread-safe (an atomic store), so no queue item.
  void setAudioContentLatencySamples(int latencySamples) override;
  void stopRecording() override;
  OutputSession session() const override;

  // Test/diagnostic accessors (not part of IEncoderSink).
  // Number of video / audio items dropped by the backlog policy so far.
  [[nodiscard]] uint64_t droppedVideoFrames() const;
  [[nodiscard]] uint64_t droppedAudioPackets() const;
  // Block until every item enqueued so far has been applied by the writer, or
  // `timeout` elapses. Returns true if fully drained. Lets tests observe the
  // deterministic post-drain session() without sleeping on wall-clock guesses.
  bool drainForTest(std::chrono::milliseconds timeout);

 private:
  enum class Kind { Configure, Start, Video, IsoVideo, Audio, IsoAudio, StopRecording };

  struct Item {
    Kind kind;
    uint64_t seq = 0;
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
    bool stop = false;
    bool writerDone = false;

    std::atomic<bool> active{false};
    std::atomic<uint64_t> droppedVideo{0};
    std::atomic<uint64_t> droppedAudio{0};

    std::mutex snapshotMutex;
    OutputSession snapshot;

    size_t maxVideoQueue = 6;
    size_t maxAudioQueue = 96;
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
