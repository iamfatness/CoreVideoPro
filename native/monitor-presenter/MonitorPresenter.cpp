#define NOMINMAX
#include "MonitorPresenter.h"
#include <Windows.h>
#include <d3d11_4.h>
#include <dxgi1_3.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <optional>

namespace {
using Microsoft::WRL::ComPtr;
std::int64_t qpc() { LARGE_INTEGER value{}; QueryPerformanceCounter(&value); return value.QuadPart; }
std::int64_t frequency() { static const auto result = [] { LARGE_INTEGER v{}; QueryPerformanceFrequency(&v); return v.QuadPart; }(); return result; }
double milliseconds(std::int64_t start) { return (qpc() - start) * (1000.0 / frequency()); }
struct Handle {
  HANDLE value = nullptr;
  ~Handle() { if (value) CloseHandle(value); }
};
struct Apartment {
  HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};

// Readers pin an immutable slot only inside a short native API call. The video
// writer never waits for a control reader, even if that reader is descheduled.
struct StatsExchange {
  struct Slot { std::atomic<int> readers{0}; CvMonitorStats value{}; };
  std::array<Slot, 3> slots;
  std::atomic<int> published{0};
  void publish(const CvMonitorStats& value) {
    const auto old = published.load(std::memory_order_relaxed);
    for (int index = 0; index < 3; ++index) {
      if (index == old) continue;
      int free = 0;
      if (!slots[index].readers.compare_exchange_strong(free, -1, std::memory_order_acquire)) continue;
      slots[index].value = value;
      slots[index].readers.store(0, std::memory_order_release);
      published.store(index, std::memory_order_release);
      return; // Statistics may lag; video is never delayed for observation.
    }
  }
  bool read(CvMonitorStats& result) {
    for (int attempt = 0; attempt < 8; ++attempt) {
      const auto index = published.load(std::memory_order_acquire);
      auto readers = slots[index].readers.load(std::memory_order_relaxed);
      if (readers < 0 || !slots[index].readers.compare_exchange_weak(readers, readers + 1, std::memory_order_acquire)) continue;
      if (index != published.load(std::memory_order_acquire)) { --slots[index].readers; continue; }
      result = slots[index].value;
      --slots[index].readers;
      return true;
    }
    return false;
  }
};
struct Source {
  ComPtr<ID3D11Texture2D> shared, latest;
  ComPtr<IDXGIKeyedMutex> mutex;
  std::uint64_t generation = 0, attempts = 0, busy = 0;
};
struct Group;
struct Surface {
  ComPtr<IDXGISwapChain1> chain;
  ComPtr<ID3D11Texture2D> backBuffer, readback;
  std::shared_ptr<Source> source; // native owning thread only
  std::weak_ptr<Group> group;
  Handle frameReady;
  std::atomic<bool> active{true}, retired{false}, resetProbe{false}, readPixel{false};
  std::uint64_t handle = 0, presentedGeneration = UINT64_MAX;
  std::uint32_t flags = 0;
  bool credit = false, pending = true;
  std::uint64_t commitAcquireBusy = 0, commitNotReady = 0;
  CvMonitorStats stats;
  StatsExchange exchange;
};
struct Lease { std::shared_ptr<Surface> surface; };

struct Group : std::enable_shared_from_this<Group> {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  Handle wake, timer;
  std::mutex gate; // configuration only; never held while doing GPU work
  std::vector<std::shared_ptr<Surface>> surfaces;
  std::array<Lease*, 64> retirements{};
  std::size_t retirementCount = 0;
  std::unordered_map<std::uint64_t, std::weak_ptr<Source>> sources;
  bool accepting = true;
  explicit Group(ID3D11Device* d, ID3D11DeviceContext* c) : device(d), context(c) {
    surfaces.reserve(64);
    wake.value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    timer.value = CreateWaitableTimerExW(nullptr, nullptr, 2, TIMER_ALL_ACCESS);
    if (!timer.value) timer.value = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    ComPtr<ID3D11Multithread> protection;
    if (SUCCEEDED(context.As(&protection))) protection->SetMultithreadProtected(TRUE);
  }
  int add(const std::shared_ptr<Surface>& surface) {
    std::lock_guard lock(gate);
    if (!accepting) return 0;
    if (surfaces.size() >= 64 || !wake.value || !timer.value) return -1;
    surface->group = shared_from_this();
    surfaces.push_back(surface);
    SetEvent(wake.value);
    return 1;
  }
  std::optional<std::uint32_t> readPixelForProbe(Surface& surface) {
    if (!(surface.flags & 1) && !surface.readPixel.exchange(false)) return {};
    HRESULT result = S_OK;
    if (!surface.readback) {
      D3D11_TEXTURE2D_DESC desc{}; surface.backBuffer->GetDesc(&desc);
      desc.Width = desc.Height = 1; desc.MipLevels = desc.ArraySize = 1;
      desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0;
      result = device->CreateTexture2D(&desc, nullptr, &surface.readback);
    }
    if (FAILED(result)) { surface.stats.hresult = result; return {}; }
    const D3D11_BOX box{0, 0, 0, 1, 1, 1};
    context->CopySubresourceRegion(surface.readback.Get(), 0, 0, 0, 0, surface.backBuffer.Get(), 0, &box);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    result = context->Map(surface.readback.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (SUCCEEDED(result)) {
      const auto pixel = *static_cast<const std::uint32_t*>(mapped.pData);
      context->Unmap(surface.readback.Get(), 0);
      return pixel;
    } else surface.stats.hresult = result;
    return {};
  }
  void pump(Surface& surface, bool retry, std::int64_t slotDue, std::int64_t wokeAt, DWORD waitResult) {
    if (surface.retired.load() || !surface.active.load()) return;
    const auto started = qpc();
    ULONG64 startedCycles = 0;
    QueryThreadCycleTime(GetCurrentThread(), &startedCycles);
    surface.stats.workerThreadId = GetCurrentThreadId();
    if (!retry) {
      const auto late = std::max<std::int64_t>(0, started - slotDue);
      ++surface.stats.periodicWakes;
      surface.stats.maximumWakeLatenessQpc = std::max(surface.stats.maximumWakeLatenessQpc, late);
      if (late > frequency() / 1000) ++surface.stats.wakesOver1Ms;
      if (late > frequency() * 4 / 1000) ++surface.stats.wakesOver4Ms;
      if (late > frequency() * 8 / 1000) ++surface.stats.wakesOver8Ms;
    }
    ++surface.stats.attempts;
    if (retry) ++surface.stats.retryAttempts;
    if (surface.resetProbe.exchange(false)) {
      surface.stats.lastCommitQpc = started;
      surface.stats.maximumGapQpc = 0;
    }
    auto finish = [&] {
      surface.stats.active = surface.active.load() && !surface.retired.load();
      surface.stats.observedQpc = qpc();
      surface.stats.totalMs += milliseconds(started);
      surface.exchange.publish(surface.stats);
    };
    if (!surface.source) {
      if (const auto found = sources.find(surface.handle); found != sources.end()) surface.source = found->second.lock();
      if (!surface.source) {
        auto source = std::make_shared<Source>();
        auto result = device->OpenSharedResource(reinterpret_cast<HANDLE>(surface.handle), IID_PPV_ARGS(&source->shared));
        if (SUCCEEDED(result)) result = source->shared.As(&source->mutex);
        D3D11_TEXTURE2D_DESC desc{};
        if (SUCCEEDED(result)) {
          source->shared->GetDesc(&desc);
          D3D11_TEXTURE2D_DESC target{}; surface.backBuffer->GetDesc(&target);
          if (desc.Width != target.Width || desc.Height != target.Height || desc.Format != target.Format) result = E_INVALIDARG;
          desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
          desc.CPUAccessFlags = 0; desc.MiscFlags = 0;
          if (SUCCEEDED(result)) result = device->CreateTexture2D(&desc, nullptr, &source->latest);
        }
        if (FAILED(result)) { surface.stats.hresult = result; surface.pending = false; finish(); return; }
        sources[surface.handle] = source;
        surface.source = std::move(source);
      }
    }
    auto& source = *surface.source;
    const auto ingestStart = qpc();
    ++source.attempts;
    const auto acquired = source.mutex->AcquireSync(1, 0);
    if (acquired == S_OK) {
      context->CopyResource(source.latest.Get(), source.shared.Get());
      ++source.generation;
      source.mutex->ReleaseSync(0); // Never hold the producer key across Present/readback.
    } else {
      ++source.busy;
      if (FAILED(acquired) || acquired == WAIT_ABANDONED) {
        surface.stats.hresult = FAILED(acquired) ? acquired : E_ABORT;
        surface.pending = false; finish(); return;
      }
    }
    surface.stats.ingestMs += milliseconds(ingestStart);
    const auto acquiredAt = qpc();
    surface.stats.ingested = source.generation;
    surface.stats.acquireAttempts = source.attempts; surface.stats.acquireBusy = source.busy;
    surface.pending = true;
    if (!source.generation || surface.presentedGeneration == source.generation) {
      finish(); return;
    }
    if (!surface.credit && surface.frameReady.value) {
      surface.credit = WaitForSingleObject(surface.frameReady.value, 0) == WAIT_OBJECT_0;
      if (!surface.credit) { ++surface.stats.notReady; finish(); return; }
    }
    if (surface.retired.load() || !surface.active.load()) { finish(); return; }
    const auto creditAt = qpc();
    const auto copyStart = qpc();
    context->CopyResource(surface.backBuffer.Get(), source.latest.Get());
    const auto copyMs = milliseconds(copyStart);
    surface.stats.copyMs += copyMs; surface.stats.copyMaximumMs = std::max(surface.stats.copyMaximumMs, copyMs);
    // A flip can change which image GetBuffer(0) denotes. Sample the actual
    // copied back buffer before Present, and publish that pixel only if this
    // presentation commits. Never map a fresh, unwritten post-flip buffer.
    const auto observedPixel = readPixelForProbe(surface);
    const auto presentStart = qpc();
    // Flip-model interval zero permits DXGI to discard queued images. A
    // successful submission is then not one displayed video frame. Retain one
    // refresh for each accepted picture; the existing per-chain latency credit
    // and nonblocking flag keep the queue bounded without adding a UI wait.
    const auto result = surface.chain->Present(1, DXGI_PRESENT_DO_NOT_WAIT);
    const auto presentMs = milliseconds(presentStart);
    surface.stats.presentMs += presentMs; surface.stats.presentMaximumMs = std::max(surface.stats.presentMaximumMs, presentMs);
    if (result == DXGI_ERROR_WAS_STILL_DRAWING) {
      if (observedPixel) surface.readPixel.store(true);
      ++surface.stats.presentBusy; finish(); return;
    }
    if (FAILED(result)) { surface.stats.hresult = result; surface.pending = false; finish(); return; }
    const auto committed = qpc();
    if (surface.stats.lastCommitQpc) {
      const auto gap = committed - surface.stats.lastCommitQpc;
      surface.stats.maximumGapQpc = std::max(surface.stats.maximumGapQpc, gap);
      ++surface.stats.gaps;
      if (gap > frequency() * 25 / 1000) {
        ++surface.stats.gapsOver25Ms;
        surface.stats.lastGapSequence = surface.stats.presents + 1;
        surface.stats.lastGapSourceGeneration = source.generation;
        surface.stats.lastGapAcquireBusy = source.busy - surface.commitAcquireBusy;
        surface.stats.lastGapNotReady = surface.stats.notReady - surface.commitNotReady;
        surface.stats.lastGapPreviousCommitQpc = surface.stats.lastCommitQpc;
        surface.stats.lastGapStartQpc = started;
        surface.stats.lastGapDueQpc = slotDue;
        surface.stats.lastGapWakeQpc = wokeAt;
        surface.stats.lastGapAcquiredQpc = acquiredAt;
        surface.stats.lastGapCreditQpc = creditAt;
        surface.stats.lastGapPresentQpc = presentStart;
        surface.stats.lastGapCommitQpc = committed;
        surface.stats.lastGapWaitResult = waitResult;
        ULONG64 cycles = 0;
        if (QueryThreadCycleTime(GetCurrentThread(), &cycles) && cycles >= startedCycles)
          surface.stats.lastGapCpuCycles = cycles - startedCycles;
      }
      if (gap > frequency() * 33 / 1000) ++surface.stats.gapsOver33Ms;
      if (gap > frequency() * 50 / 1000) ++surface.stats.gapsOver50Ms;
    }
    surface.stats.lastCommitQpc = committed;
    ++surface.stats.presents;
    surface.commitAcquireBusy = source.busy;
    surface.commitNotReady = surface.stats.notReady;
    surface.presentedGeneration = source.generation;
    surface.credit = false; surface.pending = false;
    if (observedPixel) { surface.stats.pixel = *observedPixel; surface.stats.pixelValid = 1; }
    finish();
  }
  void run() {
    Apartment apartment;
    const auto period = frequency() / 60;
    const auto retryPeriod = std::max<std::int64_t>(1, frequency() / 1000);
    std::int64_t next = 0, retryAt = 0, emptySince = 0;
    std::int64_t slotDue = 0, wokeAt = qpc();
    DWORD waitResult = WAIT_OBJECT_0;
    std::vector<std::shared_ptr<Surface>> current;
    current.reserve(64);
    std::shared_ptr<Surface> creditWake;
    for (;;) {
      std::array<Lease*, 64> retired{};
      std::size_t retiredCount;
      {
        std::lock_guard lock(gate);
        retired = retirements; retiredCount = retirementCount; retirementCount = 0;
        std::erase_if(surfaces, [](const auto& surface) { return surface->retired.load(); });
        if (surfaces.empty()) {
          if (!emptySince) emptySince = qpc();
          if (qpc() - emptySince > frequency() * 5) { accepting = false; break; }
        } else emptySince = 0;
        current = surfaces;
      }
      for (std::size_t i = 0; i < retiredCount; ++i) delete retired[i];
      std::erase_if(sources, [](const auto& entry) { return entry.second.expired(); });
      const bool active = std::any_of(current.begin(), current.end(), [](const auto& surface) { return surface->active.load(); });
      const auto now = qpc();
      const bool periodic = active && now >= next;
      bool retry = false;
      if (periodic) {
        slotDue = next ? next : now;
        next = !next || now - next >= period ? now + period : next + period;
        retryAt = now + retryPeriod;
      } else if (active && now >= retryAt && now < next) {
        retry = true; retryAt = now + retryPeriod;
      }
      for (const auto& surface : current) {
        if (periodic || (retry && surface->pending) || surface == creditWake)
          pump(*surface, !periodic, slotDue, wokeAt, waitResult);
      }
      creditWake.reset();
      const bool pending = std::any_of(current.begin(), current.end(), [](const auto& surface) {
        return surface->active.load() && (surface->pending || surface->readPixel.load()); });
      const auto dueAt = active ? (pending ? std::min(next, retryAt) : next) : qpc() + frequency();
      LARGE_INTEGER due{};
      due.QuadPart = -std::max<std::int64_t>(1, (dueAt - qpc()) * 10'000'000 / frequency());
      SetWaitableTimer(timer.value, &due, 0, nullptr, nullptr, FALSE);
      // A second free-running 60 Hz timer must not defer a ready display queue.
      // Wait on available per-chain credits directly. Missing source pixels
      // still use the bounded retry timer; producers and the UI never wait here.
      std::array<HANDLE, MAXIMUM_WAIT_OBJECTS> waits{wake.value, timer.value};
      std::array<std::shared_ptr<Surface>, MAXIMUM_WAIT_OBJECTS> waitOwners;
      DWORD count = 2;
      for (const auto& surface : current) {
        if (!surface->active.load() || surface->retired.load() || surface->credit || !surface->frameReady.value) continue;
        if (count == waits.size()) break; // excess surfaces retain periodic/retry polling
        waits[count] = surface->frameReady.value; waitOwners[count++] = surface;
      }
      waitResult = WaitForMultipleObjects(count, waits.data(), FALSE, INFINITE);
      wokeAt = qpc();
      if (waitResult >= WAIT_OBJECT_0 + 2 && waitResult < WAIT_OBJECT_0 + count) {
        creditWake = waitOwners[waitResult - WAIT_OBJECT_0];
        creditWake->credit = true; // retain the auto-reset signal exactly once
        creditWake->pending = true;
      }
    }
  }
  void failAndDrain() {
    {
      std::lock_guard lock(gate);
      accepting = false;
      for (const auto& surface : surfaces) {
        surface->stats.hresult = E_OUTOFMEMORY;
        surface->stats.active = 0;
        surface->exchange.publish(surface->stats);
      }
    }
    for (;;) {
      std::array<Lease*, 64> retired{};
      std::size_t retiredCount;
      bool empty;
      {
        std::lock_guard lock(gate);
        retired = retirements; retiredCount = retirementCount; retirementCount = 0;
        std::erase_if(surfaces, [](const auto& surface) { return surface->retired.load(); });
        empty = surfaces.empty();
      }
      for (std::size_t i = 0; i < retiredCount; ++i) delete retired[i];
      if (empty) break;
      WaitForSingleObject(wake.value, 1000);
    }
  }
};
std::mutex groupsGate;
std::unordered_map<void*, std::weak_ptr<Group>> groups;
}

std::uint32_t cv_monitor_stats_size() { return sizeof(CvMonitorStats); }
void* cv_monitor_create(void* device, void* context, void* chain, void* buffer,
    std::uint64_t sharedHandle, void* frameReady, std::uint32_t flags) {
  if (!device || !context || !chain || !buffer || !sharedHandle || flags > 1) return nullptr;
  try {
    ComPtr<ID3D11Device> contextDevice, bufferDevice, chainDevice;
    static_cast<ID3D11DeviceContext*>(context)->GetDevice(&contextDevice);
    static_cast<ID3D11Texture2D*>(buffer)->GetDevice(&bufferDevice);
    if (FAILED(static_cast<IDXGISwapChain1*>(chain)->GetDevice(IID_PPV_ARGS(&chainDevice))) ||
        contextDevice.Get() != device || bufferDevice.Get() != device || chainDevice.Get() != device ||
        static_cast<ID3D11DeviceContext*>(context)->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return nullptr;
    auto surface = std::make_shared<Surface>();
    auto lease = std::make_unique<Lease>(Lease{surface});
    surface->chain = static_cast<IDXGISwapChain1*>(chain);
    surface->backBuffer = static_cast<ID3D11Texture2D*>(buffer);
    surface->handle = sharedHandle; surface->flags = flags; surface->stats.handle = sharedHandle;
    if (frameReady && !DuplicateHandle(GetCurrentProcess(), frameReady, GetCurrentProcess(),
        &surface->frameReady.value, 0, FALSE, DUPLICATE_SAME_ACCESS)) return nullptr;
    std::lock_guard lock(groupsGate);
    std::erase_if(groups, [](const auto& entry) { return entry.second.expired(); });
    auto group = groups[context].lock();
    if (group) {
      const auto admission = group->add(surface);
      if (admission < 0) return nullptr; // Capacity cannot create a second consumer group.
      if (!admission) group.reset();
    }
    if (!group) {
      group = std::make_shared<Group>(static_cast<ID3D11Device*>(device), static_cast<ID3D11DeviceContext*>(context));
      if (group->add(surface) != 1) return nullptr;
      groups[context] = group;
      std::thread([group] {
        try { group->run(); }
        catch (...) { group->failAndDrain(); }
      }).detach();
    }
    return lease.release();
  } catch (...) { return nullptr; }
}
void cv_monitor_release(void* value) {
  auto* lease = static_cast<Lease*>(value);
  if (!lease) return;
  if (const auto group = lease->surface->group.lock()) {
    {
      std::lock_guard lock(group->gate);
      lease->surface->retired.store(true);
      group->retirements[group->retirementCount++] = lease;
    }
    SetEvent(group->wake.value);
  } else delete lease; // A failed construction has no submitted GPU work.
}
void cv_monitor_active(void* value, std::int32_t active) {
  if (auto* lease = static_cast<Lease*>(value)) {
    if (lease->surface->active.exchange(active != 0) != (active != 0))
      if (const auto group = lease->surface->group.lock()) SetEvent(group->wake.value);
  }
}
void cv_monitor_probe_reset(void* value) {
  if (auto* lease = static_cast<Lease*>(value)) lease->surface->resetProbe.store(true);
}
void cv_monitor_probe_pixel(void* value) {
  if (auto* lease = static_cast<Lease*>(value)) {
    lease->surface->readPixel.store(true);
    if (const auto group = lease->surface->group.lock()) SetEvent(group->wake.value);
  }
}
std::int32_t cv_monitor_snapshot(void* value, CvMonitorStats* result) {
  auto* lease = static_cast<Lease*>(value);
  if (!lease || !result) return 0;
  if (!lease->surface->exchange.read(*result)) return 0;
  result->active = lease->surface->active.load() && !lease->surface->retired.load();
  return 1;
}
