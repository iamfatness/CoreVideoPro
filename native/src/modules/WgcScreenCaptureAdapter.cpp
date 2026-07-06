// Screen capture via Windows.Graphics.Capture (docs/capture-sources-spec.md
// Phase SC). Monitors enumerate as capture devices ("screen:<n>"); connect()
// starts a WGC session; frames arrive on WGC's free-threaded callback where
// the staging copy happens (LAW: no pixel work under shared locks or on the
// poll caller's tick - pollVideoFrames only swaps the latest ready frame out
// under a small mutex). Dev-gated behind COREVIDEO_WITH_WGC, same pattern as
// the UVC adapter; returns nullptr when the flag is off.

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

#include <atomic>
#include <chrono>
#include <cstring>
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
  std::string id;
  std::string name;
  int width = 0;
  int height = 0;
};

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
  bool start(const MonitorTarget& target) {
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
    if (FAILED(interop->CreateForMonitor(target.monitor,
                                         winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                         winrt::put_abi(item)))) {
      return false;
    }
    width_ = item.Size().Width;
    height_ = item.Size().Height;
    framePool_ = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
        winrtDevice, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, item.Size());
    frameArrived_ = framePool_.FrameArrived(
        winrt::auto_revoke, [this](const wgc::Direct3D11CaptureFramePool& pool, const auto&) {
          onFrame(pool);
        });
    session_ = framePool_.CreateCaptureSession(item);
    session_.StartCapture();
    running_.store(true, std::memory_order_release);
    return true;
  }

  void stop() {
    running_.store(false, std::memory_order_release);
    frameArrived_.revoke();
    if (session_) {
      session_.Close();
      session_ = nullptr;
    }
    if (framePool_) {
      framePool_.Close();
      framePool_ = nullptr;
    }
  }

  // Poll side: move the latest ready frame out (tiny lock, no pixel work).
  bool takeLatest(std::vector<std::uint8_t>& outBgra, int& outWidth, int& outHeight) {
    std::lock_guard<std::mutex> lock(latestMutex_);
    if (latestBgra_.empty()) {
      return false;
    }
    outBgra = std::move(latestBgra_);
    latestBgra_.clear();
    outWidth = latestWidth_;
    outHeight = latestHeight_;
    return true;
  }

  int width() const { return width_; }
  int height() const { return height_; }

 private:
  void onFrame(const wgc::Direct3D11CaptureFramePool& pool) {
    if (!running_.load(std::memory_order_acquire)) {
      return;
    }
    auto frame = pool.TryGetNextFrame();
    if (!frame) {
      return;
    }
    auto surface = frame.Surface();
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
    if (FAILED(context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
      return;
    }
    std::vector<std::uint8_t> bgra(static_cast<size_t>(stagingWidth_) * stagingHeight_ * 4);
    const auto* src = static_cast<const std::uint8_t*>(mapped.pData);
    for (int row = 0; row < stagingHeight_; ++row) {
      std::memcpy(bgra.data() + static_cast<size_t>(row) * stagingWidth_ * 4,
                  src + static_cast<size_t>(row) * mapped.RowPitch,
                  static_cast<size_t>(stagingWidth_) * 4);
    }
    context_->Unmap(staging_.Get(), 0);
    {
      std::lock_guard<std::mutex> lock(latestMutex_);
      latestBgra_ = std::move(bgra);
      latestWidth_ = stagingWidth_;
      latestHeight_ = stagingHeight_;
    }
  }

  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<ID3D11Texture2D> staging_;
  int stagingWidth_ = 0;
  int stagingHeight_ = 0;
  wgc::Direct3D11CaptureFramePool framePool_{nullptr};
  wgc::GraphicsCaptureSession session_{nullptr};
  wgc::Direct3D11CaptureFramePool::FrameArrived_revoker frameArrived_;
  std::atomic<bool> running_{false};
  std::mutex latestMutex_;
  std::vector<std::uint8_t> latestBgra_;
  int latestWidth_ = 0;
  int latestHeight_ = 0;
  int width_ = 0;
  int height_ = 0;
};

class WgcScreenCaptureDevice : public ICaptureDevice {
 public:
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

  std::vector<CaptureDeviceInfo> connect(const std::string& deviceId) override {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto targets = enumerateMonitors();
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
      if (session->start(target)) {
        sessions_[deviceId] = std::move(session);
      }
      break;
    }
    return infosLocked();
  }

  std::vector<VideoFrame> pollVideoFrames(int64_t timestampMs) override {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<VideoFrame> frames;
    for (auto& [deviceId, session] : sessions_) {
      std::vector<std::uint8_t> bgra;
      int width = 0;
      int height = 0;
      if (!session->takeLatest(bgra, width, height)) {
        continue;
      }
      VideoFrame frame;
      frame.participantId = "capture:" + deviceId;
      frame.width = width;
      frame.height = height;
      frame.naturalWidth = width;
      frame.naturalHeight = height;
      frame.timestampMs = timestampMs;
      frame.pixels = std::make_shared<const std::vector<std::uint8_t>>(std::move(bgra));
      frame.pixelWidth = width;
      frame.pixelHeight = height;
      frame.pixelStride = width * 4;
      frames.push_back(std::move(frame));
    }
    return frames;
  }

 private:
  std::vector<CaptureDeviceInfo> infosLocked() const {
    std::vector<CaptureDeviceInfo> infos;
    for (const auto& target : enumerateMonitors()) {
      CaptureDeviceInfo info;
      info.id = target.id;
      info.name = target.name;
      info.kind = "screen";
      info.vendor = "Windows Graphics Capture";
      info.inputIds = {"screen"};
      info.inputLabels = {"Entire display"};
      info.inputHasEmbeddedAudio = {false};
      info.selectedInputId = "screen";
      info.width = target.width;
      info.height = target.height;
      info.frameRate = 60;
      const bool live = sessions_.count(target.id) != 0;
      info.connectionState = live ? "connected" : "detected";
      info.signalPresent = live;
      infos.push_back(std::move(info));
    }
    return infos;
  }

  mutable std::mutex mutex_;
  std::map<std::string, std::unique_ptr<WgcSession>> sessions_;
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
