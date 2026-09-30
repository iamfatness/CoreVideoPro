#include "modules/SharedGpuVideoEncoder.h"

#include "modules/MediaFoundationGpuVideoEncoder.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace corevideo::modules {
namespace {

bool sameProfile(const GpuVideoEncoderConfig& a, const GpuVideoEncoderConfig& b) {
  return a.width == b.width && a.height == b.height && a.fps == b.fps &&
      a.bitrateKbps == b.bitrateKbps &&
      a.keyframeIntervalSeconds == b.keyframeIntervalSeconds &&
      a.rateControl == b.rateControl && a.h264Profile == b.h264Profile &&
      a.codec == b.codec;
}

struct Session {
  static constexpr size_t kMaxSubscribers = 16;
  explicit Session(GpuVideoEncoderConfig profile) : profile(std::move(profile)) {}
  struct Subscriber {
    Subscriber(uint64_t value, GpuEncodedChunkSink callback)
        : id(value), sink(std::move(callback)) {}
    uint64_t id = 0;
    GpuEncodedChunkSink sink;
    std::mutex mutex;
    std::condition_variable idle;
    bool active = true;
    int inFlight = 0;

    void publish(const GpuEncodedChunk& chunk) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (!active) return;
        ++inFlight;
      }
      sink(chunk);
      {
        std::lock_guard<std::mutex> lock(mutex);
        --inFlight;
        if (inFlight == 0) idle.notify_all();
      }
    }
    void retire() {
      std::unique_lock<std::mutex> lock(mutex);
      active = false;
      idle.wait(lock, [&] { return inFlight == 0; });
    }
  };
  GpuVideoEncoderConfig profile;
  std::unique_ptr<GpuVideoEncoder> encoder;
  std::mutex sinksMutex;
  std::vector<std::shared_ptr<Subscriber>> sinks;
  uint64_t nextSinkId = 1;
  bool hasSubmittedFrame = false;
  int64_t lastSubmittedFrame = 0;

  // Borrow subscribers without holding the list lock through sender callbacks.
  // A subscriber's retire() fences any callback already in flight.
  void publish(const GpuEncodedChunk& chunk) {
    std::array<std::shared_ptr<Subscriber>, kMaxSubscribers> targets{};
    size_t count = 0;
    {
      std::lock_guard<std::mutex> lock(sinksMutex);
      for (const auto& sink : sinks) targets[count++] = sink;
    }
    for (size_t i = 0; i < count; ++i) targets[i]->publish(chunk);
  }
};

}  // namespace

struct SharedGpuVideoEncoderPool::State {
  explicit State(Factory create) : factory(std::move(create)) {}
  Factory factory;
  std::mutex mutex;
  std::condition_variable stopped;
  std::vector<std::shared_ptr<Session>> sessions;
  std::vector<GpuVideoEncoderConfig> stoppingProfiles;
};

class SharedGpuVideoEncoderPool::Client final : public GpuVideoEncoder {
 public:
  explicit Client(std::shared_ptr<State> state) : state_(std::move(state)) {}
  ~Client() override { stop(); }

  bool start(const GpuVideoEncoderConfig& config, GpuEncodedChunkSink sink) override {
    if (!sink) { lastFailure_ = "missing-sink"; return false; }
    std::unique_lock<std::mutex> lock(state_->mutex);
    if (session_) return true;
    state_->stopped.wait(lock, [&] {
      return std::none_of(state_->stoppingProfiles.begin(), state_->stoppingProfiles.end(),
          [&](const auto& profile) { return sameProfile(profile, config); });
    });
    auto found = std::find_if(state_->sessions.begin(), state_->sessions.end(),
        [&](const auto& candidate) { return sameProfile(candidate->profile, config); });
    if (found == state_->sessions.end()) {
      auto session = std::make_shared<Session>(config);
      session->encoder = state_->factory ? state_->factory() : nullptr;
      if (!session->encoder) { lastFailure_ = "encoder-create-failed"; return false; }
      const uint64_t sinkId = session->nextSinkId++;
      session->sinks.push_back(std::make_shared<Session::Subscriber>(sinkId, std::move(sink)));
      const std::weak_ptr<Session> weak = session;
      if (!session->encoder->start(config, [weak](const GpuEncodedChunk& chunk) {
            if (auto active = weak.lock()) active->publish(chunk);
          })) {
        lastFailure_ = session->encoder->lastFailure();
        session->sinks.front()->retire();
        session->sinks.clear();
        return false;
      }
      state_->sessions.push_back(session);
      session_ = std::move(session);
      sinkId_ = sinkId;
    } else {
      session_ = *found;
      if (!session_->encoder->healthy()) {
        lastFailure_ = "shared-encoder-unhealthy";
        session_.reset();
        return false;
      }
      std::lock_guard<std::mutex> sinkLock(session_->sinksMutex);
      if (session_->sinks.size() >= Session::kMaxSubscribers) {
        lastFailure_ = "too-many-stream-destinations";
        session_.reset();
        return false;
      }
      sinkId_ = session_->nextSinkId++;
      session_->sinks.push_back(std::make_shared<Session::Subscriber>(sinkId_, std::move(sink)));
    }
    std::atomic_store(&keyframeSession_, session_);
    lastFailure_.clear();
    return true;
  }

