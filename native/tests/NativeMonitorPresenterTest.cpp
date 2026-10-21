#include <gtest/gtest.h>
#include "MonitorPresenter.h"
#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <wrl/client.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <stdexcept>

namespace {
using Microsoft::WRL::ComPtr;
void require(HRESULT result) { if (FAILED(result)) throw std::runtime_error("D3D monitor test setup failed"); }
struct MonitorRig {
  ComPtr<ID3D11Device> producer, consumer;
  ComPtr<ID3D11DeviceContext> producerContext, consumerContext;
  ComPtr<ID3D11Texture2D> source, backBuffer;
  ComPtr<ID3D11RenderTargetView> target;
  ComPtr<IDXGIKeyedMutex> mutex;
  ComPtr<IDXGISwapChain1> chain;
  HWND window = nullptr;
  std::uint64_t handle = 0;
  void* owner = nullptr;
  std::atomic<bool> stop{false};
  std::thread publisher;
  MonitorRig() {
    auto driver = D3D_DRIVER_TYPE_HARDWARE;
    auto result = D3D11CreateDevice(nullptr, driver, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &producer, nullptr, &producerContext);
    if (FAILED(result)) {
      driver = D3D_DRIVER_TYPE_WARP;
      result = D3D11CreateDevice(nullptr, driver, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
          nullptr, 0, D3D11_SDK_VERSION, &producer, nullptr, &producerContext);
    }
    require(result);
    require(D3D11CreateDevice(nullptr, driver, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &consumer, nullptr, &consumerContext));
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 64; desc.Height = 36; desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    require(producer->CreateTexture2D(&desc, nullptr, &source));
    require(producer->CreateRenderTargetView(source.Get(), nullptr, &target));
    require(source.As(&mutex));
    ComPtr<IDXGIResource> resource; require(source.As(&resource));
    HANDLE shared{}; require(resource->GetSharedHandle(&shared)); handle = reinterpret_cast<std::uint64_t>(shared);
    WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"CoreVideoOwnedMonitorTest"; RegisterClassW(&wc);
    window = CreateWindowExW(0, wc.lpszClassName, L"CoreVideo monitor QA", WS_OVERLAPPEDWINDOW,
        -20000, -20000, 96, 64, nullptr, nullptr, wc.hInstance, nullptr);
    if (!window) throw std::runtime_error("Owned offscreen window failed");
    ComPtr<IDXGIDevice> dxgi; require(consumer.As(&dxgi));
    ComPtr<IDXGIAdapter> adapter; require(dxgi->GetAdapter(&adapter));
    ComPtr<IDXGIFactory2> factory; require(adapter->GetParent(IID_PPV_ARGS(&factory)));
    DXGI_SWAP_CHAIN_DESC1 swap{};
    swap.Width = 64; swap.Height = 36; swap.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swap.SampleDesc.Count = 1; swap.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap.BufferCount = 2; swap.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    swap.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    require(factory->CreateSwapChainForHwnd(consumer.Get(), window, &swap, nullptr, nullptr, &chain));
    require(chain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)));
    // Hidden HWND / no latency-credit handle: functional GPU ownership proof,
    // explicitly NOT physical display cadence qualification.
    owner = create();
    if (!owner) throw std::runtime_error("Native monitor lease failed");
  }
  void* create(std::uint32_t flags = 1) {
    return cv_monitor_create(consumer.Get(), consumerContext.Get(), chain.Get(), backBuffer.Get(), handle, nullptr, flags);
  }
  void publish(bool blue) {
    auto acquired = mutex->AcquireSync(0, 0);
    if (acquired != S_OK) acquired = mutex->AcquireSync(1, 0);
    if (acquired != S_OK) return;
    const float red[]{1, 0, 0, 1}, blueColor[]{0, 0, 1, 1};
    producerContext->ClearRenderTargetView(target.Get(), blue ? blueColor : red);
    producerContext->Flush(); mutex->ReleaseSync(1);
  }
  void beginBlue() {
    publisher = std::thread([this] { while (!stop.load()) { publish(true); std::this_thread::sleep_for(std::chrono::milliseconds(16)); } });
  }
  CvMonitorStats stats(void* lease = nullptr) {
    CvMonitorStats result{};
    cv_monitor_snapshot(lease ? lease : owner, &result);
    return result;
  }
  bool pixel(std::uint32_t value, void* lease = nullptr) {
    for (int i = 0; i < 200; ++i) {
      const auto result = stats(lease);
      if (result.presents && result.pixelValid && result.pixel == value) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
  }
  ~MonitorRig() {
    stop.store(true); if (publisher.joinable()) publisher.join();
    if (owner) cv_monitor_release(owner);
    if (window) DestroyWindow(window);
  }
};
}

