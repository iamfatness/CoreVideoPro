#pragma once

#include "compositor/ComPtrLite.h"
#include "modules/Interfaces.h"
#include <array>
#include <deque>
#include <map>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <chrono>

namespace corevideo::modules {

// In-process immutable GPU publications. These handles never reach the shell.
// The exporter proves GPU completion before publish; readers retain the slot
// until their GPU copy completes. No keyed mutex or CPU wait on the monitor.
class D3DProgramDisplaySnapshots {
 public:
  // Two timeline publications + construction + pending mailbox + executing
  // request + two GPU reads + writer. Duplicate holders share the same slot.
  static constexpr size_t Capacity = 8;
  static constexpr int64_t PresentationOffset100ns = 10000000 / 60;
  struct Snapshot {
    ComPtrLite<ID3D11Texture2D> texture;
    ProgramFrame frame;
    uint64_t generation = 0;
    size_t slot = 0;
    mutable std::atomic<bool> quarantined{false};
  };
  bool initialize(ID3D11Device* device, int width, int height, uint64_t generation) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height; desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    for (size_t i = 0; i < Capacity; ++i) {
      auto value = std::make_shared<Snapshot>();
      value->generation = generation; value->slot = i;
      ComPtrLite<IDXGIResource> resource; HANDLE handle = nullptr;
      if (FAILED(device->CreateTexture2D(&desc, nullptr, value->texture.put())) ||
          FAILED(value->texture->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(resource.put()))) ||
          FAILED(resource->GetSharedHandle(&handle)) || !handle) return false;
      char encoded[32]; std::snprintf(encoded, sizeof(encoded), "0x%llX",
          static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(handle)));
      value->frame.sharedTexture = {encoded, 0, width, height, "B8G8R8A8_UNORM", 0};
      slots_[i] = std::move(value);
    }
    return true;
  }
  // Caller holds the buffer mutex. The pool is the sole source of new leases.
  std::shared_ptr<Snapshot> reserve() {
    for (auto& slot : slots_) if (slot.use_count() == 1 && !slot->quarantined.load()) return slot;
    return {};
  }
  // Writer owns this reservation exclusively. All string allocation/destruction
  // happens before taking the Program delivery mutex to expose the publication.
  static void stamp(const std::shared_ptr<Snapshot>& slot, const ProgramFrame& frame) {
    auto descriptor = slot->frame.sharedTexture;
    descriptor.frameNumber = frame.frameNumber;
    // Retain identity only. CPU thumbnails/NV12 and other output leases belong
    // to their output packets, not this display pool.
    ProgramFrame identity;
    identity.frameNumber = frame.frameNumber;
    identity.deliverySequence = frame.deliverySequence;
    identity.productionSlot = frame.productionSlot;
    identity.productionAnchorNs = frame.productionAnchorNs;
    identity.producedAt100ns = frame.producedAt100ns;
    identity.deliveredAt100ns = frame.deliveredAt100ns;
    identity.timelineTimestamp100ns = frame.timelineTimestamp100ns;
    identity.renderPlanId = frame.renderPlanId;
    identity.renderPlanSignature = frame.renderPlanSignature;
    slot->frame = std::move(identity);
    slot->frame.sharedTexture = std::move(descriptor);
  }
  void publish(const std::shared_ptr<Snapshot>& slot) {
    history_[next_] = slot;
    next_ = (next_ + 1) % history_.size();
  }
  bool select(int64_t target, ProgramFrameSharedTexture& texture, std::shared_ptr<const void>& owner) const {
    for (size_t age = 0; age < history_.size(); ++age) {
      const auto& slot = history_[(next_ + history_.size() - 1 - age) % history_.size()];
      if (!slot || slot->frame.timelineTimestamp100ns > target) continue;
      texture = slot->frame.sharedTexture; owner = slot; return true;
    }
    return false;
  }
 private:
  std::array<std::shared_ptr<Snapshot>, Capacity> slots_;
  std::array<std::shared_ptr<const Snapshot>, 2> history_;
  size_t next_ = 0;
};