  bool submit(const GpuVideoEncoderFrame& frame) override {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!session_ || !session_->encoder) return false;
    if (session_->hasSubmittedFrame && frame.frameNumber <= session_->lastSubmittedFrame)
      return session_->encoder->healthy();
    if (!session_->encoder->submit(frame)) return false;
    session_->lastSubmittedFrame = frame.frameNumber;
    session_->hasSubmittedFrame = true;
    return true;
  }

  bool requestKeyframe() override {
    // The encoder callback may ask for an IDR while holding one destination's
    // queue lock. Do not hold the pool lock across the encoder call: a submit
    // can synchronously publish to that queue.
    const auto session = std::atomic_load(&keyframeSession_);
    return session && session->encoder && session->encoder->requestKeyframe();
  }

  void stop() override {
    std::unique_lock<std::mutex> lock(state_->mutex);
    if (!session_) return;
    std::shared_ptr<Session::Subscriber> subscriber;
    {
      std::lock_guard<std::mutex> sinkLock(session_->sinksMutex);
      auto& sinks = session_->sinks;
      const auto it = std::find_if(sinks.begin(), sinks.end(),
          [&](const auto& entry) { return entry->id == sinkId_; });
      if (it != sinks.end()) { subscriber = *it; sinks.erase(it); }
    }
    const bool last = session_->sinks.empty();
    auto retiring = session_;
    if (last) {
      state_->sessions.erase(std::remove(state_->sessions.begin(), state_->sessions.end(), retiring),
                             state_->sessions.end());
      state_->stoppingProfiles.push_back(retiring->profile);
    }
    std::atomic_store(&keyframeSession_, std::shared_ptr<Session>{});
    session_.reset();
    sinkId_ = 0;
    lock.unlock();
    if (subscriber) subscriber->retire();
    if (last) {
      // A draining MFT can still call a sink. Never join its thread while
      // holding the pool lock that a callback may need for an IDR request.
      retiring->encoder->stop();
      lock.lock();
      auto& stopping = state_->stoppingProfiles;
      auto it = std::find_if(stopping.begin(), stopping.end(),
          [&](const auto& profile) { return sameProfile(profile, retiring->profile); });
      if (it != stopping.end()) stopping.erase(it);
      lock.unlock();
      state_->stopped.notify_all();
    }
  }

  bool healthy() const override {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return session_ && session_->encoder && session_->encoder->healthy();
  }

  std::string lastFailure() const override { return lastFailure_; }

 private:
  std::shared_ptr<State> state_;
  std::shared_ptr<Session> session_;
  // Use the atomic shared_ptr free functions: macOS CI's libc++ does not yet
  // provide std::atomic<std::shared_ptr<T>> on every supported runner.
  std::shared_ptr<Session> keyframeSession_;
  uint64_t sinkId_ = 0;
  std::string lastFailure_;
};

SharedGpuVideoEncoderPool::SharedGpuVideoEncoderPool(Factory factory)
    : state_(std::make_shared<State>(std::move(factory))) {}
SharedGpuVideoEncoderPool::~SharedGpuVideoEncoderPool() = default;

std::unique_ptr<GpuVideoEncoder> SharedGpuVideoEncoderPool::createClient() {
  return std::make_unique<Client>(state_);
}

std::unique_ptr<GpuVideoEncoder> createSharedProgramGpuVideoEncoder() {
  static SharedGpuVideoEncoderPool pool(&createMediaFoundationGpuVideoEncoder);
  return pool.createClient();
}

}  // namespace corevideo::modules
