#pragma once
// Windows-only. Capture creates/imports these resources off the render thread.
#include "modules/GpuVideoFrame.h"
#include "core/BoundedAsyncLog.h"
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace corevideo::modules {
using Microsoft::WRL::ComPtr;

struct D3DVideoConsumer {
  ComPtr<ID3D11Device> device;
  LUID adapter{};
  uint64_t id = 0;
  bool monitor = false;
};

class D3DVideoConsumers {
 public:
  static std::shared_ptr<D3DVideoConsumer> add(ID3D11Device* device, bool monitor = false) {
    auto consumer = std::make_shared<D3DVideoConsumer>();
    consumer->device = device;
    consumer->monitor = monitor;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&adapter)) ||
        FAILED(adapter->GetDesc(&desc))) return {};
    consumer->adapter = desc.AdapterLuid;
    std::lock_guard<std::mutex> lock(mutex_);
    consumer->id = ++nextId_;
    consumers_.push_back(consumer);
    return consumer;
  }
  static std::vector<std::shared_ptr<D3DVideoConsumer>> snapshot() {
    std::vector<std::shared_ptr<D3DVideoConsumer>> result;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = consumers_.begin(); it != consumers_.end();) {
      if (auto consumer = it->lock()) { result.push_back(std::move(consumer)); ++it; }
      else it = consumers_.erase(it);
    }
    return result;
  }
  static uint64_t revision() { std::lock_guard<std::mutex> lock(mutex_); return nextId_; }
 private:
  inline static std::mutex mutex_;
  inline static uint64_t nextId_ = 0;
  inline static std::vector<std::weak_ptr<D3DVideoConsumer>> consumers_;
};

struct D3DVideoImage final : GpuVideoFrame {
  struct View {
    uint64_t consumer = 0;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
  };
  ComPtr<ID3D11Texture2D> producer;
  std::vector<View> views;
  size_t bytes = 0;
  bool monitor = false;
  inline static std::atomic<size_t> residentBytes{0};
  inline static std::atomic<size_t> monitorResidentBytes{0};
  ~D3DVideoImage() override { (monitor ? monitorResidentBytes : residentBytes).fetch_sub(bytes); }
  const View* view(uint64_t consumer) const {
    for (const auto& item : views) if (item.consumer == consumer) return &item;
    return nullptr;
  }
};

// Three immutable leases. No image can be overwritten while any VideoFrame,
// source cache, or GPU read-completion query still owns it. All methods run on
// the capture context's owner. Admission accounts images retained after resize.
class D3DVideoFramePool {
 public:
  bool initialize(ID3D11Device* device, int width, int height, uint64_t generation, bool monitor = false) {
    if (width <= 0 || height <= 0 || width > 7680 || height > 4320) return false;
    const size_t bytes = static_cast<size_t>(width) * height * 4;
    const auto consumers = D3DVideoConsumers::snapshot();
    if (consumers.empty()) return false;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC adapterDesc{};
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&adapter)) ||
        FAILED(adapter->GetDesc(&adapterDesc))) return false;
    for (auto& slot : slots_) {
      auto image = std::make_shared<D3DVideoImage>();
      auto& residency = monitor ? D3DVideoImage::monitorResidentBytes : D3DVideoImage::residentBytes;
      const size_t limit = (monitor ? 256u : 512u) * 1024u * 1024u;
      auto reserved = residency.load();
      do {
        if (reserved + bytes > limit) return false;
      } while (!residency.compare_exchange_weak(reserved, reserved + bytes));
      image->bytes = bytes;
      image->monitor = monitor;
      image->width = width; image->height = height; image->generation = generation;
      image->monitorPrivate = monitor;
      D3D11_TEXTURE2D_DESC desc{};
      desc.Width = width; desc.Height = height; desc.MipLevels = desc.ArraySize = 1;
      desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
      desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
      if (FAILED(device->CreateTexture2D(&desc, nullptr, &image->producer))) return false;
      ComPtr<IDXGIResource> resource;
      HANDLE handle = nullptr;
      if (FAILED(image->producer.As(&resource)) || FAILED(resource->GetSharedHandle(&handle))) return false;
      for (const auto& consumer : consumers) {
        if (consumer->monitor != monitor) continue;
        if (consumer->adapter.LowPart != adapterDesc.AdapterLuid.LowPart ||
            consumer->adapter.HighPart != adapterDesc.AdapterLuid.HighPart) continue;
        D3DVideoImage::View view;
        view.consumer = consumer->id;
        if (FAILED(consumer->device->OpenSharedResource(handle, IID_PPV_ARGS(&view.texture))) ||
            FAILED(consumer->device->CreateShaderResourceView(view.texture.Get(), nullptr, &view.srv))) return false;
        image->views.push_back(std::move(view));
      }
      if (image->views.empty()) return false;
      D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
      if (FAILED(device->CreateQuery(&query, &slot.ready))) return false;
      slot.image = std::move(image);
    }
    width_ = width; height_ = height;
    return true;
  }
  bool dimensions(int width, int height) const { return width == width_ && height == height_; }
  // Begin before the compatibility CPU readback. Publication below checks the
  // actual copy completion, never just the return from CopyResource.
  int beginCopy(ID3D11DeviceContext* context, ID3D11Texture2D* source) {
    poll(context);
    for (size_t i = 0; i < slots_.size(); ++i) {
      auto& slot = slots_[i];
      if (!slot.pending && slot.image && slot.image.use_count() == 1) {
        context->CopyResource(slot.image->producer.Get(), source);
        context->End(slot.ready.Get());
        slot.pending = true;
        return static_cast<int>(i);
      }
    }
    return -1;
  }
  std::shared_ptr<const GpuVideoFrame> completed(ID3D11DeviceContext* context, int index) {
    if (index < 0) return {};
    poll(context);
    return slots_[index].pending ? nullptr : slots_[index].image;
  }
  bool idle(ID3D11DeviceContext* context) {
    poll(context);
    for (const auto& slot : slots_)
      if (slot.pending || (slot.image && slot.image.use_count() != 1)) return false;
    return true;
  }
 private:
  void poll(ID3D11DeviceContext* context) {
    for (auto& slot : slots_) if (slot.pending) {
      BOOL ready = FALSE;
      if (context->GetData(slot.ready.Get(), &ready, sizeof(ready), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && ready)
        slot.pending = false;
    }
  }
  struct Slot { std::shared_ptr<D3DVideoImage> image; ComPtr<ID3D11Query> ready; bool pending = false; };
  std::array<Slot, 3> slots_;
  int width_ = 0, height_ = 0;
};

