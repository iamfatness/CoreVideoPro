#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <limits>
#include <string>
#include <exception>
#include <thread>
#include <utility>

namespace corevideo::modules {

// One ordered file writer. Producers never wait for encode or disk. At capacity,
// the recording policy replaces only this file's oldest queued video; audio is
// refused and counted. Closed queues cannot evict their accepted Stop tail.
// Legacy users may retain reject-new overflow. Close all tracks before joining.
// This worker is
// itself owned by AsyncEncoderSink's bounded-teardown control block.
class RecordingTrackWorker {
 public:
  enum class Kind { Video, Audio };
  struct Evidence {
    uint64_t acceptedVideo = 0, acceptedAudio = 0;
    uint64_t droppedVideo = 0, droppedAudio = 0;
    uint64_t completedVideo = 0, completedAudio = 0;
    uint64_t videoWorkUs = 0, audioWorkUs = 0, maximumWorkUs = 0;
    size_t queuedVideo = 0, queuedAudio = 0;
    size_t videoCapacity = 0, queueHighWater = 0, queuedBytes = 0, highWaterBytes = 0;
    uint64_t queuedAudioSamples = 0, startupDroppedVideo = 0, startupDroppedAudio = 0;
    int64_t oldestQueuedAgeMs = 0;
    const char* lastOverflowReason = "none";
    std::string error;
  };
  RecordingTrackWorker(std::function<void()> initialize, std::function<void()> finalize,
                       size_t videoCapacity = 32, size_t audioCapacity = 96, size_t startupVideoCapacity = 0,
                       bool replaceOldestVideo = false, uint64_t audioSampleCapacity = 0,
                       size_t byteCapacity = (std::numeric_limits<size_t>::max)(), uint64_t startupAudioSampleCapacity = 0)
      : initialize_(std::move(initialize)), finalize_(std::move(finalize)),
        videoCapacity_(std::max(size_t{1}, videoCapacity)),
        audioCapacity_(std::max(size_t{1}, audioCapacity)),
        startupVideoCapacity_(std::max(videoCapacity_, startupVideoCapacity)),
        replaceOldestVideo_(replaceOldestVideo), audioSampleCapacity_(audioSampleCapacity), byteCapacity_(byteCapacity),
        startupAudioSampleCapacity_(std::max(audioSampleCapacity, startupAudioSampleCapacity)),
        startupFinished_(startupVideoCapacity == 0),
        thread_([this] { run(); }) {}
  ~RecordingTrackWorker() { close(); join(); }
  RecordingTrackWorker(const RecordingTrackWorker&) = delete;
  RecordingTrackWorker& operator=(const RecordingTrackWorker&) = delete;

