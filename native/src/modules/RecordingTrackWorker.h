#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <exception>
#include <thread>
#include <utility>

namespace corevideo::modules {

// One ordered file writer. Producers never wait for an encode. Overflow rejects
// the new item (and reports it); accepted media is never erased, including Stop's
// tail. The owner closes all tracks before joining any of them. This worker is
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
    std::string error;
  };
  RecordingTrackWorker(std::function<void()> initialize, std::function<void()> finalize,
                       size_t videoCapacity = 6, size_t audioCapacity = 96,
                       size_t startupVideoCapacity = 0)
      : initialize_(std::move(initialize)), finalize_(std::move(finalize)),
        videoCapacity_(std::max(size_t{1}, videoCapacity)),
        audioCapacity_(std::max(size_t{1}, audioCapacity)),
        startupVideoCapacity_(std::max(videoCapacity_, startupVideoCapacity)),
        thread_([this] { run(); }) {}
  ~RecordingTrackWorker() { close(); join(); }
  RecordingTrackWorker(const RecordingTrackWorker&) = delete;
  RecordingTrackWorker& operator=(const RecordingTrackWorker&) = delete;

  bool post(Kind kind, std::function<void()> work) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) return false;
    auto& pending = kind == Kind::Video ? evidence_.queuedVideo : evidence_.queuedAudio;
    // MFT startup includes the first writes, not just creation. Keep the bounded
    // preroll budget for one second, then shrink only after its backlog drains.
    if (!steady_ && std::chrono::steady_clock::now() >= startupEnds_ &&
        evidence_.queuedVideo <= videoCapacity_) steady_ = true;
    const auto videoLimit = steady_ ? videoCapacity_ : startupVideoCapacity_;
    if (pending >= (kind == Kind::Video ? videoLimit : audioCapacity_)) {
      ++(kind == Kind::Video ? evidence_.droppedVideo : evidence_.droppedAudio);
      return false;
    }
    ++pending;
    ++(kind == Kind::Video ? evidence_.acceptedVideo : evidence_.acceptedAudio);
    queue_.push_back({kind, std::move(work)});
    cv_.notify_one();
    return true;
  }
  void close() { std::lock_guard<std::mutex> lock(mutex_); closed_ = true; cv_.notify_one(); }
  void join() { if (thread_.joinable()) thread_.join(); }
  Evidence evidence() const { std::lock_guard<std::mutex> lock(mutex_); return evidence_; }

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
  struct Item { Kind kind; std::function<void()> work; };
  std::function<void()> initialize_, finalize_;
  size_t videoCapacity_, audioCapacity_, startupVideoCapacity_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Item> queue_;
  Evidence evidence_;
  bool closed_ = false;
  bool steady_ = false;
  std::chrono::steady_clock::time_point startupEnds_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  std::thread thread_;
};
} // namespace corevideo::modules
