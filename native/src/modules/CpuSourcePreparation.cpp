#include "modules/CpuSourcePreparation.h"
#include "core/BoundedAsyncLog.h"
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#if defined(_WIN32) && COREVIDEO_WITH_D3D11 && !COREVIDEO_STUB
#include "modules/D3DPreparedSourcePool.h"
#endif

namespace corevideo::modules {
struct CpuSourcePreparation::Impl {
  mutable std::mutex mutex;
  Stats measured;
#if defined(_WIN32) && COREVIDEO_WITH_D3D11 && !COREVIDEO_STUB
  struct Source {
    std::string id;
    uint64_t epoch = 0;
    int64_t lastOfferedFrameId = -1;
    int width = 0, height = 0;
    bool bgra = false;
    std::shared_ptr<CpuSourceGpuDemand> demand = std::make_shared<CpuSourceGpuDemand>();
    std::deque<std::weak_ptr<CpuSourceGpuView>> queued; // protected by Impl::mutex
    std::shared_ptr<D3DPreparedSourcePool> pool; // setup handoff protected by mutex
    bool buildAttempted = false, building = false;
    // GPU owner only from here down.
    std::shared_ptr<CpuSourceGpuView> pending;
    int pendingSlot = -1;
    int64_t lastSubmittedFrameId = -1;
    struct Ready { std::weak_ptr<CpuSourceGpuView> token; std::shared_ptr<const GpuVideoFrame> image; };
    std::array<Ready, 3> ready;
  };
  std::map<std::string, std::shared_ptr<Source>> sources;
  std::vector<std::shared_ptr<Source>> retiring;
  std::deque<std::shared_ptr<Source>> builds;
  std::condition_variable changed;
  std::condition_variable gpuChanged;
  std::atomic<uint64_t> workRevision{0};
  bool pendingWrites = false; // GPU owner only
  std::atomic<bool> stopping{false};
  std::atomic<bool> resourcesStopped{false};
  bool deviceFailed = false; // GPU owner only
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  uint64_t consumerRevision = 0; // GPU owner only
  std::vector<uint64_t> productionConsumers;
  std::function<void(const std::string&)> beforeResources;
  std::function<void(const std::string&)> afterUpload;
  std::thread gpuThread, resourceThread;
  inline static std::mutex quarantineMutex;
  inline static std::vector<std::shared_ptr<D3DPreparedSourcePool>> quarantined;

