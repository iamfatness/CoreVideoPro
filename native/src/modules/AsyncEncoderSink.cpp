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
  state_->maxAudioQueue = std::max<size_t>(1, options_.maxAudioQueue);
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
    seq = state_->nextSeq++;
    item.seq = seq;

    // Drop-to-latest / bounded backlog: when the pending count for this item's
    // media kind is at capacity, drop the OLDEST pending item of that kind so we
    // keep flowing the freshest frames rather than blocking or growing unbounded.
    if (item.kind == Kind::Video || item.kind == Kind::Audio) {
      const Kind kind = item.kind;
      const size_t cap = kind == Kind::Video ? state_->maxVideoQueue : state_->maxAudioQueue;
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
            if (kind == Kind::Video) {
              state_->droppedVideo.fetch_add(1);
            } else {
              state_->droppedAudio.fetch_add(1);
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
  std::unique_lock<std::mutex> lock(state_->queueMutex);
  return state_->appliedCv.wait_for(lock, timeout,
                                    [&] { return state_->appliedSeq >= seq || state_->writerDone; });
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
  enqueue(std::move(item));
}

OutputSession AsyncEncoderSink::session() const {
  std::lock_guard<std::mutex> lock(state_->snapshotMutex);
  return state_->snapshot;
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
      item = std::move(state->queue.front());
      state->queue.pop_front();
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
      state->appliedSeq = item.seq;  // monotonic; dropped seqs are skipped over
    }
    state->appliedCv.notify_all();
  }
}

}  // namespace corevideo::modules
