#include "modules/Interfaces.h"
#include "modules/UvcCaptureSupport.h"

#include <memory>

// Native UVC capture: Media Foundation VIDCAP enumeration + IMFSourceReader
// streaming inside the C++ core. Frames enter the compositor as first-class
// I420 VideoFrames keyed "capture:<stableDeviceId>" — the same id scheme the
// WinUI shell derives from the device symbolic link — so existing scene routes
// composite native camera pixels with no WinUI shared-memory hop. The YUV->RGB
// color conversion stays on the GPU (the compositor's I420 HLSL path); this
// adapter only repacks NV12/YUY2 bytes into I420 planes on its capture thread.
//
// Hot-unplug safety: every Media Foundation failure (device invalidated,
// stream error, negotiation failure) downgrades the device to
// connectionState "error" with a human-readable warning and stops the session
// thread. Nothing throws across the ICaptureDevice boundary.

#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_UVC && defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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
  T* get() const { return value_; }
  T** put() {
    reset();
    return &value_;
  }
  T* operator->() const { return value_; }
  explicit operator bool() const { return value_ != nullptr; }
  void reset() {
    if (value_) {
      value_->Release();
      value_ = nullptr;
    }
  }

 private:
  T* value_ = nullptr;
};

std::string narrowWide(const wchar_t* value, size_t length) {
  if (!value || length == 0) {
    return {};
  }
  const int count = WideCharToMultiByte(
      CP_UTF8, 0, value, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
  if (count <= 0) {
    return {};
  }
  std::string result(static_cast<size_t>(count), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value, static_cast<int>(length), result.data(), count, nullptr, nullptr);
  return result;
}

std::string fourccToString(DWORD fourcc) {
  std::string result;
  for (int shift = 0; shift < 32; shift += 8) {
    const char ch = static_cast<char>((fourcc >> shift) & 0xff);
    result.push_back((ch >= 32 && ch < 127) ? ch : '?');
  }
  return result;
}

std::string subtypeFourcc(const GUID& subtype) {
  // For video subtypes the FourCC lives in Data1 (MFVideoFormat_NV12 etc.).
  if (subtype == MFVideoFormat_NV12) {
    return "NV12";
  }
  if (subtype == MFVideoFormat_YUY2) {
    return "YUY2";
  }
  if (subtype == MFVideoFormat_MJPG) {
    return "MJPG";
  }
  return fourccToString(subtype.Data1);
}

std::string describeHr(HRESULT hr) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "hr=0x%08lX", static_cast<unsigned long>(hr));
  return buffer;
}