// Lease lifetime includes every submitted GPU read, not merely the CPU draw
// call. Exhaustion refuses GPU images for this pass; existing CPU data remains
// the explicit fallback. Queries are preallocated before production starts.
class D3DVideoReadLeases {
 public:
  explicit D3DVideoReadLeases(ID3D11Device* device, ID3D11DeviceContext* context) : context_(context) {
    D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT, 0};
    for (auto& slot : slots_) { device->CreateQuery(&desc, &slot.query); slot.images.reserve(32); }
  }
  ~D3DVideoReadLeases() {
    finish(); context_->Flush();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (pending() && std::chrono::steady_clock::now() < deadline) {
      poll(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (pending()) {
      // Device failure must not release an image back to the capture writer
      // while its GPU read might still exist. Quarantine until process teardown.
      std::lock_guard<std::mutex> lock(quarantineMutex_);
      for (auto& slot : slots_) for (auto& image : slot.images) quarantine_.push_back(std::move(image));
      core::nativeLogf("[gpu-ingress] read retirement timed out; leases quarantined\n");
    }
  }
  bool hold(std::shared_ptr<const GpuVideoFrame> image) {
    if (active_ < 0) {
      poll();
      for (size_t i = 0; i < slots_.size(); ++i)
        if (!slots_[i].pending && slots_[i].query) { active_ = static_cast<int>(i); break; }
    }
    if (active_ < 0) return false;
    auto& images = slots_[active_].images;
    for (const auto& held : images) if (held == image) return true;
    if (images.size() == 32) return false;
    images.push_back(std::move(image));
    return true;
  }
  void finish() {
    if (active_ < 0) return;
    auto& slot = slots_[active_];
    context_->End(slot.query.Get()); slot.pending = true; active_ = -1;
  }
 private:
  bool pending() const { for (const auto& slot : slots_) if (slot.pending) return true; return false; }
  void poll() {
    for (auto& slot : slots_) if (slot.pending) {
      BOOL ready = FALSE;
      if (context_->GetData(slot.query.Get(), &ready, sizeof(ready), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && ready) {
        slot.images.clear(); slot.pending = false;
      }
    }
  }
  struct Slot { ComPtr<ID3D11Query> query; std::vector<std::shared_ptr<const GpuVideoFrame>> images; bool pending = false; };
  ComPtr<ID3D11DeviceContext> context_;
  std::array<Slot, 16> slots_;
  int active_ = -1;
  inline static std::mutex quarantineMutex_;
  inline static std::vector<std::shared_ptr<const GpuVideoFrame>> quarantine_;
};
}
