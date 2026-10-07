#include "core/DeliveryTrace.h"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <stdexcept>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace corevideo::core {
namespace {
static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "Trace append requires lock-free 64-bit atomics");
std::atomic<DeliveryTraceCapture*> active{nullptr};
std::uint64_t frequency() {
#if defined(_WIN32)
  LARGE_INTEGER value; QueryPerformanceFrequency(&value); return value.QuadPart;
#else
  return 1000000000;
#endif
}
}
std::int64_t deliveryTraceNow() noexcept {
#if defined(_WIN32)
  LARGE_INTEGER value; QueryPerformanceCounter(&value); return value.QuadPart;
#else
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}
struct DeliveryTraceCapture::State {
  struct Slot { std::atomic<std::uint64_t> sequence{0}; DeliveryTraceEvent event; };
  static_assert(sizeof(Slot) == 80);
  static constexpr std::size_t capacity = kStorageBytes / sizeof(Slot);
  static constexpr std::uint64_t maxExportBytes = 256 * 1024 * 1024;
  std::unique_ptr<Slot[]> slots = std::make_unique<Slot[]>(capacity);
  std::atomic<std::uint64_t> head{0}, tail{0}, lost{0}, exported{0}, failures{0}, writers{0};
  std::atomic<bool> accepting{true};
  std::mutex doneMutex;
  std::condition_variable doneChanged;
  bool done = false;
  std::thread worker;
  std::string path;
  std::function<void()> beforeExport;
  DeliveryTraceHeader header;
  explicit State(std::string value, std::function<void()> hook) : path(std::move(value)), beforeExport(std::move(hook)) {
    header.clockFrequency = frequency(); header.started = deliveryTraceNow();
    header.sessionEpoch = static_cast<std::uint64_t>(header.started);
    for (std::size_t i = 0; i < capacity; ++i) slots[i].sequence.store(i);
  }
  bool pop(DeliveryTraceEvent& event) {
    const auto position = tail.load(std::memory_order_relaxed);
    auto& slot = slots[position % capacity];
    if (slot.sequence.load(std::memory_order_acquire) != position + 1) return false;
    event = slot.event;
    slot.sequence.store(position + capacity, std::memory_order_release);
    tail.store(position + 1, std::memory_order_relaxed);
    return true;
  }
  void run() noexcept {
    try {
      if (beforeExport) beforeExport();
      // Caller supplies a new explicit capture path. Never truncate evidence.
      if (std::filesystem::exists(path)) throw std::runtime_error("trace exists");
      std::ofstream output(path, std::ios::binary);
      if (!output) throw std::runtime_error("trace open");
      output.write(reinterpret_cast<const char*>(&header), sizeof(header));
      std::array<DeliveryTraceEvent, 128> batch;
      while (accepting.load() || writers.load() || tail.load() < head.load()) {
        std::size_t count = 0;
        while (count < batch.size() && pop(batch[count])) ++count;
        if (!count) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); continue; }
        if (sizeof(header) + (exported.load() + count) * sizeof(batch[0]) > maxExportBytes)
          throw std::runtime_error("trace export capacity");
        output.write(reinterpret_cast<const char*>(batch.data()), count * sizeof(batch[0]));
        if (!output) throw std::runtime_error("trace write");
        exported.fetch_add(count);
      }
      output.flush();
      if (!output) throw std::runtime_error("trace body flush");
      header.exported = exported.load(); header.lost = lost.load();
      header.failures = failures.load(); header.ended = deliveryTraceNow(); header.complete = 1;
      output.seekp(0); output.write(reinterpret_cast<const char*>(&header), sizeof(header)); output.flush();
      if (!output) throw std::runtime_error("trace finalize");
    } catch (...) { ++failures; accepting.store(false); }
    { std::lock_guard<std::mutex> lock(doneMutex); done = true; }
    doneChanged.notify_all();
  }
};
DeliveryTraceCapture::DeliveryTraceCapture(const std::string& path, std::function<void()> beforeExport)
    : state_(std::make_shared<State>(path, std::move(beforeExport))) {
  const auto state = state_;
  state_->worker = std::thread([state] { state->run(); });
}
DeliveryTraceCapture::~DeliveryTraceCapture() {
  // Media workers have already stopped for the process-global capture.
  auto* expected = this; active.compare_exchange_strong(expected, nullptr);
  close();
}
bool DeliveryTraceCapture::record(DeliveryTraceEvent event) noexcept {
  auto& state = *state_;
  ++state.writers;
  if (!state.accepting.load()) { --state.writers; return false; }
  for (int attempt = 0; attempt < 4; ++attempt) {
    auto position = state.head.load(std::memory_order_relaxed);
    auto& slot = state.slots[position % State::capacity];
    if (slot.sequence.load(std::memory_order_acquire) != position) break;
    if (!state.head.compare_exchange_strong(position, position + 1, std::memory_order_relaxed)) continue;
    event.sessionEpoch = state.header.sessionEpoch;
    if (!event.timestamp) event.timestamp = deliveryTraceNow();
    slot.event = event;
    slot.sequence.store(position + 1, std::memory_order_release);
    --state.writers; return true;
  }
  ++state.lost; --state.writers; return false;
}
bool DeliveryTraceCapture::close() {
  if (!state_->worker.joinable()) return state_->failures.load() == 0;
  state_->accepting.store(false);
  std::unique_lock<std::mutex> lock(state_->doneMutex);
  const bool done = state_->doneChanged.wait_for(lock, std::chrono::seconds(2), [&] { return state_->done; });
  lock.unlock();
  if (done) state_->worker.join();
  else { ++state_->failures; state_->worker.detach(); }
  return done && state_->failures.load() == 0;
}
rpc::Json DeliveryTraceCapture::snapshot() const {
  return rpc::Json::Object{{"schemaVersion", "delivery-evidence-v1"}, {"enabled", true},
    {"sessionEpoch", std::to_string(state_->header.sessionEpoch)},
    {"observedAtTicks", std::to_string(deliveryTraceNow())},
    {"clock", 
#if defined(_WIN32)
    "qpc"
#else
    "steady-nanoseconds"
#endif
    }, {"clockFrequency", static_cast<double>(state_->header.clockFrequency)},
    {"storageBytes", static_cast<double>(kStorageBytes)},
    {"capacity", static_cast<double>(State::capacity)},
    {"accepted", static_cast<double>(state_->head.load())},
    {"exported", static_cast<double>(state_->exported.load())},
    {"lost", static_cast<double>(state_->lost.load())},
    {"exportFailures", static_cast<double>(state_->failures.load())},
    {"cameraReaderObserved", false}, {"displayObserved", false}, {"sourceAcquisitionObserved", false},
    {"boundaries", "source-gpu-ready/source-requested/source-draw-submitted/program-submitted/program-gpu-ready/program-delivered"}};
}
std::unique_ptr<DeliveryTraceCapture> startDeliveryTraceFromEnvironment() {
  const char* path = std::getenv("COREVIDEO_DELIVERY_TRACE_PATH");
  if (!path || !*path) return {};
  auto capture = std::make_unique<DeliveryTraceCapture>(path);
  active.store(capture.get()); return capture;
}
void recordDeliveryTrace(DeliveryTraceEvent event) noexcept {
  if (auto* capture = active.load(std::memory_order_relaxed)) capture->record(event);
}
rpc::Json deliveryEvidenceSnapshot() {
  if (auto* capture = active.load()) return capture->snapshot();
  return rpc::Json::Object{{"schemaVersion", "delivery-evidence-v1"}, {"enabled", false},
    {"cameraReaderObserved", false}, {"displayObserved", false}, {"sourceAcquisitionObserved", false}};
}
std::uint64_t DeliveryTraceCapture::tag(std::string_view identity) const noexcept {
  std::uint64_t hash = 14695981039346656037ULL ^ state_->header.sessionEpoch;
  for (unsigned char c : identity) { hash ^= c; hash *= 1099511628211ULL; }
  return hash;
}
std::uint64_t deliveryTraceTag(std::string_view identity) noexcept {
  if (auto* capture = active.load(std::memory_order_relaxed)) return capture->tag(identity);
  return 0;
}
}
