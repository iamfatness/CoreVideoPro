#include "modules/VirtualCameraPublisher.h"

#include "modules/ImageResize.h"
#include "modules/VirtualCameraFrame.h"
#include "modules/VirtualCameraShm.h"

#include <cstdio>
#include <cstring>
#include <vector>

#if defined(COREVIDEO_WITH_VIRTUALCAM) && COREVIDEO_WITH_VIRTUALCAM
#include <windows.h>
#include <mfapi.h>
#include <mfvirtualcamera.h>
#include <wrl/client.h>
#endif

namespace corevideo::modules {
namespace {

#if defined(COREVIDEO_WITH_VIRTUALCAM) && COREVIDEO_WITH_VIRTUALCAM

// The registered CLSID of corevideo-virtualcam.dll's media source (V2b). The
// DLL self-registers this; MFCreateVirtualCamera references it by string.
// {8B4B2C9E-2C4A-4E1D-9C7A-CDEF01234567}
constexpr wchar_t kMediaSourceClsid[] = L"{8B4B2C9E-2C4A-4E1D-9C7A-CDEF01234567}";

class WindowsVirtualCameraPublisher final : public IVirtualCameraPublisher {
 public:
  ~WindowsVirtualCameraPublisher() override { stop(); }

  bool start(int width, int height, int fps) override {
    if (started_) {
      return true;
    }
    status_.width = width;
    status_.height = height;
    status_.fps = fps;
    status_.state = "starting";

    // 1) Create the shared-memory slot (writer owns it).
    const auto name = virtualCameraShmName();
    mapping_ = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                  static_cast<DWORD>(virtualCameraShmSize()), name.c_str());
    if (mapping_ == nullptr) {
      fail("Could not create the virtual-camera shared memory.");
      return false;
    }
    view_ = MapViewOfFile(mapping_, FILE_MAP_WRITE, 0, 0, virtualCameraShmSize());
    if (view_ == nullptr) {
      fail("Could not map the virtual-camera shared memory.");
      return false;
    }
    header_ = static_cast<VirtualCameraShmHeader*>(view_);
    header_->seq = 0;
    header_->width = width;
    header_->height = height;
    header_->fps = fps;
    header_->byteLen = 0;
    header_->frameNumber = 0;
    header_->magic = kVirtualCameraMagic;  // publish magic last

    // 2) Register the OS virtual camera (Win11 MFCreateVirtualCamera). The DLL
    //    named by kMediaSourceClsid serves NV12 samples read from the slot.
    std::wstring name16(status_.deviceName.begin(), status_.deviceName.end());
    const HRESULT hr = MFCreateVirtualCamera(
        MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_Session,
        MFVirtualCameraAccess_CurrentUser, name16.c_str(), kMediaSourceClsid, nullptr, 0,
        camera_.GetAddressOf());
    if (FAILED(hr) || camera_ == nullptr) {
      std::fprintf(stderr, "[virtualcam] MFCreateVirtualCamera failed hr=0x%08lx "
                           "(needs Win11 22000+ and the registered DLL)\n",
                   static_cast<unsigned long>(hr));
      fail("Windows rejected the virtual camera (needs Win11 22000+ and the registered DLL).");
      // Keep the SHM slot; the control plane can retry once the DLL is present.
      return false;
    }
    camera_->Start(nullptr);

    started_ = true;
    status_.enabled = true;
    status_.state = "live";
    status_.warning.clear();
    std::fprintf(stderr, "[virtualcam] started %dx%d@%d '%s'\n", width, height, fps,
                 status_.deviceName.c_str());
    return true;
  }

  void publish(const ProgramFrame& frame) override {
    if (!started_ || header_ == nullptr) {
      return;
    }
    const auto& px = frame.preview;
    if (px.width <= 0 || px.height <= 0 || px.bgra.empty()) {
      return;
    }
    // Publish at the DECLARED size (the DLL's media type is fixed), so the camera
    // shows the real program instead of falling back to the slate on a size
    // mismatch. Scale the program preview to status_.width x status_.height.
    const int w = status_.width & ~1;
    const int h = status_.height & ~1;
    const std::uint8_t* srcBgra = px.bgra.data();
    if (px.width != w || px.height != h) {
      if (!resizeBgraBilinear(px.bgra.data(), px.width, px.height, w, h, resized_)) {
        return;
      }
      srcBgra = resized_.data();
    }
    if (!convertBgraToNv12(srcBgra, w, h, nv12_)) {
      return;
    }
    if (mirror_) {
      mirrorNv12InPlace(nv12_.data(), w, h);  // mirror-me self-view
    }
    auto* payload = static_cast<std::uint8_t*>(view_) + sizeof(VirtualCameraShmHeader);
    const std::size_t bytes = nv12_.size() <= kVirtualCameraMaxPayload ? nv12_.size()
                                                                        : kVirtualCameraMaxPayload;

    // Seqlock write: bump to odd, write, bump to even.
    header_->seq = header_->seq + 1;  // odd
    std::memcpy(payload, nv12_.data(), bytes);
    header_->width = w;
    header_->height = h;
    header_->byteLen = static_cast<std::uint32_t>(bytes);
    header_->frameNumber = frame.frameNumber;
    header_->seq = header_->seq + 1;  // even = complete

    ++status_.framesPublished;
  }

  void stop() override {
    if (camera_ != nullptr) {
      camera_->Stop();
      camera_->Remove();
      camera_.Reset();
    }
    if (view_ != nullptr) {
      UnmapViewOfFile(view_);
      view_ = nullptr;
      header_ = nullptr;
    }
    if (mapping_ != nullptr) {
      CloseHandle(mapping_);
      mapping_ = nullptr;
    }
    started_ = false;
    status_.enabled = false;
    if (status_.state == "live" || status_.state == "starting") {
      status_.state = "off";
    }
  }

  VirtualCameraStatus status() const override { return status_; }

  void setMirror(bool mirror) override { mirror_ = mirror; }

 private:
  void fail(const std::string& message) {
    status_.state = "failed";
    status_.warning = message;
    std::fprintf(stderr, "[virtualcam] %s\n", message.c_str());
  }

  bool started_ = false;
  bool mirror_ = false;
  HANDLE mapping_ = nullptr;
  void* view_ = nullptr;
  VirtualCameraShmHeader* header_ = nullptr;
  std::vector<std::uint8_t> nv12_;
  std::vector<std::uint8_t> resized_;
  Microsoft::WRL::ComPtr<IMFVirtualCamera> camera_;
  VirtualCameraStatus status_;
};

#endif  // COREVIDEO_WITH_VIRTUALCAM

class NoopVirtualCameraPublisher final : public IVirtualCameraPublisher {
 public:
  bool start(int width, int height, int fps) override {
    status_.width = width;
    status_.height = height;
    status_.fps = fps;
    status_.enabled = true;
    status_.state = "live";  // simulated for the stub / non-Windows build
    return true;
  }
  void publish(const ProgramFrame&) override { ++status_.framesPublished; }
  void stop() override {
    status_.enabled = false;
    status_.state = "off";
  }
  VirtualCameraStatus status() const override { return status_; }

 private:
  VirtualCameraStatus status_;
};

}  // namespace

std::unique_ptr<IVirtualCameraPublisher> createVirtualCameraPublisher() {
#if defined(COREVIDEO_WITH_VIRTUALCAM) && COREVIDEO_WITH_VIRTUALCAM
  return std::make_unique<WindowsVirtualCameraPublisher>();
#else
  return std::make_unique<NoopVirtualCameraPublisher>();
#endif
}

}  // namespace corevideo::modules
