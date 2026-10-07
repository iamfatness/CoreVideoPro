#include "modules/ShmCapturePreparation.h"
#include "core/BoundedAsyncLog.h"
#include "core/FrameAllocation.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <cstdlib>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#if defined(_WIN32) && COREVIDEO_WITH_D3D11 && !COREVIDEO_STUB
#include "modules/BgraSourcePreparation.h"
#endif

namespace corevideo::modules {
struct ShmCapturePreparation::Mapping {
  Request request;
  const uint8_t* view = nullptr;
  void* handle = nullptr;
  std::vector<std::shared_ptr<std::vector<uint8_t>>> pool;
  VideoFrame last;
  uint32_t sequence = 0;
  bool seen = false;
#if defined(_WIN32) && COREVIDEO_WITH_D3D11 && !COREVIDEO_STUB
  BgraSourcePreparation gpu;
#endif
  ~Mapping() {
#ifdef _WIN32
    if (view) UnmapViewOfFile(view);
    if (handle) CloseHandle(static_cast<HANDLE>(handle));
#endif
  }
  bool open() {
#ifdef _WIN32
    const std::wstring name(request.name.begin(), request.name.end());
    handle = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
    if (!handle) return false;
    view = static_cast<const uint8_t*>(MapViewOfFile(static_cast<HANDLE>(handle), FILE_MAP_READ, 0, 0, request.bytes + 16));
    if (!view) return false;
    for (size_t i = 0; i < kPoolFrames; ++i) {
      auto bytes = core::tryMakeFrameBuffer(request.bytes);
      if (!bytes) throw std::bad_alloc();
      pool.push_back(std::move(bytes));
    }
    return true;
#else
    return false;
#endif
  }
  bool released() const {
    return std::all_of(pool.begin(), pool.end(), [](const auto& p) { return p.use_count() == 1; });
  }
};

ShmCapturePreparation::ShmCapturePreparation(std::function<void()> beforeCopy)
    : beforeCopy_(std::move(beforeCopy)),
      gpuRequested_([] { const char* flag = std::getenv("COREVIDEO_CPU_SOURCE_PREPARATION");
        return flag && std::string(flag) == "1"; }()), thread_([this] { run(); }) {}
ShmCapturePreparation::~ShmCapturePreparation() {
  { std::lock_guard<std::mutex> lock(control_); stopping_ = true; }
  changed_.notify_one();
  thread_.join(); // lifecycle owner only; no render tick joins or retires mappings
}
bool ShmCapturePreparation::registerBuffer(const std::string& id, const std::string& name, int width, int height) {
  std::lock_guard<std::mutex> lock(control_);
  auto refuse = [&](const char* reason) { ++stats_.refused; stats_.reason = stats_.lastRefusalReason = reason; stats_.state = "degraded"; return false; };
  if (id.empty() || id.size() > 1024 || name.empty() || name.size() > 260 || width <= 0 || height <= 0) return refuse("invalid-mapping");
  const auto pixels = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
  if (pixels > (kBudgetBytes - 16) / (4 * (kPoolFrames + 1))) return refuse("capture-budget");
  const size_t bytes = static_cast<size_t>(pixels * 4);
  auto next = std::make_shared<Requests>(*std::atomic_load(&wanted_));
  auto entry = std::find_if(next->begin(), next->end(), [&](const auto& r) { return r.id == id; });
  if (entry != next->end() && entry->name == name && entry->width == width && entry->height == height) return true;
  if (entry == next->end() && next->size() >= kMaxSources) return refuse("capture-source-capacity");
  size_t reserved = bytes * (kPoolFrames + 1) + 16;
  for (const auto& request : *next) if (request.id != id) reserved += request.bytes * (kPoolFrames + 1) + 16;
  if (reserved > kBudgetBytes) return refuse("capture-budget");
  Request request{id, name, width, height, nextGeneration_.fetch_add(1), bytes};
  if (entry != next->end()) {
    request.held = entry->held;
    for (const auto& completed : *std::atomic_load(&completed_)) {
      if (completed.generation == entry->generation && completed.frame.participantId == "capture:" + id)
        request.held = completed.frame;
    }
  }
  if (entry == next->end()) next->push_back(std::move(request)); else *entry = std::move(request);
  std::shared_ptr<const Requests> immutable = std::move(next);
  std::atomic_store(&wanted_, std::move(immutable));
  ++stats_.accepted;
  stats_.state = "warming"; stats_.reason.clear();
  changed_.notify_one();
  return true;
}
void ShmCapturePreparation::unregisterBuffer(const std::string& id) {
  std::lock_guard<std::mutex> lock(control_);
  auto next = std::make_shared<Requests>(*std::atomic_load(&wanted_));
  next->erase(std::remove_if(next->begin(), next->end(), [&](const auto& r) { return r.id == id; }), next->end());
  std::shared_ptr<const Requests> immutable = std::move(next);
  std::atomic_store(&wanted_, std::move(immutable));
  changed_.notify_one();
}
std::vector<VideoFrame> ShmCapturePreparation::latest() const {
  const auto wanted = std::atomic_load(&wanted_);
  const auto ready = std::atomic_load(&completed_);
  std::vector<VideoFrame> frames;
  frames.reserve(wanted->size());
  for (const auto& request : *wanted) {
    auto completed = std::find_if(ready->begin(), ready->end(), [&](const auto& frame) {
      return frame.frame.participantId == "capture:" + request.id && frame.generation == request.generation;
    });
    if (completed != ready->end()) frames.push_back(completed->frame);
    else if (request.held.hasPixels()) frames.push_back(request.held);
  }
  return frames;
}
ShmCapturePreparation::Stats ShmCapturePreparation::stats() const {
  std::lock_guard<std::mutex> lock(control_); return stats_;
}
void ShmCapturePreparation::run() {
  std::map<std::string, std::unique_ptr<Mapping>> active, retiring;
  uint64_t identity = 0;
#if defined(_WIN32) && COREVIDEO_WITH_D3D11 && !COREVIDEO_STUB
  ComPtr<ID3D11Device> gpuDevice;
  ComPtr<ID3D11DeviceContext> gpuContext;
  bool gpuDeviceAttempted = false;
  BgraSourcePreparation::Stats gpuTotals;
  auto accumulateGpu = [&](const BgraSourcePreparation::Stats& before, const BgraSourcePreparation::Stats& after) {
    gpuTotals.prepared += after.prepared - before.prepared;
    gpuTotals.busy += after.busy - before.busy;
    gpuTotals.failed += after.failed - before.failed;
    gpuTotals.superseded += after.superseded - before.superseded;
  };
#endif
  for (;;) {
    { std::unique_lock<std::mutex> lock(control_);
      if (active.empty() && retiring.empty() && std::atomic_load(&wanted_)->empty()) {
        changed_.wait(lock, [&] { return stopping_ || !std::atomic_load(&wanted_)->empty(); });
      } else {
        changed_.wait_for(lock, std::chrono::milliseconds(2));
      }
      if (stopping_) break;
    }
    const auto wanted = std::atomic_load(&wanted_);
    uint64_t refused = 0, prepared = 0, torn = 0, busy = 0, failed = 0;
    uint64_t copyTotalNs = 0, copyMaximumNs = 0;
    std::string reason;
    std::string refusalReason;
    const auto refuse = [&](const char* value) { ++refused; refusalReason = reason = value; };
    for (auto it = retiring.begin(); it != retiring.end();) {
      bool released = it->second->released();
#if defined(_WIN32) && COREVIDEO_WITH_D3D11 && !COREVIDEO_STUB
      if (gpuContext) released = released && it->second->gpu.released(gpuContext.Get());
#endif
      it = released ? retiring.erase(it) : std::next(it);
    }
    for (auto it = active.begin(); it != active.end();) {
      const auto desired = std::find_if(wanted->begin(), wanted->end(), [&](const auto& r) { return r.id == it->first; });
      if (desired != wanted->end() && desired->generation == it->second->request.generation) { ++it; continue; }
      if (retiring.count(it->first)) { refuse("capture-retirement-pending"); ++it; continue; }
      it->second->last = {}; // release this owner's payload before checking external leases
      retiring.emplace(it->first, std::move(it->second));
      it = active.erase(it);
    }
    auto residency = [&] {
      size_t bytes = 0;
      for (const auto* set : {&active, &retiring})
        for (const auto& [id, mapping] : *set) bytes += mapping->request.bytes * (kPoolFrames + 1) + 16;
      return bytes;
    };
    for (const auto& request : *wanted) {
      if (active.count(request.id)) continue;
      if (active.size() + retiring.size() >= kMaxSources * 2) { refuse("capture-source-capacity"); continue; }
      if (residency() + request.bytes * (kPoolFrames + 1) + 16 > kBudgetBytes) { refuse("capture-budget"); continue; }
      auto mapping = std::make_unique<Mapping>(); mapping->request = request;
      // The desired registry owns the frozen fallback until replacement pixels
      // arrive. A mapping must not keep that retired generation alive itself.
      mapping->request.held = {};
      try {
        if (!mapping->open()) { ++failed; reason = "capture-mapping-unavailable"; continue; }
      } catch (...) { ++failed; reason = "capture-allocation"; continue; }
      active.emplace(request.id, std::move(mapping));
    }
    bool changed = false;
#if defined(_WIN32) && COREVIDEO_WITH_D3D11 && !COREVIDEO_STUB
    // No device or pool exists without both an explicit opt-in and a real
    // compositor consumer. Create on that consumer's adapter, not a guessed GPU.
    if (gpuRequested_ && !gpuDeviceAttempted) {
      const auto consumers = D3DVideoConsumers::snapshot();
      auto consumer = std::find_if(consumers.begin(), consumers.end(), [](const auto& c) { return !c->monitor; });
      if (consumer != consumers.end()) {
        gpuDeviceAttempted = true;
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        if (SUCCEEDED((*consumer)->device.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)))
          D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
              nullptr, 0, D3D11_SDK_VERSION, &gpuDevice, nullptr, &gpuContext);
        if (!gpuDevice || !gpuContext) core::nativeLogf("[cpu-source-preparation] GPU device unavailable; CPU retained\n");
      }
    }
    if (gpuContext) for (auto& [id, mapping] : active) {
      const auto before = mapping->gpu.stats();
      if (mapping->gpu.poll(gpuContext.Get(), mapping->last)) changed = true;
      mapping->gpu.offer(gpuDevice.Get(), gpuContext.Get(), mapping->last);
      accumulateGpu(before, mapping->gpu.stats());
    }
#endif
    for (auto& [id, mapping] : active) {
      if (!mapping->view) continue;
      auto sequence = [&] { return *reinterpret_cast<const volatile uint32_t*>(mapping->view); };
      const auto first = sequence();
      std::atomic_thread_fence(std::memory_order_acquire);
      if ((first & 1) || (!mapping->seen && first == 0) || (mapping->seen && first == mapping->sequence)) continue;
      uint32_t width, height;
      std::memcpy(&width, mapping->view + 4, 4); std::memcpy(&height, mapping->view + 8, 4);
      if (width != static_cast<uint32_t>(mapping->request.width) || height != static_cast<uint32_t>(mapping->request.height)) {
        refuse("capture-dimensions"); continue;
      }
      auto available = std::find_if(mapping->pool.begin(), mapping->pool.end(), [](const auto& p) { return p.use_count() == 1; });
      if (available == mapping->pool.end()) { ++busy; reason = "capture-pool-busy"; continue; }
      const auto copyStart = std::chrono::steady_clock::now();
      const auto captureTime = std::chrono::duration_cast<std::chrono::nanoseconds>(copyStart.time_since_epoch()).count() / 100;
      try { if (beforeCopy_) beforeCopy_(); }
      catch (...) { ++failed; reason = "capture-copy"; continue; }
      std::atomic_thread_fence(std::memory_order_acquire);
      std::memcpy((*available)->data(), mapping->view + 16, mapping->request.bytes);
      std::atomic_thread_fence(std::memory_order_acquire);
      const auto workNs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - copyStart).count());
      copyTotalNs += workNs; copyMaximumNs = std::max(copyMaximumNs, workNs);
      if (sequence() != first) { ++torn; continue; }
      auto& frame = mapping->last;
      frame = {};
      frame.participantId = "capture:" + id;
      frame.width = frame.naturalWidth = frame.pixelWidth = mapping->request.width;
      frame.height = frame.naturalHeight = frame.pixelHeight = mapping->request.height;
      frame.pixelStride = frame.width * 4;
      frame.pixels = *available;
      frame.frameId = static_cast<int64_t>(++identity);
      frame.sourceEpoch = mapping->request.generation;
      frame.captureTimestamp100ns = captureTime; // observation boundary, not sender-provided acquisition time
      if (gpuRequested_) {
        auto token = std::make_shared<CpuSourceGpuView>();
        token->sourceId = frame.participantId; token->sourceEpoch = frame.sourceEpoch; token->frameId = frame.frameId;
        token->captureTimestamp100ns = frame.captureTimestamp100ns; token->width = frame.pixelWidth; token->height = frame.pixelHeight;
        token->cpu = frame.pixels; token->demand = std::make_shared<CpuSourceGpuDemand>(); frame.preparedGpu = std::move(token);
#if defined(_WIN32) && COREVIDEO_WITH_D3D11 && !COREVIDEO_STUB
        if (gpuDeviceAttempted && !gpuContext) frame.preparedGpu->demand->failed.store(true);
#endif
      }
      mapping->seen = true; mapping->sequence = first;
      ++prepared; changed = true;