TEST(NativeMonitorPresenter, VersionedAbiRejectsMissingResources) {
  EXPECT_EQ(cv_monitor_stats_size(), sizeof(CvMonitorStats));
  EXPECT_TRUE(cv_monitor_create(nullptr, nullptr, nullptr, nullptr, 1, nullptr, 0) == nullptr);
}
TEST(NativeMonitorPresenter, ActualPixelsAdvanceWithoutControlPolling) {
  MonitorRig rig;
  rig.publish(false);
  EXPECT_TRUE(rig.pixel(0xffff0000));
  const auto before = rig.stats().presents;
  rig.beginBlue();
  std::this_thread::sleep_for(std::chrono::milliseconds(300)); // No control calls drive this interval.
  EXPECT_TRUE(rig.stats().presents >= before + 8);
  const auto observed = rig.stats();
  EXPECT_TRUE(observed.gaps >= 8);
  EXPECT_EQ(observed.gaps, observed.presents - 1);
  EXPECT_TRUE(observed.gapsOver50Ms <= observed.gapsOver33Ms && observed.gapsOver33Ms <= observed.gapsOver25Ms);
  EXPECT_TRUE(rig.pixel(0xff0000ff));
}
TEST(NativeMonitorPresenter, SuspensionStopsAndResumeUsesActualLatestPixels) {
  MonitorRig rig; rig.publish(false); EXPECT_TRUE(rig.pixel(0xffff0000));
  cv_monitor_active(rig.owner, 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const auto before = rig.stats().presents;
  rig.beginBlue();
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  EXPECT_EQ(rig.stats().presents, before);
  EXPECT_EQ(rig.stats().active, 0u);
  cv_monitor_active(rig.owner, 1);
  EXPECT_TRUE(rig.pixel(0xff0000ff));
  EXPECT_TRUE(rig.stats().presents > before);
}
TEST(NativeMonitorPresenter, RetiringOneAliasKeepsOtherAliasAndProducerAlive) {
  MonitorRig rig;
  void* alias = rig.create(); EXPECT_TRUE(alias != nullptr);
  rig.publish(false); EXPECT_TRUE(rig.pixel(0xffff0000)); EXPECT_TRUE(rig.pixel(0xffff0000, alias));
  const auto before = rig.stats(alias).presents;
  cv_monitor_release(rig.owner); rig.owner = nullptr;
  rig.beginBlue(); std::this_thread::sleep_for(std::chrono::milliseconds(250));
  EXPECT_TRUE(rig.stats(alias).presents >= before + 6);
  EXPECT_TRUE(rig.pixel(0xff0000ff, alias));
  cv_monitor_release(alias);
}
TEST(NativeMonitorPresenter, CrossDeviceResourceMixIsRefused) {
  MonitorRig rig;
  EXPECT_TRUE(cv_monitor_create(rig.producer.Get(), rig.consumerContext.Get(), rig.chain.Get(),
      rig.backBuffer.Get(), rig.handle, nullptr, 0) == nullptr);
}
