#include "modules/Interfaces.h"
#include "modules/NdiPixelConvert.h"
#include "modules/NdiSourceName.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_NDI_OUTPUT
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#endif

namespace corevideo::modules {
namespace {

#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_NDI_OUTPUT

struct RuntimeCandidate {
  std::string name;
  std::string status;
  std::string detail;
};

struct RuntimeProbe {
  bool available = false;
  std::string detail;
  std::vector<RuntimeCandidate> candidates;
};

// ---------------------------------------------------------------------------
// Minimal libNDI ABI surface, declared locally so this scaffold COMPILES on a
// machine without the NDI SDK headers (mirrors how the SRT scaffold builds with
// COREVIDEO_HAS_LIBSRT=0). We runtime-probe Processing.NDI.Lib.x64.dll / the
// POSIX shared object and resolve the v5 C entrypoints by name. The struct
// layouts below match the published `Processing.NDI.Lib.h` v5 ABI; if the real
// SDK is present at build time the dynamic resolution still uses these local
// definitions (we never include the SDK header), which keeps the build
// hermetic. On a dev rig with libNDI installed the resolved symbols drive a
// real sender; without it the probe reports unavailable and the synthetic
// sender remains in the module set.
extern "C" {

typedef enum {
  NDIlib_FourCC_type_UYVY = 0x59565955,  // 'UYVY'
  NDIlib_FourCC_type_BGRA = 0x41524742,  // 'BGRA'
} NDIlib_FourCC_video_type_e;

typedef enum {
  NDIlib_frame_format_type_progressive = 1,
} NDIlib_frame_format_type_e;

typedef enum {
  NDIlib_FourCC_audio_type_FLTP = 0x70746C66,  // 'fltp'
} NDIlib_FourCC_audio_type_e;

typedef struct {
  const char* p_ndi_name;
  const char* p_groups;
  bool clock_video;
  bool clock_audio;
} NDIlib_send_create_t;

typedef struct {
  int xres;
  int yres;
  NDIlib_FourCC_video_type_e FourCC;
  int frame_rate_N;
  int frame_rate_D;
  float picture_aspect_ratio;
  NDIlib_frame_format_type_e frame_format_type;
  int64_t timecode;
  const uint8_t* p_data;
  int line_stride_in_bytes;
  const char* p_metadata;
  int64_t timestamp;
} NDIlib_video_frame_v2_t;

typedef struct {
  int sample_rate;
  int no_channels;
  int no_samples;
  int64_t timecode;
  const float* p_data;
  int channel_stride_in_bytes;
  const char* p_metadata;
  int64_t timestamp;
} NDIlib_audio_frame_v2_t;

}  // extern "C"

using NDIlib_send_instance_t = void*;

using fn_NDIlib_initialize = bool (*)();
using fn_NDIlib_destroy = void (*)();
using fn_NDIlib_send_create = NDIlib_send_instance_t (*)(const NDIlib_send_create_t*);
using fn_NDIlib_send_destroy = void (*)(NDIlib_send_instance_t);
using fn_NDIlib_send_send_video_v2 = void (*)(NDIlib_send_instance_t, const NDIlib_video_frame_v2_t*);
using fn_NDIlib_send_send_audio_v2 = void (*)(NDIlib_send_instance_t, const NDIlib_audio_frame_v2_t*);
using fn_NDIlib_send_get_no_connections = int (*)(NDIlib_send_instance_t, uint32_t);

struct NdiApi {
  fn_NDIlib_initialize initialize = nullptr;
  fn_NDIlib_destroy destroy = nullptr;
  fn_NDIlib_send_create send_create = nullptr;
  fn_NDIlib_send_destroy send_destroy = nullptr;
  fn_NDIlib_send_send_video_v2 send_video = nullptr;
  fn_NDIlib_send_send_audio_v2 send_audio = nullptr;
  fn_NDIlib_send_get_no_connections get_no_connections = nullptr;
  bool complete() const {
    return initialize && destroy && send_create && send_destroy && send_video && send_audio;
  }
};

#if defined(_WIN32)
using LibraryHandle = HMODULE;
LibraryHandle loadLibrary(const std::string& name) { return LoadLibraryA(name.c_str()); }
void* resolveSymbol(LibraryHandle handle, const char* symbol) {
  return reinterpret_cast<void*>(GetProcAddress(handle, symbol));
}
void unloadLibrary(LibraryHandle handle) {
  if (handle) {
    FreeLibrary(handle);
  }
}
std::string lastLoadError() { return "Win32 error " + std::to_string(GetLastError()); }
#else
using LibraryHandle = void*;
LibraryHandle loadLibrary(const std::string& name) { return dlopen(name.c_str(), RTLD_LAZY | RTLD_LOCAL); }
void* resolveSymbol(LibraryHandle handle, const char* symbol) { return dlsym(handle, symbol); }
void unloadLibrary(LibraryHandle handle) {
  if (handle) {
    dlclose(handle);
  }
}
std::string lastLoadError() {
  const char* error = dlerror();
  return error ? error : "dlopen failed";
}
#endif

std::vector<std::string> ndiLibraryCandidates() {
#if defined(_WIN32)
  std::vector<std::string> candidates = {"Processing.NDI.Lib.x64.dll"};
  const auto addCandidate = [&](const std::filesystem::path& path) {
    if (!path.empty()) {
      candidates.push_back(path.string());
    }
  };
  const auto addRuntimeRoot = [&](const char* root) {
    if (!root || root[0] == '\0') {
      return;
    }
    const std::filesystem::path base(root);
    addCandidate(base / "NDI" / "NDI 6 Runtime" / "v6" / "Bin" / "x64" / "Processing.NDI.Lib.x64.dll");
    addCandidate(base / "NDI" / "NDI 5 Runtime" / "v5" / "Bin" / "x64" / "Processing.NDI.Lib.x64.dll");
    addCandidate(base / "NewTek" / "NDI 4 Runtime" / "Bin" / "x64" / "Processing.NDI.Lib.x64.dll");
  };
  addRuntimeRoot(std::getenv("PROGRAMFILES"));
  addRuntimeRoot(std::getenv("PROGRAMFILES(X86)"));
  if (const char* ndiRuntimeDir = std::getenv("NDI_RUNTIME_DIR"); ndiRuntimeDir && ndiRuntimeDir[0] != '\0') {
    addCandidate(std::filesystem::path(ndiRuntimeDir) / "Processing.NDI.Lib.x64.dll");
  }
  candidates.push_back("Processing.NDI.Lib.x86.dll");
  return candidates;
#else
  return {"libndi.so.5", "libndi.so.4", "libndi.so", "libndi.dylib"};
#endif
}

LibraryHandle openNdiLibrary(RuntimeProbe& probe) {
  const auto candidates = ndiLibraryCandidates();
  for (const auto& candidate : candidates) {
    if (LibraryHandle handle = loadLibrary(candidate)) {
      probe.candidates.push_back({candidate, "available", "library loaded"});
      probe.available = true;
      probe.detail = std::string("available:") + candidate;
      return handle;
    }
    probe.candidates.push_back({candidate, "unavailable", lastLoadError()});
  }
  probe.available = false;
  probe.detail = "missing:libNDI runtime";
  return nullptr;
}

bool resolveNdiApi(LibraryHandle handle, NdiApi& api) {
  api.initialize = reinterpret_cast<fn_NDIlib_initialize>(resolveSymbol(handle, "NDIlib_initialize"));
  api.destroy = reinterpret_cast<fn_NDIlib_destroy>(resolveSymbol(handle, "NDIlib_destroy"));
  api.send_create = reinterpret_cast<fn_NDIlib_send_create>(resolveSymbol(handle, "NDIlib_send_create"));
  api.send_destroy = reinterpret_cast<fn_NDIlib_send_destroy>(resolveSymbol(handle, "NDIlib_send_destroy"));
  api.send_video = reinterpret_cast<fn_NDIlib_send_send_video_v2>(resolveSymbol(handle, "NDIlib_send_send_video_v2"));
  api.send_audio = reinterpret_cast<fn_NDIlib_send_send_audio_v2>(resolveSymbol(handle, "NDIlib_send_send_audio_v2"));
  api.get_no_connections = reinterpret_cast<fn_NDIlib_send_get_no_connections>(resolveSymbol(handle, "NDIlib_send_get_no_connections"));
  return api.complete();
}

const OutputDestinationSettings* findNdiSettings(const std::vector<OutputDestinationSettings>& destinationSettings) {
  for (const auto& settings : destinationSettings) {
    if (settings.id == "ndi" || settings.protocol == "ndi") {
      return &settings;
    }
  }
  return nullptr;
}

// Convert a packed BGRA buffer to packed UYVY (4:2:2). NDI's SpeedHQ path is fed
// most efficiently with UYVY, so we down-sample chroma horizontally here and
// hand the result to send_video_v2. Output stride is width*2 bytes.
void convertBgraToUyvy(const uint8_t* bgra, int width, int height, std::vector<uint8_t>& uyvy) {
  const size_t outBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 2u;
  uyvy.resize(outBytes);
  const auto clamp8 = [](int value) -> uint8_t {
    return static_cast<uint8_t>(value < 0 ? 0 : (value > 255 ? 255 : value));
  };
  const auto rgbToY = [&](int b, int g, int r) {
    return clamp8(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
  };
  const auto rgbToU = [&](int b, int g, int r) {
    return clamp8(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
  };
  const auto rgbToV = [&](int b, int g, int r) {
    return clamp8(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
  };
  size_t out = 0;
  for (int y = 0; y < height; ++y) {
    const uint8_t* row = bgra + static_cast<size_t>(y) * static_cast<size_t>(width) * 4u;
    for (int x = 0; x + 1 < width; x += 2) {
      const uint8_t* p0 = row + static_cast<size_t>(x) * 4u;
      const uint8_t* p1 = p0 + 4u;
      const int b0 = p0[0], g0 = p0[1], r0 = p0[2];
      const int b1 = p1[0], g1 = p1[1], r1 = p1[2];
      const int avgB = (b0 + b1) / 2;
      const int avgG = (g0 + g1) / 2;
      const int avgR = (r0 + r1) / 2;
      uyvy[out++] = rgbToU(avgB, avgG, avgR);  // U
      uyvy[out++] = rgbToY(b0, g0, r0);        // Y0
      uyvy[out++] = rgbToV(avgB, avgG, avgR);  // V
      uyvy[out++] = rgbToY(b1, g1, r1);        // Y1
    }
    if (width % 2 != 0) {
      // Odd trailing pixel: emit it as its own macropixel pair.
      const uint8_t* p0 = row + static_cast<size_t>(width - 1) * 4u;
      const int b0 = p0[0], g0 = p0[1], r0 = p0[2];
      uyvy[out++] = rgbToU(b0, g0, r0);
      uyvy[out++] = rgbToY(b0, g0, r0);
      uyvy[out++] = rgbToV(b0, g0, r0);
      uyvy[out++] = rgbToY(b0, g0, r0);
    }
  }
}

int64_t estimatedFrameBytes(double bitrateMbps, int fps) {
  const int safeFps = (std::max)(1, fps);
  return static_cast<int64_t>((bitrateMbps * 1000000.0 / 8.0 / static_cast<double>(safeFps)) + 0.5);
}

class NdiOutputSender final : public IOutputSender {
 public:
  explicit NdiOutputSender(RuntimeProbe runtimeProbe, LibraryHandle library, NdiApi api)
      : runtimeProbe_(std::move(runtimeProbe)),
        runtimeDetail_(runtimeProbe_.detail),
        runtimeAvailable_(runtimeProbe_.available),
        library_(library),
        api_(api) {}

  ~NdiOutputSender() override {
    destroySender();
    if (ndiInitialized_ && api_.destroy) {
      api_.destroy();
    }
    unloadLibrary(library_);
  }

  // NOTE FOR INTEGRATION: this matches the CURRENT IOutputSender::sync signature
  // (destinations, const ProgramFrame*, elapsedMs, destinationSettings). A
  // parallel agent is widening sync(...) to carry the program-audio PCM tap.
  // Once that lands, the program-audio parameter should be forwarded to
  // submitProgramAudio() below (send_audio_v2), replacing the silence keep-alive.
  OutputSenderSession sync(
      const std::vector<std::string>& destinations,
      const ProgramFrame* frame,
      double elapsedMs,
      const std::vector<OutputDestinationSettings>& destinationSettings = {},
      const std::vector<float>* programAudioPcm = nullptr,
      int audioChannels = 0,
      int audioSampleRate = 0) override {
    const bool wantsNdi = std::find(destinations.begin(), destinations.end(), "ndi") != destinations.end();
    if (!wantsNdi) {
      destroySender();
      if (sender_.status != "idle" && sender_.status != "stopped") {
        sender_.status = "stopped";
        sender_.stoppedAtMs = elapsedMs;
        sender_.warning.clear();
        sender_.destinationHealth = "stopped";
        sender_.lastResultCode = "stopped";
      }
      return snapshot();
    }

    ensureSender(elapsedMs);
    const auto* settings = findNdiSettings(destinationSettings);
    if (settings) {
      const std::string requested = !settings->ndiName.empty()
                                        ? settings->ndiName
                                        : !settings->streamKey.empty() ? settings->streamKey : settings->label;
      const auto parsed = parseNdiSourceName(requested.empty() ? "Program" : requested);
      if (parsed.valid) {
        configuredSourceName_ = parsed.canonical;
      } else if (!parsed.errors.empty()) {
        configuredSourceNameError_ = parsed.errors.front();
      }
      configuredFps_ = (std::max)(1, settings->fps);
      sender_.bitrateMbps = (std::max)(0.5, settings->targetBitrateMbps);
    }
    if (configuredSourceName_.empty()) {
      const auto fallback = parseNdiSourceName("Program");
      configuredSourceName_ = fallback.canonical;
    }

    sender_.runtimeDetail = runtimeDetail_;

    if (!runtimeAvailable_ || !api_.complete()) {
      sender_.status = "warning";
      sender_.warning = "NDI sender requires the libNDI runtime on this machine (" + runtimeDetail_ + ").";
      sender_.destinationHealth = "warning";
      sender_.lastResultCode = "runtime-missing";
      sender_.lastError = sender_.warning;
      return snapshot();
    }

    if (!configuredSourceNameError_.empty()) {
      sender_.status = "warning";
      sender_.warning = "NDI source name rejected: " + configuredSourceNameError_;
      sender_.destinationHealth = "warning";
      sender_.lastResultCode = "source-name-invalid";
      sender_.lastError = sender_.warning;
      return snapshot();
    }

    if (!ensureSenderInstance()) {
      sender_.status = "failed";
      sender_.warning = "NDIlib_send_create failed for source \"" + configuredSourceName_ + "\".";
      sender_.destinationHealth = "failed";
      sender_.lastResultCode = "send-create-failed";
      sender_.lastError = sender_.warning;
      return snapshot();
    }

    if (!frame || frame->frameNumber == 0) {
      sender_.status = "starting";
      sender_.warning = "NDI sender is waiting for a program frame.";
      sender_.destinationHealth = "starting";
      sender_.lastResultCode = "waiting-for-frame";
      return snapshot();
    }
    const bool hasProgramPixels =
        (!frame->programNv12Bytes().empty() && frame->programNv12Width > 0 && frame->programNv12Height > 0) ||
        (!frame->programFullBgra.bgra.empty() && frame->programFullBgra.width > 0) ||
        (!frame->preview.bgra.empty() && frame->preview.width > 0 && frame->preview.height > 0);
    if (!hasProgramPixels) {
      sender_.status = "warning";
      sender_.warning = "NDI sender is waiting for composed program pixels.";
      sender_.destinationHealth = "warning";
      sender_.lastResultCode = "frame-pixels-missing";
      sender_.lastError = sender_.warning;
      return snapshot();
    }

    submitProgramVideo(*frame);
    submitProgramAudio(programAudioPcm, audioChannels, audioSampleRate);
    refreshConnectionCount();

    sender_.status = "live";
    sender_.warning.clear();
    sender_.lastFrameNumber = frame->frameNumber;
    ++sender_.framesSent;
    sender_.bytesSent += estimatedFrameBytes(sender_.bitrateMbps, configuredFps_);
    sender_.destinationHealth = connectionCount_ > 0 ? "ok" : "starting";
    sender_.lastResultCode = "ok";
    return snapshot();
  }

  OutputSenderSession fail(const std::string& destination, const std::string& message, double elapsedMs) override {
    if (destination != "ndi") {
      return snapshot();
    }
    ensureSender(elapsedMs);
    sender_.status = "failed";
    sender_.stoppedAtMs = elapsedMs;
    ++sender_.retryCount;
    sender_.warning = message;
    sender_.destinationHealth = "failed";
    sender_.lastResultCode = "failed";
    sender_.lastError = message;
    return snapshot();
  }

  OutputSenderSession recover(const std::string& destination, double elapsedMs, const std::string& reason) override {
    if (destination != "ndi") {
      return snapshot();
    }
    destroySender();
    ensureSender(elapsedMs);
    sender_.status = runtimeAvailable_ ? "starting" : "warning";
    sender_.startedAtMs = elapsedMs;
    sender_.stoppedAtMs = 0;
    sender_.warning = reason.empty() ? "NDI sender recovered." : reason;
    sender_.runtimeDetail = runtimeDetail_;
    sender_.destinationHealth = runtimeAvailable_ ? "starting" : "warning";
    sender_.lastResultCode = "recovered";
    return snapshot();
  }

  OutputSenderSession session() const override { return snapshot(); }

 private:
  void ensureSender(double elapsedMs) {
    if (!sender_.senderId.empty()) {
      return;
    }
    sender_.senderId = "ndi:program";
    sender_.destination = "ndi";
    sender_.status = "starting";
    sender_.startedAtMs = elapsedMs;
    sender_.latencyMs = 16;  // NDI is a near-zero-latency LAN transport.
    sender_.bitrateMbps = 125.0;  // 1080p60 SpeedHQ estimate (matches ndiOutput.ts).
    sender_.runtimeDetail = runtimeDetail_;
    sender_.destinationHealth = "starting";
    sender_.lastResultCode = "waiting-for-frame";
  }

  bool ensureSenderInstance() {
    if (sendInstance_ && activeSourceName_ == configuredSourceName_) {
      return true;
    }
    destroySender();
    if (!ndiInitialized_) {
      if (!api_.initialize()) {
        return false;
      }
      ndiInitialized_ = true;
    }
    NDIlib_send_create_t create{};
    create.p_ndi_name = configuredSourceName_.c_str();
    create.p_groups = nullptr;
    create.clock_video = true;
    create.clock_audio = true;
    sendInstance_ = api_.send_create(&create);
    if (!sendInstance_) {
      return false;
    }
    activeSourceName_ = configuredSourceName_;
    return true;
  }

  // Publish the FULL-RESOLUTION program. This used to send `frame.preview` — the
  // 320x180 UI thumbnail — so every NDI receiver (Studio Monitor, OBS NDI) saw a
  // postage stamp instead of the show. Same defect the program recorder had: the
  // preview is a UI artifact, not the program. Source order:
  //   1. programNv12      — the Windows GPU tap (also feeds vcam + RTMP)
  //   2. programFullBgra  — the macOS Metal full readback
  //   3. preview          — last resort, and now LOUD in the snapshot
  void submitProgramVideo(const ProgramFrame& frame) {
    int width = 0;
    int height = 0;
    const auto& programNv12 = frame.programNv12Bytes();
    if (!programNv12.empty() && frame.programNv12Width > 0 && frame.programNv12Height > 0 &&
        nv12ToUyvy(programNv12.data(), frame.programNv12Width, frame.programNv12Height,
                   programNv12.size(), uyvyBuffer_)) {
      width = frame.programNv12Width;
      height = frame.programNv12Height;
      videoSourceDetail_ = "program-nv12";
    } else if (!frame.programFullBgra.bgra.empty() && frame.programFullBgra.width > 0 &&
               frame.programFullBgra.height > 0) {
      convertBgraToUyvy(frame.programFullBgra.bgra.data(), frame.programFullBgra.width,
                        frame.programFullBgra.height, uyvyBuffer_);
      width = frame.programFullBgra.width;
      height = frame.programFullBgra.height;
      videoSourceDetail_ = "program-bgra";
    } else {
      convertBgraToUyvy(frame.preview.bgra.data(), frame.preview.width, frame.preview.height,
                        uyvyBuffer_);
      width = frame.preview.width;
      height = frame.preview.height;
      videoSourceDetail_ = "PREVIEW-THUMBNAIL";  // never silent: receivers see a postage stamp
    }

    NDIlib_video_frame_v2_t video{};
    video.xres = width;
    video.yres = height;
    video.FourCC = NDIlib_FourCC_type_UYVY;
    video.frame_rate_N = configuredFps_ * 1000;
    video.frame_rate_D = 1000;
    video.picture_aspect_ratio = static_cast<float>(width) / static_cast<float>((std::max)(1, height));
    video.frame_format_type = NDIlib_frame_format_type_progressive;
    video.timecode = INT64_MAX;  // NDIlib_send_timecode_synthesize
    video.p_data = uyvyBuffer_.data();
    video.line_stride_in_bytes = width * 2;
    video.p_metadata = nullptr;
    video.timestamp = 0;
    api_.send_video(sendInstance_, &video);
  }

  // Send the real F2 program-audio tap to NDI receivers. The tap is interleaved
  // stereo float; NDI's audio_frame_v2 is planar, so deinterleave (mono fans out
  // to both channels) into [L...][R...]. Falls back to a silent keep-alive buffer
  // when no PCM is routed this tick so receivers keep a continuous audio clock.
  void submitProgramAudio(const std::vector<float>* pcm, int channels, int sampleRate) {
    if (pcm == nullptr || pcm->empty() || channels <= 0) {
      submitProgramAudioKeepAlive();
      return;
    }
    const int outChannels = 2;
    const int samplesPerChannel = static_cast<int>(pcm->size() / static_cast<size_t>(channels));
    if (samplesPerChannel <= 0) {
      submitProgramAudioKeepAlive();
      return;
    }
    audioBuffer_.assign(static_cast<size_t>(samplesPerChannel) * static_cast<size_t>(outChannels), 0.0f);
    for (int i = 0; i < samplesPerChannel; ++i) {
      const float left = (*pcm)[static_cast<size_t>(i) * static_cast<size_t>(channels)];
      const float right = channels == 1 ? left : (*pcm)[static_cast<size_t>(i) * static_cast<size_t>(channels) + 1];
      audioBuffer_[static_cast<size_t>(i)] = left;
      audioBuffer_[static_cast<size_t>(samplesPerChannel) + static_cast<size_t>(i)] = right;
    }
    NDIlib_audio_frame_v2_t audio{};
    audio.sample_rate = sampleRate > 0 ? sampleRate : 48000;
    audio.no_channels = outChannels;
    audio.no_samples = samplesPerChannel;
    audio.timecode = INT64_MAX;
    audio.p_data = audioBuffer_.data();
    audio.channel_stride_in_bytes = samplesPerChannel * static_cast<int>(sizeof(float));
    audio.p_metadata = nullptr;
    audio.timestamp = 0;
    api_.send_audio(sendInstance_, &audio);
  }

  // Push a short silent buffer so receivers see a continuous audio clock when no
  // program PCM is routed this tick.
  void submitProgramAudioKeepAlive() {
    constexpr int kChannels = 2;
    const int samplesPerChannel = 48000 / (std::max)(1, configuredFps_);
    audioBuffer_.assign(static_cast<size_t>(samplesPerChannel) * static_cast<size_t>(kChannels), 0.0f);
    NDIlib_audio_frame_v2_t audio{};
    audio.sample_rate = 48000;
    audio.no_channels = kChannels;
    audio.no_samples = samplesPerChannel;
    audio.timecode = INT64_MAX;
    audio.p_data = audioBuffer_.data();
    audio.channel_stride_in_bytes = samplesPerChannel * static_cast<int>(sizeof(float));
    audio.p_metadata = nullptr;
    audio.timestamp = 0;
    api_.send_audio(sendInstance_, &audio);
  }

  void refreshConnectionCount() {
    if (api_.get_no_connections && sendInstance_) {
      connectionCount_ = api_.get_no_connections(sendInstance_, 0);
    }
  }

  void destroySender() {
    if (sendInstance_ && api_.send_destroy) {
      api_.send_destroy(sendInstance_);
    }
    sendInstance_ = nullptr;
    activeSourceName_.clear();
    connectionCount_ = 0;
  }

  OutputSenderSession snapshot() const {
    OutputSenderSession session;
    if (!sender_.senderId.empty()) {
      OutputSender sender = sender_;
      // Surface the live receiver count via retryCount-adjacent fields would be
      // wrong; instead encode it into runtimeDetail so the snapshot carries the
      // real connection counter without a protocol change.
      sender.runtimeDetail = runtimeDetail_ + " connections=" + std::to_string(connectionCount_) +
                             " source=\"" + configuredSourceName_ + "\"" +
                             " video=" + videoSourceDetail_;
      session.senders.push_back(sender);
      if (sender.status == "live" || sender.status == "warning" || sender.status == "starting") {
        session.activeSenderCount = 1;
      }
      if (!sender.warning.empty()) {
        session.warnings.push_back(sender.warning);
      }
      session.status = sender.status == "failed" ? "failed"
                       : !sender.warning.empty() || sender.status == "warning" ? "warning"
                       : session.activeSenderCount > 0 ? "live"
                                                       : "idle";
    }
    return session;
  }

  RuntimeProbe runtimeProbe_;
  std::string runtimeDetail_;
  bool runtimeAvailable_ = false;
  LibraryHandle library_ = nullptr;
  NdiApi api_;
  bool ndiInitialized_ = false;
  NDIlib_send_instance_t sendInstance_ = nullptr;
  std::string configuredSourceName_;
  std::string configuredSourceNameError_;
  std::string activeSourceName_;
  int configuredFps_ = 30;
  int connectionCount_ = 0;
  std::vector<uint8_t> uyvyBuffer_;
  // Which program source the last frame came from. Surfaced in the snapshot so
  // a fallback to the 320x180 preview is visible instead of silently shipping a
  // postage stamp to every receiver.
  std::string videoSourceDetail_ = "none";
  std::vector<float> audioBuffer_;
  OutputSender sender_;
};

#endif  // dev gate

}  // namespace

std::unique_ptr<IOutputSender> createNdiOutputSender() {
#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_NDI_OUTPUT
  // REQUIRES DEV MACHINE: a real NDI sender needs the libNDI runtime. We
  // runtime-probe the shared library (mirrors the RTMP FFmpeg-probe approach);
  // when it is absent we return a sender that reports a clear runtime-missing
  // warning rather than nullptr, so the operator sees why NDI is unavailable.
  RuntimeProbe probe;
  LibraryHandle library = openNdiLibrary(probe);
  NdiApi api;
  if (library && !resolveNdiApi(library, api)) {
    probe.available = false;
    probe.detail = "incomplete:libNDI symbol set";
    probe.candidates.push_back({"NDIlib_send_*", "unavailable", "required v5 send entrypoints not resolved"});
  }
  return std::make_unique<NdiOutputSender>(std::move(probe), library, api);
#else
  return nullptr;
#endif
}

}  // namespace corevideo::modules