#if defined(_WIN32) && COREVIDEO_WITH_D3D11 && !COREVIDEO_STUB
      if (gpuContext) {
        const auto before = mapping->gpu.stats();
        mapping->gpu.offer(gpuDevice.Get(), gpuContext.Get(), frame);
        accumulateGpu(before, mapping->gpu.stats());
      }
#endif
    }
    const auto old = std::atomic_load(&completed_);
    // Publish after all copies; the descriptor handoff never holds control_.
    if (changed || old->size() != active.size() || !retiring.empty()) {
      auto next = std::make_shared<Frames>();
      for (const auto& [id, mapping] : active) if (mapping->last.hasPixels())
        next->push_back({mapping->request.generation, mapping->last});
      std::shared_ptr<const Frames> immutable = std::move(next);
      std::atomic_store(&completed_, std::move(immutable));
    }
    { std::lock_guard<std::mutex> lock(control_);
      // Drop frozen fallback leases on the preparation owner once replacement
      // pixels exist. Never leave a retired pool pinned by the desired registry.
      if (std::atomic_load(&wanted_) == wanted) {
        std::shared_ptr<Requests> next;
        for (size_t i = 0; i < wanted->size(); ++i) {
          const auto& request = wanted->at(i);
          const auto found = active.find(request.id);
          if (request.held.hasPixels() && found != active.end() &&
              found->second->request.generation == request.generation && found->second->last.hasPixels()) {
            if (!next) next = std::make_shared<Requests>(*wanted);
            next->at(i).held = {};
          }
        }
        if (next) {
          std::shared_ptr<const Requests> immutable = std::move(next);
          std::atomic_store(&wanted_, std::move(immutable));
        }
      }
      stats_.refused += refused; stats_.prepared += prepared; stats_.torn += torn;
      if (!refusalReason.empty()) stats_.lastRefusalReason = refusalReason;
      stats_.poolBusy += busy; stats_.failed += failed;
      stats_.copyTotalNs += copyTotalNs; stats_.copyMaximumNs = std::max(stats_.copyMaximumNs, copyMaximumNs);
      stats_.residentBytes = residency(); stats_.active = active.size(); stats_.retiring = retiring.size();
      stats_.gpuRequested = gpuRequested_;
#if defined(_WIN32) && COREVIDEO_WITH_D3D11 && !COREVIDEO_STUB
      stats_.gpuPrepared = gpuTotals.prepared; stats_.gpuBusy = gpuTotals.busy;
      stats_.gpuFailed = gpuTotals.failed + (gpuDeviceAttempted && !gpuDevice ? 1u : 0u);
      stats_.gpuSuperseded = gpuTotals.superseded;
#endif
      if (std::atomic_load(&wanted_) != wanted) { stats_.state = "warming"; }
      else if (!reason.empty()) { stats_.reason = reason; stats_.state = "degraded"; }
      else if (wanted->empty()) { stats_.reason.clear(); stats_.state = "idle"; }
      else {
        const bool ready = std::all_of(wanted->begin(), wanted->end(), [&](const auto& request) {
          auto found = active.find(request.id);
          return found != active.end() && found->second->request.generation == request.generation && found->second->last.hasPixels();
        });
        if (ready) { stats_.reason.clear(); stats_.state = "ready"; }
        else stats_.state = "warming";
      }
    }
  }
  // Keep pool ownership until external consumers release it during normal
  // retirement. Shutdown runs after the render owner stops consuming frames.
  std::atomic_store(&completed_, std::make_shared<const Frames>());
}
}
