#include "modules/VirtualCameraPublisher.h"

#include "modules/ImageResize.h"
#include "modules/VirtualCameraFrame.h"
#include "modules/VirtualCameraShm.h"

#include <cstdio>
#include <cstring>
#include <mutex>
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
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) {
      return true;
    }
    status_.width = width;
    status_.height = height;
    status_.fps = fps;
    status_.state = "starting";

    // 1) Create the cross-session shared-memory slot (writer owns it). File-backed
    //    on %ProgramData% so the Frame Server, which serves the camera from
    //    session 0, can read what we publish from the user's session.
    shmFile_ = openVirtualCameraShmFile(/*writer=*/true);
    if (shmFile_ == INVALID_HANDLE_VALUE) {
      fail("Could not create the virtual-camera shared-memory file.");
      return false;
    }
    view_ = mapVirtualCameraShmView(shmFile_, /*writer=*/true, &mapping_);
    if (view_ == nullptr) {
      fail("Could not map the virtual-camera shared memory.");
      return false;
    }
    header_ = static_cast<VirtualCameraShmHeader*>(view_);
    // The slot file is REUSED across app runs (never deleted - deleting would
    // orphan any Frame Server reader holding the old file object). It may hold
    // a previous session's frame, or an ODD seq if that session died mid-write.
    // Re-initialize under the seqlock so live readers never see a torn header,
    // and land on an EVEN seq so the odd/even write discipline stays intact.
    header_->seq = header_->seq | 1u;  // odd: writing (from any prior state)
    header_->width = width;
    header_->height = height;
    header_->fps = fps;
    header_->byteLen = 0;      // no frame yet - readers hold/slate until we publish
    header_->frameNumber = 0;
    header_->magic = kVirtualCameraMagic;
    header_->seq = (header_->seq | 1u) + 1u;  // even = complete
    ensureVirtualCameraServeLogFile();  // serving processes can append diagnostics

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
    const HRESULT startHr = camera_->Start(nullptr);
    if (FAILED(startHr)) {
      std::fprintf(stderr, "[virtualcam] IMFVirtualCamera::Start failed hr=0x%08lx\n",
                   static_cast<unsigned long>(startHr));
      fail("The virtual camera was created but could not be started.");
      camera_->Remove();
      camera_.Reset();
      return false;
    }

    started_ = true;
    status_.enabled = true;
    status_.state = "live";
    status_.warning.clear();
    std::fprintf(stderr, "[virtualcam] started %dx%d@%d '%s' (create+start ok)\n", width, height,
                 fps, status_.deviceName.c_str());
    return true;
  }

  void publish(const ProgramFrame& frame) override {
    // publish() runs on the audio/output worker while start()/stop() run on the
    // command thread - serialize so a concurrent stop() can't free view_/header_
    // out from under us (use-after-free -> access violation).
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_ || header_ == nullptr || view_ == nullptr) {
      return;
    }
    // Prefer the native full-resolution program frame (the vcam serves a real
    // webcam, so no upscale from the 320x180 UI thumbnail). Fall back to the
    // small preview only if the full readback is not present this tick.
    const auto& px = !frame.programFullBgra.bgra.empty() ? frame.programFullBgra : frame.preview;
    if (px.width <= 0 || px.height <= 0 || px.bgra.empty()) {
      return;
    }
    // Guard the raw reads below: the preview buffer must hold width*height*4
    // bytes, or resize/convert would read out of bounds (crash).
    if (px.bgra.size() < static_cast<std::size_t>(px.width) * px.height * 4) {
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

  // Fast path: the frame is ALREADY NV12 (converted on the compositor tap thread).
  // Just mirror (optional) + seqlock-write the SHM. No per-pixel conversion here,
  // so this stays cheap on the audio/output worker.
  void publishNv12(const std::uint8_t* nv12, int width, int height) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_ || header_ == nullptr || view_ == nullptr || nv12 == nullptr) {
      return;
    }
    const int w = status_.width & ~1;
    const int h = status_.height & ~1;
    if (width != w || height != h) {
      return;  // must match the DLL's fixed media type, else the DLL rejects it
    }
    const std::size_t bytes = nv12FrameSize(w, h);
    if (bytes == 0 || bytes > kVirtualCameraMaxPayload) {
      return;
    }
    nv12_.assign(nv12, nv12 + bytes);
    if (mirror_) {
      mirrorNv12InPlace(nv12_.data(), w, h);
    }
    auto* payload = static_cast<std::uint8_t*>(view_) + sizeof(VirtualCameraShmHeader);
    header_->seq = header_->seq + 1;  // odd
    std::memcpy(payload, nv12_.data(), bytes);
    header_->width = w;
    header_->height = h;
    header_->byteLen = static_cast<std::uint32_t>(bytes);
    header_->frameNumber = header_->frameNumber + 1;
    header_->seq = header_->seq + 1;  // even = complete
    ++status_.framesPublished;
  }

  void stop() override {
    std::lock_guard<std::mutex> lock(mutex_);
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
    if (shmFile_ != INVALID_HANDLE_VALUE) {
      CloseHandle(shmFile_);
      shmFile_ = INVALID_HANDLE_VALUE;
      ::DeleteFileA(virtualCameraShmFilePath().c_str());  // best-effort cleanup
    }
    started_ = false;
    status_.enabled = false;
    if (status_.state == "live" || status_.state == "starting") {
      status_.state = "off";
    }
  }

  VirtualCameraStatus status() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
  }

  void setMirror(bool mirror) override {
    std::lock_guard<std::mutex> lock(mutex_);
    mirror_ = mirror;
  }

  void setDeviceName(const std::string& name) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_ && !name.empty()) {
      status_.deviceName = name;  // takes effect at the next start()
    }
  }

 private:
  void fail(const std::string& message) {
    status_.state = "failed";
    status_.warning = message;
    std::fprintf(stderr, "[virtualcam] %s\n", message.c_str());
  }

  mutable std::mutex mutex_;
  bool started_ = false;
  bool mirror_ = false;
  HANDLE shmFile_ = INVALID_HANDLE_VALUE;
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
  void setDeviceName(const std::string& name) override {
    if (!name.empty()) status_.deviceName = name;
  }

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
