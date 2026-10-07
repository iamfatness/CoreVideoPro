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
    if (pending_ < 0) return false;
    auto image = pool_.completed(context, pending_);
    if (!image) return false;
    pending_ = -1;
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
      if (attempted_) return;
      attempted_ = true;
      try { initialized_ = pool_.initialize(device, frame.pixelWidth, frame.pixelHeight, frame.sourceEpoch); }
      catch (...) { initialized_ = false; }
      if (!initialized_) { pool_ = D3DVideoFramePool{}; ++stats_.failed; return; }
    }
    if (!pool_.dimensions(frame.pixelWidth, frame.pixelHeight)) { ++stats_.failed; return; }
    const int slot = pool_.beginUpload(context, frame.pixels->data(), frame.pixels->size(), frame.pixelStride);
    if (slot < 0) { ++stats_.busy; return; }
    pending_ = slot; frameId_ = frame.frameId; epoch_ = frame.sourceEpoch;
    sourceId_ = frame.participantId; offered_ = true;
    context->Flush(); // preparation owner submits; Program never flushes this context
  }
  const Stats& stats() const { return stats_; }
  bool released(ID3D11DeviceContext* context) {
    return !initialized_ || pool_.idle(context);
  }
 private:
  D3DVideoFramePool pool_;
  Stats stats_;
  int pending_ = -1;
  int64_t frameId_ = 0;
  uint64_t epoch_ = 0;
  std::string sourceId_;
  bool attempted_ = false, initialized_ = false, offered_ = false;
};
}
