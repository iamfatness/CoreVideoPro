#pragma once
#include "modules/D3DProgramDisplaySnapshots.h"

namespace corevideo::modules {
// Owns only scheduled Program copies and their completion queries. Optional
// uploads, scene drawing and exports never run on this immediate context.
class D3DProgramCopyOwner {
 public:
  explicit D3DProgramCopyOwner(IDXGIAdapter* adapter, int width = 1920, int height = 1080) {
    if (FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        device_.put(), nullptr, context_.put()))) throw std::runtime_error("Program copy device unavailable");
    consumer_ = D3DVideoConsumers::add(device_.get(), true);
    reader_.prewarm(device_.get(), context_.get(), width, height, true);
  }
  MonitorRenderRequest copy(MonitorRenderRequest request) {
    if (request.multiviewActive && request.bufferedProgram &&
        (request.acquireDeliveredProgram || request.deliveredProgramOwner)) {
      if (request.acquireDeliveredProgram) {
        request.deliveredProgram = {}; request.deliveredProgramOwner.reset();
        while (!request.cancelled()) {
          if (request.acquireDeliveredProgram(request.deliveredProgram, request.deliveredProgramOwner)) break;
          const auto remaining = request.programDisplayDeadlineNs - nowNs();
          if (remaining <= 0) break;
          std::this_thread::sleep_for(std::chrono::nanoseconds((std::min)(remaining, int64_t(100000))));
        }
      }
      if (request.acquireDeliveredProgram && !request.deliveredProgramOwner)
        reader_.publicationUnavailable(device_.get(), request);
      else reader_.copy(device_.get(), context_.get(), request.deliveredProgramOwner,
          request.programDisplayTargetNs, request.programDisplayDeadlineNs, &request);
      reader_.completeBeforeAuxiliary(context_.get(), request);
    }
    request.programCopyPrepared = true;
    request.readyProgramImage = reader_.completedImage();
    request.readyProgramObservation = reader_.completedObservation();
    // Downstream owns only a completed private image, never an input-pool lease
    // or a callback that could retry the production timeline on a late batch.
    request.deliveredProgramOwner.reset(); request.acquireDeliveredProgram = {};
    return request;
  }
  ProgramDisplayObservation observation() const { return reader_.observation(); }
  bool retire() { reader_.retire(context_.get()); return reader_.needsRetirement(); }
 private:
  static int64_t nowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count(); }
  ComPtrLite<ID3D11Device> device_;
  ComPtrLite<ID3D11DeviceContext> context_;
  std::shared_ptr<D3DVideoConsumer> consumer_;
  D3DProgramDisplayReader reader_;
};
}
