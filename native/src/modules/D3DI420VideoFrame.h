#pragma once
#include "modules/D3DVideoFrame.h"
#include "modules/CpuSourceGpuView.h"

namespace corevideo::modules {
struct D3DI420Storage {
  struct View {
    uint64_t consumer = 0;
    std::array<ComPtr<ID3D11Texture2D>, 3> textures;
    std::array<ComPtr<ID3D11ShaderResourceView>, 3> srvs;
  };
  std::array<ComPtr<ID3D11Texture2D>, 3> producer;
  std::vector<View> views;
  size_t bytes = 0;
  ~D3DI420Storage() { D3DVideoImage::residentBytes.fetch_sub(bytes); }
};

// Unique per completed frame. The underlying three-plane storage may be
// reused only when this wrapper and all Program GPU read leases are gone.
struct D3DI420VideoImage final : GpuVideoFrame {
  std::shared_ptr<const D3DI420Storage> storage;
  D3DI420VideoImage(std::shared_ptr<const D3DI420Storage> value, const CpuSourceGpuView& frame)
      : storage(std::move(value)) {
    backend = Backend::D3D11I420;
    width = frame.width; height = frame.height;
    sourceEpoch = frame.sourceEpoch; sourceFrameId = frame.frameId;
    sourceId = frame.sourceId;
    sourceCaptureTimestamp100ns = frame.captureTimestamp100ns;
  }
  const D3DI420Storage::View* view(uint64_t consumer) const {
    for (const auto& item : storage->views) if (item.consumer == consumer) return &item;
    return nullptr;
  }
};

class D3DI420FramePool {
 public:
  // Resource preparation owner only; no immediate-context calls here.
  bool initialize(ID3D11Device* device, int width, int height) {
    if (width <= 0 || height <= 0 || width > 7680 || height > 4320 || (width & 1) || (height & 1)) return false;
    const size_t bytes = static_cast<size_t>(width) * height * 3 / 2;
    const auto consumers = D3DVideoConsumers::snapshot();
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&adapter)) ||
        FAILED(adapter->GetDesc(&desc))) return false;
    for (auto& slot : slots_) {
      auto storage = std::make_shared<D3DI420Storage>();
      auto reserved = D3DVideoImage::residentBytes.load();
      do {
        if (reserved + bytes > 512u * 1024u * 1024u) { capacityRefused_ = true; return false; }
      } while (!D3DVideoImage::residentBytes.compare_exchange_weak(reserved, reserved + bytes));
      storage->bytes = bytes;
      for (size_t plane = 0; plane < 3; ++plane) {
        D3D11_TEXTURE2D_DESC texture{};
        texture.Width = plane ? width / 2 : width; texture.Height = plane ? height / 2 : height;
        texture.MipLevels = texture.ArraySize = 1; texture.SampleDesc.Count = 1;
        texture.Format = DXGI_FORMAT_R8_UNORM; texture.Usage = D3D11_USAGE_DEFAULT;
        texture.BindFlags = D3D11_BIND_SHADER_RESOURCE; texture.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        if (FAILED(device->CreateTexture2D(&texture, nullptr, &storage->producer[plane]))) return false;
      }
      for (const auto& consumer : consumers) {
        if (consumer->monitor || consumer->adapter.LowPart != desc.AdapterLuid.LowPart ||
            consumer->adapter.HighPart != desc.AdapterLuid.HighPart) continue;
        D3DI420Storage::View view; view.consumer = consumer->id;
        for (size_t plane = 0; plane < 3; ++plane) {
          ComPtr<IDXGIResource> resource; HANDLE handle = nullptr;
          if (FAILED(storage->producer[plane].As(&resource)) || FAILED(resource->GetSharedHandle(&handle)) ||
              FAILED(consumer->device->OpenSharedResource(handle, IID_PPV_ARGS(&view.textures[plane]))) ||
              FAILED(consumer->device->CreateShaderResourceView(view.textures[plane].Get(), nullptr, &view.srvs[plane]))) return false;
        }
        storage->views.push_back(std::move(view));
      }
      if (storage->views.empty()) return false;
      D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
      if (FAILED(device->CreateQuery(&query, &slot.ready))) return false;
      slot.storage = std::move(storage);
    }
    width_ = width; height_ = height;
    return true;
  }
  // Upload context owner only. CPU data is consumed before this call returns;
  // a successful CPU submission is not a ready shared image.
  int beginUpload(ID3D11DeviceContext* context, const std::vector<uint8_t>& cpu) {
    const size_t y = static_cast<size_t>(width_) * height_;
    if (!y || cpu.size() < y * 3 / 2) return -1;
    poll(context);
    for (size_t i = 0; i < slots_.size(); ++i) {
      auto& slot = slots_[i];
      if (slot.pending || !slot.storage || slot.storage.use_count() != 1) continue;
      context->UpdateSubresource(slot.storage->producer[0].Get(), 0, nullptr, cpu.data(), width_, 0);
      context->UpdateSubresource(slot.storage->producer[1].Get(), 0, nullptr, cpu.data() + y, width_ / 2, 0);
      context->UpdateSubresource(slot.storage->producer[2].Get(), 0, nullptr, cpu.data() + y + y / 4, width_ / 2, 0);
      context->End(slot.ready.Get()); slot.pending = true;
      return static_cast<int>(i);
    }
    return -1;
  }
  std::shared_ptr<const D3DI420Storage> completed(ID3D11DeviceContext* context, int index) {
    if (index < 0 || index >= static_cast<int>(slots_.size())) return {};
    poll(context);
    return slots_[index].pending ? nullptr : slots_[index].storage;
  }
  bool idle(ID3D11DeviceContext* context) {
    poll(context);
    for (const auto& slot : slots_) if (slot.pending || (slot.storage && slot.storage.use_count() != 1)) return false;
    return true;
  }
  HRESULT failure() const { return failure_; }
  bool capacityRefused() const { return capacityRefused_; }
 private:
  void poll(ID3D11DeviceContext* context) {
    for (auto& slot : slots_) if (slot.pending) {
      BOOL ready = FALSE;
      // Worker-owned polling must allow query progress. DONOTFLUSH left the
      // production 1080p60 capture queries pending for hundreds of milliseconds
      // despite the initial submit Flush. This never runs on Program's context.
      const auto result = context->GetData(slot.ready.Get(), &ready, sizeof(ready), 0);
      if (FAILED(result)) failure_ = result; // never recycle an unproven write
      if (result == S_OK && ready)
        slot.pending = false;
    }
  }
  struct Slot { std::shared_ptr<D3DI420Storage> storage; ComPtr<ID3D11Query> ready; bool pending = false; };
  std::array<Slot, 3> slots_;
  int width_ = 0, height_ = 0;
  HRESULT failure_ = S_OK;
  bool capacityRefused_ = false;
};
}
