#include "modules/Interfaces.h"

#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_D3D11

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <memory>
#include <iterator>
#include <utility>

namespace corevideo::modules {
namespace {

template <typename T>
class ComPtrLite {
 public:
  ComPtrLite() = default;
  ~ComPtrLite() { reset(); }
  ComPtrLite(const ComPtrLite&) = delete;
  ComPtrLite& operator=(const ComPtrLite&) = delete;
  ComPtrLite(ComPtrLite&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
  ComPtrLite& operator=(ComPtrLite&& other) noexcept {
    if (this != &other) {
      reset();
      value_ = std::exchange(other.value_, nullptr);
    }
    return *this;
  }

  T** put() {
    reset();
    return &value_;
  }

  T* get() const { return value_; }
  T* operator->() const { return value_; }
  explicit operator bool() const { return value_ != nullptr; }

 private:
  void reset() {
    if (value_) {
      value_->Release();
      value_ = nullptr;
    }
  }

  T* value_ = nullptr;
};

class D3D11Compositor final : public ICompositor {
 public:
  D3D11Compositor(ComPtrLite<ID3D11Device> device, ComPtrLite<ID3D11DeviceContext> context)
      : device_(std::move(device)), context_(std::move(context)) {}

  std::string rendererName() const override { return "d3d11"; }

  ProgramFrame render(const CompositorRenderPlan& renderPlan, const std::vector<VideoFrame>& frames) override {
    ++frameNumber_;
    if (context_) {
      context_->Flush();
    }

    ProgramFrame frame;
    frame.width = renderPlan.width;
    frame.height = renderPlan.height;
    frame.layerCount = renderPlan.layers.empty() ? static_cast<int>(frames.size()) : static_cast<int>(renderPlan.layers.size());
    frame.frameNumber = frameNumber_;
    frame.renderPlanId = renderPlan.renderPlanId;
    frame.renderer = "d3d11";
    return frame;
  }

 private:
  ComPtrLite<ID3D11Device> device_;
  ComPtrLite<ID3D11DeviceContext> context_;
  int64_t frameNumber_ = 0;
};

}  // namespace

std::unique_ptr<ICompositor> createD3D11Compositor() {
  ComPtrLite<ID3D11Device> device;
  ComPtrLite<ID3D11DeviceContext> context;
  D3D_FEATURE_LEVEL selectedFeatureLevel = D3D_FEATURE_LEVEL_11_0;
  constexpr D3D_FEATURE_LEVEL featureLevels[] = {
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0,
  };

  const HRESULT result = D3D11CreateDevice(
      nullptr,
      D3D_DRIVER_TYPE_HARDWARE,
      nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT,
      featureLevels,
      static_cast<UINT>(std::size(featureLevels)),
      D3D11_SDK_VERSION,
      device.put(),
      &selectedFeatureLevel,
      context.put());

  if (FAILED(result) || !device || !context) {
    return nullptr;
  }

  return std::make_unique<D3D11Compositor>(std::move(device), std::move(context));
}

}  // namespace corevideo::modules

#else

namespace corevideo::modules {

std::unique_ptr<ICompositor> createD3D11Compositor() {
  return nullptr;
}

}  // namespace corevideo::modules

#endif