std::string lowercased(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

constexpr int kTargetWidth = 1920;
constexpr int kTargetHeight = 1080;
constexpr double kTargetFps = 60.0;
constexpr auto kEnumerateRefreshInterval = std::chrono::milliseconds(2000);

// One streaming device: a worker thread that owns the IMFSourceReader and
// publishes the latest converted I420 frame under a light mutex. The render
// tick (pollVideoFrames) only swaps shared_ptrs — no MF calls, no conversion.
class UvcCaptureSession {
 public:
  UvcCaptureSession(std::string deviceId, std::wstring symbolicLink)
      : deviceId_(std::move(deviceId)), symbolicLink_(std::move(symbolicLink)) {
    thread_ = std::thread([this] { run(); });
  }

  ~UvcCaptureSession() {
    stop_.store(true);
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  struct Snapshot {
    std::shared_ptr<const std::vector<uint8_t>> i420;
    int width = 0;
    int height = 0;
    int64_t frameId = 0;
    bool fullRange = false;
    bool bt601 = false;
    int negotiatedWidth = 0;
    int negotiatedHeight = 0;
    int negotiatedFps = 0;
    std::string negotiatedFourcc;
    std::string error;
    bool running = false;
  };

  Snapshot snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Snapshot copy;
    copy.i420 = latestI420_;
    copy.width = width_;
    copy.height = height_;
    copy.frameId = frameId_;
    copy.fullRange = fullRange_;
    copy.bt601 = bt601_;
    copy.negotiatedWidth = negotiatedWidth_;
    copy.negotiatedHeight = negotiatedHeight_;
    copy.negotiatedFps = negotiatedFps_;
    copy.negotiatedFourcc = negotiatedFourcc_;
    copy.error = error_;
    copy.running = running_;
    return copy;
  }

 private:
  void fail(const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (error_.empty()) {
      error_ = message;
    }
    running_ = false;
    std::fprintf(stderr, "[uvc-capture] '%s' failed: %s\n", deviceId_.c_str(), message.c_str());
  }

  // Reads dimensions + color hints from the reader's CURRENT output type.
  bool refreshOutputType(IMFSourceReader* reader) {
    ComPtrLite<IMFMediaType> current;
    if (FAILED(reader->GetCurrentMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), current.put())) ||
        !current) {
      return false;
    }
    UINT64 frameSize = 0;
    if (FAILED(current->GetUINT64(MF_MT_FRAME_SIZE, &frameSize))) {
      return false;
    }
    const int width = static_cast<int>(frameSize >> 32);
    const int height = static_cast<int>(frameSize & 0xffffffffu);
    if (width <= 0 || height <= 0 || (width & 1) != 0 || (height & 1) != 0) {
      return false;
    }
    GUID subtype = GUID_NULL;
    current->GetGUID(MF_MT_SUBTYPE, &subtype);
    UINT32 nominalRange = 0;
    current->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &nominalRange);
    UINT32 yuvMatrix = 0;
    current->GetUINT32(MF_MT_YUV_MATRIX, &yuvMatrix);
    const auto hints = uvc::deriveYuvColorHints(
        static_cast<int>(nominalRange), static_cast<int>(yuvMatrix), height);

    std::lock_guard<std::mutex> lock(mutex_);
    outputWidth_ = width;
    outputHeight_ = height;
    outputIsNv12_ = subtype == MFVideoFormat_NV12;
    fullRange_ = hints.fullRange;
    bt601_ = hints.bt601;
    return true;
  }

  void run() {
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comInitialized = comHr == S_OK || comHr == S_FALSE;
    const bool mfStarted = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET));
    if (!mfStarted) {
      fail("Media Foundation failed to start.");
      if (comInitialized) {
        CoUninitialize();
      }
      return;
    }

    {
      ComPtrLite<IMFAttributes> deviceAttributes;
      ComPtrLite<IMFMediaSource> source;
      ComPtrLite<IMFSourceReader> reader;
      HRESULT hr = MFCreateAttributes(deviceAttributes.put(), 2);
      if (SUCCEEDED(hr)) {
        hr = deviceAttributes->SetGUID(
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
      }
      if (SUCCEEDED(hr)) {
        hr = deviceAttributes->SetString(
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, symbolicLink_.c_str());
      }
      if (SUCCEEDED(hr)) {
        hr = MFCreateDeviceSource(deviceAttributes.get(), source.put());
      }
      if (FAILED(hr) || !source) {
        fail("Camera could not be opened (in use by another app, or unplugged). " + describeHr(hr));
      } else {
        ComPtrLite<IMFAttributes> readerAttributes;
        if (SUCCEEDED(MFCreateAttributes(readerAttributes.put(), 2))) {
          // Advanced video processing gives us MJPG decode + YUY2->NV12
          // conversion inside Media Foundation, on this capture thread.
          readerAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        }
        hr = MFCreateSourceReaderFromMediaSource(source.get(), readerAttributes.get(), reader.put());
        if (FAILED(hr) || !reader) {
          fail("Source reader creation failed. " + describeHr(hr));
        } else if (negotiate(reader.get())) {
          readLoop(reader.get());
        }
      }
      // COM objects release here, before MFShutdown.
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      running_ = false;
    }
    MFShutdown();
    if (comInitialized) {
      CoUninitialize();
    }
  }

  bool negotiate(IMFSourceReader* reader) {
    const auto stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

    // Enumerate the device's native media types.
    std::vector<uvc::UvcFormatCandidate> candidates;
    for (DWORD index = 0;; ++index) {
      ComPtrLite<IMFMediaType> nativeType;
      const HRESULT hr = reader->GetNativeMediaType(stream, index, nativeType.put());
      if (hr == MF_E_NO_MORE_TYPES) {
        break;
      }
      if (FAILED(hr) || !nativeType) {
        break;
      }
      UINT64 frameSize = 0;
      UINT64 frameRate = 0;
      GUID subtype = GUID_NULL;
      if (FAILED(nativeType->GetUINT64(MF_MT_FRAME_SIZE, &frameSize)) ||
          FAILED(nativeType->GetGUID(MF_MT_SUBTYPE, &subtype))) {
        continue;
      }
      nativeType->GetUINT64(MF_MT_FRAME_RATE, &frameRate);
      uvc::UvcFormatCandidate candidate;
      candidate.width = static_cast<int>(frameSize >> 32);
      candidate.height = static_cast<int>(frameSize & 0xffffffffu);
      candidate.fpsNumerator = static_cast<int>(frameRate >> 32);
      candidate.fpsDenominator = std::max(1, static_cast<int>(frameRate & 0xffffffffu));
      candidate.fourcc = subtypeFourcc(subtype);
      candidate.nativeIndex = static_cast<int>(index);
      candidates.push_back(std::move(candidate));
    }

    const int best = uvc::pickBestUvcFormat(candidates, kTargetWidth, kTargetHeight, kTargetFps);
    if (best < 0) {
      fail("Device advertises no video formats.");
      return false;
    }
    const auto& chosen = candidates[static_cast<size_t>(best)];

    // Select the chosen native format on the device.
    ComPtrLite<IMFMediaType> nativeType;
    if (FAILED(reader->GetNativeMediaType(
            stream, static_cast<DWORD>(chosen.nativeIndex), nativeType.put())) ||
        !nativeType ||
        FAILED(reader->SetCurrentMediaType(stream, nullptr, nativeType.get()))) {
      fail("Selecting the native format failed (" + chosen.fourcc + " " +
           std::to_string(chosen.width) + "x" + std::to_string(chosen.height) + ").");
      return false;
    }

    // Ask the reader for NV12 output at the chosen geometry (advanced video
    // processing decodes MJPG / converts YUY2 for us). YUY2 output is the
    // fallback; we repack either into I420 for the GPU shader.
    const auto requestOutput = [&](const GUID& subtype, bool pinFrameSize) -> bool {
      ComPtrLite<IMFMediaType> outputType;
      if (FAILED(MFCreateMediaType(outputType.put()))) {
        return false;
      }
      outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
      outputType->SetGUID(MF_MT_SUBTYPE, subtype);
      if (pinFrameSize) {
        outputType->SetUINT64(
            MF_MT_FRAME_SIZE,
            (static_cast<UINT64>(chosen.width) << 32) | static_cast<UINT64>(chosen.height));
      }
      return SUCCEEDED(reader->SetCurrentMediaType(stream, nullptr, outputType.get()));
    };

    // Prefer the chosen geometry pinned; some converters only accept a partial
    // (subtype-only) request, so retry unpinned before giving up on a subtype.
    if (!requestOutput(MFVideoFormat_NV12, true) && !requestOutput(MFVideoFormat_NV12, false) &&
        !requestOutput(MFVideoFormat_YUY2, true) && !requestOutput(MFVideoFormat_YUY2, false)) {
      fail("Neither NV12 nor YUY2 output could be negotiated for " + chosen.fourcc + ".");
      return false;
    }
    if (!refreshOutputType(reader)) {
      fail("Negotiated output type could not be read back.");
      return false;
    }

    const double fps = uvc::uvcCandidateFps(chosen);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      negotiatedWidth_ = chosen.width;
      negotiatedHeight_ = chosen.height;
      negotiatedFps_ = static_cast<int>(fps + 0.5);
      negotiatedFourcc_ = chosen.fourcc;
      running_ = true;
    }
    std::fprintf(
        stderr,
        "[uvc-capture] '%s' negotiated %s %dx%d@%d -> %s (fullRange=%d bt601=%d)\n",
        deviceId_.c_str(),
        chosen.fourcc.c_str(),
        chosen.width,
        chosen.height,
        static_cast<int>(fps + 0.5),
        outputIsNv12_ ? "NV12" : "YUY2",
        fullRange_ ? 1 : 0,
        bt601_ ? 1 : 0);
    return true;
  }

  void readLoop(IMFSourceReader* reader) {
    const auto stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    std::vector<uint8_t> converted;
    bool loggedFirstFrame = false;

    while (!stop_.load()) {
      DWORD actualStream = 0;
      DWORD flags = 0;
      LONGLONG timestamp = 0;
      ComPtrLite<IMFSample> sample;
      const HRESULT hr = reader->ReadSample(stream, 0, &actualStream, &flags, &timestamp, sample.put());
      if (FAILED(hr)) {
        fail(hr == MF_E_VIDEO_RECORDING_DEVICE_INVALIDATED
                 ? std::string("Camera was removed or reset (device invalidated).")
                 : "Camera read failed. " + describeHr(hr));
        return;
      }
      if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
        fail("Camera stream ended.");
        return;
      }
      if (flags & MF_SOURCE_READERF_ERROR) {
        fail("Camera stream reported an error.");
        return;
      }
      if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
        if (!refreshOutputType(reader)) {
          fail("Output type changed and could not be re-read.");
          return;
        }
      }
      if (!sample) {
        continue;  // Stream tick / gap.
      }

      ComPtrLite<IMFMediaBuffer> buffer;
      if (FAILED(sample->ConvertToContiguousBuffer(buffer.put())) || !buffer) {
        continue;
      }
      BYTE* data = nullptr;
      DWORD length = 0;
      if (FAILED(buffer->Lock(&data, nullptr, &length)) || !data) {
        continue;
      }

      int width = 0;
      int height = 0;
      bool isNv12 = false;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        width = outputWidth_;
        height = outputHeight_;
        isNv12 = outputIsNv12_;
      }

      converted.clear();
      if (isNv12) {
        // Contiguous NV12: packed Y plane then interleaved UV, stride = width.
        const size_t needed =
            static_cast<size_t>(width) * static_cast<size_t>(height) * 3 / 2;
        if (width > 0 && height > 0 && length >= needed) {
          uvc::nv12ToI420(
              data,
              width,
              data + static_cast<size_t>(width) * static_cast<size_t>(height),
              width,
              width,
              height,
              converted);
        }
      } else {
        // Contiguous YUY2: packed 4:2:2, stride = width * 2.
        const size_t needed =
            static_cast<size_t>(width) * static_cast<size_t>(height) * 2;
        if (width > 0 && height > 0 && length >= needed) {
          uvc::yuy2ToI420(data, width * 2, width, height, converted);
        }
      }
      buffer->Unlock();

      if (converted.empty()) {
        continue;
      }
      auto frame = std::make_shared<std::vector<uint8_t>>(std::move(converted));
      converted = std::vector<uint8_t>();
      {
        std::lock_guard<std::mutex> lock(mutex_);
        latestI420_ = std::move(frame);
        width_ = width;
        height_ = height;
        ++frameId_;
      }
      if (!loggedFirstFrame) {
        loggedFirstFrame = true;
        std::fprintf(stderr, "[uvc-capture] first frame '%s' %dx%d (native MF capture live)\n",
                     deviceId_.c_str(), width, height);
      }
    }
  }

  const std::string deviceId_;
  const std::wstring symbolicLink_;
  std::thread thread_;
  std::atomic<bool> stop_{false};

  mutable std::mutex mutex_;
  std::shared_ptr<const std::vector<uint8_t>> latestI420_;
  int width_ = 0;
  int height_ = 0;
  int64_t frameId_ = 0;
  bool fullRange_ = false;
  bool bt601_ = false;
  int outputWidth_ = 0;
  int outputHeight_ = 0;
  bool outputIsNv12_ = true;
  int negotiatedWidth_ = 0;
  int negotiatedHeight_ = 0;
  int negotiatedFps_ = 0;
  std::string negotiatedFourcc_;
  std::string error_;
  bool running_ = false;
};

