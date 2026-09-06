#include "modules/AsyncEncoderSink.h"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <utility>

namespace corevideo::modules {

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
    std::fprintf(stderr,
                 "[asyncEncoder] writer still finalizing after %lldms grace; detaching for shutdown\n",
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
      state_->hasLastProgramFrameNumber = true;
      state_->lastProgramFrameNumber = item.frame.frameNumber;
    } else if (item.kind == Kind::IsoVideo && item.isoSources.size() == 1) {
      const auto& source = item.isoSources.front();
      const auto last = state_->lastIsoFrameIdBySource.find(source.sourceId);
      if (last != state_->lastIsoFrameIdBySource.end() && last->second == source.frame.frameId) {
        return 0;
      }
      state_->lastIsoFrameIdBySource[source.sourceId] = source.frame.frameId;
    } else if (item.kind == Kind::Start) {
      // Zoom/shared-memory frame sequences may restart between meeting or
      // recording generations, so no dedup identity crosses a Start barrier.
      state_->hasLastProgramFrameNumber = false;
      state_->lastIsoFrameIdBySource.clear();
      state_->consecutiveProgramItems = 0;
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
        state_->snapshot.lifecycle = contracts::OutputLifecycle{
            state_->configuredSessionId + ":" + state_->epoch + ":" + std::to_string(item.generation),
            true, "starting", "unknown", false, std::nullopt};
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
      if (kind == Kind::IsoVideo && item.isoSources.size() == 1) {
        const std::string& sourceId = item.isoSources.front().sourceId;
        for (auto it = state_->queue.begin(); it != state_->queue.end(); ++it) {
          if (it->generation == item.generation && it->kind == Kind::IsoVideo && it->isoSources.size() == 1 &&
              it->isoSources.front().sourceId == sourceId) {
            state_->queue.erase(it);
            state_->droppedVideo.fetch_add(1);
            break;
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
        if (queued.generation == item.generation && queued.kind == kind) {
          ++pending;
        }
      }
      if (pending >= cap) {
        for (auto it = state_->queue.begin(); it != state_->queue.end(); ++it) {
          if (it->generation == item.generation && it->kind == kind) {
            state_->queue.erase(it);
            if (kind == Kind::Audio || kind == Kind::IsoAudio) {
              state_->droppedAudio.fetch_add(1);
            } else {
              // Video + IsoVideo both count as dropped video frames (ISO frames
              // drop-to-latest under disk pressure — logged as ISO health, never
              // program A/V, per spec §9).
              state_->droppedVideo.fetch_add(1);
            }
            break;
          }
        }
      }
    }

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
  if (!state_->active.load() || interleaved == nullptr || frameCount <= 0 || channels <= 0) {
    return;
  }
  Item item;
  item.kind = Kind::Audio;
  item.audioPcm.assign(interleaved, interleaved + static_cast<size_t>(frameCount) * static_cast<size_t>(channels));
  item.audioFrameCount = frameCount;
  item.audioChannels = channels;
  item.audioSampleRate = sampleRate;
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
    state_->queue.push_back(std::move(item));
    std::lock_guard<std::mutex> snapshotLock(state_->snapshotMutex);
    state_->snapshot.active = false;
    if (state_->snapshot.lifecycle) {
      state_->snapshot.lifecycle->desiredActive = false;
      if (state_->snapshot.lifecycle->state != "failed")
        state_->snapshot.lifecycle->state = "stopping";
    }
  }
  state_->queueCv.notify_one();
}

OutputSession AsyncEncoderSink::session() const {
  std::lock_guard<std::mutex> lock(state_->snapshotMutex);
  auto snapshot = state_->snapshot;
  snapshot.encoderQueueDroppedVideoFrames =
      static_cast<int64_t>(state_->droppedVideo.load(std::memory_order_relaxed));
  snapshot.encoderQueueDroppedAudioPackets =
      static_cast<int64_t>(state_->droppedAudio.load(std::memory_order_relaxed));
  return snapshot;
}

uint64_t AsyncEncoderSink::droppedVideoFrames() const { return state_->droppedVideo.load(); }
uint64_t AsyncEncoderSink::droppedAudioPackets() const { return state_->droppedAudio.load(); }

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
    }

    // Finalization remains pending until the actual writer returns. Only this
    // generation may publish; queued old media/Stop must not revive a new take.
    if (item.kind == Kind::StopRecording) {
      std::lock_guard<std::mutex> queueLock(state->queueMutex);
      std::lock_guard<std::mutex> lock(state->snapshotMutex);
      if (state->snapshot.lifecycle && item.generation == state->generation &&
          state->snapshot.lifecycle->state != "failed")
        state->snapshot.lifecycle->state = "finalizing";
    }
    OutputSession fresh;
    std::string failure;
    try {
      if (!state->inner) throw std::runtime_error("Recording writer is unavailable");
      if (item.generation != failedGeneration || item.kind == Kind::StopRecording) {
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
          state->inner->submitAudio(item.audioPcm.data(), item.audioFrameCount, item.audioChannels,
                                    item.audioSampleRate);
          break;
        case Kind::StopRecording:
          state->inner->stopRecording();
          break;
      }
      }
      fresh = state->inner->session();
      if (item.kind == Kind::Start) {
        startVideoCount = fresh.recordingVideoFrameCount;
        madeProgress = false;
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
      std::lock_guard<std::mutex> lock(state->snapshotMutex);
      if (item.generation == state->generation && state->snapshot.lifecycle &&
          (item.kind != Kind::Configure || !failure.empty())) {
        auto lifecycle = *state->snapshot.lifecycle;
        if (!failure.empty()) {
          lifecycle.state = "failed";
          lifecycle.health = "failed";
          lifecycle.error = failure;
          state->active.store(false);
        } else if (lifecycle.state != "failed") {
          if (item.kind == Kind::StopRecording) {
            lifecycle.finalized = madeProgress;
            lifecycle.state = madeProgress ? "completed" : "failed";
            lifecycle.health = madeProgress ? "healthy" : "failed";
            if (!madeProgress) lifecycle.error = "Recording stopped before any media was written";
          } else if (lifecycle.desiredActive) {
            lifecycle.state = madeProgress ? "live" : "starting";
            lifecycle.health = madeProgress ? "healthy" : "unknown";
          }
          if (!fresh.recordingWarning.empty() && lifecycle.health == "healthy")
            lifecycle.health = "degraded";
        }
        fresh.lifecycle = std::move(lifecycle);
        fresh.active = fresh.lifecycle->state == "live";
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
    }
    state->appliedCv.notify_all();
  }
}

}  // namespace corevideo::modules
