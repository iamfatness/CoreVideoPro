#include "modules/AsyncOutputSender.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <utility>

namespace corevideo::modules {

AsyncOutputSender::AsyncOutputSender(std::unique_ptr<IOutputSender> inner)
    : AsyncOutputSender(std::move(inner), Options{}) {}

AsyncOutputSender::AsyncOutputSender(std::unique_ptr<IOutputSender> inner, Options options)
    : options_(options), state_(std::make_shared<State>()) {
  state_->inner = std::move(inner);
  if (state_->inner) {
    state_->snapshot = state_->inner->session();
  }
  writer_ = std::thread(&AsyncOutputSender::writerLoop, state_);
}

AsyncOutputSender::~AsyncOutputSender() {
  // Break a sender blocked in pipe/network I/O before asking its worker to exit.
  interrupt("rtmp");
  interrupt("srt");
  interrupt("ndi");
  {
    std::lock_guard<std::mutex> lock(state_->queueMutex);
    state_->stop = true;
    state_->queue.clear();
  }
  state_->queueCv.notify_all();

  bool done = false;
  {
    std::unique_lock<std::mutex> lock(state_->queueMutex);
    done = state_->appliedCv.wait_for(lock, options_.shutdownGrace, [&] { return state_->writerDone; });
  }
  if (done) {
    if (writer_.joinable()) {
      writer_.join();
    }
  } else {
    std::fprintf(stderr, "[asyncOutput] writer still blocked after %lldms grace; detaching\n",
                 static_cast<long long>(options_.shutdownGrace.count()));
    if (writer_.joinable()) {
      writer_.detach();
    }
  }
}

uint64_t AsyncOutputSender::enqueue(Item&& item) {
  uint64_t seq = 0;
  {
    std::lock_guard<std::mutex> lock(state_->queueMutex);
    seq = state_->nextSeq++;
    item.seq = seq;
    if (item.kind == Kind::Sync) {
      for (auto it = state_->queue.begin(); it != state_->queue.end();) {
        if (it->kind == Kind::Sync) {
          // Video is state: only the newest frame matters. Audio is a timeline:
          // dropping the PCM attached to a stale video frame makes FFmpeg's
          // audio clock fall behind its video clock until the two-input muxer
          // stops consuming the video pipe. Preserve compatible audio blocks
          // in chronological order while replacing the stale video frame.
          if (it->audioChannels == item.audioChannels &&
              it->audioSampleRate == item.audioSampleRate &&
              !it->audioPcm.empty()) {
            it->audioPcm.insert(it->audioPcm.end(), item.audioPcm.begin(), item.audioPcm.end());
            item.audioPcm = std::move(it->audioPcm);

            // A blocked network endpoint must not grow memory without bound.
            // Five seconds is ample to bridge normal encoder/ingest startup;
            // beyond that, keep the newest samples and let recovery restart.
            const size_t maxSamples = static_cast<size_t>((std::max)(1, item.audioSampleRate)) *
                                      static_cast<size_t>((std::max)(1, item.audioChannels)) * 5;
            if (item.audioPcm.size() > maxSamples) {
              item.audioPcm.erase(
                  item.audioPcm.begin(),
                  item.audioPcm.begin() + static_cast<std::ptrdiff_t>(item.audioPcm.size() - maxSamples));
            }
          }
          it = state_->queue.erase(it);
          state_->dropped.fetch_add(1);
        } else {
          ++it;
        }
      }
    }
    state_->queue.push_back(std::move(item));
  }
  state_->queueCv.notify_one();
  return seq;
}

OutputSenderSession AsyncOutputSender::sync(
    const std::vector<std::string>& destinations,
    const ProgramFrame* frame,
    double elapsedMs,
    const std::vector<OutputDestinationSettings>& destinationSettings,
    const std::vector<float>* programAudioPcm,
    int audioChannels,
    int audioSampleRate) {
  // Stop must be able to release a currently blocked worker immediately. The
  // inner sender's interrupt contract is non-blocking and may only cancel I/O;
  // normal state cleanup remains serialized on the writer below.
  for (const char* destination : {"rtmp", "srt", "ndi"}) {
    if (std::find(destinations.begin(), destinations.end(), destination) == destinations.end()) {
      interrupt(destination);
    }
  }

  Item item;
  item.kind = Kind::Sync;
  item.destinations = destinations;
  item.elapsedMs = elapsedMs;
  item.destinationSettings = destinationSettings;
  item.audioChannels = audioChannels;
  item.audioSampleRate = audioSampleRate;
  if (frame) {
    item.frame = std::make_unique<ProgramFrame>(*frame);
  }
  if (programAudioPcm) {
    item.audioPcm = *programAudioPcm;
  }
  enqueue(std::move(item));
  return session();
}

OutputSenderSession AsyncOutputSender::fail(const std::string& destination, const std::string& message, double elapsedMs) {
  interrupt(destination);
  Item item;
  item.kind = Kind::Fail;
  item.destination = destination;
  item.message = message;
  item.elapsedMs = elapsedMs;
  enqueue(std::move(item));
  return session();
}

OutputSenderSession AsyncOutputSender::recover(const std::string& destination, double elapsedMs, const std::string& reason) {
  interrupt(destination);
  Item item;
  item.kind = Kind::Recover;
  item.destination = destination;
  item.message = reason;
  item.elapsedMs = elapsedMs;
  enqueue(std::move(item));
  return session();
}

OutputSenderSession AsyncOutputSender::session() const {
  std::lock_guard<std::mutex> lock(state_->snapshotMutex);
  return state_->snapshot;
}

void AsyncOutputSender::interrupt(const std::string& destination) {
  if (state_->inner) {
    state_->inner->interrupt(destination);
  }
}

uint64_t AsyncOutputSender::droppedSyncs() const { return state_->dropped.load(); }

bool AsyncOutputSender::drainForTest(std::chrono::milliseconds timeout) {
  uint64_t target = 0;
  {
    std::lock_guard<std::mutex> lock(state_->queueMutex);
    target = state_->nextSeq - 1;
  }
  std::unique_lock<std::mutex> lock(state_->queueMutex);
  return state_->appliedCv.wait_for(lock, timeout,
                                    [&] { return state_->appliedSeq >= target || state_->writerDone; });
}

void AsyncOutputSender::writerLoop(std::shared_ptr<State> state) {
  for (;;) {
    Item item;
    {
      std::unique_lock<std::mutex> lock(state->queueMutex);
      state->queueCv.wait(lock, [&] { return !state->queue.empty() || state->stop; });
      if (state->queue.empty()) {
        state->writerDone = true;
        state->appliedCv.notify_all();
        return;
      }
      item = std::move(state->queue.front());
      state->queue.pop_front();
    }

    OutputSenderSession fresh;
    if (state->inner) {
      if (item.kind == Kind::Sync) {
        fresh = state->inner->sync(
            item.destinations,
            item.frame.get(),
            item.elapsedMs,
            item.destinationSettings,
            item.audioPcm.empty() ? nullptr : &item.audioPcm,
            item.audioChannels,
            item.audioSampleRate);
      } else if (item.kind == Kind::Fail) {
        fresh = state->inner->fail(item.destination, item.message, item.elapsedMs);
      } else {
        fresh = state->inner->recover(item.destination, item.elapsedMs, item.message);
      }
    }
    {
      std::lock_guard<std::mutex> lock(state->snapshotMutex);
      state->snapshot = std::move(fresh);
    }
    {
      std::lock_guard<std::mutex> lock(state->queueMutex);
      state->appliedSeq = item.seq;
    }
    state->appliedCv.notify_all();
  }
}

}  // namespace corevideo::modules