  explicit Impl(bool enabled, std::function<void(const std::string&)> hook, std::function<void(const std::string&)> uploaded)
      : beforeResources(std::move(hook)), afterUpload(std::move(uploaded)) {
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
    stopping.store(true); changed.notify_all(); gpuChanged.notify_all();
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
      std::shared_ptr<D3DPreparedSourcePool> pool;
      bool capacityRefused = false;
      if (!source->demand->stopped.load()) {
        try {
          if (beforeResources) beforeResources(source->id);
          if (!source->demand->stopped.load()) {
            auto candidate = std::make_shared<D3DPreparedSourcePool>();
            if (candidate->initialize(producer.Get(), source->width, source->height, source->epoch, source->bgra)) pool = std::move(candidate);
            else capacityRefused = candidate->capacityRefused();
          }
        } catch (...) { pool.reset(); }
      }
      {
        std::lock_guard<std::mutex> lock(mutex);
        source->building = false;
        if (pool && !source->demand->stopped.load()) source->pool = std::move(pool);
        else {
          if (measured.active) --measured.active;
          if (source->demand->stopped.load()) ++measured.superseded;
          else if (capacityRefused) {
            source->demand->capacity.store(CpuPreparationCapacity::Residency);
            source->demand->stopped.store(true); ++measured.refused;
          }
          else { source->demand->failed.store(true); ++measured.failed; }
        }
      }
      workRevision.fetch_add(1); gpuChanged.notify_one();
    }
  }
  bool createDevice() {
    // Read revision before snapshot: a concurrent addition is revisited on the
    // next tick instead of being accidentally treated as already imported.
    consumerRevision = D3DVideoConsumers::revision();
    const auto consumers = D3DVideoConsumers::snapshot();
    productionConsumers.clear();
    for (const auto& consumer : consumers) if (!consumer->monitor) productionConsumers.push_back(consumer->id);
    for (const auto& consumer : consumers) if (!consumer->monitor) {
      ComPtr<IDXGIDevice> dxgi; ComPtr<IDXGIAdapter> adapter;
      ComPtr<ID3D11Device> producer; ComPtr<ID3D11DeviceContext> owner;
      if (FAILED(consumer->device.As(&dxgi)) || FAILED(dxgi->GetAdapter(&adapter)) ||
          FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
              D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &producer, nullptr, &owner))) return false;
      std::lock_guard<std::mutex> lock(mutex);
      device = std::move(producer); context = std::move(owner);
      measured.supported = true;
      return true;
    }
    return false;
  }
  void haltPreparation(HRESULT reason) {
    deviceFailed = true;
    pendingWrites = false;
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
    core::nativeLogf("[cpu-source-preparation] GPU preparation stopped reason=0x%08lx\n",
        static_cast<unsigned long>(reason));
  }
  void tick() {
    pendingWrites = false;
    const auto removed = device->GetDeviceRemovedReason();
    if (FAILED(removed)) { haltPreparation(removed); return; }
    const auto current = snapshot();
    const auto revision = D3DVideoConsumers::revision();
    if (revision != consumerRevision) {
      std::vector<uint64_t> ids;
      for (const auto& consumer : D3DVideoConsumers::snapshot())
        if (!consumer->monitor) ids.push_back(consumer->id);
      consumerRevision = revision;
      if (ids != productionConsumers) {
        productionConsumers = std::move(ids);
        // Shared views are immutable and consumer-specific. Never import into
        // live storage or on Program. Existing producer refresh/arrival taps
        // replace stopped tokens; the old pool drains reads before retirement.
        // Optional monitor registration alone does not invalidate production.
        for (const auto& source : current) {
          std::lock_guard<std::mutex> lock(mutex);
          if (source->buildAttempted) source->demand->stopped.store(true);
        }
      }
    }
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count() / 100;
    bool submitted = false;
    for (const auto& source : current) {
      const auto lastDemand = source->demand->lastDemand100ns.load();
      if (lastDemand && now - lastDemand > 50'000'000) source->demand->stopped.store(true);
      std::shared_ptr<D3DPreparedSourcePool> pool;
      {
        std::lock_guard<std::mutex> lock(mutex);
        pool = source->pool;
        if (!source->demand->stopped.load() && lastDemand && !source->buildAttempted) {
          bool quarantineFull;
          { std::lock_guard<std::mutex> quarantineLock(quarantineMutex); quarantineFull = quarantined.size() >= kMaxActive * 2; }
          if (measured.active >= kMaxActive) source->demand->capacity.store(CpuPreparationCapacity::ActiveGenerations);
          else if (quarantineFull) source->demand->capacity.store(CpuPreparationCapacity::Quarantine);
          else {
            source->demand->capacity.store(CpuPreparationCapacity::None);
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
        if (ready.image && (source->demand->stopped.load() || !token || token->consumed.load()))
          ready = {}; // active Program source/read caches still own their wrapper
      }
      if (source->pendingSlot >= 0) {
        if (auto image = pool->completed(context.Get(), source->pendingSlot, *source->pending)) {
          if (!source->demand->stopped.load() && source->pending) {
            auto& ready = source->ready[source->pendingSlot];
            ready = {source->pending, image};
            source->pending->ready.store(image);
            source->pending->completionPublished.store(true);
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
      if (source->pendingSlot >= 0) { pendingWrites = true; continue; }
      std::shared_ptr<CpuSourceGpuView> next;
      {
        std::lock_guard<std::mutex> lock(mutex);
        while (!source->queued.empty()) {
          next = source->queued.front().lock();
          if (next && next->frameId >= selected && !next->cpu.expired()) break;
          if (next) next->superseded.store(true); // selected token was never submitted
          source->queued.pop_front(); ++measured.superseded; next.reset();
        }
      }
      if (!next) continue;
      // CPU playout/guest trim may select an arrival well behind the decode
      // head. Preparing future arrivals into three slots evicts that exact
      // selected image before Program can read it. Keep future CPU tokens in
      // the weak queue and admit at most one future arrival. That one image
      // can be ready before selection without unbounded GPU lookahead or a
      // new CPU playout buffer. Older submissions remain attributable.
      if (next->frameId > selected && source->lastSubmittedFrameId > selected) continue;
      auto cpu = next->cpu.lock(); if (!cpu) continue;
      int slot = pool->beginUpload(context.Get(), *cpu, next->cpuStride);
      if (slot < 0) {
        // Late completions remain readable across newer CPU selections, but
        // producer-owned completion retention cannot consume all three slots.
        auto oldest = source->ready.end();
        size_t count = 0;
        for (auto it = source->ready.begin(); it != source->ready.end(); ++it) if (it->image) {
          ++count;
          if (oldest == source->ready.end() || it->image->sourceFrameId < oldest->image->sourceFrameId) oldest = it;
        }
        if (count > 1) { *oldest = {}; slot = pool->beginUpload(context.Get(), *cpu, next->cpuStride); }
      }
      if (slot >= 0) {
        source->pendingSlot = slot; source->pending = next; submitted = pendingWrites = true;
        source->lastSubmittedFrameId = next->frameId;
        {
          std::lock_guard<std::mutex> lock(mutex);
          if (!source->queued.empty() && source->queued.front().lock() == next) source->queued.pop_front();
        }
        if (afterUpload) afterUpload(source->id); // test-only interleaving seam
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
      const auto observedRevision = workRevision.load();
      try {
        // An initial creation failure has no pools/context to retire. Retry
        // once for a changed production consumer set, never on every CPU
        // arrival or optional monitor registration. Actual device loss with
        // live resources retains the existing conservative stopped state.
        if (deviceFailed && !context && D3DVideoConsumers::revision() != consumerRevision) {
          const auto revision = D3DVideoConsumers::revision();
          std::vector<uint64_t> ids;
          for (const auto& consumer : D3DVideoConsumers::snapshot())
            if (!consumer->monitor) ids.push_back(consumer->id);
          consumerRevision = revision;
          if (!ids.empty() && ids != productionConsumers) {
            deviceFailed = false; attempted = false;
          }
        }
        if (!context && !attempted) {
          bool needed = false;
          for (const auto& source : snapshot()) if (source->demand->lastDemand100ns.load()) needed = true;
          if (needed) { attempted = true; if (!createDevice()) haltPreparation(E_FAIL); }
        }
        if (context && !deviceFailed) tick();
      } catch (...) { std::lock_guard<std::mutex> lock(mutex); ++measured.failed; }
      // Arrival wakes the owner immediately. Active event queries need prompt
      // progress; a fixed 2 ms sleep before AND after upload added a source
      // frame at unfavorable capture/Program phases. No polling on Program.
      std::unique_lock<std::mutex> wait(mutex);
      gpuChanged.wait_for(wait, pendingWrites ? std::chrono::microseconds(100) : std::chrono::microseconds(2000),
          [&] { return stopping.load() || workRevision.load() != observedRevision; });
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
        while (!source->pool->completed(context.Get(), source->pendingSlot, *source->pending) && std::chrono::steady_clock::now() < deadline)
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (!source->pool->completed(context.Get(), source->pendingSlot, *source->pending)) {
          std::lock_guard<std::mutex> lock(quarantineMutex); quarantined.push_back(source->pool);
          core::nativeLogf("[cpu-source-preparation] pending GPU write quarantined at shutdown\n");
        }
      }
    }
  }
#else
  explicit Impl(bool enabled, std::function<void(const std::string&)>, std::function<void(const std::string&)>) { measured.requested = enabled; }
#endif
};

CpuSourcePreparation::CpuSourcePreparation(bool enabled, std::function<void(const std::string&)> hook,
    std::function<void(const std::string&)> uploaded)
    : impl_(std::make_unique<Impl>(enabled, std::move(hook), std::move(uploaded))) {}
CpuSourcePreparation::~CpuSourcePreparation() = default;
CpuSourcePreparation::Stats CpuSourcePreparation::stats() const { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->measured; }
std::shared_ptr<CpuSourceGpuView> CpuSourcePreparation::offer(const std::string& id, uint64_t epoch,
    int64_t frameId, int64_t captureTimestamp100ns, int width, int height,
    const std::shared_ptr<const std::vector<uint8_t>>& cpu) {
  return offerCpu(id, epoch, frameId, captureTimestamp100ns, width, height, 0, false, cpu);
}
std::shared_ptr<CpuSourceGpuView> CpuSourcePreparation::offerBgra(const std::string& id, uint64_t epoch,
    int64_t frameId, int64_t captureTimestamp100ns, int width, int height, int stride,
    const std::shared_ptr<const std::vector<uint8_t>>& cpu) {
  return offerCpu(id, epoch, frameId, captureTimestamp100ns, width, height, stride, true, cpu);
}
std::shared_ptr<CpuSourceGpuView> CpuSourcePreparation::offerCpu(const std::string& id, uint64_t epoch,
    int64_t frameId, int64_t captureTimestamp100ns, int width, int height, int stride, bool bgra,
    const std::shared_ptr<const std::vector<uint8_t>>& cpu) {
#if defined(_WIN32) && COREVIDEO_WITH_D3D11 && !COREVIDEO_STUB
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->measured.requested || !cpu) return {};
  if (id.empty() || id.size() > 1024 || !epoch || width <= 0 || height <= 0 ||
      frameId < 0 ||
      width > 7680 || height > 4320 ||
      (!bgra && ((width & 1) || (height & 1) || cpu->size() < static_cast<size_t>(width) * height * 3 / 2)) ||
      (bgra && (stride < width * 4 || cpu->size() < static_cast<size_t>(height - 1) * stride + static_cast<size_t>(width) * 4))) {
    ++impl_->measured.refused; return {};
  }
  try {
    const auto refused = [&](CpuPreparationCapacity reason) {
      ++impl_->measured.refused;
      auto token = std::make_shared<CpuSourceGpuView>();
      token->sourceId = id; token->sourceEpoch = epoch; token->frameId = frameId;
      token->captureTimestamp100ns = captureTimestamp100ns; token->width = width; token->height = height;
      token->cpuStride = stride; token->cpu = cpu;
      token->demand = std::make_shared<CpuSourceGpuDemand>();
      token->demand->capacity.store(reason);
      // Attributed refusal owns no queue entry, pool or pixels. Existing
      // producer held-token refresh retries it after capacity becomes free.
      token->demand->stopped.store(true);
      return token;
    };
    // A failed owner still provides an attributable failed token for new CPU
    // arrivals. Returning null would let Program keep a cached pre-failure
    // image without observing the stopped producer.
    if (!impl_->measured.supported) {
      auto token = std::make_shared<CpuSourceGpuView>();
      token->sourceId = id; token->sourceEpoch = epoch; token->frameId = frameId;
      token->captureTimestamp100ns = captureTimestamp100ns; token->width = width; token->height = height;
      token->cpuStride = stride; token->cpu = cpu;
      token->demand = std::make_shared<CpuSourceGpuDemand>(); token->demand->failed.store(true);
      if (!impl_->device) token->demand->stopped.store(true); // retryable initial setup, no live GPU resources
      return token;
    }
    auto found = impl_->sources.find(id);
    std::shared_ptr<Impl::Source> source = found == impl_->sources.end() ? nullptr : found->second;
    if (source && source->epoch > epoch) { ++impl_->measured.superseded; return {}; }
    if (source && source->epoch == epoch && !source->demand->stopped.load() &&
        (source->width != width || source->height != height || source->bgra != bgra || frameId <= source->lastOfferedFrameId)) {
      ++impl_->measured.refused; return {};
    }
    if (!source || source->epoch != epoch || source->width != width || source->height != height || source->bgra != bgra || source->demand->stopped.load()) {
      if (!source && impl_->sources.size() >= kMaxSources) return refused(CpuPreparationCapacity::Sources);
      if (impl_->retiring.size() >= kMaxSources) return refused(CpuPreparationCapacity::RetiringGenerations);
      if (source && std::any_of(impl_->retiring.begin(), impl_->retiring.end(), [&](const auto& old) {
          return old->id == id && (old->pool || old->building);
        })) return refused(CpuPreparationCapacity::RetiringGenerations); // one charged retiring generation per source
      auto replacement = std::make_shared<Impl::Source>();
      replacement->id = id; replacement->epoch = epoch; replacement->width = width; replacement->height = height;
      replacement->bgra = bgra;
      if (source) {
        source->demand->stopped.store(true);
        if (source->pool || source->building) impl_->retiring.push_back(source);
      }
      source = std::move(replacement); impl_->sources.insert_or_assign(id, source);
    }
    while (!source->queued.empty() && source->queued.front().expired()) source->queued.pop_front();
    if (source->queued.size() >= kPendingPerSource) return refused(CpuPreparationCapacity::PendingTokens);
    auto token = std::make_shared<CpuSourceGpuView>();
    token->sourceId = id; token->sourceEpoch = epoch; token->frameId = frameId;
    token->captureTimestamp100ns = captureTimestamp100ns; token->width = width; token->height = height;
    token->cpuStride = stride;
    token->cpu = cpu; token->demand = source->demand; source->queued.push_back(token);
    source->lastOfferedFrameId = frameId;
    impl_->measured.sources = impl_->sources.size();
    impl_->workRevision.fetch_add(1); impl_->gpuChanged.notify_one();
    return token;
  } catch (...) { ++impl_->measured.failed; return {}; }
#else
  return {};
#endif
}
}
