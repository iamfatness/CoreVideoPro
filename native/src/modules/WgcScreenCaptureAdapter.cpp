// Screen capture via Windows.Graphics.Capture (docs/capture-sources-spec.md
// Phase SC). Monitors enumerate as capture devices ("screen:<n>"); connect()
// starts a WGC session; frames arrive on WGC's free-threaded callback where
// the staging copy happens (LAW: no pixel work under shared locks or on the
// poll caller's tick - pollVideoFrames only swaps the latest ready frame out
// under a small mutex). Dev-gated behind COREVIDEO_WITH_WGC, same pattern as
// the UVC adapter; returns nullptr when the flag is off.

#include "core/FrameAllocation.h"
#include "modules/Interfaces.h"

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

#include <atomic>
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

// One active WGC session: frame pool + session + the latest BGRA frame, copied
// on WGC's own callback thread.
class WgcSession {
 public:
  // Drain any in-flight FrameArrived callback before this object's D3D members are
  // destroyed — the free-threaded frame pool can invoke onFrame on a DWM thread, and
  // the revoker does not wait for a callback already running (this was crashing the
  // whole core: WgcSession::onFrame deref'ing a torn-down context_). 2026-07-10.
  ~WgcSession() { stop(); }

  bool start(const MonitorTarget& target) {
    const char* gpuCapture = std::getenv("COREVIDEO_GPU_CAPTURE");
    gpuEnabled_ = gpuCapture && std::string(gpuCapture) == "1";
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                                 device_.GetAddressOf(), nullptr, context_.GetAddressOf()))) {
      return false;
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
    framePool_ = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
        winrtDevice, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, item.Size());
    if (gpuEnabled_)
      worker_ = std::make_unique<CaptureFrameWorker<CapturedFrame>>(
          [this](const CapturedFrame& frame) { processFrame(frame); });
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
      session_.Close();
      session_ = nullptr;
    }
    if (framePool_) {
      framePool_.Close();
      framePool_ = nullptr;
    }
  }

  // Poll side: SHARE the latest frame every call (tiny lock, no pixel work).
  // The compositor holds per-source textures and re-uploads only when the
  // frameId changes, so returning the same buffer every tick is free - and
  // take-and-clear made the source VANISH on ticks between WGC deliveries
  // (owner-reported flashing in the multiviewer).
  bool getLatest(std::shared_ptr<const std::vector<std::uint8_t>>& outBgra, int& outWidth,
                 int& outHeight, std::int64_t& outFrameId, std::shared_ptr<const GpuVideoFrame>& outGpu) {
    std::lock_guard<std::mutex> lock(latestMutex_);
    if ((!latestBgra_ || latestBgra_->empty()) && !latestGpu_) {
      return false;
    }
    outBgra = latestBgra_;
    outWidth = latestWidth_;
    outHeight = latestHeight_;
    outFrameId = latestFrameId_;
    outGpu = latestGpu_;
    return true;
  }

  int width() const { return width_; }
  int height() const { return height_; }
  void requireCpu(bool required) { cpuRequired_.store(required, std::memory_order_release); }
  uint64_t droppedFrames() const {
    const auto stats = worker_ ? worker_->stats() : CaptureFrameWorker<CapturedFrame>::Stats{};
    return stats.superseded + stats.failed + stats.refused + refusedFrames_.load();
  }

 private:
  struct CapturedFrame {
    wgc::Direct3D11CaptureFrame frame{nullptr};
    int64_t sequence = 0;
  };
  void onFrame(const wgc::Direct3D11CaptureFramePool& pool) {
    // Stop drains this callback before stopping the owner worker. The callback
    // only transfers one OS frame when GPU ingress is enabled.
    std::lock_guard<std::mutex> frameLock(frameMutex_);
    if (!running_.load(std::memory_order_acquire)) {
      return;
    }
    auto frame = pool.TryGetNextFrame();
    if (!frame) {
      return;
    }
    try {
      CapturedFrame captured{std::move(frame), ++captureSequence_};
      if (worker_) worker_->submit(std::move(captured), cpuRequired_.load(std::memory_order_acquire));
      else processFrame(captured);
    } catch (...) { ++refusedFrames_; }
  }

  void processFrame(const CapturedFrame& captured) {
    if (!running_.load(std::memory_order_acquire)) return;
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
    int gpuSlot = -1;
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
    } catch (...) {
      // Optional GPU admission must not throw out of the OS capture callback.
      // Preserve the existing CPU path on resource/allocation failure.
      gpuSlot = -1;
    }
    // Only the capture owner polls its immediate context. A completed GPU
    // image does not need a CPU Map as a readiness barrier. The bounded wait
    // runs on the capture worker, never on Program or WGC's OS callback.
    std::shared_ptr<const GpuVideoFrame> gpu;
    if (gpuSlot >= 0 && !cpuRequired_.load(std::memory_order_acquire)) {
      context_->Flush();
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
      do {
        gpu = gpuPool_->completed(context_.Get(), gpuSlot);
        if (gpu || !running_.load(std::memory_order_acquire)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      } while (std::chrono::steady_clock::now() < deadline);
      if (gpu) {
        publish(captured.sequence, desc.Width, desc.Height, {}, std::move(gpu), false);
        return;
      }
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
    publish(captured.sequence, stagingWidth_, stagingHeight_,
        std::make_shared<const std::vector<std::uint8_t>>(std::move(bgra)), std::move(gpu), true);
  }

  void publish(int64_t sequence, int width, int height,
      std::shared_ptr<const std::vector<uint8_t>> bgra, std::shared_ptr<const GpuVideoFrame> gpu, bool cpuMirror) {
    if (gpu) ++gpuFrames_;
    else if (gpuEnabled_) ++gpuFallbackFrames_;
    if (cpuMirror) ++cpuReadbacks_;
    {
      std::lock_guard<std::mutex> lock(latestMutex_);
      latestBgra_ = std::move(bgra);
      latestGpu_ = std::move(gpu);
      latestWidth_ = width;
      latestHeight_ = height;
      latestFrameId_ = sequence;
      if (gpuEnabled_ && latestFrameId_ % 120 == 0)
        core::nativeLogf("[wgc-gpu-ingress] ready=%llu cpuFallback=%llu cpuMirror=%d cpuReadbacks=%llu residentBytes=%llu generation=%llu\n",
            static_cast<unsigned long long>(gpuFrames_), static_cast<unsigned long long>(gpuFallbackFrames_),
            cpuMirror ? 1 : 0, static_cast<unsigned long long>(cpuReadbacks_),
            static_cast<unsigned long long>(D3DVideoImage::residentBytes.load()), static_cast<unsigned long long>(gpuGeneration_));
    }
  }

  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<ID3D11Texture2D> staging_;
  bool gpuEnabled_ = false;
  std::atomic<bool> cpuRequired_{true}; // compatibility until explicit demand arrives
  std::atomic<uint64_t> refusedFrames_{0};
  int64_t captureSequence_ = 0;
  uint64_t cpuReadbacks_ = 0;
  std::unique_ptr<CaptureFrameWorker<CapturedFrame>> worker_;
  uint64_t gpuGeneration_ = 0, gpuConsumerRevision_ = 0, gpuFrames_ = 0, gpuFallbackFrames_ = 0;
  std::unique_ptr<D3DVideoFramePool> gpuPool_, retiredGpuPool_;
  std::shared_ptr<const GpuVideoFrame> latestGpu_;
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
  int width_ = 0;
  int height_ = 0;
};

