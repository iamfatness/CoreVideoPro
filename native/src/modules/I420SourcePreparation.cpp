#include "modules/I420SourcePreparation.h"
#include "core/BoundedAsyncLog.h"
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#if defined(_WIN32) && COREVIDEO_WITH_D3D11 && !COREVIDEO_STUB
#include "modules/D3DI420VideoFrame.h"
#endif

namespace corevideo::modules {
struct I420SourcePreparation::Impl {
  mutable std::mutex mutex;
  Stats measured;
#if defined(_WIN32) && COREVIDEO_WITH_D3D11 && !COREVIDEO_STUB
  struct Source {
    std::string id;
    uint64_t epoch = 0;
    int64_t lastOfferedFrameId = -1;
    int width = 0, height = 0;
    std::shared_ptr<CpuSourceGpuDemand> demand = std::make_shared<CpuSourceGpuDemand>();
    std::deque<std::weak_ptr<CpuSourceGpuView>> queued; // protected by Impl::mutex
    std::shared_ptr<D3DI420FramePool> pool; // setup handoff protected by mutex
    bool buildAttempted = false, building = false;
    // GPU owner only from here down.
    std::shared_ptr<CpuSourceGpuView> pending;
    int pendingSlot = -1;
    struct Ready { std::weak_ptr<CpuSourceGpuView> token; std::shared_ptr<const GpuVideoFrame> image; };
    std::array<Ready, 3> ready;
  };
  std::map<std::string, std::shared_ptr<Source>> sources;
  std::vector<std::shared_ptr<Source>> retiring;
  std::deque<std::shared_ptr<Source>> builds;
  std::condition_variable changed;
  std::atomic<bool> stopping{false};
  std::atomic<bool> resourcesStopped{false};
  bool deviceFailed = false; // GPU owner only
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  std::function<void(const std::string&)> beforeResources;
  std::thread gpuThread, resourceThread;
  inline static std::mutex quarantineMutex;
  inline static std::vector<std::shared_ptr<D3DI420FramePool>> quarantined;