  bool post(Kind kind, std::function<void()> work, size_t bytes = 0, uint64_t audioSamples = 0) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) return false;
    auto& pending = kind == Kind::Video ? evidence_.queuedVideo : evidence_.queuedAudio;
    // Opening eight 1080p codecs can exceed one second. Retain a bounded
    // startup burst until the first real frame is written and its backlog
    // drains, then return to the smaller steady-state budget.
    if (startupFinished_ && evidence_.queuedVideo <= videoCapacity_ &&
        (!audioSampleCapacity_ || evidence_.queuedAudioSamples <= audioSampleCapacity_)) steady_ = true;
    const auto videoLimit = steady_ ? videoCapacity_ : startupVideoCapacity_;
    const auto audioLimit = steady_ ? audioSampleCapacity_ : startupAudioSampleCapacity_;
    const auto full = [&] {
      return pending >= (kind == Kind::Video ? videoLimit : audioCapacity_) ||
          bytes > byteCapacity_ || evidence_.queuedBytes > byteCapacity_ - bytes ||
          (kind == Kind::Audio && audioLimit &&
           (audioSamples > audioLimit || evidence_.queuedAudioSamples > audioLimit - audioSamples));
    };
    if (full()) {
      evidence_.lastOverflowReason = bytes > byteCapacity_ || evidence_.queuedBytes > byteCapacity_ - bytes
          ? "byte-capacity" : kind == Kind::Video ? "video-capacity" : "audio-capacity";
      // Only this file's oldest queued video may leave. Audio stays ordered;
      // refused packets become clock-aligned silence at the writer.
      if (kind == Kind::Video && replaceOldestVideo_ && bytes <= byteCapacity_) {
        const auto victim = std::find_if(queue_.begin(), queue_.end(), [](const Item& item) { return item.kind == Kind::Video; });
        if (victim != queue_.end() && pending - 1 < videoLimit &&
            evidence_.queuedBytes - victim->bytes <= byteCapacity_ - bytes) {
          evidence_.queuedBytes -= victim->bytes;
          --pending;
          queue_.erase(victim);
          noteDrop(kind);
        }
      }
      if (full()) { noteDrop(kind); return false; }
    }
    ++pending;
    ++(kind == Kind::Video ? evidence_.acceptedVideo : evidence_.acceptedAudio);
    evidence_.queuedBytes += bytes;
    evidence_.queuedAudioSamples += audioSamples;
    evidence_.queueHighWater = std::max(evidence_.queueHighWater, evidence_.queuedVideo);
    evidence_.highWaterBytes = std::max(evidence_.highWaterBytes, evidence_.queuedBytes);
    queue_.push_back({kind, std::move(work), bytes, audioSamples, std::chrono::steady_clock::now()});
    cv_.notify_one();
    return true;
  }
  void close() { std::lock_guard<std::mutex> lock(mutex_); closed_ = true; cv_.notify_one(); }
  void finishStartup() { std::lock_guard<std::mutex> lock(mutex_); startupFinished_ = true; }
  void join() { if (thread_.joinable()) thread_.join(); }
  Evidence evidence() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = evidence_;
    result.videoCapacity = videoCapacity_;
    if (!queue_.empty()) result.oldestQueuedAgeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - queue_.front().enqueuedAt).count();
    return result;
  }

 private:
  void run() {
    invoke(initialize_);
    for (;;) {
      Item item;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return closed_ || !queue_.empty(); });
        if (queue_.empty()) break;
        item = std::move(queue_.front()); queue_.pop_front();
        --(item.kind == Kind::Video ? evidence_.queuedVideo : evidence_.queuedAudio);
        evidence_.queuedBytes -= item.bytes;
        evidence_.queuedAudioSamples -= item.audioSamples;
      }
      const auto begin = std::chrono::steady_clock::now();
      invoke(item.work);
      const auto us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - begin).count());
      std::lock_guard<std::mutex> lock(mutex_);
      ++(item.kind == Kind::Video ? evidence_.completedVideo : evidence_.completedAudio);
      (item.kind == Kind::Video ? evidence_.videoWorkUs : evidence_.audioWorkUs) += us;
      evidence_.maximumWorkUs = std::max(evidence_.maximumWorkUs, us);
    }
    invoke(finalize_);
  }
  void invoke(const std::function<void()>& work) {
    try { work(); }
    catch (const std::exception& ex) { setError(ex.what()); }
    catch (...) { setError("Unknown ISO writer failure"); }
  }
  void setError(const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (evidence_.error.empty()) evidence_.error = message;
  }
  void noteDrop(Kind kind) {
    ++(kind == Kind::Video ? evidence_.droppedVideo : evidence_.droppedAudio);
    if (!startupFinished_) ++(kind == Kind::Video ? evidence_.startupDroppedVideo : evidence_.startupDroppedAudio);
  }
  struct Item {
    Kind kind; std::function<void()> work;
    size_t bytes; uint64_t audioSamples;
    std::chrono::steady_clock::time_point enqueuedAt;
  };
  std::function<void()> initialize_, finalize_;
  size_t videoCapacity_, audioCapacity_, startupVideoCapacity_;
  bool replaceOldestVideo_;
  uint64_t audioSampleCapacity_;
  size_t byteCapacity_;
  uint64_t startupAudioSampleCapacity_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Item> queue_;
  Evidence evidence_;
  bool closed_ = false;
  bool startupFinished_ = false, steady_ = false;
  std::thread thread_;
};
} // namespace corevideo::modules
