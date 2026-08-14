#include "modules/AsyncEncoderSink.h"

#include <algorithm>
#include <cstdio>
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
          if (it->kind == Kind::IsoVideo && it->isoSources.size() == 1 &&
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
        if (queued.kind == kind) {
          ++pending;
        }
      }
      if (pending >= cap) {
        for (auto it = state_->queue.begin(); it != state_->queue.end(); ++it) {
          if (it->kind == kind) {
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
  // Publish active BEFORE enqueue so any submit() racing right behind this call is
  // enqueued (ordered after the Start item) instead of dropped.
  state_->active.store(true);
  // Optimistically reflect the started session in the snapshot so the immediate
  // return (and any session() read before the writer applies Start) shows active;
  // the writer overwrites this with the wrapped sink's real session shortly.
  {
    std::lock_guard<std::mutex> lock(state_->snapshotMutex);
    state_->snapshot.active = true;
    state_->snapshot.destinations = destinations;
  }
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
    item.seq = state_->nextSeq++;

    // STOP IS A CONTROL BARRIER, not one more item behind minutes of obsolete
    // media. Discard every pending media submission; the writer may finish the
    // single item it is currently applying, then it reaches Finalize next. Keep
    // Configure/Start controls in FIFO order so a rapid next take still starts
    // only after the previous container is closed.
    for (auto it = state_->queue.begin(); it != state_->queue.end();) {
      const bool video = it->kind == Kind::Video || it->kind == Kind::IsoVideo;
      const bool audio = it->kind == Kind::Audio || it->kind == Kind::IsoAudio;
      if (!video && !audio) {
        ++it;
        continue;
      }
      if (video) {
        state_->droppedVideo.fetch_add(1);
      } else {
        state_->droppedAudio.fetch_add(1);
      }
      it = state_->queue.erase(it);
    }
    state_->queue.push_back(std::move(item));
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

    // Apply against the wrapped sink WITHOUT holding queueMutex — this is the
    // (potentially blocking) I/O the async layer exists to keep off the worker.
    if (state->inner) {
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

    // Refresh the published snapshot from the wrapped session. NOTE: do NOT touch
    // `active` here — it is owned by start() (set true synchronously) and the
    // wrapped session's `active` LAGS behind the queue, so refreshing it from here
    // could clobber the flag back to false between start() and the writer applying
    // Start, causing a racing submit() to wrongly drop a frame.
    OutputSession fresh;
    if (state->inner) {
      fresh = state->inner->session();
    }
    {
      std::lock_guard<std::mutex> lock(state->snapshotMutex);
      state->snapshot = fresh;
      // Keep `active` sticky once start() has run: the wrapped session's active flag
      // lags the queue, so a snapshot refresh triggered by an earlier item (e.g. the
      // queued Configure) must not report the session as inactive after start().
      if (state->active.load()) {
        state->snapshot.active = true;
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