  explicit Impl(bool enabled, std::function<void(const std::string&)> hook) : beforeResources(std::move(hook)) {
    measured.requested = enabled; measured.supported = true;
    if (enabled) {
      try {
        resourceThread = std::thread([this] { resourceLoop(); });
        gpuThread = std::thread([this] { gpuLoop(); });
      } catch (...) {
        stopping.store(true); changed.notify_all();
        if (resourceThread.joinable()) resourceThread.join();
        measured.supported = false; ++measured.failed;
      }
    }
  }
  ~Impl() {
    stopping.store(true); changed.notify_all();
    if (resourceThread.joinable()) resourceThread.join();
    if (gpuThread.joinable()) gpuThread.join();
  }
  std::vector<std::shared_ptr<Source>> snapshot() {
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<std::shared_ptr<Source>> result = retiring;
    for (const auto& [id, source] : sources) result.push_back(source);
    return result;
  }
  void resourceLoop() {
    for (;;) {
      std::shared_ptr<Source> source;
      ComPtr<ID3D11Device> producer;
      {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [&] { return stopping.load() || !builds.empty(); });
        if (stopping.load()) { resourcesStopped.store(true); return; }
        source = std::move(builds.front()); builds.pop_front(); producer = device;
      }
      std::shared_ptr<D3DI420FramePool> pool;
      if (!source->demand->stopped.load()) {
        try {
          if (beforeResources) beforeResources(source->id);
          if (!source->demand->stopped.load()) {
            auto candidate = std::make_shared<D3DI420FramePool>();
            if (candidate->initialize(producer.Get(), source->width, source->height)) pool = std::move(candidate);
          }
        } catch (...) { pool.reset(); }
      }
      {
        std::lock_guard<std::mutex> lock(mutex);
        source->building = false;
        if (pool && !source->demand->stopped.load()) source->pool = std::move(pool);
        else { source->demand->failed.store(true); if (measured.active) --measured.active; ++measured.failed; }
      }
    }
  }
  bool createDevice() {
    const auto consumers = D3DVideoConsumers::snapshot();
    for (const auto& consumer : consumers) if (!consumer->monitor) {
      ComPtr<IDXGIDevice> dxgi; ComPtr<IDXGIAdapter> adapter;
      ComPtr<ID3D11Device> producer; ComPtr<ID3D11DeviceContext> owner;
      if (FAILED(consumer->device.As(&dxgi)) || FAILED(dxgi->GetAdapter(&adapter)) ||
          FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
              D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &producer, nullptr, &owner))) return false;
      std::lock_guard<std::mutex> lock(mutex);
      device = std::move(producer); context = std::move(owner);
      return true;
    }
    return false;
  }
  void haltPreparation(HRESULT reason) {
    deviceFailed = true;
    for (const auto& source : snapshot()) {
      source->demand->stopped.store(true);
      source->ready = {};
    }
    {
      std::lock_guard<std::mutex> lock(mutex);
      measured.supported = false; ++measured.failed;
    }
    // Keep pools charged and pending writes unreusable until shutdown drains
    // or quarantines them. CPU descriptors remain available for fallback.
    core::nativeLogf("[cpu-i420-preparation] GPU preparation stopped reason=0x%08lx\n",
        static_cast<unsigned long>(reason));
  }
  void tick() {
    const auto removed = device->GetDeviceRemovedReason();
    if (FAILED(removed)) { haltPreparation(removed); return; }
    const auto current = snapshot();
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count() / 100;
    bool submitted = false;
    for (const auto& source : current) {
      const auto lastDemand = source->demand->lastDemand100ns.load();
      if (lastDemand && now - lastDemand > 50'000'000) source->demand->stopped.store(true);
      std::shared_ptr<D3DI420FramePool> pool;
      {
        std::lock_guard<std::mutex> lock(mutex);
        pool = source->pool;
        if (!source->demand->stopped.load() && lastDemand && !source->buildAttempted && measured.active < kMaxActive) {
          bool quarantineFull;
          { std::lock_guard<std::mutex> quarantineLock(quarantineMutex); quarantineFull = quarantined.size() >= kMaxActive * 2; }
          if (!quarantineFull) {
            source->buildAttempted = source->building = true; ++measured.active;
            builds.push_back(source); changed.notify_one();
          }
        }
      }
      if (!pool) continue;
      if (FAILED(pool->failure())) { haltPreparation(pool->failure()); return; }
      const auto selected = source->demand->selectedFrameId.load();
      for (auto& ready : source->ready) {
        const auto token = ready.token.lock();
        if (ready.image && (source->demand->stopped.load() || !token || token->consumed.load() || token->frameId < selected))
          ready = {}; // active Program source/read caches still own their wrapper
      }
      if (source->pendingSlot >= 0) {
        if (auto storage = pool->completed(context.Get(), source->pendingSlot)) {
          if (!source->demand->stopped.load() && source->pending && source->pending->frameId >= selected) {
            auto image = std::make_shared<D3DI420VideoImage>(std::move(storage), *source->pending);
            auto& ready = source->ready[source->pendingSlot];
            ready = {source->pending, image};
            source->pending->ready.store(image);
            std::lock_guard<std::mutex> lock(mutex); ++measured.prepared;
          } else { std::lock_guard<std::mutex> lock(mutex); ++measured.superseded; }
          source->pendingSlot = -1; source->pending.reset();
        }
      }
      if (source->demand->stopped.load()) {
        if (pool->idle(context.Get())) {
          std::lock_guard<std::mutex> lock(mutex);
          source->pool.reset(); if (measured.active) --measured.active;
        }
        continue;
      }
      if (source->pendingSlot >= 0) continue;
      std::shared_ptr<CpuSourceGpuView> next;
      {
        std::lock_guard<std::mutex> lock(mutex);
        while (!source->queued.empty()) {
          next = source->queued.front().lock();
          if (next && next->frameId >= selected && !next->cpu.expired()) break;
          source->queued.pop_front(); ++measured.superseded; next.reset();
        }
      }
      if (!next) continue;
      auto cpu = next->cpu.lock(); if (!cpu) continue;
      const int slot = pool->beginUpload(context.Get(), *cpu);
      if (slot >= 0) {
        source->pendingSlot = slot; source->pending = next; submitted = true;
        std::lock_guard<std::mutex> lock(mutex);
        if (!source->queued.empty() && source->queued.front().lock() == next) source->queued.pop_front();
      }
    }
    if (submitted) context->Flush(); // one batch submission, only on GPU owner
    {
      std::lock_guard<std::mutex> lock(mutex);
      const auto released = [](const auto& source) { return source->demand->stopped.load() && !source->pool && !source->building; };
      retiring.erase(std::remove_if(retiring.begin(), retiring.end(), released), retiring.end());
      for (auto it = sources.begin(); it != sources.end();) it = released(it->second) ? sources.erase(it) : std::next(it);
      measured.sources = sources.size();
    }
  }
  void gpuLoop() {
    bool attempted = false;
    while (!stopping.load()) {
      try {
        if (!context && !attempted) {
          bool needed = false;
          for (const auto& source : snapshot()) if (source->demand->lastDemand100ns.load()) needed = true;
          if (needed) { attempted = true; if (!createDevice()) haltPreparation(E_FAIL); }
        }
        if (context && !deviceFailed) tick();
      } catch (...) { std::lock_guard<std::mutex> lock(mutex); ++measured.failed; }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    // Shutdown/lifecycle owner drains writes; external reads keep immutable
    // storage alive. A pending write cannot become an uncharged reusable pool.
    while (!resourcesStopped.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (context) {
      auto current = snapshot();
      for (auto& source : current) { source->demand->stopped.store(true); source->ready = {}; }
      context->Flush();
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      for (auto& source : current) if (source->pool && source->pendingSlot >= 0) {
        while (!source->pool->completed(context.Get(), source->pendingSlot) && std::chrono::steady_clock::now() < deadline)
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (!source->pool->completed(context.Get(), source->pendingSlot)) {
          std::lock_guard<std::mutex> lock(quarantineMutex); quarantined.push_back(source->pool);
          core::nativeLogf("[cpu-i420-preparation] pending GPU write quarantined at shutdown\n");
        }
      }
    }
  }
#else
  explicit Impl(bool enabled, std::function<void(const std::string&)>) { measured.requested = enabled; }
#endif
};

I420SourcePreparation::I420SourcePreparation(bool enabled, std::function<void(const std::string&)> hook)
    : impl_(std::make_unique<Impl>(enabled, std::move(hook))) {}
I420SourcePreparation::~I420SourcePreparation() = default;
I420SourcePreparation::Stats I420SourcePreparation::stats() const { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->measured; }
std::shared_ptr<CpuSourceGpuView> I420SourcePreparation::offer(const std::string& id, uint64_t epoch,
    int64_t frameId, int64_t captureTimestamp100ns, int width, int height,
    const std::shared_ptr<const std::vector<uint8_t>>& cpu) {
#if defined(_WIN32) && COREVIDEO_WITH_D3D11 && !COREVIDEO_STUB
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->measured.requested || !impl_->measured.supported || !cpu) return {};
  if (id.empty() || id.size() > 1024 || !epoch || width <= 0 || height <= 0 ||
      frameId < 0 ||
      width > 7680 || height > 4320 || (width & 1) || (height & 1) ||
      cpu->size() < static_cast<size_t>(width) * height * 3 / 2) { ++impl_->measured.refused; return {}; }
  try {
    auto found = impl_->sources.find(id);
    std::shared_ptr<Impl::Source> source = found == impl_->sources.end() ? nullptr : found->second;
    if (source && source->epoch > epoch) { ++impl_->measured.superseded; return {}; }
    if (source && source->epoch == epoch && !source->demand->stopped.load() &&
        (source->width != width || source->height != height || frameId <= source->lastOfferedFrameId)) {
      ++impl_->measured.refused; return {};
    }
    if (!source || source->epoch != epoch || source->width != width || source->height != height || source->demand->stopped.load()) {
      if ((!source && impl_->sources.size() >= kMaxSources) || impl_->retiring.size() >= kMaxSources) {
        ++impl_->measured.refused; return {};
      }
      auto replacement = std::make_shared<Impl::Source>();
      replacement->id = id; replacement->epoch = epoch; replacement->width = width; replacement->height = height;
      if (source) { source->demand->stopped.store(true); impl_->retiring.push_back(source); }
      source = std::move(replacement); impl_->sources.insert_or_assign(id, source);
    }
    while (!source->queued.empty() && source->queued.front().expired()) source->queued.pop_front();
    if (source->queued.size() >= kPendingPerSource) { ++impl_->measured.refused; return {}; }
    auto token = std::make_shared<CpuSourceGpuView>();
    token->sourceId = id; token->sourceEpoch = epoch; token->frameId = frameId;
    token->captureTimestamp100ns = captureTimestamp100ns; token->width = width; token->height = height;
    token->cpu = cpu; token->demand = source->demand; source->queued.push_back(token);
    source->lastOfferedFrameId = frameId;
    impl_->measured.sources = impl_->sources.size();
    return token;
  } catch (...) { ++impl_->measured.failed; return {}; }
#else
  return {};
#endif
}
}
