#pragma once
#include "modules/D3DVideoFrame.h"
#include "modules/Interfaces.h"

namespace corevideo::modules {
// One source, one producer-context owner. The caller keeps authoritative CPU
// arrival/selection; a late completion never substitutes an older or newer
// source identity. This is an optional representation, not a playout queue.
class BgraSourcePreparation {
 public:
  struct Stats { uint64_t prepared = 0, busy = 0, failed = 0, superseded = 0; };
  bool poll(ID3D11DeviceContext* context, VideoFrame& latest) {
    for (auto& ready : ready_) {
      const auto token = ready.token.lock();
      if (!token || token->consumed.load()) ready = {};
    }
    if (pending_ < 0) return false;
    auto image = pool_.completed(context, pending_);
    if (!image) return false;
    const auto completedSlot = pending_; pending_ = -1;
    if (pendingToken_) {
      auto storage = std::dynamic_pointer_cast<const D3DVideoImage>(image);
      auto prepared = std::make_shared<D3DVideoImage>();
      prepared->storage = storage; prepared->producer = storage->producer; prepared->views = storage->views;
      prepared->width = storage->width; prepared->height = storage->height; prepared->generation = storage->generation;
      prepared->sourceId = pendingToken_->sourceId; prepared->sourceEpoch = pendingToken_->sourceEpoch;
      prepared->sourceFrameId = pendingToken_->frameId; prepared->sourceCaptureTimestamp100ns = pendingToken_->captureTimestamp100ns;
      pendingToken_->ready.store(prepared);
      pendingToken_->completionPublished.store(true);
      ready_[completedSlot] = {pendingToken_, std::move(prepared), pendingToken_->frameId}; pendingToken_.reset();
      if (readyCount() > 2) releaseOldest();
      ++stats_.prepared;
      return true;
    }
    if (latest.participantId != sourceId_ || latest.frameId != frameId_ || latest.sourceEpoch != epoch_ ||
        latest.pixelWidth != image->width || latest.pixelHeight != image->height) {
      ++stats_.superseded;
      return false;
    }
    latest.gpuPixels = std::move(image);
    ++stats_.prepared;
    return true;
  }
  void offer(ID3D11Device* device, ID3D11DeviceContext* context, const VideoFrame& frame) {
    if (!frame.hasPixels() || frame.gpuPixels || (offered_ && sourceId_ == frame.participantId &&
        frameId_ == frame.frameId && epoch_ == frame.sourceEpoch)) return;
    if (pending_ >= 0) { ++stats_.busy; return; }
    if (!initialized_) {
      // Failed capability/resource admission is explicit and does not retry
      // allocations on every owner poll. A new source generation may retry.
      if (attempted_) { if (frame.preparedGpu && frame.preparedGpu->demand) frame.preparedGpu->demand->failed.store(true); return; }
      attempted_ = true;
      try { initialized_ = pool_.initialize(device, frame.pixelWidth, frame.pixelHeight, frame.sourceEpoch); }
      catch (...) { initialized_ = false; }
      if (!initialized_) { pool_ = D3DVideoFramePool{}; if (frame.preparedGpu && frame.preparedGpu->demand) frame.preparedGpu->demand->failed.store(true); ++stats_.failed; return; }
    }
    if (!pool_.dimensions(frame.pixelWidth, frame.pixelHeight)) { ++stats_.failed; return; }
    int slot = pool_.beginUpload(context, frame.pixels->data(), frame.pixels->size(), frame.pixelStride);
    if (slot < 0 && readyCount() > 1) {
      releaseOldest(); // bounded completion residency, independent of CPU/ISO token lifetime
      slot = pool_.beginUpload(context, frame.pixels->data(), frame.pixels->size(), frame.pixelStride);
    }
    if (slot < 0) { ++stats_.busy; return; }
    pending_ = slot; frameId_ = frame.frameId; epoch_ = frame.sourceEpoch;
    sourceId_ = frame.participantId; offered_ = true;
    pendingToken_ = frame.preparedGpu;
    context->Flush(); // preparation owner submits; Program never flushes this context
  }
  const Stats& stats() const { return stats_; }
  void supersedeUnsubmitted(const VideoFrame& frame) {
    const auto& token = frame.preparedGpu;
    if (token && token != pendingToken_ && !token->completionPublished.load() &&
        token->demand && !token->demand->failed.load()) token->superseded.store(true);
  }
  bool released(ID3D11DeviceContext* context) {
    ready_ = {}; pendingToken_.reset(); // retirement never publishes new completions
    return !initialized_ || pool_.idle(context);
  }
 private:
  D3DVideoFramePool pool_;
  Stats stats_;
  int pending_ = -1;
  int64_t frameId_ = 0;
  uint64_t epoch_ = 0;
  std::string sourceId_;
  std::shared_ptr<CpuSourceGpuView> pendingToken_;
  struct Ready { std::weak_ptr<CpuSourceGpuView> token; std::shared_ptr<const GpuVideoFrame> image; int64_t frameId = 0; };
  std::array<Ready, 3> ready_;
  size_t readyCount() const { size_t count = 0; for (const auto& ready : ready_) if (ready.image) ++count; return count; }
  void releaseOldest() {
    auto oldest = ready_.end();
    for (auto it = ready_.begin(); it != ready_.end(); ++it)
      if (it->image && (oldest == ready_.end() || it->frameId < oldest->frameId)) oldest = it;
    if (oldest != ready_.end()) *oldest = {};
  }
  bool attempted_ = false, initialized_ = false, offered_ = false;
};
}
