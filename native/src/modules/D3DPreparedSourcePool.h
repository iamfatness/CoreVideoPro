#pragma once
#include "modules/D3DI420VideoFrame.h"

namespace corevideo::modules {
// Format-specific storage behind the shared CPU preparation owner. Resources
// import once on the resource thread; uploads/queries stay on its GPU thread.
class D3DPreparedSourcePool {
 public:
  bool initialize(ID3D11Device* device, int width, int height, uint64_t epoch, bool bgra) {
    if (bgra) {
      bgra_ = std::make_unique<D3DVideoFramePool>();
      return bgra_->initialize(device, width, height, epoch, false, true);
    }
    i420_ = std::make_unique<D3DI420FramePool>();
    return i420_->initialize(device, width, height);
  }
  int beginUpload(ID3D11DeviceContext* context, const std::vector<uint8_t>& cpu, int stride) {
    return bgra_ ? bgra_->beginUpload(context, cpu.data(), cpu.size(), stride) : i420_->beginUpload(context, cpu);
  }
  std::shared_ptr<const GpuVideoFrame> completed(ID3D11DeviceContext* context, int slot, const CpuSourceGpuView& token) {
    if (!bgra_) {
      auto storage = i420_->completed(context, slot);
      return storage ? std::make_shared<D3DI420VideoImage>(std::move(storage), token) : nullptr;
    }
    auto storage = std::dynamic_pointer_cast<const D3DVideoImage>(bgra_->completed(context, slot));
    if (!storage) return {};
    auto image = std::make_shared<D3DVideoImage>();
    image->storage = storage; image->producer = storage->producer; image->views = storage->views;
    image->width = storage->width; image->height = storage->height; image->generation = storage->generation;
    image->sourceId = token.sourceId; image->sourceEpoch = token.sourceEpoch;
    image->sourceFrameId = token.frameId; image->sourceCaptureTimestamp100ns = token.captureTimestamp100ns;
    return image;
  }
  bool idle(ID3D11DeviceContext* context) { return bgra_ ? bgra_->idle(context) : i420_->idle(context); }
  HRESULT failure() const { return bgra_ ? bgra_->failure() : i420_->failure(); }
 private:
  std::unique_ptr<D3DVideoFramePool> bgra_;
  std::unique_ptr<D3DI420FramePool> i420_;
};
}
