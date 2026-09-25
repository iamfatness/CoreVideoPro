#pragma once

#include "core/BoundedAsyncLog.h"

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

namespace corevideo::modules {

// Each submitted texture has one owner at a time: render (key 0), preparation
// (key 1), delivery (key 2), then render again. Preparation and delivery use
// distinct devices/contexts; neither calls into the render immediate context.
class D3DProgramBuffer {
  friend struct D3DProgramBufferTestAccess;
 public:
  D3DProgramBuffer(ID3D11Device* producer, int width, int height, int depth, uint64_t generation,
      std::function<void(const ProgramFrame&)> delivered)
      : width_(width), height_(height), depth_(depth == 2 ? 2 : 3), deliveredCallback_(std::move(delivered)) {
    diagnostics_.requestedFrames = depth_;
    diagnostics_.generation = generation;
    diagnostics_.capacity = depth_ + 3;
    if (!initialize(producer)) { fail("initialize"); return; }
    diagnostics_.activeFrames = depth_;
    diagnostics_.status = "priming";
    try {
      prepareThread_ = std::thread([this] { try { prepareLoop(); } catch (...) { fail("prepare-worker"); } });
      deliveryThread_ = std::thread([this] { try { deliveryLoop(); } catch (...) { fail("delivery-worker"); } });
    } catch (...) {
      fail("start-workers");
      if (prepareThread_.joinable()) prepareThread_.join();
      initialized_ = false;
    }
  }
  ~D3DProgramBuffer() {
    { std::lock_guard<std::mutex> lock(mutex_); stopped_ = true; }
    changed_.notify_all();
    if (prepareThread_.joinable()) prepareThread_.join();
    if (deliveryThread_.joinable()) deliveryThread_.join();
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
      slot->frame = std::move(frame);
      slot->needsNv12 = nv12;
      slot->submittedAt = std::chrono::steady_clock::now();
      slot->state = State::Submitted;
      submitted_.push_back(slot);
      delivery_.push_back(slot);
      ++diagnostics_.produced;
      diagnostics_.occupancy = static_cast<int>(delivery_.size());
    }
    changed_.notify_all();
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
    if (!latest_) return false;
    texture = {multiviewOutput_->handle, 0, width_, height_, "B8G8R8A8_UNORM", latest_->frameNumber};
    owner = multiviewOutput_; return true;
  }
  ProgramBufferDiagnostics diagnostics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return diagnostics_;
  }

 private:
  void fail(const char* stage) {
    { std::lock_guard<std::mutex> lock(mutex_); diagnostics_.status = "failed"; diagnostics_.activeFrames = 0; stopped_ = true; }
    ::corevideo::core::nativeLogf("[program-buffer-failure] stage=%s prepare_device_hr=0x%08lx delivery_device_hr=0x%08lx\n",
        stage, static_cast<unsigned long>(prepareDevice_ ? prepareDevice_->GetDeviceRemovedReason() : E_FAIL),
        static_cast<unsigned long>(deliveryDevice_ ? deliveryDevice_->GetDeviceRemovedReason() : E_FAIL));
    changed_.notify_all();
  }
  enum class State { Free, Writing, Submitted, Preparing, Ready, Delivering };
  struct Slot {
    State state = State::Free;
    ComPtrLite<ID3D11Texture2D> producerTexture, prepareTexture, deliveryTexture;
    ComPtrLite<IDXGIKeyedMutex> producerMutex, prepareMutex, deliveryMutex;
    ComPtrLite<ID3D11ShaderResourceView> sourceView;
    ProgramFrame frame;
    bool needsNv12 = false;
    int64_t productionSlot = 0;
    std::chrono::steady_clock::time_point submittedAt{};
    std::chrono::steady_clock::time_point readyAt{};
  };
  // Retained by every published frame, so replacing the buffer cannot destroy
  // the shell's exported resource while its last delivered metadata is leased.
  struct Output {
    ComPtrLite<ID3D11Texture2D> texture;
    ComPtrLite<IDXGIKeyedMutex> mutex;
    std::string handle;
  };
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
        !makeDevice(adapter.get(), deliveryDevice_, deliveryContext_)) return false;
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
          FAILED(deliveryDevice_->OpenSharedResource(handle, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(slot->deliveryTexture.put()))) ||
          FAILED(slot->prepareTexture->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(slot->prepareMutex.put()))) ||
          FAILED(slot->deliveryTexture->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(slot->deliveryMutex.put()))) ||
          FAILED(prepareDevice_->CreateShaderResourceView(slot->prepareTexture.get(), nullptr, slot->sourceView.put()))) return false;
      slots_.push_back(std::move(slot));
    }
    output_ = std::make_shared<Output>();
    HANDLE handle = nullptr;
    if (FAILED(deliveryDevice_->CreateTexture2D(&desc, nullptr, output_->texture.put())) ||
        !share(output_->texture.get(), handle, output_->mutex)) return false;
    char encoded[32]; std::snprintf(encoded, sizeof(encoded), "0x%llX", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(handle)));
    output_->handle = encoded;
    multiviewOutput_ = std::make_shared<Output>();
    handle = nullptr;
    if (FAILED(deliveryDevice_->CreateTexture2D(&desc, nullptr, multiviewOutput_->texture.put())) ||
        !share(multiviewOutput_->texture.get(), handle, multiviewOutput_->mutex)) return false;
    std::snprintf(encoded, sizeof(encoded), "0x%llX", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(handle)));
    multiviewOutput_->handle = encoded;
    if (!initializeNv12()) return false;
    D3D11_QUERY_DESC complete{D3D11_QUERY_EVENT, 0};
    if (FAILED(prepareDevice_->CreateQuery(&complete, prepareComplete_.put()))) return false;
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
    auto pixels = std::make_shared<std::vector<uint8_t>>(1920u * 1080u * 3u / 2u);
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
        slot = submitted_.front(); slot->state = State::Preparing;
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
      // Submit the keyed handoff before waiting for its GPU event. State stays
      // Preparing, so delivery cannot acquire key 2 until the query completes;
      // render cannot reacquire key 0 until delivery retires the slot.
      const bool released = slot->prepareMutex->ReleaseSync(2) == S_OK;
      const bool ready = pixelsReady && released && completePreparation();
      {
        std::lock_guard<std::mutex> lock(mutex_);
        diagnostics_.lastPreparationMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - prepareBegin).count();
        diagnostics_.maxPreparationMs = (std::max)(diagnostics_.maxPreparationMs, diagnostics_.lastPreparationMs);
        ++diagnostics_.prepared;
        submitted_.pop_front();
        slot->readyAt = std::chrono::steady_clock::now();
        slot->state = State::Ready;
        // Delivery also retires expired ready slots, returning key 0 exactly once.
        if (!ready) { diagnostics_.status = "failed"; diagnostics_.activeFrames = 0; stopped_ = true; }
      }
      prepareContext_->Flush();
      changed_.notify_all();
      if (!ready) return;
    }
  }
  void deliveryLoop() {
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait(lock, [&] { return stopped_ || !delivery_.empty(); });
    if (stopped_) return;
    diagnostics_.status = "running";
    while (!stopped_) {
      while (!delivery_.empty() && delivery_.front()->state == State::Ready && timeline_->isExpired(delivery_.front()->productionSlot)) {
        auto* expired = delivery_.front();
        if (expired->deliveryMutex->AcquireSync(2, 0) != S_OK) break;
        expired->deliveryMutex->ReleaseSync(0); expired->state = State::Free; delivery_.pop_front(); ++diagnostics_.overflows;
      }
      const auto targetSlot = timeline_->nextSlot();
      const auto deadline = std::chrono::steady_clock::time_point(std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::nanoseconds(timeline_->nextDeadlineNs())));
      changed_.wait_until(lock, deadline, [&] {
        return stopped_ || (!delivery_.empty() && delivery_.front()->state == State::Ready && delivery_.front()->productionSlot == targetSlot);
      });
      if (stopped_) break;
      if (delivery_.empty() || delivery_.front()->state != State::Ready || delivery_.front()->productionSlot != targetSlot) {
        if (const auto due = timeline_->takeDue(now100ns() * 100)) {
          diagnostics_.underruns += due->skippedSlots + 1;
          ::corevideo::core::nativeLogf("[program-buffer-miss] stage=source target=%lld front=%lld state=%d skipped=%lld late_ns=%lld\n",
              static_cast<long long>(targetSlot), delivery_.empty() ? -1LL : static_cast<long long>(delivery_.front()->productionSlot),
              delivery_.empty() ? -1 : static_cast<int>(delivery_.front()->state), static_cast<long long>(due->skippedSlots),
              static_cast<long long>(now100ns() * 100 - due->deadlineNs));
        }
        continue;
      }
      // Preparation already verified the retained image on the GPU. Own that
      // immutable slot until publication; do not introduce a second copy/query
      // between adjacent delivery deadlines. Stable monitor exports remain
      // readable throughout the lead.
      if (stopped_) break;
      auto* slot = delivery_.front(); slot->state = State::Delivering;
      const auto acquireBegin = std::chrono::steady_clock::now();
      lock.unlock();
      const bool acquired = slot->deliveryMutex->AcquireSync(2, 0) == S_OK;
      const bool gpuReady = acquired; // Ready state requires completed preparation.
      const auto completed = std::chrono::steady_clock::now();
      lock.lock();
      const auto acquireNs = std::chrono::duration_cast<std::chrono::nanoseconds>(completed - acquireBegin).count();
      const auto deliveryLeadNs = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - acquireBegin).count();
      const auto readyLeadNs = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - slot->readyAt).count();
      const auto beginAfterReadyNs = std::chrono::duration_cast<std::chrono::nanoseconds>(acquireBegin - slot->readyAt).count();
      const auto completionLateNs = std::chrono::duration_cast<std::chrono::nanoseconds>(completed - deadline).count();
      maximumAcquireNs_ = (std::max)(maximumAcquireNs_, static_cast<int64_t>(acquireNs));
      minimumDeliveryLeadNs_ = (std::min)(minimumDeliveryLeadNs_, static_cast<int64_t>(deliveryLeadNs));
      maximumCompletionLateNs_ = (std::max)(maximumCompletionLateNs_, static_cast<int64_t>(completionLateNs));
      if (completed > deadline) {
        ++diagnostics_.deadlineMisses;
        ::corevideo::core::nativeLogf("[program-buffer-miss] stage=delivery-acquire slot=%lld lead_ns=%lld ready_lead_ns=%lld begin_after_ready_ns=%lld acquire_ns=%lld late_ns=%lld\n",
             static_cast<long long>(slot->productionSlot), static_cast<long long>(deliveryLeadNs),
             static_cast<long long>(readyLeadNs), static_cast<long long>(beginAfterReadyNs),
             static_cast<long long>(acquireNs), static_cast<long long>(completionLateNs));
      }
      if (!stopped_) changed_.wait_until(lock, deadline, [&] { return stopped_; });
      const auto due = timeline_->takeDue(now100ns() * 100);
      const bool current = due && due->slot == slot->productionSlot;
      if (due) diagnostics_.underruns += due->skippedSlots;
      const bool publish = current && gpuReady && !stopped_;
      const int64_t expiresAtNs = timeline_->deadlineNs(slot->productionSlot + 1);
      lock.unlock();
      uint64_t unconsumed = 0, busy = 0;
      // Copy only at publication, never hold a display key during the lead or
      // readiness query. ReleaseSync orders the GPU copy for its consumer;
      // this is submission evidence, not measured display GPU completion.
      auto exportFrame = [&](Output& output) {
        bool owned = output.mutex->AcquireSync(0, 0) == S_OK;
        if (!owned) { owned = output.mutex->AcquireSync(1, 0) == S_OK; if (owned) ++unconsumed; }
        if (!owned) { ++busy; return false; }
        deliveryContext_->CopyResource(output.texture.get(), slot->deliveryTexture.get());
        deliveryContext_->Flush();
        return true; // Release both exports together after checking submission expiry.
      };
      const auto exportBegin = std::chrono::steady_clock::now();
      const bool shellCopied = publish && exportFrame(*output_);
      const bool multiviewCopied = publish && exportFrame(*multiviewOutput_);
      const auto exportEnd = std::chrono::steady_clock::now();
      const bool exportExpired = std::chrono::duration_cast<std::chrono::nanoseconds>(exportEnd.time_since_epoch()).count() >= expiresAtNs;
      if (shellCopied) output_->mutex->ReleaseSync(exportExpired ? 0 : 1);
      if (multiviewCopied) multiviewOutput_->mutex->ReleaseSync(exportExpired ? 0 : 1);
      if (acquired) slot->deliveryMutex->ReleaseSync(0);
      lock.lock();
      maximumExportSubmitNs_ = (std::max)(maximumExportSubmitNs_, static_cast<int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(exportEnd - exportBegin).count()));
      maximumExportSubmitLateNs_ = (std::max)(maximumExportSubmitLateNs_, static_cast<int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(exportEnd - deadline).count()));
      if (lastTimingLog_.time_since_epoch().count() == 0 || exportEnd - lastTimingLog_ >= std::chrono::seconds(2)) {
        ::corevideo::core::nativeVerboseLogf("[program-buffer-timing] depth=%d preparation=gpu-ready-before-delivery max_acquire_ns=%lld min_delivery_lead_ns=%lld max_completion_late_ns=%lld max_export_submit_ns=%lld max_export_submit_late_ns=%lld deadline_misses=%llu delivered=%llu retained_frame_gpu_ready_checked=1 export_gpu_completion_verified=0 display_presentation_verified=0\n",
            depth_, static_cast<long long>(maximumAcquireNs_), static_cast<long long>(minimumDeliveryLeadNs_),
            static_cast<long long>(maximumCompletionLateNs_), static_cast<long long>(maximumExportSubmitNs_),
            static_cast<long long>(maximumExportSubmitLateNs_), static_cast<unsigned long long>(diagnostics_.deadlineMisses),
            static_cast<unsigned long long>(diagnostics_.delivered));
        lastTimingLog_ = exportEnd;
      }
      diagnostics_.displayUnconsumed += unconsumed; diagnostics_.displayBusy += busy;
      if (stopped_) break;
      // A driver submission may block past subsequent slots. Consume those
      // deadlines and discard this packet instead of delivering an expired PTS.
      if (const auto missed = timeline_->takeDue(now100ns() * 100))
        diagnostics_.underruns += missed->skippedSlots + 1;
      const bool deliveryExpired = exportExpired || now100ns() * 100 >= expiresAtNs;
      if (!current || deliveryExpired) {
        ::corevideo::core::nativeLogf("[program-buffer-miss] stage=publish slot=%lld current=%d expired=%d export_ns=%lld late_ns=%lld\n",
            static_cast<long long>(slot->productionSlot), current, deliveryExpired,
            static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(exportEnd - exportBegin).count()),
            static_cast<long long>(now100ns() * 100 - timeline_->deadlineNs(slot->productionSlot)));
        if (current) ++diagnostics_.underruns;
        else if (due) ++diagnostics_.underruns; // The selected due slot had no delivered packet.
        if (shellCopied || multiviewCopied) latest_.reset(); // Export contents no longer prove the old snapshot.
        delivery_.pop_front(); slot->state = State::Free; ++diagnostics_.overflows;
        diagnostics_.occupancy = static_cast<int>(delivery_.size());
        continue;
      }
      if (!acquired || !gpuReady) {
        ++diagnostics_.underruns; ++diagnostics_.gpuNotReady;
        // A failed GPU query cannot prove this slot or either exported image.
        diagnostics_.status = "failed"; diagnostics_.activeFrames = 0; stopped_ = true; changed_.notify_all(); break;
      }
      slot->frame.deliverySequence = ++diagnostics_.delivered;
      slot->frame.deliveredAt100ns = now100ns();
      slot->frame.timelineTimestamp100ns = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline.time_since_epoch()).count() / 100;
      slot->frame.gpuOwner = output_;
      if (shellCopied) shellFrameNumber_ = slot->frame.frameNumber;
      slot->frame.sharedTexture = {output_->handle, 0, width_, height_, "B8G8R8A8_UNORM", shellFrameNumber_};
      if (!shellCopied) slot->frame.sharedTexture = {}; // Never pair new Program proof with an older shell export.
      auto published = std::make_shared<const ProgramFrame>(std::move(slot->frame));
      // Main Program is the multiview PGM cell. Its snapshot cannot advance
      // source/content proof when that independently owned export was busy.
      if (multiviewCopied) latest_ = published;
      if (delivered_.size() >= static_cast<size_t>(depth_ + 2)) { delivered_.pop_front(); ++diagnostics_.overflows; }
      delivered_.push_back(*published);
      delivery_.pop_front(); slot->state = State::Free;
      diagnostics_.occupancy = static_cast<int>(delivery_.size());
      changed_.notify_all();
      lock.unlock();
      if (deliveredCallback_) deliveredCallback_(*published);
      lock.lock();
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
  std::shared_ptr<const ProgramFrame> latest_;
  std::shared_ptr<Output> output_;
  std::shared_ptr<Output> multiviewOutput_;
  int64_t shellFrameNumber_ = 0;
  int64_t maximumAcquireNs_ = 0, maximumCompletionLateNs_ = 0;
  int64_t minimumDeliveryLeadNs_ = INT64_MAX;
  std::chrono::steady_clock::time_point lastTimingLog_{};
  int64_t maximumExportSubmitNs_ = 0, maximumExportSubmitLateNs_ = 0;
  ComPtrLite<ID3D11Query> prepareComplete_;
  std::function<void(const ProgramFrame&)> deliveredCallback_;
  std::thread prepareThread_, deliveryThread_;
  ComPtrLite<ID3D11Device> prepareDevice_, deliveryDevice_;
  ComPtrLite<ID3D11DeviceContext> prepareContext_, deliveryContext_;
  ComPtrLite<ID3D11VertexShader> vs_;
  ComPtrLite<ID3D11PixelShader> psY_, psUv_;
  ComPtrLite<ID3D11SamplerState> sampler_;
  ComPtrLite<ID3D11Texture2D> y_, uv_, yRead_, uvRead_;
  ComPtrLite<ID3D11RenderTargetView> yView_, uvView_;
};
}  // namespace corevideo::modules
