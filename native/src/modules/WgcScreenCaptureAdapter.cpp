// Screen capture via Windows.Graphics.Capture (docs/capture-sources-spec.md
// Phase SC). Monitors enumerate as capture devices ("screen:<n>"); connect()
// starts a WGC session; frames arrive on WGC's free-threaded callback. GPU
// ingress transfers processing to its bounded owner worker (LAW: no pixel work on the
// poll caller's tick - pollVideoFrames only swaps the latest ready frame out
// under a small mutex). Dev-gated behind COREVIDEO_WITH_WGC, same pattern as
// the UVC adapter; returns nullptr when the flag is off.

#include "core/FrameAllocation.h"
#include "modules/Interfaces.h"
#include "modules/CpuSourcePreparation.h"

#if defined(COREVIDEO_WITH_WGC) && COREVIDEO_WITH_WGC

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include "modules/D3DVideoFrame.h"
#include "modules/CaptureFrameWorker.h"
#include "modules/CaptureSessionLifecycle.h"
#include "modules/D3DCaptureCpuBranch.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace corevideo::modules {
namespace {

using Microsoft::WRL::ComPtr;
namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;

struct MonitorTarget {
  HMONITOR monitor = nullptr;
  HWND window = nullptr;   // set for window targets; monitor is null then
  bool isWindow = false;
  std::string id;          // "screen:<n>" or "window:<hwnd-hex>"
  std::string name;
  int width = 0;
  int height = 0;
};

// Top-level, visible, titled application windows are capturable sources
// (capture-sources-spec: capture a browser, a slide deck, any app). Tool
// windows, our own windows, and cloaked/hidden windows are excluded.
inline std::vector<MonitorTarget> enumerateWindows() {
  std::vector<MonitorTarget> targets;
  EnumWindows(
      [](HWND hwnd, LPARAM state) -> BOOL {
        auto* list = reinterpret_cast<std::vector<MonitorTarget>*>(state);
        if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) {
          return TRUE;
        }
        const LONG exStyle = GetWindowLong(hwnd, GWL_EXSTYLE);
        if ((exStyle & WS_EX_TOOLWINDOW) != 0) {
          return TRUE;
        }
        char title[256] = {};
        const int len = GetWindowTextA(hwnd, title, sizeof(title));
        if (len <= 0) {
          return TRUE;
        }
        RECT rect{};
        if (!GetWindowRect(hwnd, &rect)) {
          return TRUE;
        }
        const int w = rect.right - rect.left;
        const int h = rect.bottom - rect.top;
        if (w < 64 || h < 64) {
          return TRUE;  // tray/zero-size helpers
        }
        MonitorTarget target;
        target.window = hwnd;
        target.isWindow = true;
        target.width = w;
        target.height = h;
        char idbuf[32] = {};
        std::snprintf(idbuf, sizeof(idbuf), "window:%p", reinterpret_cast<void*>(hwnd));
        target.id = idbuf;
        target.name = std::string(title, static_cast<size_t>(len));
        list->push_back(std::move(target));
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&targets));
  return targets;
}

std::vector<MonitorTarget> enumerateMonitors() {
  std::vector<MonitorTarget> targets;
  EnumDisplayMonitors(
      nullptr, nullptr,
      [](HMONITOR handle, HDC, LPRECT rect, LPARAM state) -> BOOL {
        auto* list = reinterpret_cast<std::vector<MonitorTarget>*>(state);
        MonitorTarget target;
        target.monitor = handle;
        target.width = rect->right - rect->left;
        target.height = rect->bottom - rect->top;
        target.id = "screen:" + std::to_string(list->size());
        MONITORINFOEXA info{};
        info.cbSize = sizeof(info);
        std::string deviceName = "Display " + std::to_string(list->size() + 1);
        if (GetMonitorInfoA(handle, &info)) {
          deviceName = info.szDevice;
          if (deviceName.rfind("\\\\.\\", 0) == 0) {
            deviceName = deviceName.substr(4);
          }
        }
        target.name = deviceName + " (" + std::to_string(target.width) + "x" +
                      std::to_string(target.height) + ")";
        list->push_back(std::move(target));
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&targets));
  return targets;
}

// One active WGC session with immutable completed representations. GPU ingress
// uses an owner worker when either preparation path is enabled; the legacy
// compatibility path runs on the callback.
class WgcSession {
 public:
  explicit WgcSession(std::shared_ptr<CpuSourcePreparation> preparation)
      : preparation_(std::move(preparation)) {}
  // Drain any in-flight FrameArrived callback before this object's D3D members are
  // destroyed — the free-threaded frame pool can invoke onFrame on a DWM thread, and
  // the revoker does not wait for a callback already running (this was crashing the
  // whole core: WgcSession::onFrame deref'ing a torn-down context_). 2026-07-10.
  ~WgcSession() { stop(); }

