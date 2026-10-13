#include "modules/AsyncAudioMonitorOutput.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include "core/PublishedObservationStore.h"
#ifdef _WIN32
#include <windows.h>
#include <avrt.h>
#endif

namespace corevideo::modules {
struct AsyncAudioMonitorOutput::State {
  struct Facts {
    std::uint64_t generation = 0;
    std::string name, endpoint;
    std::vector<std::string> warnings;
    std::int64_t underruns = 0;
  };
  core::PublishedObservationStore<Facts> observations;
  struct Packet {
    std::vector<float> pcm;
    int frames = 0, channels = 0;
    double volume = 0;
    std::uint64_t generation = 0;
    std::chrono::steady_clock::time_point queuedAt;
  };
  std::unique_ptr<IAudioMonitorOutput> inner;
  mutable std::mutex mutex;
  std::condition_variable changed, finished;
  bool stopping = false, done = false, desired = false, ready = false, pending = false, hardware = false;
  std::uint64_t generation = 0;
  std::uint64_t lastAcceptedGeneration = 0;
  int sampleRate = 48000, channels = 2;
  std::string deviceId;
  std::int64_t accepted = 0, dropped = 0, underruns = 0;
  std::deque<Packet> packets;
  std::chrono::steady_clock::time_point failedAt{};
};

AsyncAudioMonitorOutput::AsyncAudioMonitorOutput(std::unique_ptr<IAudioMonitorOutput> inner)
    : state_(std::make_shared<State>()) {
  state_->hardware = inner && inner->hardwareOutput();
  state_->inner = std::move(inner);
  worker_ = std::thread([state = state_] { run(state); });
}
AsyncAudioMonitorOutput::~AsyncAudioMonitorOutput() {
  std::deque<State::Packet> retired;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->stopping = true; ++state_->generation;
    retired.swap(state_->packets);
  }
  state_->changed.notify_one();
  bool done;
  {
    std::unique_lock<std::mutex> lock(state_->mutex);
    done = state_->finished.wait_for(lock, std::chrono::milliseconds(200), [&] { return state_->done; });
  }
  // An uninterruptible driver cannot hold core shutdown. The shared worker
  // state owns its backend until it returns; it never references MediaCore.
  if (worker_.joinable()) { if (done) worker_.join(); else worker_.detach(); }
}
bool AsyncAudioMonitorOutput::start(const std::string& id, int rate, int channels) {
  std::deque<State::Packet> retired;
  bool ready;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->stopping) return false;
    const bool same = state_->desired && state_->deviceId == id && state_->sampleRate == rate && state_->channels == channels;
    const bool retry = same && !state_->ready && !state_->pending &&
        std::chrono::steady_clock::now() - state_->failedAt >= std::chrono::seconds(1);
    if (!same || retry) {
      state_->desired = true; state_->deviceId = id; state_->sampleRate = rate; state_->channels = channels;
      ++state_->generation; state_->ready = false; state_->pending = true;
      for (const auto& packet : state_->packets) state_->dropped += packet.frames;
      retired.swap(state_->packets);
    }
    ready = state_->ready;
  }
  state_->changed.notify_one(); return ready;
}
void AsyncAudioMonitorOutput::stop() {
  std::deque<State::Packet> retired;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->desired) return;
    state_->desired = false; state_->ready = false; state_->pending = true; ++state_->generation;
    for (const auto& packet : state_->packets) state_->dropped += packet.frames;
    retired.swap(state_->packets);
  }
  state_->changed.notify_one();
}
bool AsyncAudioMonitorOutput::render(const float* pcm, int frames, int channels, double volume) {
  if (!pcm || frames <= 0 || frames > 4096 || channels <= 0 || channels > 8) return false;
  State::Packet packet;
  packet.pcm.assign(pcm, pcm + static_cast<std::size_t>(frames) * channels);
  packet.frames = frames; packet.channels = channels; packet.volume = volume;
  packet.queuedAt = std::chrono::steady_clock::now();
  State::Packet retired;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->ready || !state_->desired || state_->stopping) return false;
    packet.generation = state_->generation;
    if (state_->packets.size() == 4) {
      state_->dropped += state_->packets.front().frames;
      retired = std::move(state_->packets.front()); state_->packets.pop_front();
    }
    state_->packets.push_back(std::move(packet));
  }
  state_->changed.notify_one(); return true;
}
void AsyncAudioMonitorOutput::run(const std::shared_ptr<State>& state) {
#ifdef _WIN32
  DWORD taskIndex = 0;
  HANDLE task = ::AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
  if (task) ::AvSetMmThreadPriority(task, AVRT_PRIORITY_HIGH);
#endif
  std::uint64_t applied = 0;
  for (;;) {
    State::Packet packet;
    std::uint64_t generation;
    bool configure, desired;
    std::string id;
    int rate, channels;
    {
      std::unique_lock<std::mutex> lock(state->mutex);
      state->changed.wait(lock, [&] { return state->stopping || applied != state->generation || !state->packets.empty(); });
      if (state->stopping) break;
      generation = state->generation; configure = applied != generation;
      desired = state->desired; id = state->deviceId; rate = state->sampleRate; channels = state->channels;
      if (!configure) { packet = std::move(state->packets.front()); state->packets.pop_front(); }
    }
    bool ready = false;
    std::string name, endpoint;
    std::vector<std::string> warnings;
    std::int64_t underruns = 0;
    bool accepted = false;
    try {
      if (configure) {
        if (state->inner) {
          state->inner->stop();
          ready = desired && state->inner->start(id, rate, channels);
        }
        applied = generation;
      } else if (packet.generation == generation &&
                 std::chrono::steady_clock::now() - packet.queuedAt <= std::chrono::milliseconds(80)) {
        accepted = state->inner && state->inner->render(packet.pcm.data(), packet.frames, packet.channels, packet.volume);
      }
      if (state->inner) {
        ready = state->inner->active(); name = state->inner->deviceName();
        endpoint = state->inner->resolvedEndpointId(); warnings = state->inner->warnings();
        underruns = state->inner->underrunCount();
      }
    } catch (...) {
      warnings = {"Audio monitor device operation failed."}; applied = generation;
    }
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (!configure) {
        if (accepted) { state->accepted += packet.frames; state->lastAcceptedGeneration = generation; }
        else state->dropped += packet.frames;
      }
    }
    const bool published = state->observations.publish(generation, [&](auto& facts) {
      facts.generation = generation; facts.name = std::move(name); facts.endpoint = std::move(endpoint);
      facts.warnings = std::move(warnings); facts.underruns = underruns;
      std::size_t bytes = sizeof(facts) + facts.name.capacity() + facts.endpoint.capacity() + 96;
      bytes += facts.warnings.capacity() * sizeof(std::string);
      for (const auto& warning : facts.warnings) bytes += warning.capacity() + 32;
      return bytes;
    });
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (state->generation == generation) {
        state->ready = desired && ready && published; state->pending = false;
        state->underruns = underruns;
        if (desired && !state->ready) state->failedAt = std::chrono::steady_clock::now();
      }
    }
  }
  try { if (state->inner) state->inner->stop(); } catch (...) {}
  state->inner.reset(); // COM/device retirement on its owner, not the caller