class UvcCaptureDeviceAdapter final : public ICaptureDevice {
 public:
  UvcCaptureDeviceAdapter() = default;

  ~UvcCaptureDeviceAdapter() override {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : devices_) {
      entry.session.reset();  // joins the reader thread
    }
    devices_.clear();
  }

  std::vector<CaptureDeviceInfo> enumerate() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    refreshLocked(false);
    return snapshotLocked();
  }

  std::vector<CaptureDeviceInfo> selectInput(const std::string& deviceId, const std::string& inputId) override {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : devices_) {
      if (entry.info.id == deviceId &&
          std::find(entry.info.inputIds.begin(), entry.info.inputIds.end(), inputId) !=
              entry.info.inputIds.end()) {
        entry.info.selectedInputId = inputId;
      }
    }
    return snapshotLocked();
  }

  std::vector<CaptureDeviceInfo> setAudioSyncOffset(const std::string& deviceId, int offsetMs) override {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : devices_) {
      if (entry.info.id == deviceId) {
        entry.info.audioSyncOffsetMs = std::max(-500, std::min(500, offsetMs));
      }
    }
    return snapshotLocked();
  }

  std::vector<CaptureDeviceInfo> connect(const std::string& deviceId) override {
    return connect(deviceId, std::string());
  }

  std::vector<CaptureDeviceInfo> connect(const std::string& deviceId,
                                         const std::string& outputSourceId) override {
    std::lock_guard<std::mutex> lock(mutex_);
    refreshLocked(true);
    for (auto& entry : devices_) {
      if (!matchesDeviceId(entry, deviceId)) {
        continue;
      }
      // (Re)start the capture session — a fresh attempt clears a prior error
      // (e.g. the camera was re-plugged after an unplug).
      entry.session.reset();
      entry.session = std::make_unique<UvcCaptureSession>(entry.info.id, entry.symbolicLink);
      // Emit frames keyed by the shell's routing id when it supplied one (WinRT vs
      // Media Foundation stable-id reconciliation), else the adapter's own id.
      entry.outputSourceId = outputSourceId;
      entry.info.connectionState = "connected";
      entry.info.signalPresent = false;
      entry.info.warning = "Waiting for the first camera frame.";
      std::fprintf(stderr, "[uvc-capture] connect '%s' (%s) frameKey='%s'\n",
                   entry.info.id.c_str(), entry.info.name.c_str(),
                   outputSourceId.empty() ? entry.info.id.c_str() : outputSourceId.c_str());
      break;
    }
    return snapshotLocked();
  }

  std::vector<VideoFrame> pollVideoFrames(int64_t timestampMs) override {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<VideoFrame> frames;
    for (auto& entry : devices_) {
      if (!entry.session) {
        continue;
      }
      const auto snapshot = entry.session->snapshot();

      // Fold session telemetry into the device metadata.
      if (!snapshot.error.empty()) {
        entry.info.connectionState = "error";
        entry.info.signalPresent = false;
        entry.info.warning = snapshot.error;
      } else if (snapshot.running) {
        if (snapshot.negotiatedWidth > 0) {
          entry.info.width = snapshot.negotiatedWidth;
          entry.info.height = snapshot.negotiatedHeight;
          entry.info.frameRate = snapshot.negotiatedFps;
        }
        if (snapshot.frameId > 0) {
          entry.info.signalPresent = true;
          entry.info.warning.clear();
        }
      }

      if (!snapshot.i420 || snapshot.width <= 0 || snapshot.height <= 0) {
        continue;
      }

      // Re-emit the latest held frame every tick (the capture rate is slower
      // than the render tick) so the compositor never starves back to a slate.
      VideoFrame frame;
      frame.participantId =
          "capture:" + (entry.outputSourceId.empty() ? entry.info.id : entry.outputSourceId);
      frame.width = snapshot.width;
      frame.height = snapshot.height;
      frame.naturalWidth = snapshot.width;
      frame.naturalHeight = snapshot.height;
      frame.timestampMs = timestampMs;
      frame.frameId = snapshot.frameId;
      frame.i420 = snapshot.i420;
      frame.i420Width = snapshot.width;
      frame.i420Height = snapshot.height;
      frame.i420FullRange = snapshot.fullRange;
      frame.i420Bt601 = snapshot.bt601;
      frames.push_back(std::move(frame));
    }
    return frames;
  }

 private:
  struct DeviceEntry {
    CaptureDeviceInfo info;
    std::wstring symbolicLink;
    // Case-variant stable ids so a shell-computed id still matches when the
    // WinRT and MF symbolic-link casing differ.
    std::vector<std::string> candidateIds;
    // The shell's routing id to key emitted frames by (WinRT vs MF stable-id
    // reconciliation). Empty -> key by info.id.
    std::string outputSourceId;
    std::unique_ptr<UvcCaptureSession> session;
  };

  static bool matchesDeviceId(const DeviceEntry& entry, const std::string& deviceId) {
    if (entry.info.id == deviceId || entry.info.nativeDeviceId == deviceId) {
      return true;
    }
    // The shell may address the device by its OS symbolic link; WinRT and MF
    // report the same interface path with different casing, so compare folded.
    if (!entry.info.nativeDeviceId.empty() &&
        lowercased(entry.info.nativeDeviceId) == lowercased(deviceId)) {
      return true;
    }
    return std::find(entry.candidateIds.begin(), entry.candidateIds.end(), deviceId) !=
           entry.candidateIds.end();
  }

  std::vector<CaptureDeviceInfo> snapshotLocked() const {
    std::vector<CaptureDeviceInfo> result;
    result.reserve(devices_.size());
    for (const auto& entry : devices_) {
      result.push_back(entry.info);
    }
    return result;
  }

  // Re-enumerates VIDCAP devices at most every kEnumerateRefreshInterval
  // (`force` bypasses the throttle). Enumeration lists devices only — it never
  // opens a camera. Devices that vanish while streaming stay listed in an
  // error state (hot-unplug surfaces as a warning, never a crash); idle
  // vanished devices are dropped.
  void refreshLocked(bool force) const {
    const auto now = std::chrono::steady_clock::now();
    if (!force && lastRefresh_.time_since_epoch().count() != 0 &&
        now - lastRefresh_ < kEnumerateRefreshInterval) {
      return;
    }
    lastRefresh_ = now;

    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comInitialized = comHr == S_OK || comHr == S_FALSE;
    if (!mfStarted_) {
      mfStarted_ = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET));
    }
    if (!mfStarted_) {
      if (comInitialized) {
        CoUninitialize();
      }
      return;
    }

    std::vector<DeviceEntry> discovered;
    ComPtrLite<IMFAttributes> attributes;
    if (SUCCEEDED(MFCreateAttributes(attributes.put(), 1)) &&
        SUCCEEDED(attributes->SetGUID(
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID))) {
      IMFActivate** activates = nullptr;
      UINT32 count = 0;
      if (SUCCEEDED(MFEnumDeviceSources(attributes.get(), &activates, &count)) && activates) {
        for (UINT32 index = 0; index < count; ++index) {
          IMFActivate* activate = activates[index];
          if (!activate) {
            continue;
          }
          WCHAR* friendly = nullptr;
          UINT32 friendlyLength = 0;
          WCHAR* symlink = nullptr;
          UINT32 symlinkLength = 0;
          activate->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &friendly, &friendlyLength);
          activate->GetAllocatedString(
              MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &symlink, &symlinkLength);
          const std::string name = narrowWide(friendly, friendlyLength);
          const std::string link = narrowWide(symlink, symlinkLength);
          std::wstring linkW = symlink ? std::wstring(symlink, symlinkLength) : std::wstring();
          if (friendly) {
            CoTaskMemFree(friendly);
          }
          if (symlink) {
            CoTaskMemFree(symlink);
          }
          activate->Release();
          if (link.empty()) {
            continue;
          }
          // DeckLink/AJA SDI cards are served by their vendor adapters; their
          // WDM shims here would double-list the device.
          const std::string lowerName = lowercased(name);
          if (lowerName.find("blackmagic") != std::string::npos ||
              lowerName.find("decklink") != std::string::npos) {
            continue;
          }

          DeviceEntry entry;
          entry.symbolicLink = std::move(linkW);
          entry.info.id = uvc::stableCaptureDeviceIdFromSymbolicLink(link);
          entry.info.nativeDeviceId = link;
          entry.info.name = name.empty() ? "UVC Camera" : name;
          entry.info.kind = "video";
          entry.info.vendor = "uvc";
          entry.info.inputIds = {"camera"};
          entry.info.inputLabels = {"Camera"};
          entry.info.inputHasEmbeddedAudio = {false};
          entry.info.selectedInputId = "camera";
          entry.info.width = kTargetWidth;
          entry.info.height = kTargetHeight;
          entry.info.frameRate = 30;
          entry.info.connectionState = "detected";
          entry.info.signalPresent = false;
          entry.candidateIds = {
              entry.info.id,
              uvc::stableCaptureDeviceIdFromSymbolicLink(lowercased(link)),
          };
          discovered.push_back(std::move(entry));
        }
        CoTaskMemFree(activates);
      }
    }
    if (comInitialized) {
      CoUninitialize();
    }

    // Merge: keep runtime state (session, connection, telemetry) for devices
    // that are still present; add new ones; keep streaming-but-vanished devices
    // visible in an error state; drop idle vanished devices.
    std::vector<DeviceEntry> merged;
    merged.reserve(discovered.size() + devices_.size());
    for (auto& fresh : discovered) {
      auto existing = std::find_if(devices_.begin(), devices_.end(), [&](DeviceEntry& entry) {
        return entry.info.id == fresh.info.id;
      });
      if (existing != devices_.end()) {
        existing->info.name = fresh.info.name;
        existing->info.nativeDeviceId = fresh.info.nativeDeviceId;
        merged.push_back(std::move(*existing));
        devices_.erase(existing);
      } else {
        merged.push_back(std::move(fresh));
      }
    }
    for (auto& leftover : devices_) {
      if (leftover.session) {
        leftover.info.connectionState = "error";
        leftover.info.signalPresent = false;
        if (leftover.info.warning.empty()) {
          leftover.info.warning = "Camera is no longer present (unplugged?).";
        }
        merged.push_back(std::move(leftover));
      }
    }
    devices_ = std::move(merged);
  }

  mutable std::mutex mutex_;
  mutable std::vector<DeviceEntry> devices_;
  mutable std::chrono::steady_clock::time_point lastRefresh_{};
  mutable bool mfStarted_ = false;
};

}  // namespace

std::unique_ptr<ICaptureDevice> createUvcCaptureDevice() {
  return std::make_unique<UvcCaptureDeviceAdapter>();
}

}  // namespace corevideo::modules

#else

namespace corevideo::modules {

std::unique_ptr<ICaptureDevice> createUvcCaptureDevice() {
  return nullptr;
}

}  // namespace corevideo::modules

#endif
