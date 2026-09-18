#include <gtest/gtest.h>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#if defined(_WIN32) && !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_D3D11
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <dxgi.h>
#include "modules/D3DDecoupledExport.h"

namespace corevideo::modules {
struct D3DDecoupledExportTestAccess {
  static std::uint64_t published(const D3DDecoupledExport& e) { return e.published_; }
  static std::uint64_t dropped(const D3DDecoupledExport& e) { return e.dropped_; }
};
}  // namespace corevideo::modules

using namespace corevideo::modules;

namespace {
constexpr int kW = 128, kH = 64;

bool makeDevice(ComPtrLite<ID3D11Device>& device, ComPtrLite<ID3D11DeviceContext>& context) {
  return SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
      device.put(), nullptr, context.put()));
}

// A DEFAULT BGRA source texture uploaded with one uniform 32-bit value.
bool makeUniformSource(ID3D11Device* device, ID3D11DeviceContext* context,
                       std::uint32_t bgra, ComPtrLite<ID3D11Texture2D>& out) {
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = kW; desc.Height = kH; desc.MipLevels = desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  if (FAILED(device->CreateTexture2D(&desc, nullptr, out.put()))) return false;
  std::vector<std::uint32_t> pixels(static_cast<size_t>(kW) * kH, bgra);
  context->UpdateSubresource(out.get(), 0, nullptr, pixels.data(), kW * 4, 0);
  context->Flush();
  return true;
}

// Consumer side: open the exporter's output by handle on a *separate* device,
// acquire key 1, read back the first pixel. Returns false when no new frame.
bool readOutputPixel(ID3D11Device* device, ID3D11DeviceContext* context, HANDLE handle,
                     std::uint32_t& pixel) {
  ComPtrLite<ID3D11Texture2D> opened;
  if (FAILED(device->OpenSharedResource(handle, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(opened.put())))) return false;
  ComPtrLite<IDXGIKeyedMutex> mutex;
  if (FAILED(opened->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(mutex.put())))) return false;
  if (mutex->AcquireSync(1, 4) != S_OK) return false;  // producer holds it / no new frame
  D3D11_TEXTURE2D_DESC desc{};
  opened->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0;
  ComPtrLite<ID3D11Texture2D> staging;
  bool ok = false;
  if (SUCCEEDED(device->CreateTexture2D(&desc, nullptr, staging.put()))) {
    context->CopyResource(staging.get(), opened.get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
      pixel = *static_cast<const std::uint32_t*>(mapped.pData);
      context->Unmap(staging.get(), 0);
      ok = true;
    }
  }
  mutex->ReleaseSync(0);  // hand key 0 back so the exporter can publish the next frame
  return ok;
}
}  // namespace

TEST(D3DDecoupledExport, PublishesFrameToSharedOutputAcrossDevices) {
  ComPtrLite<ID3D11Device> device; ComPtrLite<ID3D11DeviceContext> context;
  ASSERT_TRUE(makeDevice(device, context));
  D3DDecoupledExport exporter(device.get(), kW, kH, "test");
  ASSERT_TRUE(exporter.valid());
  ASSERT_NE(exporter.handle(), nullptr);

  constexpr std::uint32_t kColor = 0xFF112233u;
  ComPtrLite<ID3D11Texture2D> src;
  ASSERT_TRUE(makeUniformSource(device.get(), context.get(), kColor, src));

  ComPtrLite<ID3D11Device> consumer; ComPtrLite<ID3D11DeviceContext> consumerCtx;
  ASSERT_TRUE(makeDevice(consumer, consumerCtx));

  std::uint32_t got = 0;
  bool matched = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline) {
    exporter.submit(context.get(), src.get());
    context->Flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
    if (readOutputPixel(consumer.get(), consumerCtx.get(), exporter.handle(), got) && got == kColor) {
      matched = true;
      break;
    }
  }
  EXPECT_TRUE(matched) << "output pixel 0x" << std::hex << got << " never matched 0x" << kColor;
  EXPECT_GT(D3DDecoupledExportTestAccess::published(exporter), 0u);
}

TEST(D3DDecoupledExport, ProducerNeverWedgesWithNoConsumer) {
  // No consumer ever attaches. The exporter must keep accepting submissions (the
  // reclaim path keeps the output live) and must never wedge the producer.
  ComPtrLite<ID3D11Device> device; ComPtrLite<ID3D11DeviceContext> context;
  ASSERT_TRUE(makeDevice(device, context));
  D3DDecoupledExport exporter(device.get(), kW, kH, "test");
  ASSERT_TRUE(exporter.valid());
  ComPtrLite<ID3D11Texture2D> src;
  ASSERT_TRUE(makeUniformSource(device.get(), context.get(), 0xFF00FF00u, src));

  int accepted = 0;
  for (int i = 0; i < 120; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = exporter.submit(context.get(), src.get());
    context->Flush();
    // submit must never block the render thread, regardless of consumer state.
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count(), 50);
    if (ok) ++accepted;
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
  }
  // Slots recycle, so the great majority of frames are accepted rather than dropped.
  EXPECT_GT(accepted, 60);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (D3DDecoupledExportTestAccess::published(exporter) == 0 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_GT(D3DDecoupledExportTestAccess::published(exporter), 0u);
}

#endif  // _WIN32 && dev adapters