  bool start(const MonitorTarget& target, bool gpuEnabled) {
    sourceId_ = "capture:" + target.id;
    sourceEpoch_ = ++nextSourceEpoch_;
    // WGC supplies the compositor's QPC timestamp in 100 ns units. Calibrate
    // its origin to the core's steady clock instead of stamping at conversion.
    LARGE_INTEGER before{}, after{}, frequency{};
    if (QueryPerformanceFrequency(&frequency) && QueryPerformanceCounter(&before)) {
      const auto steady = std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count() / 100;
      if (QueryPerformanceCounter(&after)) {
        const auto midpoint = before.QuadPart + (after.QuadPart - before.QuadPart) / 2;
        const auto qpc100ns = midpoint / frequency.QuadPart * 10000000 +
            midpoint % frequency.QuadPart * 10000000 / frequency.QuadPart;
        captureClockOffset100ns_ = steady - qpc100ns;
        captureClockCalibrated_ = true;
      }
    }
    gpuEnabled_ = gpuEnabled;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                                 device_.GetAddressOf(), nullptr, context_.GetAddressOf()))) {
      return false;
    }
    if (gpuEnabled_) {
      D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
      if (FAILED(device_->CreateQuery(&query, &captureReleaseQuery_))) return false;
    }
    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(device_.As(&dxgiDevice))) {
      return false;
    }
    winrt::com_ptr<IInspectable> inspectable;
    if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(),
                                                    reinterpret_cast<::IInspectable**>(
                                                        winrt::put_abi(inspectable))))) {
      return false;
    }
    const auto winrtDevice =
        inspectable.as<wgd::Direct3D11::IDirect3DDevice>();

    auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem>()
                       .as<IGraphicsCaptureItemInterop>();
    wgc::GraphicsCaptureItem item{nullptr};
    const HRESULT created = target.isWindow
        ? interop->CreateForWindow(target.window, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                   winrt::put_abi(item))
        : interop->CreateForMonitor(target.monitor, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                    winrt::put_abi(item));
    if (FAILED(created)) {
      return false;
    }
    width_ = item.Size().Width;
    height_ = item.Size().Height;
    if (gpuEnabled_) {
      // Device/context creation belongs to the session lifecycle owner. CPU
      // pool generations are subsequently prepared on their own worker.
      try {
        auto cpu = std::make_shared<D3DCaptureCpuBranch>(device_.Get(), sourceId_);
        if (cpu->valid()) { std::lock_guard<std::mutex> lock(cpuBranchMutex_); cpuBranch_ = std::move(cpu); }
        else ++cpuAdmissionFailed_;
      } catch (...) { ++cpuAdmissionFailed_; }
    }
    framePool_ = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
        winrtDevice, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, item.Size());
    if (gpuEnabled_ || preparation_)
      worker_ = std::make_unique<CaptureFrameWorker<CapturedFrame>>(
          [this](const CapturedFrame& frame) { processFrame(frame); },
          [this] {
            pollCapturedRetirement();
            // A static screen may deliver no next OS frame. Retire the last
            // CPU copy on the capture owner rather than stranding its ISO view.
            if (auto cpu = cpuBranch()) cpu->publishReady(context_.Get());
            refreshCpuPreparation();
          }, [this] {
            if (auto cpu = cpuBranch()) cpu->stopOnCaptureOwner(context_.Get());
            context_->Flush();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (retiringCapture_ && std::chrono::steady_clock::now() < deadline) {
              pollCapturedRetirement();
              if (retiringCapture_) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (retiringCapture_) {
              std::lock_guard<std::mutex> lock(captureQuarantineMutex_);
              captureQuarantine_.push_back(std::move(retiringCapture_));
            }
          });
    frameArrived_ = framePool_.FrameArrived(
        winrt::auto_revoke, [this](const wgc::Direct3D11CaptureFramePool& pool, const auto&) {
          onFrame(pool);
        });
    session_ = framePool_.CreateCaptureSession(item);
    // No capture border (owner: no orange frame around the captured display).
    // Requires Win10 21H1+/Win11; harmless no-op where unsupported.
    try {
      session_.IsBorderRequired(false);
    } catch (...) {
    }
    try {
      session_.IsCursorCaptureEnabled(true);
    } catch (...) {
    }
    session_.StartCapture();
    running_.store(true, std::memory_order_release);
    return true;
  }

  void stop() {
    running_.store(false, std::memory_order_release);
    frameArrived_.revoke();
    // Wait out any callback that already passed the running_ check before we close the
    // pool / free the D3D members it is using. revoke() unsubscribes future calls but
    // does NOT synchronize with one in flight on the free-threaded pool thread.
    { std::lock_guard<std::mutex> drain(frameMutex_); }
    if (worker_) worker_->stop();
    if (session_) {
      auto closing = std::exchange(session_, nullptr);
      try { closing.Close(); }
      catch (...) { core::nativeLogf("[wgc-lifecycle] capture session close failed\n"); }
    }
    if (framePool_) {
      auto closing = std::exchange(framePool_, nullptr);
      try { closing.Close(); }
      catch (...) { core::nativeLogf("[wgc-lifecycle] frame pool close failed\n"); }
    }
  }

  // Poll side: SHARE the latest frame every call (tiny lock, no pixel work).
  // The compositor holds per-source textures and re-uploads only when the
  // frameId changes, so returning the same buffer every tick is free - and
  // take-and-clear made the source VANISH on ticks between WGC deliveries
  // (owner-reported flashing in the multiviewer).
  bool getLatest(std::shared_ptr<const std::vector<std::uint8_t>>& outBgra, int& outWidth,
                 int& outHeight, std::int64_t& outFrameId, std::shared_ptr<const GpuVideoFrame>& outGpu,
                 std::shared_ptr<const GpuVideoFrame>& outMonitorGpu, int64_t& outCapture100ns,
                 uint64_t& outEpoch, std::shared_ptr<CpuSourceGpuView>& outPrepared) {
    std::lock_guard<std::mutex> lock(latestMutex_);
    if ((!latestBgra_ || latestBgra_->empty()) && !latestGpu_) {
      return false;
    }
    outBgra = latestBgra_;
    outWidth = latestWidth_;
    outHeight = latestHeight_;
    outFrameId = latestFrameId_;
    outGpu = latestGpu_;
    outMonitorGpu = latestMonitorGpu_;
    outCapture100ns = latestCapture100ns_;
    outEpoch = latestSourceEpoch_;
    outPrepared = latestPrepared_;
    return true;
  }

  int width() const { return width_; }
  int height() const { return height_; }
  void requireCpu(bool required) { cpuRequired_.store(required, std::memory_order_release); }
  std::vector<VideoFrame> takeCpuFrames() { if (auto cpu = cpuBranch()) return cpu->take(); return {}; }
  std::string cpuWarning() const {
    if (cpuAdmissionFailed_.load()) return "CPU/ISO branch admission failed: " + std::to_string(cpuAdmissionFailed_.load());
    if (auto cpu = cpuBranch()) {
      const auto stats = cpu->stats();
      if (stats.capacityRefused || stats.queueRefused || stats.failed || stats.outputRefused || stats.preparationRefused)
        return "CPU/ISO conversion loss: capacity=" + std::to_string(stats.capacityRefused) +
            " queue=" + std::to_string(stats.queueRefused) + " failed=" + std::to_string(stats.failed) +
            " output=" + std::to_string(stats.outputRefused) + " preparation=" + std::to_string(stats.preparationRefused);
    }
    return {};
  }
  uint64_t droppedFrames() const {
    const auto stats = worker_ ? worker_->stats() : CaptureFrameWorker<CapturedFrame>::Stats{};
    return stats.superseded + stats.failed + stats.refused + refusedFrames_.load();
  }

 private:
  struct CapturedFrame {
    wgc::Direct3D11CaptureFrame frame{nullptr};
    int64_t sequence = 0;
    int64_t capture100ns = 0;
  };
  std::shared_ptr<D3DCaptureCpuBranch> cpuBranch() const {
    std::lock_guard<std::mutex> lock(cpuBranchMutex_); return cpuBranch_;
  }
  void pollCapturedRetirement() {
    if (!retiringCapture_) return;
    BOOL ready = FALSE;
    if (context_->GetData(captureReleaseQuery_.Get(), &ready, sizeof(ready), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && ready) {
      retiringCapture_ = nullptr;
    }
  }
  void retainUntilSourceCopiesComplete(const wgc::Direct3D11CaptureFrame& frame) {
    context_->End(captureReleaseQuery_.Get()); context_->Flush();
    retiringCapture_ = frame; pollCapturedRetirement();
  }
  struct CaptureCopyLease {
    WgcSession& owner; const wgc::Direct3D11CaptureFrame& frame;
    ~CaptureCopyLease() { owner.retainUntilSourceCopiesComplete(frame); }
  };
  void onFrame(const wgc::Direct3D11CaptureFramePool& pool) {
    // Stop drains this callback before stopping the owner worker. The callback
    // only transfers one OS frame when a preparation path is enabled.
    std::lock_guard<std::mutex> frameLock(frameMutex_);
    if (!running_.load(std::memory_order_acquire)) {
      return;
    }
    auto frame = pool.TryGetNextFrame();
    if (!frame) {
      return;
    }
    try {
      const auto capture100ns = captureClockCalibrated_
          ? frame.SystemRelativeTime().count() + captureClockOffset100ns_ : 0;
      CapturedFrame captured{std::move(frame), ++captureSequence_, capture100ns};
      if (worker_) worker_->submit(std::move(captured), !gpuEnabled_ && cpuRequired_.load());
      else processFrame(captured);
    } catch (...) { ++refusedFrames_; }
  }

  void processFrame(const CapturedFrame& captured) {
    if (!running_.load(std::memory_order_acquire)) return;
    if (gpuEnabled_) {
      pollCapturedRetirement();
      if (retiringCapture_) { ++refusedFrames_; return; }
    }
    auto surface = captured.frame.Surface();
    winrt::com_ptr<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess> access;
    if (!surface || FAILED(winrt::get_unknown(surface)->QueryInterface(
                        winrt::guid_of<::Windows::Graphics::DirectX::Direct3D11::
                                           IDirect3DDxgiInterfaceAccess>(),
                        access.put_void()))) {
      return;
    }
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(access->GetInterface(IID_PPV_ARGS(texture.GetAddressOf())))) {
      return;
    }
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    // Capture owner fences geometry before either CPU or GPU publication.
    // The published descriptor snapshots this epoch together with its pixels.
    if (captureWidth_ && (captureWidth_ != desc.Width || captureHeight_ != desc.Height))
      sourceEpoch_ = ++nextSourceEpoch_;
    captureWidth_ = desc.Width; captureHeight_ = desc.Height;
    std::optional<CaptureCopyLease> captureLease;
    if (gpuEnabled_) captureLease.emplace(*this, captured.frame);
    int gpuSlot = -1;
    int monitorSlot = -1;
    try {
    if (gpuEnabled_ && desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM) {
      if (retiredGpuPool_ && retiredGpuPool_->idle(context_.Get())) retiredGpuPool_.reset();
      const auto revision = D3DVideoConsumers::revision();
      if ((!gpuPool_ || !gpuPool_->dimensions(desc.Width, desc.Height) || revision != gpuConsumerRevision_) &&
          !retiredGpuPool_) {
        auto candidate = std::make_unique<D3DVideoFramePool>();
        if (candidate->initialize(device_.Get(), desc.Width, desc.Height, ++gpuGeneration_)) {
          retiredGpuPool_ = std::move(gpuPool_);
          gpuPool_ = std::move(candidate);
          gpuConsumerRevision_ = revision;
        }
      }
      if (gpuPool_ && gpuPool_->dimensions(desc.Width, desc.Height))
        gpuSlot = gpuPool_->beginCopy(context_.Get(), texture.Get());
    }
    } catch (...) { gpuSlot = -1; }
    try {
    if (gpuEnabled_ && desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM) {
      const auto revision = D3DVideoConsumers::revision();
      // Optional residency and copies are independent of the production pool.
      // Exhaustion refuses this branch; it cannot hold production slots hostage.
      if (retiredMonitorPool_ && retiredMonitorPool_->idle(context_.Get())) retiredMonitorPool_.reset();
      const auto consumers = D3DVideoConsumers::snapshot();
      const bool monitorDemand = std::any_of(consumers.begin(), consumers.end(),
          [](const auto& consumer) { return consumer->monitor; });
      monitorDemand_ = monitorDemand;
      if (monitorDemand && (!monitorPool_ || !monitorPool_->dimensions(desc.Width, desc.Height) ||
          revision != monitorConsumerRevision_) && !retiredMonitorPool_) {
        auto candidate = std::make_unique<D3DVideoFramePool>();
        if (candidate->initialize(device_.Get(), desc.Width, desc.Height, ++monitorGeneration_, true)) {
          retiredMonitorPool_ = std::move(monitorPool_);
          monitorPool_ = std::move(candidate);
          monitorConsumerRevision_ = revision;
        }
      }
      if (monitorDemand && monitorPool_ && monitorPool_->dimensions(desc.Width, desc.Height))
        monitorSlot = monitorPool_->beginCopy(context_.Get(), texture.Get());
    }
    } catch (...) {
      // Optional monitor allocation cannot discard a valid production copy.
      monitorSlot = -1;
    }
    // Only the capture owner polls its immediate context. A completed GPU
    // image does not need a CPU Map as a readiness barrier. The bounded wait
    // runs on the capture worker, never on Program or WGC's OS callback.
    std::shared_ptr<const GpuVideoFrame> gpu;
    try {
    if (gpuEnabled_ && cpuRequired_.load(std::memory_order_acquire)) {
      auto cpu = cpuBranch();
      if (cpu) cpu->copy(context_.Get(), texture.Get(), captured.sequence, sourceEpoch_, captured.capture100ns);
      else ++cpuAdmissionFailed_;
    } else if (auto cpu = cpuBranch()) { cpu->publishReady(context_.Get()); }
    } catch (...) { ++cpuAdmissionFailed_; } // CPU allocation/admission cannot discard Program's copy
    if (gpuEnabled_ && gpuSlot < 0 &&
        gpuPool_ && gpuPool_->dimensions(desc.Width, desc.Height)) {
      ++gpuCapacityRefused_;
      ++refusedFrames_;
      return; // a leased slot is pressure, not an unsupported-sharing fallback
    }
    if (gpuSlot >= 0) {
      captureLease.reset(); // fence all source copies in their submission batch
      context_->Flush();
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
      do {
        gpu = gpuPool_->completed(context_.Get(), gpuSlot);
        if (gpu || !running_.load(std::memory_order_acquire)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      } while (std::chrono::steady_clock::now() < deadline);
      if (!gpu) gpu = gpuPool_->completed(context_.Get(), gpuSlot);
      if (gpu) {
        auto monitorGpu = monitorPool_ ? monitorPool_->completed(context_.Get(), monitorSlot) : nullptr;
        publish(captured.sequence, desc.Width, desc.Height, {}, std::move(gpu), std::move(monitorGpu), false, captured.capture100ns);
        return;
      }
      ++gpuNotReady_;
      ++refusedFrames_;
      return; // retain last completed GPU content; do not force a CPU round trip
    }
    if (!running_.load(std::memory_order_acquire)) return;
    if (staging_ == nullptr || stagingWidth_ != static_cast<int>(desc.Width) ||
        stagingHeight_ != static_cast<int>(desc.Height)) {
      D3D11_TEXTURE2D_DESC stagingDesc = desc;
      stagingDesc.Usage = D3D11_USAGE_STAGING;
      stagingDesc.BindFlags = 0;
      stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      stagingDesc.MiscFlags = 0;
      staging_.Reset();
      if (FAILED(device_->CreateTexture2D(&stagingDesc, nullptr, staging_.GetAddressOf()))) {
        return;
      }
      stagingWidth_ = static_cast<int>(desc.Width);
      stagingHeight_ = static_cast<int>(desc.Height);
    }
    context_->CopyResource(staging_.Get(), texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT mappedResult = E_FAIL;
    if (gpuEnabled_) {
      context_->Flush();
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
      do {
        if (!running_.load(std::memory_order_acquire)) return;
        mappedResult = context_->Map(staging_.Get(), 0, D3D11_MAP_READ,
                                    D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (mappedResult != DXGI_ERROR_WAS_STILL_DRAWING) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      } while (std::chrono::steady_clock::now() < deadline);
    } else {
      mappedResult = context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    }
    if (FAILED(mappedResult)) {
      ++refusedFrames_;
      return;
    }
    // #728: a frame that cannot be allocated is dropped; this is an OS callback
    // thread, so an escaping std::bad_alloc would terminate the core.
    static core::FrameAllocationFailures allocationFailures("screen-capture");
    const size_t bgraBytes = static_cast<size_t>(stagingWidth_) * stagingHeight_ * 4;
    std::vector<std::uint8_t> bgra;
    if (!core::tryResizeFrameBuffer(bgra, bgraBytes)) {
      context_->Unmap(staging_.Get(), 0);
      allocationFailures.note(bgraBytes);
      return;
    }
    const auto* src = static_cast<const std::uint8_t*>(mapped.pData);
    for (int row = 0; row < stagingHeight_; ++row) {
      std::memcpy(bgra.data() + static_cast<size_t>(row) * stagingWidth_ * 4,
                  src + static_cast<size_t>(row) * mapped.RowPitch,
                  static_cast<size_t>(stagingWidth_) * 4);
    }
    context_->Unmap(staging_.Get(), 0);
    // Keep the CPU representation for explicit CPU consumers or GPU fallback. The
    // production compositor can now read the pre-imported full-size GPU image
    // directly; it no longer uploads these BGRA bytes to its context.
    gpu = gpuPool_ ? gpuPool_->completed(context_.Get(), gpuSlot) : nullptr;
    auto monitorGpu = monitorPool_ ? monitorPool_->completed(context_.Get(), monitorSlot) : nullptr;
    publish(captured.sequence, stagingWidth_, stagingHeight_,
        std::make_shared<const std::vector<std::uint8_t>>(std::move(bgra)), std::move(gpu), std::move(monitorGpu), true, captured.capture100ns);
  }

  void publish(int64_t sequence, int width, int height,
      std::shared_ptr<const std::vector<uint8_t>> bgra, std::shared_ptr<const GpuVideoFrame> gpu,
      std::shared_ptr<const GpuVideoFrame> monitorGpu, bool cpuMirror, int64_t capture100ns) {
    std::shared_ptr<CpuSourceGpuView> prepared;
    if (!capture100ns) capture100ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count() / 100;
    // CPU fallback is offered on the capture owner, never the poll caller.
    // Direct GPU ingress and original CPU/ISO bytes remain independent.
    if (preparation_ && bgra && !gpu)
      prepared = preparation_->offerBgra(sourceId_, sourceEpoch_, sequence, capture100ns,
          width, height, width * 4, bgra);
    if (gpu) ++gpuFrames_;
    else if (gpuEnabled_) ++gpuFallbackFrames_;
    if (cpuMirror) ++cpuReadbacks_;
    if (monitorGpu) ++monitorFrames_;
    else if (monitorDemand_) ++monitorRefusedFrames_;
    {
      std::lock_guard<std::mutex> lock(latestMutex_);
      latestBgra_ = std::move(bgra);
      latestGpu_ = std::move(gpu);
      latestMonitorGpu_ = std::move(monitorGpu);
      latestWidth_ = width;
      latestHeight_ = height;
      latestFrameId_ = sequence;
      latestCapture100ns_ = capture100ns;
      latestSourceEpoch_ = sourceEpoch_;
      latestPrepared_ = std::move(prepared);
      if (gpuEnabled_ && latestFrameId_ % 120 == 0)
        core::nativeLogf("[wgc-gpu-ingress] ready=%llu cpuFallback=%llu cpuMirror=%d cpuReadbacks=%llu residentBytes=%llu generation=%llu monitorCopied=%llu monitorRefused=%llu monitorResidentBytes=%llu gpuCapacityRefused=%llu gpuNotReady=%llu\n",
            static_cast<unsigned long long>(gpuFrames_), static_cast<unsigned long long>(gpuFallbackFrames_),
            cpuMirror ? 1 : 0, static_cast<unsigned long long>(cpuReadbacks_),
            static_cast<unsigned long long>(D3DVideoImage::residentBytes.load()), static_cast<unsigned long long>(gpuGeneration_),
            static_cast<unsigned long long>(monitorFrames_), static_cast<unsigned long long>(monitorRefusedFrames_),
            static_cast<unsigned long long>(D3DVideoImage::monitorResidentBytes.load()),
            static_cast<unsigned long long>(gpuCapacityRefused_), static_cast<unsigned long long>(gpuNotReady_));
    }
  }

  void refreshCpuPreparation() {
    if (!preparation_) return;
    std::lock_guard<std::mutex> lock(latestMutex_);
    if (!latestBgra_ || latestGpu_ || (latestPrepared_ && latestPrepared_->demand &&
        !latestPrepared_->demand->stopped.load())) return;
    auto token = preparation_->offerBgra(sourceId_, latestSourceEpoch_, latestFrameId_,
        latestCapture100ns_, latestWidth_, latestHeight_, latestWidth_ * 4, latestBgra_);
    if (token) latestPrepared_ = std::move(token);
  }

  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<ID3D11Query> captureReleaseQuery_;
  wgc::Direct3D11CaptureFrame retiringCapture_{nullptr};
  inline static std::mutex captureQuarantineMutex_;
  inline static std::vector<wgc::Direct3D11CaptureFrame> captureQuarantine_;
  ComPtr<ID3D11Texture2D> staging_;
  bool gpuEnabled_ = false;
  std::atomic<bool> cpuRequired_{true}; // compatibility until explicit demand arrives
  std::atomic<uint64_t> refusedFrames_{0};
  int64_t captureSequence_ = 0;
  uint64_t cpuReadbacks_ = 0;
  uint64_t gpuCapacityRefused_ = 0, gpuNotReady_ = 0;
  std::unique_ptr<CaptureFrameWorker<CapturedFrame>> worker_;
  mutable std::mutex cpuBranchMutex_;
  std::shared_ptr<D3DCaptureCpuBranch> cpuBranch_;
  std::atomic<uint64_t> cpuAdmissionFailed_{0};
  std::string sourceId_;
  uint64_t sourceEpoch_ = 0;
  UINT captureWidth_ = 0, captureHeight_ = 0; // capture owner only
  std::shared_ptr<CpuSourcePreparation> preparation_;
  bool captureClockCalibrated_ = false;
  int64_t captureClockOffset100ns_ = 0;
  inline static std::atomic<uint64_t> nextSourceEpoch_{0};
  uint64_t gpuGeneration_ = 0, gpuConsumerRevision_ = 0, gpuFrames_ = 0, gpuFallbackFrames_ = 0;
  std::unique_ptr<D3DVideoFramePool> gpuPool_, retiredGpuPool_;
  std::shared_ptr<const GpuVideoFrame> latestGpu_;
  uint64_t monitorGeneration_ = 0, monitorConsumerRevision_ = 0;
  bool monitorDemand_ = false;
  uint64_t monitorFrames_ = 0, monitorRefusedFrames_ = 0;
  std::unique_ptr<D3DVideoFramePool> monitorPool_, retiredMonitorPool_;
  std::shared_ptr<const GpuVideoFrame> latestMonitorGpu_;
  int stagingWidth_ = 0;
  int stagingHeight_ = 0;
  wgc::Direct3D11CaptureFramePool framePool_{nullptr};
  wgc::GraphicsCaptureSession session_{nullptr};
  wgc::Direct3D11CaptureFramePool::FrameArrived_revoker frameArrived_;
  std::atomic<bool> running_{false};
  // Serializes onFrame against stop()/destructor teardown of the D3D members.
  std::mutex frameMutex_;
  std::mutex latestMutex_;
  std::shared_ptr<const std::vector<std::uint8_t>> latestBgra_;
  int latestWidth_ = 0;
  int latestHeight_ = 0;
  std::int64_t latestFrameId_ = 0;
  int64_t latestCapture100ns_ = 0;
  uint64_t latestSourceEpoch_ = 0;
  std::shared_ptr<CpuSourceGpuView> latestPrepared_;
  int width_ = 0;
  int height_ = 0;
};

class WgcScreenCaptureDevice : public ICaptureDevice {
 public:
  explicit WgcScreenCaptureDevice(std::shared_ptr<CpuSourcePreparation> preparation)
      : preparation_(std::move(preparation)), gpuEnabled_([] {
    const char* value = std::getenv("COREVIDEO_GPU_CAPTURE");
    return value && std::string(value) == "1";
  }()), lifecycle_([this](const std::string& id) {
    auto targets = enumerateMonitors();
    const auto windows = enumerateWindows();
    targets.insert(targets.end(), windows.begin(), windows.end());
    for (const auto& target : targets) if (target.id == id) {
      auto session = std::make_shared<WgcSession>(preparation_);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (demandKnown_) session->requireCpu(sourceNeedsCpuVideo(demands_, "capture:" + id));
      }
      if (!session->start(target, gpuEnabled_)) return std::shared_ptr<WgcSession>{};
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (demandKnown_) session->requireCpu(sourceNeedsCpuVideo(demands_, "capture:" + id));
      }
      return session;
    }
    return std::shared_ptr<WgcSession>{};
  }, [](WgcSession& session) { session.stop(); }) {}
  std::vector<VideoFrame> takeCpuVideoFrames() override {
    std::vector<VideoFrame> result;
    for (auto& [id, session] : lifecycle_.snapshot()) {
      auto frames = session->takeCpuFrames();
      result.insert(result.end(), std::make_move_iterator(frames.begin()), std::make_move_iterator(frames.end()));
    }
    return result;
  }
  void setVideoConsumerDemand(const std::vector<SourceVideoDemand>& demands) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      demands_ = demands; demandKnown_ = true;
    }
    for (auto& [id, session] : lifecycle_.snapshot())
      session->requireCpu(sourceNeedsCpuVideo(demands, "capture:" + id));
  }
  std::vector<CaptureDeviceInfo> enumerate() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return infosLocked();
  }

  std::vector<CaptureDeviceInfo> selectInput(const std::string& deviceId,
                                             const std::string&) override {
    std::lock_guard<std::mutex> lock(mutex_);
    (void)deviceId;
    return infosLocked();
  }

  std::vector<CaptureDeviceInfo> setAudioSyncOffset(const std::string&, int) override {
    std::lock_guard<std::mutex> lock(mutex_);
    return infosLocked();
  }

  std::vector<CaptureDeviceInfo> disconnect(const std::string& deviceId) override {
    lifecycle_.disconnect(deviceId);
    std::lock_guard<std::mutex> lock(mutex_);
    return infosLocked();
  }

  std::vector<CaptureDeviceInfo> connect(const std::string& deviceId) override {
    const bool accepted = lifecycle_.connect(deviceId);
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = infosLocked();
    if (!accepted) for (auto& info : result) if (info.id == deviceId)
      info.warning = "Windows capture lifecycle capacity reached";
    return result;
  }

  void captureVideoTick(int64_t timestampMs) override {
    std::vector<VideoFrame> frames;
    for (auto& [deviceId, session] : lifecycle_.snapshot()) {
      std::shared_ptr<const std::vector<std::uint8_t>> bgra;
      int width = 0;
      int height = 0;
      std::int64_t frameId = 0;
      int64_t capture100ns = 0;
      std::shared_ptr<const GpuVideoFrame> gpu;
      std::shared_ptr<const GpuVideoFrame> monitorGpu;
      uint64_t epoch = 0;
      std::shared_ptr<CpuSourceGpuView> prepared;
      if (!session->getLatest(bgra, width, height, frameId, gpu, monitorGpu, capture100ns, epoch, prepared)) {
        continue;
      }
      VideoFrame frame;
      frame.participantId = "capture:" + deviceId;
      frame.width = width;
      frame.height = height;
      frame.naturalWidth = width;
      frame.naturalHeight = height;
      frame.timestampMs = timestampMs;
      frame.frameId = frameId;
      frame.sourceEpoch = epoch;
      frame.preparedGpu = std::move(prepared);
      frame.captureTimestamp100ns = capture100ns;
      frame.pixels = bgra;
      frame.gpuPixels = std::move(gpu);
      frame.monitorGpuPixels = std::move(monitorGpu);
      frame.pixelWidth = width;
      frame.pixelHeight = height;
      frame.pixelStride = width * 4;
      frames.push_back(std::move(frame));
    }
    replaceVideo(std::move(frames));
  }

 private:
  std::vector<CaptureDeviceInfo> infosLocked() const {
    std::vector<CaptureDeviceInfo> infos;
    const auto sessions = lifecycle_.snapshot();
    auto targets = enumerateMonitors();
    const auto windows = enumerateWindows();
    targets.insert(targets.end(), windows.begin(), windows.end());
    for (const auto& target : targets) {
      CaptureDeviceInfo info;
      info.id = target.id;
      info.name = target.name;
      info.kind = target.isWindow ? "window" : "screen";
      info.vendor = "Windows Graphics Capture";
      info.inputIds = {target.isWindow ? "window" : "screen"};
      info.inputLabels = {target.isWindow ? "Application window" : "Entire display"};
      info.inputHasEmbeddedAudio = {false};
      info.selectedInputId = target.isWindow ? "window" : "screen";
      info.width = target.width;
      info.height = target.height;
      info.frameRate = 60;
      const bool live = sessions.count(target.id) != 0;
      info.connectionState = lifecycle_.status(target.id);
      info.signalPresent = live;
      if (live) info.droppedFrames = static_cast<int64_t>(sessions.at(target.id)->droppedFrames());
      if (live) info.warning = sessions.at(target.id)->cpuWarning();
      else if (info.connectionState == "failed") info.warning = "Windows capture session could not start";
      infos.push_back(std::move(info));
    }
    return infos;
  }

  mutable std::mutex mutex_;
  bool demandKnown_ = false;
  std::vector<SourceVideoDemand> demands_;
  std::shared_ptr<CpuSourcePreparation> preparation_;
  bool gpuEnabled_ = false;
  CaptureSessionLifecycle<WgcSession> lifecycle_;
};

}  // namespace

std::unique_ptr<ICaptureDevice> createWgcScreenCaptureDevice(std::shared_ptr<CpuSourcePreparation> preparation) {
  return std::make_unique<WgcScreenCaptureDevice>(std::move(preparation));
}

}  // namespace corevideo::modules

#else  // COREVIDEO_WITH_WGC

namespace corevideo::modules {
std::unique_ptr<ICaptureDevice> createWgcScreenCaptureDevice(std::shared_ptr<CpuSourcePreparation>) { return nullptr; }
}  // namespace corevideo::modules

#endif  // COREVIDEO_WITH_WGC