// Owned exclusively by the monitor device thread. Imports hold COM resources,
// not pool leases. Only outstanding GPU reads retain immutable publications.
class D3DProgramDisplayReader {
 public:
  ~D3DProgramDisplayReader() {
    // A stopped worker may have submitted a copy whose completion was never
    // polled. Quarantine before releasing its lease, even if the pool survives.
    for (const auto& read : reads_) read.owner->quarantined = true;
  }
  ID3D11ShaderResourceView* copy(ID3D11Device* device, ID3D11DeviceContext* context,
      const std::shared_ptr<const void>& owner) {
    retire(context);
    held_ = true;
    if (!owner) { ++reused_; return view_.get(); }
    auto snapshot = std::static_pointer_cast<const D3DProgramDisplaySnapshots::Snapshot>(owner);
    requested_ = snapshot->frame.deliverySequence;
    if (copiedGeneration_ == snapshot->generation && copied_ == requested_) { ++reused_; return view_.get(); }
    if (reads_.size() >= 2 || failed_) { ++refused_; return view_.get(); }
    if (generation_ != snapshot->generation) {
      imports_.clear(); generation_ = snapshot->generation;
    }
    if (!queries_[0]) {
      D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
      for (auto& item : queries_) if (FAILED(device->CreateQuery(&query, item.put()))) {
        failed_ = true; ++refused_; return view_.get();
      }
    }
    auto& imported = imports_[snapshot->slot];
    if (!imported) {
      const auto handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(
          std::strtoull(snapshot->frame.sharedTexture.sharedHandleHex.c_str(), nullptr, 16)));
      if (FAILED(device->OpenSharedResource(handle, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(imported.put())))) {
        ++refused_; return view_.get();
      }
    }
    D3D11_TEXTURE2D_DESC desc{}; imported->GetDesc(&desc);
    if (!local_ || width_ != desc.Width || height_ != desc.Height) {
      ComPtrLite<ID3D11Texture2D> nextLocal;
      ComPtrLite<ID3D11ShaderResourceView> nextView;
      desc.MiscFlags = 0;
      if (FAILED(device->CreateTexture2D(&desc, nullptr, nextLocal.put())) ||
          FAILED(device->CreateShaderResourceView(nextLocal.get(), nullptr, nextView.put()))) { ++refused_; return view_.get(); }
      local_ = std::move(nextLocal); view_ = std::move(nextView);
      width_ = desc.Width; height_ = desc.Height;
    }
    Read read; read.owner = std::move(snapshot);
    for (size_t index = 0; index < queries_.size(); ++index) {
      const bool busy = std::any_of(reads_.begin(), reads_.end(), [index](const Read& item) { return item.query == index; });
      if (!busy) { read.query = index; break; }
    }
    context->CopyResource(local_.get(), imported.get());
    context->End(queries_[read.query].get());
    // The enclosing monitor pass flushes. The copy precedes its draw on the same
    // immediate context; completion retires source ownership, not presentation.
    if (copiedGeneration_ == generation_ && copied_ && requested_ > copied_ + 1) skipped_ += requested_ - copied_ - 1;
    if (copiedGeneration_ == generation_ && copied_ && requested_ < copied_) ++reordered_;
    copiedGeneration_ = generation_;
    copied_ = requested_; ++copies_;
    held_ = false;
    frameNumber_ = read.owner->frame.frameNumber;
    timeline100ns_ = read.owner->frame.timelineTimestamp100ns;
    reads_.push_back(std::move(read));
    return view_.get();
  }
  void retire(ID3D11DeviceContext* context) {
    for (auto it = reads_.begin(); it != reads_.end();) {
      BOOL done = FALSE;
      const auto hr = context->GetData(queries_[it->query].get(), &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH);
      if (hr == S_OK && done) { ++completed_; it = reads_.erase(it); }
      else { if (FAILED(hr)) { failed_ = true; it->owner->quarantined = true; } ++it; }
    }
  }
  uint64_t copied() const { return copied_; }
  uint64_t requested() const { return requested_; }
  uint64_t refused() const { return refused_; }
  ProgramDisplayObservation observation() const {
    ProgramDisplayObservation result;
    result.observed = copies_ > 0; result.generation = copiedGeneration_;
    result.requestedSequence = requested_; result.copiedSequence = copied_;
    result.frameNumber = frameNumber_; result.timelineTimestamp100ns = timeline100ns_;
    const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count() / 100;
    result.age100ns = timeline100ns_ ? (std::max)(int64_t(0), now - timeline100ns_) : 0;
    result.copies = copies_; result.completedCopies = completed_; result.reused = reused_;
    result.refused = refused_; result.skipped = skipped_; result.reordered = reordered_;
    result.capacity = D3DProgramDisplaySnapshots::Capacity;
    result.allocatedBytes = uint64_t(width_) * height_ * 4 * result.capacity;
    result.gpuReads = static_cast<int>(reads_.size());
    result.state = failed_ ? "failed" : !copied_ ? "preparing" : held_ ? "held" : "copied";
    return result;
  }
 private:
  struct Read {
    std::shared_ptr<const D3DProgramDisplaySnapshots::Snapshot> owner;
    size_t query = 0;
  };
  uint64_t generation_ = 0, copiedGeneration_ = 0, requested_ = 0, copied_ = 0;
  uint64_t copies_ = 0, completed_ = 0, reused_ = 0, refused_ = 0;
  uint64_t skipped_ = 0, reordered_ = 0;
  int64_t frameNumber_ = 0, timeline100ns_ = 0;
  UINT width_ = 0, height_ = 0;
  bool failed_ = false, held_ = true;
  std::map<size_t, ComPtrLite<ID3D11Texture2D>> imports_;
  ComPtrLite<ID3D11Texture2D> local_;
  ComPtrLite<ID3D11ShaderResourceView> view_;
  std::array<ComPtrLite<ID3D11Query>, 2> queries_;
  std::deque<Read> reads_;
};
} // namespace corevideo::modules