#ifdef _WIN32
  if (task) ::AvRevertMmThreadCharacteristics(task);
#endif
  { std::lock_guard<std::mutex> lock(state->mutex); state->done = true; }
  state->finished.notify_all();
}
bool AsyncAudioMonitorOutput::active() const { std::lock_guard<std::mutex> lock(state_->mutex); return state_->ready; }
bool AsyncAudioMonitorOutput::preparing() const { std::lock_guard<std::mutex> lock(state_->mutex); return state_->pending; }
bool AsyncAudioMonitorOutput::hardwareOutput() const { return state_->hardware; }
std::string AsyncAudioMonitorOutput::deviceName() const {
  std::uint64_t generation;
  { std::lock_guard<std::mutex> lock(state_->mutex); generation = state_->generation; }
  const auto view = state_->observations.acquire();
  return view.payload && view.payload->value.generation == generation ? view.payload->value.name : std::string{};
}
std::string AsyncAudioMonitorOutput::resolvedEndpointId() const {
  std::uint64_t generation;
  { std::lock_guard<std::mutex> lock(state_->mutex); generation = state_->generation; }
  const auto view = state_->observations.acquire();
  return view.payload && view.payload->value.generation == generation ? view.payload->value.endpoint : std::string{};
}
std::vector<std::string> AsyncAudioMonitorOutput::warnings() const {
  std::uint64_t generation;
  std::int64_t dropped;
  { std::lock_guard<std::mutex> lock(state_->mutex); generation = state_->generation; dropped = state_->dropped; }
  const auto view = state_->observations.acquire();
  auto warnings = view.payload && view.payload->value.generation == generation ? view.payload->value.warnings : std::vector<std::string>{};
  if (dropped) warnings.emplace_back("Audio monitor queue discarded " + std::to_string(dropped) + " sample-frames.");
  return warnings;
}
std::int64_t AsyncAudioMonitorOutput::underrunCount() const { std::lock_guard<std::mutex> lock(state_->mutex); return state_->underruns; }
std::int64_t AsyncAudioMonitorOutput::acceptedFrameCount() const { std::lock_guard<std::mutex> lock(state_->mutex); return state_->accepted; }
std::int64_t AsyncAudioMonitorOutput::droppedFrameCount() const { std::lock_guard<std::mutex> lock(state_->mutex); return state_->dropped; }
bool AsyncAudioMonitorOutput::deviceDeliveryObserved() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->ready && state_->lastAcceptedGeneration == state_->generation;
}

}  // namespace corevideo::modules
