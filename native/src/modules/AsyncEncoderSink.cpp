#include "core/BoundedAsyncLog.h"
#include "modules/AsyncEncoderSink.h"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <utility>

namespace corevideo::modules {
namespace {
int64_t evidenceNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
constexpr const char* operationNames[] = {
    "configure", "start", "program-video", "iso-video", "program-audio", "iso-audio", "stop"};
}  // namespace

AsyncEncoderSink::AsyncEncoderSink(std::unique_ptr<IEncoderSink> inner)
    : AsyncEncoderSink(std::move(inner), Options{}) {}

AsyncEncoderSink::AsyncEncoderSink(std::unique_ptr<IEncoderSink> inner, Options options)
    : options_(options), state_(std::make_shared<State>()) {
  state_->inner = std::move(inner);
  state_->maxVideoQueue = std::max<size_t>(1, options_.maxVideoQueue);
  state_->maxIsoVideoQueue = std::max<size_t>(1, options_.maxIsoVideoQueue);
  state_->maxAudioQueue = std::max<size_t>(1, options_.maxAudioQueue);
  state_->maxIsoAudioQueue = std::max<size_t>(1, options_.maxIsoAudioQueue);
  if (state_->inner) {
    state_->snapshot = state_->inner->session();
  }
  writer_ = std::thread(&AsyncEncoderSink::writerLoop, state_);
}

AsyncEncoderSink::~AsyncEncoderSink() {
  {
    std::lock_guard<std::mutex> lock(state_->queueMutex);
    state_->stop = true;
  }
  state_->queueCv.notify_all();

  // Bounded teardown: wait for the writer to finish (finalize the container) up
  // to finalizeGrace. If it is still stuck in a blocking Finalize, DETACH it so
  // process shutdown never hangs — the writer holds its own shared_ptr to State,
  // so the wrapped sink stays alive until the write actually completes.
  bool done = false;
  {
    std::unique_lock<std::mutex> lock(state_->queueMutex);
    done = state_->appliedCv.wait_for(lock, options_.finalizeGrace,
                                      [&] { return state_->writerDone; });
  }
  if (done) {
    if (writer_.joinable()) {
      writer_.join();
    }
  } else {
    ::corevideo::core::nativeLogf("[asyncEncoder] writer still finalizing after %lldms grace; detaching for shutdown\n",
                 static_cast<long long>(options_.finalizeGrace.count()));
    if (writer_.joinable()) {
      writer_.detach();
    }
  }
}

uint64_t AsyncEncoderSink::enqueue(Item&& item) {
  uint64_t seq = 0;
  {
    std::lock_guard<std::mutex> lock(state_->queueMutex);

    // Re-check the recording gate while holding the same mutex used by the
    // Stop barrier. A producer may have observed active=true immediately before
    // Stop acquired the mutex; without this check that stale submission could
    // land after Finalize and leak into the next recording generation.
    const bool media = item.kind == Kind::Video || item.kind == Kind::IsoVideo ||
                       item.kind == Kind::Audio || item.kind == Kind::IsoAudio;
    if (media && !state_->active.load(std::memory_order_acquire)) {
      return 0;
    }

    // Suppress held-frame re-submissions before they consume queue capacity.
    // This is deliberately producer-side: downstream RecordingPtsClock dedup is
    // too late to prevent starvation and misleading drop telemetry.
    if (item.kind == Kind::Video) {
      if (state_->hasLastProgramFrameNumber &&
          state_->lastProgramFrameNumber == item.frame.frameNumber) {
        return 0;
      }
    } else if (item.kind == Kind::IsoVideo && item.isoSources.size() == 1) {
      const auto& source = item.isoSources.front();
      const auto last = state_->lastIsoFrameIdBySource.find(source.sourceId);
      if (last != state_->lastIsoFrameIdBySource.end() && last->second == source.frame.frameId) {
        ++state_->isoVideoBySource[source.sourceId].duplicateRejected;
        return 0;
      }
    } else if (item.kind == Kind::Start) {
      // Zoom/shared-memory frame sequences may restart between meeting or
      // recording generations, so no dedup identity crosses a Start barrier.
      state_->hasLastProgramFrameNumber = false;
      state_->lastIsoFrameIdBySource.clear();
      state_->isoVideoBySource.clear();
      state_->consecutiveProgramItems = 0;
      // The wrapped writer's open is SYNCHRONOUS and applies as a FIFO item on
      // the writer thread (95-250ms on Windows). Everything the producer submits
      // meanwhile is head-of-show, not steady-state loss.
      state_->videoStartupPhase = true;
      // A new take's evidence starts empty: no open applied, nothing written.
      state_->startApplied.store(false, std::memory_order_release);
      state_->everProgressed.store(false, std::memory_order_release);
      state_->lastProgressAtMs.store(0, std::memory_order_release);
      state_->degradedWarning.store(false, std::memory_order_release);
    }

    if (item.kind == Kind::Configure) {
      state_->configuredSessionId = item.request.sessionId;
      item.generation = state_->generation + 1;
    } else if (item.kind == Kind::Start) {
      item.generation = ++state_->generation;
      state_->stopRequested = false;
      state_->active.store(true);
      std::lock_guard<std::mutex> snapshotLock(state_->snapshotMutex);
      state_->snapshot = OutputSession{};
      state_->snapshot.destinations = item.destinations;
      if (std::find(item.destinations.begin(), item.destinations.end(), "recording") != item.destinations.end()) {
        // A Start command that has been ACCEPTED. Nothing has been opened and
        // nothing has been written, so this is `requested` and never more —
        // the state advances only on observed writer evidence (Rule 7).
        const auto requested = ::corevideo::core::OutputLifecyclePolicy::requested();
        state_->snapshot.lifecycle = contracts::OutputLifecycle{
            state_->configuredSessionId + ":" + state_->epoch + ":" + std::to_string(item.generation),
            true, requested.state, requested.health, false, std::nullopt};
      }
    } else {
      item.generation = state_->generation;
    }

    seq = state_->nextSeq++;
    item.seq = seq;

    // Drop-to-latest / bounded backlog: when the pending count for this item's
    // media kind is at capacity, drop the OLDEST pending item of that kind so we
    // keep flowing the freshest frames rather than blocking or growing unbounded.
    if (item.kind == Kind::Video || item.kind == Kind::IsoVideo || item.kind == Kind::Audio ||
        item.kind == Kind::IsoAudio) {
      const Kind kind = item.kind;
      // ISO video items carry exactly one source (submitIsoVideo splits the
      // batch). Replace that source's older pending frame before applying the
      // global cap, so a fast participant cannot evict every slower guest.
      const size_t isoVideoCap = state_->maxIsoVideoQueue;
      if (kind == Kind::IsoVideo && item.isoSources.size() == 1) {
        // ONLY when the ISO budget is actually full. This used to fire
        // unconditionally, which was a silent fidelity ceiling: a producer that
        // legitimately hands the sink two DISTINCT frames for one source in
        // quick succession (which is exactly what an arrival-driven ISO drain
        // does when it catches up) had the first erased by the second, counted
        // as a drop. The stated purpose of this erase is to stop a fast
        // participant evicting every slower guest when the global cap bites, so
        // gate it on the cap and it keeps that purpose and loses the ceiling.
        const std::string& sourceId = item.isoSources.front().sourceId;
        size_t pendingIso = 0;
        for (const auto& queued : state_->queue) {
          if (queued.kind == Kind::IsoVideo) ++pendingIso;
        }
        if (pendingIso >= isoVideoCap) {
          for (auto it = state_->queue.begin(); it != state_->queue.end(); ++it) {
            if (it->generation == item.generation && it->kind == Kind::IsoVideo && it->isoSources.size() == 1 &&
                it->isoSources.front().sourceId == sourceId) {
              ++state_->isoVideoBySource[sourceId].dropped;
              state_->queue.erase(it);
              if (state_->videoStartupPhase) {
                state_->startupDroppedVideo.fetch_add(1);
              } else {
                state_->droppedVideo.fetch_add(1);
              }
              break;
            }
          }
        }
      }
      // ISO audio drops-to-latest on the AUDIO budget but with its OWN pending
      // accounting (a separate Kind) so it can NEVER evict a program-audio
      // packet — program is priority-1 (spec §9). A dropped ISO-audio tick
      // becomes silence in the stem (the next tick's wall-anchored silence-fill
      // covers the gap), the timeline stays aligned, program is untouched.
      const size_t cap = kind == Kind::Video      ? state_->maxVideoQueue
                         : kind == Kind::IsoVideo ? state_->maxIsoVideoQueue
                         : kind == Kind::Audio    ? state_->maxAudioQueue
                                                  : state_->maxIsoAudioQueue;
      size_t pending = 0;
      for (const auto& queued : state_->queue) {
        if (queued.kind == kind) {
          ++pending;
        }
      }
      if (pending >= cap) {
        // The budget belongs to the sink, not each queued take. Preserve all
        // media accepted before an older take's Stop barrier: a new generation
        // may replace its own pending media, but must drop its incoming item
        // when older generations occupy the entire budget.
        if (kind == Kind::Audio || kind == Kind::IsoAudio) {
          state_->droppedAudio.fetch_add(1);
        } else if (state_->videoStartupPhase) {
          // Head-of-show shedding behind the writer's synchronous open. Visible
          // and attributable, but NOT folded into the steady-state counter the
          // fail-closed judges watch.
          state_->startupDroppedVideo.fetch_add(1);
        } else {
          state_->droppedVideo.fetch_add(1);
        }
        if (kind == Kind::IsoVideo && item.isoSources.size() == 1) {
          ++state_->isoVideoBySource[item.isoSources.front().sourceId].dropped;
        }
        bool replaced = false;
        for (auto it = state_->queue.begin(); it != state_->queue.end(); ++it) {
          if (it->generation == item.generation && it->kind == kind) {
            state_->queue.erase(it);
            replaced = true;
            break;
          }
        }
        if (!replaced) return 0;
      }
    }

    // Only accepted frames become dedup identities. A held frame rejected
    // while an older take owns the budget must be retryable after it drains.
    if (item.kind == Kind::Video) {
      state_->hasLastProgramFrameNumber = true;
      state_->lastProgramFrameNumber = item.frame.frameNumber;
    } else if (item.kind == Kind::IsoVideo && item.isoSources.size() == 1) {
      const auto& source = item.isoSources.front();
      state_->lastIsoFrameIdBySource[source.sourceId] = source.frame.frameId;
      ++state_->isoVideoBySource[source.sourceId].submitted;
    }
    item.enqueuedMs = evidenceNowMs();
    ++state_->evidence.enqueued[static_cast<size_t>(item.kind)];
    state_->queue.push_back(std::move(item));
  }
  state_->queueCv.notify_one();
  return seq;
}

bool AsyncEncoderSink::waitApplied(uint64_t seq, std::chrono::milliseconds timeout) {
  (void)seq;
  std::unique_lock<std::mutex> lock(state_->queueMutex);
  return state_->appliedCv.wait_for(lock, timeout,
                                    [&] {
                                      return (!state_->applying && state_->queue.empty()) ||
                                             state_->writerDone;
                                    });
}

void AsyncEncoderSink::configureRecording(const RecordingSessionRequest& request) {
  // NON-BLOCKING: configure/start are re-emitted every sync while recording, so a
  // blocking wait here would couple the command thread to the writer's queue drain
  // on every tick. Ordering is preserved by the single FIFO queue, so we just
  // enqueue and return; the writer applies it ahead of the frames behind it.
  Item item;
  item.kind = Kind::Configure;
  item.request = request;
  enqueue(std::move(item));
}

OutputSession AsyncEncoderSink::start(const std::vector<std::string>& destinations,
                                      const std::vector<std::string>& isoParticipantIds) {
  Item item;
  item.kind = Kind::Start;
  item.destinations = destinations;
  item.isoParticipantIds = isoParticipantIds;
  enqueue(std::move(item));  // NON-BLOCKING, ordered via FIFO
  return session();
}

void AsyncEncoderSink::submit(const ProgramFrame& frame) {
  // Before the first start() the wrapped submit is a no-op, so an idle app never
  // pays the per-frame copy.
  if (!state_->active.load()) {
    return;
  }
  Item item;
  item.kind = Kind::Video;
  item.frame = frame;
  enqueue(std::move(item));
}

void AsyncEncoderSink::submitIsoVideo(const std::vector<IsoSourceVideoFrame>& sources) {
  if (!state_->active.load() || sources.empty()) {
    return;
  }
  // Split the eight-source batch so the writer can return to priority-1 Program
  // A/V between individual ISO encodes. Payloads remain zero-copy shared_ptrs;
  // enqueue coalesces each source to its latest pending frame.
  for (const auto& source : sources) {
    Item item;
    item.kind = Kind::IsoVideo;
    item.isoSources.push_back(source);
    enqueue(std::move(item));
  }
}

void AsyncEncoderSink::submitIsoAudio(const std::vector<IsoSourceAudio>& sources) {
  if (!state_->active.load() || sources.empty()) {
    return;
  }
  // The PCM vectors are small (~one 20ms tick per source) and copied by value —
  // safe to hand across to the writer thread. Drops-to-latest on the audio
  // budget with its OWN accounting (never evicts program audio).
  Item item;
  item.kind = Kind::IsoAudio;
  item.isoAudioSources = sources;
  enqueue(std::move(item));
}

void AsyncEncoderSink::submitAudio(const float* interleaved, int frameCount, int channels, int sampleRate) {
  submitAudioAt(interleaved, frameCount, channels, sampleRate,
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() / 100);
}

void AsyncEncoderSink::submitAudioAt(const float* interleaved, int frameCount, int channels, int sampleRate,
                                    int64_t timelineTimestamp100ns) {
  if (!state_->active.load() || interleaved == nullptr || frameCount <= 0 || channels <= 0) {
    return;
  }
  Item item;
  item.kind = Kind::Audio;
  item.audioPcm.assign(interleaved, interleaved + static_cast<size_t>(frameCount) * static_cast<size_t>(channels));
  item.audioFrameCount = frameCount;
  item.audioChannels = channels;
  item.audioSampleRate = sampleRate;
  item.audioTimelineTimestamp100ns = timelineTimestamp100ns;
  enqueue(std::move(item));
}

void AsyncEncoderSink::setAudioContentLatencySamples(int latencySamples) {
  // Thread-safe by interface contract (the inner sink stores an atomic) — no
  // queue item, so the value is in place before the next Audio item applies.
  state_->inner->setAudioContentLatencySamples(latencySamples);
}

void AsyncEncoderSink::stopRecording() {
  // NON-BLOCKING: the caller (MediaCore::stopRecordingSession) holds coreMutex, so
  // blocking here would stall the render thread for the whole finalize. Instead we
  // just enqueue the stop; the writer thread finalizes the container (moov write)
  // asynchronously within a frame or two, so the operator's stop returns instantly
  // even under disk load and the file becomes playable moments later. The bounded
  // finalize GRACE is enforced at teardown (destructor), where no lock is held.
  Item item;
  item.kind = Kind::StopRecording;
  {
    std::lock_guard<std::mutex> lock(state_->queueMutex);
    // Desired-state sync may repeat Stop while Finalize is blocked. One
    // barrier per generation keeps that repetition bounded and preserves the
    // observed finalizing/completed state. Do not use the media gate here:
    // writer failure closes it before the required cleanup Stop is submitted.
    if (state_->stopRequested) return;
    state_->stopRequested = true;
    // Close the producer gate under queueMutex, then append a FIFO control
    // barrier. Every media item accepted before this point is written before
    // Finalize; every racing or later submission is rejected by enqueue().
    // The old implementation erased the pending tail here, which produced the
    // measured ~400ms Program/ISO A/V duration mismatch at every stop.
    state_->active.store(false, std::memory_order_release);
    item.seq = state_->nextSeq++;
    item.generation = state_->generation;
    item.enqueuedMs = evidenceNowMs();
    ++state_->evidence.enqueued[static_cast<size_t>(item.kind)];
    state_->evidence.stopGeneration = item.generation;
    state_->evidence.stopRequestedMs = item.enqueuedMs;
    state_->evidence.finalizeStartedMs = 0;
    state_->evidence.finalizeFinishedMs = 0;
    state_->evidence.finalizeResult = "pending";
    state_->queue.push_back(std::move(item));
    std::lock_guard<std::mutex> snapshotLock(state_->snapshotMutex);
    state_->snapshot.active = false;
    if (state_->snapshot.lifecycle) {
      state_->snapshot.lifecycle->desiredActive = false;
      // Stop has BEGUN. It does not claim, imply or approximate completion:
      // the barrier has not drained and Finalize has not run. The terminal
      // state arrives from the writer thread when it actually does.
      if (state_->snapshot.lifecycle->state != "failed")
        state_->snapshot.lifecycle->state = ::corevideo::core::OutputLifecyclePolicy::stopping().state;
    }
  }
  state_->queueCv.notify_one();
}

void AsyncEncoderSink::setProducingStaleMsForTest(int64_t staleMs) {
  state_->producingStaleMs.store(staleMs, std::memory_order_release);
}

// The whole point of PR22's second defect: `producing` is not a latch. It is
// re-decided against the clock every time anyone reads the session, from
// evidence that only real writer progress can refresh. A writer wedged inside
// the wrapped sink applies no further items and therefore publishes no further
// snapshots, so without this a stalled recording reports healthy forever.
void AsyncEncoderSink::refreshActiveLifecycle(const State& state, contracts::OutputLifecycle& lifecycle) {
  if (!lifecycle.desiredActive) return;
  // Stopping/finalizing/terminal states are owned by the Stop barrier.
  if (lifecycle.state == "stopping" || lifecycle.state == "finalizing" ||
      ::corevideo::core::OutputLifecyclePolicy::isTerminal(lifecycle.state))
    return;
  ::corevideo::core::ActiveOutputObservation observation;
  observation.startApplied = state.startApplied.load(std::memory_order_acquire);
  observation.everProgressed = state.everProgressed.load(std::memory_order_acquire);
  observation.lastProgressMs = state.lastProgressAtMs.load(std::memory_order_acquire);
  observation.nowMs = evidenceNowMs();
  observation.staleMs = state.producingStaleMs.load(std::memory_order_acquire);
  observation.degraded = state.degradedWarning.load(std::memory_order_acquire);
  const auto decision = ::corevideo::core::OutputLifecyclePolicy::evaluateActive(observation);
  lifecycle.state = decision.state;
  lifecycle.health = decision.health;
}

OutputSession AsyncEncoderSink::session() const {
  std::lock_guard<std::mutex> lock(state_->snapshotMutex);
  auto snapshot = state_->snapshot;
  if (snapshot.lifecycle) {
    refreshActiveLifecycle(*state_, *snapshot.lifecycle);
    snapshot.active = snapshot.lifecycle->state == "producing";
  }
  snapshot.encoderQueueDroppedVideoFrames =
      static_cast<int64_t>(state_->droppedVideo.load(std::memory_order_relaxed));
  snapshot.encoderQueueDroppedAudioPackets =
      static_cast<int64_t>(state_->droppedAudio.load(std::memory_order_relaxed));
  snapshot.recordingStartupDroppedVideoFrames =
      static_cast<int64_t>(state_->startupDroppedVideo.load(std::memory_order_relaxed));
  return snapshot;
}

uint64_t AsyncEncoderSink::droppedVideoFrames() const { return state_->droppedVideo.load(); }
uint64_t AsyncEncoderSink::droppedAudioPackets() const { return state_->droppedAudio.load(); }
uint64_t AsyncEncoderSink::startupDroppedVideoFrames() const {
  return state_->startupDroppedVideo.load();
}

AsyncEncoderSink::Evidence AsyncEncoderSink::evidence() const {
  std::lock_guard<std::mutex> lock(state_->queueMutex);
  auto result = state_->evidence;
  const auto now = evidenceNowMs();
  result.generation = state_->generation;
  result.queueDepth = state_->queue.size();
  result.droppedVideo = state_->droppedVideo.load(std::memory_order_relaxed);
  result.droppedAudio = state_->droppedAudio.load(std::memory_order_relaxed);
  result.startupDroppedVideo = state_->startupDroppedVideo.load(std::memory_order_relaxed);
  result.isoVideoBySource = state_->isoVideoBySource;
  for (const auto& item : state_->queue) {
    ++result.queuedByKind[static_cast<size_t>(item.kind)];
    result.oldestQueuedAgeMs = std::max(result.oldestQueuedAgeMs, now - item.enqueuedMs);
  }
  if (state_->applying) result.operationAgeMs = now - result.operationStartedMs;
  {
    std::lock_guard<std::mutex> snapshotLock(state_->snapshotMutex);
    if (state_->snapshot.lifecycle) {
      auto lifecycle = *state_->snapshot.lifecycle;
      refreshActiveLifecycle(*state_, lifecycle);
      result.lifecycleState = lifecycle.state;
    }
  }
  return result;
}

bool AsyncEncoderSink::drainForTest(std::chrono::milliseconds timeout) {
  uint64_t target = 0;
  {
    std::lock_guard<std::mutex> lock(state_->queueMutex);
    target = state_->nextSeq - 1;  // highest seq enqueued so far
  }
  return waitApplied(target, timeout);
}

void AsyncEncoderSink::writerLoop(std::shared_ptr<State> state) {
  uint64_t failedGeneration = 0;
  std::string generationFailure;
  int64_t startVideoCount = 0;
  bool madeProgress = false;
  for (;;) {
    Item item;
    {
      std::unique_lock<std::mutex> lock(state->queueMutex);
      state->queueCv.wait(lock, [&] { return !state->queue.empty() || state->stop; });
      if (state->queue.empty()) {
        // stop requested and nothing left to write — finalize done.
        state->writerDone = true;
        state->evidence.operation = "stopped";
        state->appliedCv.notify_all();
        return;
      }
      auto selected = state->queue.begin();
      const auto isControl = [](Kind kind) {
        return kind == Kind::Configure || kind == Kind::Start || kind == Kind::StopRecording;
      };
      // Never reorder across a control barrier. Within the media run before the
      // next barrier, give Program weighted priority but never absolute
      // priority. Absolute priority starved ISO forever under a continuous
      // Program A/V feed. Four Program writes per ISO write keeps Program
      // favored while guaranteeing every coalesced ISO source makes progress.
      if (!isControl(selected->kind)) {
        const auto barrier = std::find_if(state->queue.begin(), state->queue.end(),
                                          [&](const Item& queued) { return isControl(queued.kind); });
        const auto programAudio = std::find_if(
            state->queue.begin(), barrier, [](const Item& queued) { return queued.kind == Kind::Audio; });
        const auto programVideo = std::find_if(
            state->queue.begin(), barrier, [](const Item& queued) { return queued.kind == Kind::Video; });
        const auto iso = std::find_if(state->queue.begin(), barrier, [](const Item& queued) {
          return queued.kind == Kind::IsoVideo || queued.kind == Kind::IsoAudio;
        });
        constexpr size_t kMaxProgramBurst = 4;
        const bool haveProgram = programAudio != barrier || programVideo != barrier;
        if (iso != barrier && (!haveProgram || state->consecutiveProgramItems >= kMaxProgramBurst)) {
          selected = iso;
          state->consecutiveProgramItems = 0;
        } else if (programAudio != barrier) {
          selected = programAudio;
          ++state->consecutiveProgramItems;
        } else if (programVideo != barrier) {
          selected = programVideo;
          ++state->consecutiveProgramItems;
        } else {
          // Only ISO work remains in this media run.
          selected = iso != barrier ? iso : selected;
          state->consecutiveProgramItems = 0;
        }
      } else {
        state->consecutiveProgramItems = 0;
      }
      item = std::move(*selected);
      state->queue.erase(selected);
      state->applying = true;
      auto& evidence = state->evidence;
      evidence.operation = operationNames[static_cast<size_t>(item.kind)];
      evidence.operationGeneration = item.generation;
      evidence.operationSequence = item.seq;
      evidence.operationStartedMs = evidenceNowMs();
      if (item.kind == Kind::StopRecording && evidence.stopGeneration == item.generation) {
        evidence.finalizeStartedMs = evidence.operationStartedMs;
        evidence.finalizeResult = "running";
      }
    }

    // Finalization remains pending until the actual writer returns. Only this
    // generation may publish; queued old media/Stop must not revive a new take.
    if (item.kind == Kind::StopRecording) {
      std::lock_guard<std::mutex> queueLock(state->queueMutex);
      std::lock_guard<std::mutex> lock(state->snapshotMutex);
      if (state->snapshot.lifecycle && item.generation == state->generation &&
          state->snapshot.lifecycle->state != "failed")
        state->snapshot.lifecycle->state = ::corevideo::core::OutputLifecyclePolicy::finalizing().state;
    }
    OutputSession fresh;
    std::string failure;
    bool invoked = false;
    bool observed = false;
    try {
      if (!state->inner) throw std::runtime_error("Recording writer is unavailable");
      if (item.generation != failedGeneration || item.kind == Kind::StopRecording) {
      invoked = true;
      switch (item.kind) {
        case Kind::Configure:
          state->inner->configureRecording(item.request);
          break;
        case Kind::Start:
          state->inner->start(item.destinations, item.isoParticipantIds);
          break;
        case Kind::Video:
          state->inner->submit(item.frame);
          break;
        case Kind::IsoVideo:
          state->inner->submitIsoVideo(item.isoSources);
          break;
        case Kind::IsoAudio:
          state->inner->submitIsoAudio(item.isoAudioSources);
          break;
        case Kind::Audio:
          state->inner->submitAudioAt(item.audioPcm.data(), item.audioFrameCount, item.audioChannels,
                                    item.audioSampleRate, item.audioTimelineTimestamp100ns);
          break;
        case Kind::StopRecording:
          state->inner->stopRecording();
          break;
      }
      }
      fresh = state->inner->session();
      observed = true;
      if (item.kind == Kind::Start) {
        startVideoCount = fresh.recordingVideoFrameCount;
        madeProgress = false;
        // The writer has now actually applied the (synchronous) open. This is
        // what moves the take out of `requested` — an accepted command did not.
        state->startApplied.store(true, std::memory_order_release);
        state->everProgressed.store(false, std::memory_order_release);
        state->lastProgressAtMs.store(0, std::memory_order_release);
      }
      // encodedFrameCount includes attempted submissions in the MF adapter;
      // only successfully written recording frames establish output truth.
      madeProgress = madeProgress || fresh.recordingVideoFrameCount > startVideoCount;
      // Configure may retain the previous take's terminal error until Start
      // resets the wrapped session. It cannot poison the next generation.
      if (item.kind != Kind::Configure)
        failure = item.generation == failedGeneration ? generationFailure : fresh.recordingError;
    } catch (const std::exception& ex) {
      failure = ex.what();
    } catch (...) {
      failure = "Unknown recording writer failure";
    }
    if (!failure.empty()) {
      failedGeneration = item.generation;
      generationFailure = failure;
    }
    {
      // Same lock order as producer-side Start/Stop publication.
      std::lock_guard<std::mutex> queueLock(state->queueMutex);
      auto& evidence = state->evidence;
      const auto now = evidenceNowMs();
      if (state->videoStartupPhase && (madeProgress || !failure.empty())) {
        // The head of the show is over the moment the writer commits its first
        // real video frame (which is also when the lifecycle turns "producing"), or
        // gives up. Deliberately NOT "when Start was applied": the synchronous
        // open runs inside Start, but the first WriteSample calls into a freshly
        // opened Media Foundation sink are slow too, and frames shed there are
        // part of the same clipped head. A failed writer ends the window as well,
        // so a wedged take cannot park real steady-state loss in this bucket
        // forever (its lifecycle already reads "failed" to the judges).
        state->videoStartupPhase = false;
      }
      if (item.kind == Kind::IsoVideo && invoked) {
        for (const auto& source : item.isoSources) {
          ++state->isoVideoBySource[source.sourceId].written;
        }
      }
      if (invoked) ++evidence.completedCalls[static_cast<size_t>(item.kind)];
      if (observed && item.kind != Kind::Configure) {
        const bool sameGeneration = evidence.writtenGeneration == item.generation;
        if (!sameGeneration) evidence.lastWriterProgressMs = 0;
        if (item.kind != Kind::Start &&
            (fresh.recordingVideoFrameCount > (sameGeneration ? evidence.programVideoWritten : 0) ||
             fresh.recordingAudioPacketCount > (sameGeneration ? evidence.programAudioPacketsWritten : 0))) {
          evidence.lastWriterProgressMs = now;
          // The ONLY thing that can keep a destination in `producing`.
          if (item.generation == state->generation) {
            state->everProgressed.store(true, std::memory_order_release);
            state->lastProgressAtMs.store(now, std::memory_order_release);
          }
        }
        evidence.writtenGeneration = item.generation;
        evidence.programVideoWritten = fresh.recordingVideoFrameCount;
        evidence.programAudioPacketsWritten = fresh.recordingAudioPacketCount;
      }
      if (!failure.empty() && evidence.firstFailure.empty()) {
        evidence.firstFailure = failure.substr(0, 512);
        evidence.firstFailureGeneration = item.generation;
        evidence.firstFailureMs = now;
      }
      if (item.kind == Kind::StopRecording && evidence.stopGeneration == item.generation) {
        evidence.finalizeFinishedMs = now;
        // Returned is deliberately not an artifact-validity claim.
        evidence.finalizeResult = failure.empty() ? "returned" : "failed";
      }
      std::lock_guard<std::mutex> lock(state->snapshotMutex);
      if (item.generation == state->generation && state->snapshot.lifecycle &&
          (item.kind != Kind::Configure || !failure.empty())) {
        auto lifecycle = *state->snapshot.lifecycle;
        state->degradedWarning.store(!fresh.recordingWarning.empty(), std::memory_order_release);
        if (!failure.empty()) {
          const auto decision = ::corevideo::core::OutputLifecyclePolicy::failed();
          lifecycle.state = decision.state;
          lifecycle.health = decision.health;
          lifecycle.error = failure;
          state->active.store(false);
        } else if (lifecycle.state != "failed") {
          if (item.kind == Kind::StopRecording) {
            // The stop barrier has DRAINED and Finalize has returned. Only now
            // may any state claim completion — and only if media was actually
            // written. Everything before this point is stopping/finalizing.
            const auto decision = ::corevideo::core::OutputLifecyclePolicy::finalized(madeProgress);
            lifecycle.finalized = madeProgress;
            lifecycle.state = decision.state;
            lifecycle.health = decision.health;
            if (!madeProgress) {
              lifecycle.error = "Recording stopped before any media was written";
              if (evidence.firstFailure.empty()) {
                evidence.firstFailure = lifecycle.error->substr(0, 512);
                evidence.firstFailureGeneration = item.generation;
                evidence.firstFailureMs = now;
              }
            }
          } else if (lifecycle.desiredActive) {
            // requested -> preparing -> producing, decided from observed
            // evidence and the clock, never from the request itself.
            refreshActiveLifecycle(*state, lifecycle);
          }
        }
        fresh.lifecycle = std::move(lifecycle);
        fresh.active = fresh.lifecycle->state == "producing";
        state->snapshot = std::move(fresh);
      } else if (item.generation == state->generation && !state->snapshot.lifecycle) {
        // Non-recording encoder use retains its legacy observed sink state.
        state->snapshot = std::move(fresh);
      }
    }

    {
      std::lock_guard<std::mutex> lock(state->queueMutex);
      // Diagnostic last-applied sequence. Media scheduling may reorder work
      // within a control-barrier run, so drainForTest uses queue+applying state.
      state->appliedSeq = item.seq;
      state->applying = false;
      state->evidence.operation = "idle";
      state->evidence.operationStartedMs = 0;
    }
    state->appliedCv.notify_all();
  }
}

}  // namespace corevideo::modules