class WgcScreenCaptureDevice : public ICaptureDevice {
 public:
  void setVideoConsumerDemand(const std::vector<SourceVideoDemand>& demands) override {
    std::lock_guard<std::mutex> lock(mutex_);
    demands_ = demands;
    demandKnown_ = true;
    for (auto& [id, session] : sessions_)
      session->requireCpu(sourceNeedsCpuVideo(demands_, "capture:" + id));
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
    std::lock_guard<std::mutex> lock(mutex_);
    auto session = sessions_.find(deviceId);
    if (session != sessions_.end()) {
      session->second->stop();
      sessions_.erase(session);
    }
    return infosLocked();
  }

  std::vector<CaptureDeviceInfo> connect(const std::string& deviceId) override {
    std::lock_guard<std::mutex> lock(mutex_);
    auto targets = enumerateMonitors();
    const auto windows = enumerateWindows();
    targets.insert(targets.end(), windows.begin(), windows.end());
    for (const auto& target : targets) {
      if (target.id != deviceId) {
        continue;
      }
      auto existing = sessions_.find(deviceId);
      if (existing != sessions_.end()) {
        existing->second->stop();
        sessions_.erase(existing);
      }
      auto session = std::make_unique<WgcSession>();
      if (demandKnown_) session->requireCpu(sourceNeedsCpuVideo(demands_, "capture:" + deviceId));
      if (session->start(target)) {
        sessions_[deviceId] = std::move(session);
      }
      break;
    }
    return infosLocked();
  }

  void captureVideoTick(int64_t timestampMs) override {
    std::vector<VideoFrame> frames;
    {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [deviceId, session] : sessions_) {
      std::shared_ptr<const std::vector<std::uint8_t>> bgra;
      int width = 0;
      int height = 0;
      std::int64_t frameId = 0;
      std::shared_ptr<const GpuVideoFrame> gpu;
      if (!session->getLatest(bgra, width, height, frameId, gpu)) {
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
      frame.pixels = bgra;
      frame.gpuPixels = std::move(gpu);
      frame.pixelWidth = width;
      frame.pixelHeight = height;
      frame.pixelStride = width * 4;
      frames.push_back(std::move(frame));
    }
    }
    replaceVideo(std::move(frames));
  }

 private:
  std::vector<CaptureDeviceInfo> infosLocked() const {
    std::vector<CaptureDeviceInfo> infos;
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
      const bool live = sessions_.count(target.id) != 0;
      info.connectionState = live ? "connected" : "detected";
      info.signalPresent = live;
      if (live) info.droppedFrames = static_cast<int64_t>(sessions_.at(target.id)->droppedFrames());
      infos.push_back(std::move(info));
    }
    return infos;
  }

  mutable std::mutex mutex_;
  std::map<std::string, std::unique_ptr<WgcSession>> sessions_;
  bool demandKnown_ = false;
  std::vector<SourceVideoDemand> demands_;
};

}  // namespace

std::unique_ptr<ICaptureDevice> createWgcScreenCaptureDevice() {
  return std::make_unique<WgcScreenCaptureDevice>();
}

}  // namespace corevideo::modules

#else  // COREVIDEO_WITH_WGC

namespace corevideo::modules {
std::unique_ptr<ICaptureDevice> createWgcScreenCaptureDevice() { return nullptr; }
}  // namespace corevideo::modules

#endif  // COREVIDEO_WITH_WGC
