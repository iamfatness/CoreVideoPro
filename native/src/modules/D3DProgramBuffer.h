#pragma once

#include "core/FrameAllocation.h"
#include "core/BoundedAsyncLog.h"
#include "core/DeliveryTrace.h"

// Windows-only implementation, included by the D3D adapter after the SDK headers.
#include "compositor/ComPtrLite.h"
#include "compositor/CompositorShaders.h"
#include "modules/Interfaces.h"
#include "core/ProgramPlayoutTimeline.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <functional>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <utility>

namespace corevideo::modules {

// Producer and preparation hand off the input with keys 0/1. Preparation
// makes an immutable snapshot and proves GPU completion. Delivery uses no D3D
// calls; two bounded optional readers retain the snapshot until their GPU reads
// complete. No worker calls into the render immediate context.
class D3DProgramBuffer {
  friend struct D3DProgramBufferTestAccess;
 public:
  D3DProgramBuffer(ID3D11Device* producer, int width, int height, int depth, uint64_t generation,
      std::function<void(const ProgramFrame&)> delivered)
      : width_(width), height_(height), depth_(depth == 2 ? 2 : 3), deliveredCallback_(std::move(delivered)) {
    static std::atomic<uint64_t> nextTraceEpoch{0};
    traceEpoch_ = ++nextTraceEpoch;
    diagnostics_.requestedFrames = depth_;
    diagnostics_.generation = generation;
    diagnostics_.capacity = depth_ + 3;
    if (!initialize(producer)) { fail("initialize"); return; }
    diagnostics_.activeFrames = depth_;
    diagnostics_.status = "priming";
    // Auto-reset. Delivery waits on this together with a high-resolution timer
    // because condition_variable::wait_until is quantized to milliseconds and
    // was starting publication after the frame deadline.
    wakeEvent_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    try {
      prepareThread_ = std::thread([this] { try { prepareLoop(); } catch (...) { fail("prepare-worker"); } });
      shellThread_ = std::thread([this] { exportLoop(shellBranch_, true); });
      multiviewThread_ = std::thread([this] { exportLoop(multiviewBranch_, false); });
      deliveryThread_ = std::thread([this] { try { deliveryLoop(); } catch (...) { fail("delivery-worker"); } });
    } catch (...) {
      fail("start-workers");
      if (prepareThread_.joinable()) prepareThread_.join();
      if (shellThread_.joinable()) shellThread_.join();
      if (multiviewThread_.joinable()) multiviewThread_.join();
      initialized_ = false;
    }
  }
  ~D3DProgramBuffer() {
    { std::lock_guard<std::mutex> lock(mutex_); stopped_ = true; }
    notifyChanged();
    if (prepareThread_.joinable()) prepareThread_.join();
    if (deliveryThread_.joinable()) deliveryThread_.join();
    if (shellThread_.joinable()) shellThread_.join();
    if (multiviewThread_.joinable()) multiviewThread_.join();
    if (wakeEvent_) ::CloseHandle(wakeEvent_);
  }
  bool valid() const { return initialized_; }
  bool dimensions(int width, int height, int depth) const {
    return width == width_ && height == height_ && depth == depth_;
  }
  void submit(ID3D11DeviceContext* producer, ID3D11Texture2D* texture, ProgramFrame frame, bool nv12) {
    Slot* slot = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopped_ || !initialized_) return;
      if (!timeline_) {
        timeline_ = std::make_unique<core::ProgramPlayoutTimeline>(depth_,
            frame.productionAnchorNs > 0 ? frame.productionAnchorNs : frame.producedAt100ns * 100,
            frame.productionSlot >= 0 ? frame.productionSlot : 0);
        firstFrameNumber_ = frame.frameNumber;
      }
      const auto productionSlot = frame.productionSlot >= 0 ? frame.productionSlot : frame.frameNumber - firstFrameNumber_;
      if (timeline_->isExpired(productionSlot) || productionSlot <= lastProducedSlot_) { ++diagnostics_.overflows; return; }
      for (auto& candidate : slots_) if (candidate->state == State::Free) {
        slot = candidate.get(); slot->state = State::Writing; slot->productionSlot = productionSlot; break;
      }
      if (!slot) { ++diagnostics_.overflows; return; }
      lastProducedSlot_ = productionSlot;
    }
    if (slot->producerMutex->AcquireSync(0, 0) != S_OK) {
      std::lock_guard<std::mutex> lock(mutex_);
      slot->state = State::Free; ++diagnostics_.gpuNotReady; return;
    }
    producer->CopyResource(slot->producerTexture.get(), texture);
    slot->producerMutex->ReleaseSync(1);
    // The producer's existing end-of-pass Flush submits this copy. No new
    // blocking flush or readback is introduced on the producer here.
    {
      std::lock_guard<std::mutex> lock(mutex_);
      trace(frame, core::DeliveryStage::ProgramSubmitted);
      slot->frame = std::move(frame);
      slot->needsNv12 = nv12;
      slot->submittedAt = std::chrono::steady_clock::now();
      slot->state = State::Submitted;
      submitted_.push_back(slot);
      delivery_.push_back(slot);
      ++diagnostics_.produced;
      diagnostics_.occupancy = static_cast<int>(delivery_.size());
    }
    notifyChanged();
  }
  bool latest(ProgramFrame& frame) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!latest_) return false;
    frame = *latest_;
    return true;
  }
  bool take(ProgramFrame& frame, int timeoutMs) {
    std::unique_lock<std::mutex> lock(mutex_);
    // The output worker relies on this bounded wait for its cadence. A failed
    // producer must not turn empty reads into a tight loop. Shutdown is still
    // bounded by the caller's timeout; zero-time polling remains nonblocking.
    changed_.wait_for(lock, std::chrono::milliseconds((std::max)(0, timeoutMs)), [&] { return !delivered_.empty(); });
    if (delivered_.empty()) return false;
    frame = std::move(delivered_.front()); delivered_.pop_front(); return true;
  }
  bool multiview(ProgramFrameSharedTexture& texture, std::shared_ptr<const void>& owner) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!multiviewLatest_) return false;
    texture = {multiviewOutput_->handle, 0, width_, height_, "B8G8R8A8_UNORM", multiviewLatest_->frameNumber};
    owner = multiviewOutput_; return true;
  }
  ProgramBufferDiagnostics diagnostics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return diagnostics_;
  }

 private:
  uint64_t traceEpoch_ = 0;
  void trace(const ProgramFrame& frame, core::DeliveryStage stage,
                    core::DeliveryReason reason = core::DeliveryReason::None) {
    core::DeliveryTraceEvent event;
    event.stage = stage; event.reason = reason; event.programSequence = frame.frameNumber;
    event.sourceEpoch = traceEpoch_; // Process-unique Program-buffer instance.
    event.layoutTag = frame.renderPlanSignature;
    core::recordDeliveryTrace(event);
  }
  void fail(const char* stage) {
    { std::lock_guard<std::mutex> lock(mutex_); diagnostics_.status = "failed"; diagnostics_.activeFrames = 0; stopped_ = true; }
    ::corevideo::core::nativeLogf("[program-buffer-failure] stage=%s prepare_device_hr=0x%08lx shell_export_device_hr=0x%08lx\n",
        stage, static_cast<unsigned long>(prepareDevice_ ? prepareDevice_->GetDeviceRemovedReason() : E_FAIL),
        static_cast<unsigned long>(shellBranch_.device ? shellBranch_.device->GetDeviceRemovedReason() : E_FAIL));
    notifyChanged();
  }
  void notifyChanged() {
    changed_.notify_all();
    if (wakeEvent_) ::SetEvent(wakeEvent_);
  }
  enum class State { Free, Writing, Submitted, Preparing, Ready, Delivering, Exporting };
  struct Slot {
    State state = State::Free;
    ComPtrLite<ID3D11Texture2D> producerTexture, prepareTexture;
    ComPtrLite<IDXGIKeyedMutex> producerMutex, prepareMutex;
    ComPtrLite<ID3D11ShaderResourceView> sourceView;
    // Immutable while either optional branch holds a read lease. Preparation
    // verifies the snapshot copy; each reader verifies completion before reuse.
    ComPtrLite<ID3D11Texture2D> snapshot, shellSnapshot, multiviewSnapshot;
    unsigned readers = 0;
    ProgramFrame frame;
    bool needsNv12 = false;
    int64_t productionSlot = 0;
    std::chrono::steady_clock::time_point submittedAt{};
    std::chrono::steady_clock::time_point preparingAt{};
    std::chrono::steady_clock::time_point readyAt{};
  };
  // Retained by every published frame, so replacing the buffer cannot destroy
  // the shell's exported resource while its last delivered metadata is leased.
  struct Output {
    ComPtrLite<ID3D11Texture2D> texture;
    ComPtrLite<IDXGIKeyedMutex> mutex;
    std::string handle;
  };
  struct ExportBranch {
    ComPtrLite<ID3D11Device> device;
    ComPtrLite<ID3D11DeviceContext> context;
    ComPtrLite<ID3D11Query> complete;
    Slot* pending = nullptr;
    std::shared_ptr<const ProgramFrame> frame;
    bool busy = false, failed = false;
    uint64_t refused = 0, outputBusy = 0, unconsumed = 0, completed = 0;
    int64_t maxApiNs = 0, maxAcquireNs = 0, maxCopyNs = 0, maxFlushNs = 0, maxQueryNs = 0;
    std::chrono::steady_clock::time_point lastTrace{};
  };
  // Installed only before submission by hardware fault tests.
  std::function<void(bool)> beforeExport_;
  static int64_t now100ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count() / 100;
  }
  static bool makeDevice(IDXGIAdapter* adapter, ComPtrLite<ID3D11Device>& device,
      ComPtrLite<ID3D11DeviceContext>& context) {
    return SUCCEEDED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        device.put(), nullptr, context.put()));
  }
  static bool share(ID3D11Texture2D* texture, HANDLE& handle, ComPtrLite<IDXGIKeyedMutex>& mutex) {
    ComPtrLite<IDXGIResource> resource;
    return SUCCEEDED(texture->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(resource.put()))) &&
        SUCCEEDED(resource->GetSharedHandle(&handle)) && handle &&
        SUCCEEDED(texture->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(mutex.put())));
  }
  bool initialize(ID3D11Device* producer) {
    ComPtrLite<IDXGIDevice> dxgi;
    ComPtrLite<IDXGIAdapter> adapter;
    if (FAILED(producer->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(dxgi.put()))) ||
        FAILED(dxgi->GetAdapter(adapter.put())) || !makeDevice(adapter.get(), prepareDevice_, prepareContext_) ||
        !makeDevice(adapter.get(), shellBranch_.device, shellBranch_.context) ||
        !makeDevice(adapter.get(), multiviewBranch_.device, multiviewBranch_.context)) return false;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width_; desc.Height = height_; desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    for (int i = 0; i < diagnostics_.capacity; ++i) {
      auto slot = std::make_unique<Slot>();
      HANDLE handle = nullptr;
      if (FAILED(producer->CreateTexture2D(&desc, nullptr, slot->producerTexture.put())) ||
          !share(slot->producerTexture.get(), handle, slot->producerMutex) ||
          FAILED(prepareDevice_->OpenSharedResource(handle, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(slot->prepareTexture.put()))) ||
          FAILED(slot->prepareTexture->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(slot->prepareMutex.put()))) ||
          FAILED(prepareDevice_->CreateShaderResourceView(slot->prepareTexture.get(), nullptr, slot->sourceView.put()))) return false;
      auto snapshotDesc = desc;
      snapshotDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
      ComPtrLite<IDXGIResource> resource;
      if (FAILED(prepareDevice_->CreateTexture2D(&snapshotDesc, nullptr, slot->snapshot.put())) ||
          FAILED(slot->snapshot->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(resource.put()))) ||
          FAILED(resource->GetSharedHandle(&handle)) ||
          FAILED(shellBranch_.device->OpenSharedResource(handle, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(slot->shellSnapshot.put()))) ||
          FAILED(multiviewBranch_.device->OpenSharedResource(handle, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(slot->multiviewSnapshot.put())))) return false;
      slots_.push_back(std::move(slot));
    }
    output_ = std::make_shared<Output>();
    HANDLE handle = nullptr;
    if (FAILED(shellBranch_.device->CreateTexture2D(&desc, nullptr, output_->texture.put())) ||
        !share(output_->texture.get(), handle, output_->mutex)) return false;
    char encoded[32]; std::snprintf(encoded, sizeof(encoded), "0x%llX", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(handle)));
    output_->handle = encoded;
    multiviewOutput_ = std::make_shared<Output>();
    handle = nullptr;
    if (FAILED(multiviewBranch_.device->CreateTexture2D(&desc, nullptr, multiviewOutput_->texture.put())) ||
        !share(multiviewOutput_->texture.get(), handle, multiviewOutput_->mutex)) return false;
    std::snprintf(encoded, sizeof(encoded), "0x%llX", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(handle)));
    multiviewOutput_->handle = encoded;
    if (!initializeNv12()) return false;
    D3D11_QUERY_DESC complete{D3D11_QUERY_EVENT, 0};
    if (FAILED(prepareDevice_->CreateQuery(&complete, prepareComplete_.put()))) return false;
    if (FAILED(shellBranch_.device->CreateQuery(&complete, shellBranch_.complete.put())) ||
        FAILED(multiviewBranch_.device->CreateQuery(&complete, multiviewBranch_.complete.put()))) return false;
    initialized_ = true;
    return true;
  }
  bool initializeNv12() {
    std::string error;
    const auto vs = compileShader(kCompositorVertexShader, "main", "vs_5_0", error);
    const auto y = compileShader(kVcamNv12YPixelShader, "main", "ps_5_0", error);
    const auto uv = compileShader(kVcamNv12UvPixelShader, "main", "ps_5_0", error);
    if (!vs || !y || !uv || FAILED(prepareDevice_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, vs_.put())) ||
        FAILED(prepareDevice_->CreatePixelShader(y->GetBufferPointer(), y->GetBufferSize(), nullptr, psY_.put())) ||
        FAILED(prepareDevice_->CreatePixelShader(uv->GetBufferPointer(), uv->GetBufferSize(), nullptr, psUv_.put()))) return false;
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(prepareDevice_->CreateSamplerState(&sampler, sampler_.put()))) return false;
    auto make = [&](int w, int h, DXGI_FORMAT format, bool staging, ComPtrLite<ID3D11Texture2D>& texture) {
      D3D11_TEXTURE2D_DESC desc{};
      desc.Width = w; desc.Height = h; desc.MipLevels = desc.ArraySize = 1;
      desc.Format = format; desc.SampleDesc.Count = 1;
      desc.Usage = staging ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT;
      desc.BindFlags = staging ? 0 : D3D11_BIND_RENDER_TARGET;
      desc.CPUAccessFlags = staging ? D3D11_CPU_ACCESS_READ : 0;
      return SUCCEEDED(prepareDevice_->CreateTexture2D(&desc, nullptr, texture.put()));
    };
    // Same 1080p GPU NV12 tap as the existing output path; Program export keeps
    // its native resolution, and GPU sampling scales only the output tap.
    return make(1920, 1080, DXGI_FORMAT_R8_UNORM, false, y_) &&
        make(960, 540, DXGI_FORMAT_R8G8_UNORM, false, uv_) &&
        make(1920, 1080, DXGI_FORMAT_R8_UNORM, true, yRead_) &&
        make(960, 540, DXGI_FORMAT_R8G8_UNORM, true, uvRead_) &&
        SUCCEEDED(prepareDevice_->CreateRenderTargetView(y_.get(), nullptr, yView_.put())) &&
        SUCCEEDED(prepareDevice_->CreateRenderTargetView(uv_.get(), nullptr, uvView_.put()));
  }
  bool prepareNv12(Slot& slot) {
    auto* context = prepareContext_.get();
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vs_.get(), nullptr, 0);
    ID3D11ShaderResourceView* source[] = {slot.sourceView.get()};
    ID3D11SamplerState* sampler[] = {sampler_.get()};
    context->PSSetShaderResources(0, 1, source); context->PSSetSamplers(0, 1, sampler);
    auto draw = [&](ID3D11RenderTargetView* target, ID3D11PixelShader* shader, float width, float height) {
      context->OMSetRenderTargets(1, &target, nullptr);
      D3D11_VIEWPORT viewport{0, 0, width, height, 0, 1};
      context->RSSetViewports(1, &viewport); context->PSSetShader(shader, nullptr, 0); context->Draw(3, 0);
    };
    draw(yView_.get(), psY_.get(), 1920, 1080); draw(uvView_.get(), psUv_.get(), 960, 540);
    ID3D11ShaderResourceView* none[] = {nullptr}; context->PSSetShaderResources(0, 1, none);
    context->CopyResource(yRead_.get(), y_.get()); context->CopyResource(uvRead_.get(), uv_.get());
    context->Flush(); // Submit once; the nonblocking Map polls never flush implicitly.
    static core::FrameAllocationFailures allocationFailures("program-buffer-nv12");
    auto pixels = core::tryMakeFrameBuffer(1920u * 1080u * 3u / 2u);
    if (!pixels) {
      // #728: no NV12 tap for this frame; the same outcome as a failed readback.
      allocationFailures.note(1920u * 1080u * 3u / 2u);
      return false;
    }
    auto copy = [&](ID3D11Texture2D* texture, int rows, size_t offset) {
      D3D11_MAPPED_SUBRESOURCE mapped{};
      const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      for (;;) {
        // No application mutex spans a D3D call. GPU backpressure must not
        // strand shutdown in an uninterruptible staging readback.
        const auto result = context->Map(texture, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (SUCCEEDED(result)) break;
        if (result != DXGI_ERROR_WAS_STILL_DRAWING || std::chrono::steady_clock::now() >= limit) {
          ::corevideo::core::nativeLogf("[program-buffer-failure] stage=nv12-map rows=%d hr=0x%08lx device_hr=0x%08lx timeout=%d\n",
              rows, static_cast<unsigned long>(result),
              static_cast<unsigned long>(prepareDevice_->GetDeviceRemovedReason()),
              result == DXGI_ERROR_WAS_STILL_DRAWING);
          return false;
        }
        std::unique_lock<std::mutex> wait(mutex_);
        if (changed_.wait_for(wait, std::chrono::microseconds(100), [&] { return stopped_; })) return false;
      }
      for (int row = 0; row < rows; ++row) std::memcpy(pixels->data() + offset + static_cast<size_t>(row) * 1920,
          static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(row) * mapped.RowPitch, 1920);
      context->Unmap(texture, 0); return true;
    };
    if (!copy(yRead_.get(), 1080, 0) || !copy(uvRead_.get(), 540, 1920u * 1080u)) return false;
    slot.frame.programNv12Shared = std::move(pixels);
    slot.frame.programNv12Width = 1920; slot.frame.programNv12Height = 1080;
    return true;
  }
  bool completePreparation() {
    // AcquireSync on the retained source orders the producer's writes before
    // this context. Verify completion here, while the whole buffer lead is
    // available, rather than copying the same immutable image again at playout.
    prepareContext_->End(prepareComplete_.get());
    prepareContext_->Flush();
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    for (;;) {
      BOOL complete = FALSE;
      // Allow GetData to flush this preparation-only context. On the NVIDIA
      // keyed handoff path an explicit Flush alone did not make a DONOTFLUSH
      // event observable; hardware tests stalled until the query could flush.
      const auto result = prepareContext_->GetData(prepareComplete_.get(), &complete,
          sizeof(complete), 0);
      if (result == S_OK && complete) return true;
      if (FAILED(result) || std::chrono::steady_clock::now() >= limit) {
        ::corevideo::core::nativeLogf("[program-buffer-failure] stage=prepare-query hr=0x%08lx device_hr=0x%08lx timeout=%d\n",
            static_cast<unsigned long>(result), static_cast<unsigned long>(prepareDevice_->GetDeviceRemovedReason()),
            !FAILED(result));
        return false;
      }
      std::unique_lock<std::mutex> wait(mutex_);
      if (changed_.wait_for(wait, std::chrono::microseconds(100), [&] { return stopped_; })) return false;
    }
  }
  void prepareLoop() {
    for (;;) {
      Slot* slot;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [&] { return stopped_ || !submitted_.empty(); });
        if (stopped_) return;
        slot = submitted_.front();
        // A frame whose deadline already passed cannot be published. Spending
        // the NV12 readback on it is what kept the next, still-viable frame
        // behind after a single miss: retirement used to happen only once this
        // slot reached Ready. Hand the key back and move on.
        if (timeline_->isExpired(slot->productionSlot)) {
          lock.unlock();
          const bool held = slot->prepareMutex->AcquireSync(1, 0) == S_OK;
          if (held) slot->prepareMutex->ReleaseSync(0);
          lock.lock();
          if (stopped_) return;
          if (!held) {
            ++diagnostics_.gpuNotReady;
            changed_.wait_for(lock, std::chrono::milliseconds(1), [&] { return stopped_; });
            if (stopped_) return;
            continue;
          }
          submitted_.pop_front();
          for (auto it = delivery_.begin(); it != delivery_.end(); ++it) {
            if (*it == slot) { delivery_.erase(it); break; }
          }
          slot->state = State::Free;
          ++diagnostics_.overflows;
          diagnostics_.occupancy = static_cast<int>(delivery_.size());
          const auto now = std::chrono::steady_clock::now();
          if (lastExpiredSkipLog_.time_since_epoch().count() == 0 || now - lastExpiredSkipLog_ >= std::chrono::seconds(1)) {
            lastExpiredSkipLog_ = now;
            ::corevideo::core::nativeLogf("[program-buffer-miss] stage=expired-before-prepare slot=%lld\n",
                static_cast<long long>(slot->productionSlot));
          }
          notifyChanged();
          continue;
        }
        slot->preparingAt = std::chrono::steady_clock::now();
        slot->state = State::Preparing;
        diagnostics_.lastQueueWaitMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - slot->submittedAt).count();
        diagnostics_.maxQueueWaitMs = (std::max)(diagnostics_.maxQueueWaitMs, diagnostics_.lastQueueWaitMs);
      }
      if (slot->prepareMutex->AcquireSync(1, 0) != S_OK) {
        std::unique_lock<std::mutex> lock(mutex_); ++diagnostics_.gpuNotReady;
        changed_.wait_for(lock, std::chrono::milliseconds(1), [&] { return stopped_; });
        if (stopped_) return;
        continue;
      }
      const auto prepareBegin = std::chrono::steady_clock::now();
      const bool pixelsReady = !slot->needsNv12 || prepareNv12(*slot);
      prepareContext_->CopyResource(slot->snapshot.get(), slot->prepareTexture.get());
      // Return the input key after all preparation reads. Ready still requires
      // actual GPU completion, and the slot cannot be rewritten until both
      // optional snapshot readers have completed.
      const bool released = slot->prepareMutex->ReleaseSync(0) == S_OK;
      const bool ready = pixelsReady && released && completePreparation();
      {
        std::lock_guard<std::mutex> lock(mutex_);
        diagnostics_.lastPreparationMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - prepareBegin).count();
        diagnostics_.maxPreparationMs = (std::max)(diagnostics_.maxPreparationMs, diagnostics_.lastPreparationMs);
        ++diagnostics_.prepared;
        submitted_.pop_front();
        slot->readyAt = std::chrono::steady_clock::now();
        if (ready) trace(slot->frame, core::DeliveryStage::ProgramGpuReady);
        else trace(slot->frame, core::DeliveryStage::ProgramMiss, core::DeliveryReason::Failed);
        slot->state = State::Ready;
        // Delivery also retires expired ready slots, returning key 0 exactly once.
        if (!ready) { diagnostics_.status = "failed"; diagnostics_.activeFrames = 0; stopped_ = true; }
      }
      prepareContext_->Flush();
      notifyChanged();
      if (!ready) return;
    }
  }
  // condition_variable::wait_until rounds to milliseconds and then sleeps, which
  // the render pacer already measured as a 1-2ms overshoot at timeBeginPeriod(1)
  // and several milliseconds under GPU load. Wake early when the slot becomes
  // Ready, and wake at the deadline from a high-resolution timer.
  template <typename Pred>
  void waitUntilPrecise(std::unique_lock<std::mutex>& lock,
                        std::chrono::steady_clock::time_point deadline,
                        HANDLE timer, Pred pred) {
    while (!pred()) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) return;
      if (!timer || !wakeEvent_) {
        changed_.wait_until(lock, deadline, pred);
        return;
      }
      const auto remain100ns = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now).count() / 100;
      LARGE_INTEGER due;
      due.QuadPart = remain100ns > 0 ? -remain100ns : -1;
      if (!::SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
        changed_.wait_until(lock, deadline, pred);
        return;
      }
      lock.unlock();
      HANDLE handles[2] = {timer, wakeEvent_};
      ::WaitForMultipleObjects(2, handles, FALSE, 50);
      lock.lock();
    }
  }
  void deliveryLoop() {
    const HANDLE timer = ::CreateWaitableTimerExW(
        nullptr, nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION | CREATE_WAITABLE_TIMER_MANUAL_RESET,
        TIMER_ALL_ACCESS);
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait(lock, [&] { return stopped_ || !delivery_.empty(); });
    if (stopped_) {
      if (timer) ::CloseHandle(timer);
      return;
    }
    diagnostics_.status = "running";
    while (!stopped_) {
      while (!delivery_.empty() && delivery_.front()->state == State::Ready && timeline_->isExpired(delivery_.front()->productionSlot)) {
        auto* expired = delivery_.front();
        trace(expired->frame, core::DeliveryStage::ProgramMiss, core::DeliveryReason::Failed);
        expired->state = State::Free; delivery_.pop_front(); ++diagnostics_.overflows;
      }
      const auto targetSlot = timeline_->nextSlot();
      const auto deadline = std::chrono::steady_clock::time_point(std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::nanoseconds(timeline_->nextDeadlineNs())));
      waitUntilPrecise(lock, deadline, timer, [&] {
        return stopped_ || (!delivery_.empty() && delivery_.front()->state == State::Ready && delivery_.front()->productionSlot == targetSlot);
      });
      if (stopped_) break;
      if (delivery_.empty() || delivery_.front()->state != State::Ready || delivery_.front()->productionSlot != targetSlot) {
        if (const auto due = timeline_->takeDue(now100ns() * 100)) {
          diagnostics_.underruns += due->skippedSlots + 1;
          core::DeliveryTraceEvent missed;
          missed.stage = core::DeliveryStage::ProgramMiss;
          missed.reason = core::DeliveryReason::Unavailable;
          missed.sourceEpoch = traceEpoch_;
          missed.programSequence = firstFrameNumber_ + targetSlot;
          core::recordDeliveryTrace(missed);
          const auto* front = delivery_.empty() ? nullptr : delivery_.front();
          const auto observed = std::chrono::steady_clock::now();
          const auto ageNs = [&](std::chrono::steady_clock::time_point since) {
            return static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(observed - since).count());
          };
          ::corevideo::core::nativeLogf("[program-buffer-miss] stage=source target=%lld front=%lld state=%d skipped=%lld late_ns=%lld submitted_age_ns=%lld preparing_age_ns=%lld ready_age_ns=%lld\n",
              static_cast<long long>(targetSlot), front ? static_cast<long long>(front->productionSlot) : -1LL,
              front ? static_cast<int>(front->state) : -1, static_cast<long long>(due->skippedSlots),
              static_cast<long long>(now100ns() * 100 - due->deadlineNs),
              front ? ageNs(front->submittedAt) : -1LL,
              front && front->state == State::Preparing ? ageNs(front->preparingAt) : -1LL,
              front && front->state == State::Ready ? ageNs(front->readyAt) : -1LL);
        }
        continue;
      }
      // No D3D calls on the delivery clock: Ready proves preparation and the
      // immutable snapshot completed. Optional publication has separate owners.
      auto* slot = delivery_.front(); slot->state = State::Delivering;
      if (!stopped_) waitUntilPrecise(lock, deadline, timer, [&] { return stopped_; });
      if (stopped_) break;
      const auto due = timeline_->takeDue(now100ns() * 100);
      const bool current = due && due->slot == slot->productionSlot;
      if (due) diagnostics_.underruns += due->skippedSlots;
      const bool deliveryExpired = now100ns() * 100 >= timeline_->deadlineNs(slot->productionSlot + 1);
      if (!current || deliveryExpired) {
        trace(slot->frame, core::DeliveryStage::ProgramMiss, core::DeliveryReason::Failed);
        if (current || due) ++diagnostics_.underruns;
        ::corevideo::core::nativeLogf("[program-buffer-miss] stage=publish slot=%lld current=%d expired=%d optional_export_on_delivery=0\n",
            static_cast<long long>(slot->productionSlot), current, deliveryExpired);
        delivery_.pop_front(); slot->state = State::Free; ++diagnostics_.overflows;
        diagnostics_.occupancy = static_cast<int>(delivery_.size());
        continue;
      }
      slot->frame.deliverySequence = ++diagnostics_.delivered;
      slot->frame.deliveredAt100ns = now100ns();
      slot->frame.timelineTimestamp100ns = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline.time_since_epoch()).count() / 100;
      slot->frame.gpuOwner = output_;
      slot->frame.sharedTexture = {}; // Optional snapshots publish independently.
      auto published = std::make_shared<const ProgramFrame>(std::move(slot->frame));
      auto offer = [&](ExportBranch& branch) {
        if (branch.busy || branch.failed) { ++diagnostics_.displayBusy; ++branch.refused; return; }
        branch.busy = true; branch.pending = slot; branch.frame = published; ++slot->readers;
      };
      offer(shellBranch_); offer(multiviewBranch_);
      if (delivered_.size() >= static_cast<size_t>(depth_ + 2)) { delivered_.pop_front(); ++diagnostics_.overflows; }
      delivered_.push_back(*published);
      trace(*published, core::DeliveryStage::ProgramDelivered);
      delivery_.pop_front(); slot->state = slot->readers ? State::Exporting : State::Free;
      diagnostics_.occupancy = static_cast<int>(delivery_.size());
      notifyChanged();
      lock.unlock();
      if (deliveredCallback_) deliveredCallback_(*published);
      lock.lock();
    }
    if (timer) ::CloseHandle(timer);
  }
  void exportLoop(ExportBranch& branch, bool shell) {
    try {
      for (;;) {
        Slot* slot;
        std::shared_ptr<const ProgramFrame> frame;
        {
          std::unique_lock<std::mutex> lock(mutex_);
          changed_.wait(lock, [&] { return stopped_ || branch.pending; });
          if (stopped_) return;
          slot = std::exchange(branch.pending, nullptr); frame = std::move(branch.frame);
        }
        const auto apiBegin = std::chrono::steady_clock::now();
        if (beforeExport_) beforeExport_(shell);
        auto& output = shell ? output_ : multiviewOutput_;
        const auto acquireBegin = std::chrono::steady_clock::now();
        bool unconsumed = false;
        bool held = output->mutex->AcquireSync(0, 0) == S_OK;
        if (!held) { held = output->mutex->AcquireSync(1, 0) == S_OK; unconsumed = held; }
        const auto acquireEnd = std::chrono::steady_clock::now();
        int64_t copyNs = 0, flushNs = 0, queryNs = 0;
        bool ready = !held;
        const auto started = std::chrono::steady_clock::now();
        if (held) {
          branch.context->CopyResource(output->texture.get(), shell ? slot->shellSnapshot.get() : slot->multiviewSnapshot.get());
          const auto copyEnd = std::chrono::steady_clock::now();
          branch.context->End(branch.complete.get()); branch.context->Flush();
          const auto flushEnd = std::chrono::steady_clock::now();
          copyNs = std::chrono::duration_cast<std::chrono::nanoseconds>(copyEnd - started).count();
          flushNs = std::chrono::duration_cast<std::chrono::nanoseconds>(flushEnd - copyEnd).count();
          const auto limit = started + std::chrono::seconds(2);
          for (;;) {
            BOOL done = FALSE;
            const auto hr = branch.context->GetData(branch.complete.get(), &done, sizeof(done), 0);
            if (hr == S_OK && done) { ready = true; break; }
            if (FAILED(hr) || std::chrono::steady_clock::now() >= limit) break;
            std::this_thread::sleep_for(std::chrono::microseconds(100));
          }
          queryNs = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - flushEnd).count();
        }
        std::shared_ptr<ProgramFrame> snapshot;
        if (held && ready) {
          // Optional allocation/copy cannot hold the delivery clock's mutex.
          snapshot = std::make_shared<ProgramFrame>(*frame);
          snapshot->gpuOwner = output;
          snapshot->sharedTexture = {output->handle, 0, width_, height_, "B8G8R8A8_UNORM", frame->frameNumber};
        }
        {
          std::lock_guard<std::mutex> lock(mutex_);
          if (unconsumed) { ++diagnostics_.displayUnconsumed; ++branch.unconsumed; }
          if (!held) { ++diagnostics_.displayBusy; ++branch.outputBusy; }
          const auto ended = std::chrono::steady_clock::now();
          branch.maxApiNs = (std::max)(branch.maxApiNs, static_cast<int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(ended - apiBegin).count()));
          branch.maxAcquireNs = (std::max)(branch.maxAcquireNs, static_cast<int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(acquireEnd - acquireBegin).count()));
          branch.maxCopyNs = (std::max)(branch.maxCopyNs, copyNs);
          branch.maxFlushNs = (std::max)(branch.maxFlushNs, flushNs);
          branch.maxQueryNs = (std::max)(branch.maxQueryNs, queryNs);
          if (held && ready) ++branch.completed;
          if (branch.lastTrace.time_since_epoch().count() == 0 || ended - branch.lastTrace >= std::chrono::seconds(1)) {
            ::corevideo::core::nativeVerboseLogf("[program-buffer-export] branch=%s frame=%lld generation=%llu completed=%llu refused=%llu busy=%llu unconsumed=%llu max_api_ns=%lld max_acquire_ns=%lld max_copy_ns=%lld max_end_flush_ns=%lld max_query_wait_ns=%lld gpu_read_completed=%d display_presentation_verified=0\n",
                shell ? "shell" : "multiview", static_cast<long long>(frame->frameNumber),
                static_cast<unsigned long long>(diagnostics_.generation), static_cast<unsigned long long>(branch.completed),
                static_cast<unsigned long long>(branch.refused), static_cast<unsigned long long>(branch.outputBusy),
                static_cast<unsigned long long>(branch.unconsumed), static_cast<long long>(branch.maxApiNs), static_cast<long long>(branch.maxAcquireNs),
                static_cast<long long>(branch.maxCopyNs), static_cast<long long>(branch.maxFlushNs),
                static_cast<long long>(branch.maxQueryNs), held && ready);
            branch.lastTrace = ended;
          }
          if (held && ready) {
            // Advance metadata before exposing the completed pixels (key 1).
            if (shell) latest_ = std::move(snapshot);
            else multiviewLatest_ = std::move(snapshot);
          }
          if (ready) {
            if (--slot->readers == 0) slot->state = State::Free;
            branch.busy = false;
          } else {
            // Unknown GPU completion quarantines this lease; never reuse it.
            branch.failed = true;
            ::corevideo::core::nativeLogf("[program-buffer-export-failure] branch=%s frame=%lld source_quarantined=1\n",
                shell ? "shell" : "multiview", static_cast<long long>(frame->frameNumber));
          }
        }
        if (held) output->mutex->ReleaseSync(ready ? 1 : 0);
        notifyChanged();
        if (!ready) return;
      }
    } catch (...) {
      std::lock_guard<std::mutex> lock(mutex_);
      branch.failed = true; // An uncertain read keeps its source lease quarantined.
      ::corevideo::core::nativeLogf("[program-buffer-export-failure] branch=%s exception=1 source_quarantined=1\n", shell ? "shell" : "multiview");
    }
  }
  int width_, height_, depth_;
  bool initialized_ = false, stopped_ = false;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  ProgramBufferDiagnostics diagnostics_;
  std::vector<std::unique_ptr<Slot>> slots_;
  std::deque<Slot*> submitted_, delivery_;
  std::deque<ProgramFrame> delivered_;
  std::unique_ptr<core::ProgramPlayoutTimeline> timeline_;
  int64_t lastProducedSlot_ = -1;
  int64_t firstFrameNumber_ = 0;
  std::shared_ptr<const ProgramFrame> latest_, multiviewLatest_;
  ExportBranch shellBranch_, multiviewBranch_;
  std::shared_ptr<Output> output_;
  std::shared_ptr<Output> multiviewOutput_;
  ComPtrLite<ID3D11Query> prepareComplete_;
  std::function<void(const ProgramFrame&)> deliveredCallback_;
  std::thread prepareThread_, deliveryThread_, shellThread_, multiviewThread_;
  HANDLE wakeEvent_ = nullptr;
  std::chrono::steady_clock::time_point lastExpiredSkipLog_{};
  ComPtrLite<ID3D11Device> prepareDevice_;
  ComPtrLite<ID3D11DeviceContext> prepareContext_;
  ComPtrLite<ID3D11VertexShader> vs_;
  ComPtrLite<ID3D11PixelShader> psY_, psUv_;
  ComPtrLite<ID3D11SamplerState> sampler_;
  ComPtrLite<ID3D11Texture2D> y_, uv_, yRead_, uvRead_;
  ComPtrLite<ID3D11RenderTargetView> yView_, uvView_;
};
}  // namespace corevideo::modules
