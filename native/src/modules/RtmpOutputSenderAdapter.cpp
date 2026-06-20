#include "modules/Interfaces.h"
#include "modules/RtmpCompatibility.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_RTMP_OUTPUT
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#endif

namespace corevideo::modules {
namespace {

#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_RTMP_OUTPUT
struct RuntimeCandidate {
  std::string name;
  std::string status;
  std::string detail;
};

struct RuntimeProbe {
  bool available = false;
  std::string detail;
  std::string ffmpegExecutable;
  std::vector<RuntimeCandidate> candidates;
};

std::string jsonEscape(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size() + 8);
  for (const char ch : value) {
    switch (ch) {
      case '\\':
        escaped += "\\\\";
        break;
      case '"':
        escaped += "\\\"";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        escaped += ch;
        break;
    }
  }
  return escaped;
}

std::string jsonString(const std::string& value) {
  return "\"" + jsonEscape(value) + "\"";
}

std::string runtimeCandidatesJson(const std::vector<RuntimeCandidate>& candidates) {
  std::string json = "[";
  for (size_t index = 0; index < candidates.size(); ++index) {
    if (index > 0) {
      json += ",";
    }
    json += "{\"name\":" + jsonString(candidates[index].name) +
            ",\"status\":" + jsonString(candidates[index].status) +
            ",\"detail\":" + jsonString(candidates[index].detail) + "}";
  }
  json += "]";
  return json;
}

RuntimeProbe probeLibavformatRuntime() {
  RuntimeProbe probe;
#if defined(_WIN32)
  const char* candidates[] = {"avformat-62.dll", "avformat-61.dll", "avformat-60.dll", "avformat-59.dll", "avformat.dll"};
  for (const auto* candidate : candidates) {
    if (HMODULE module = LoadLibraryA(candidate)) {
      char modulePath[MAX_PATH] = {};
      const auto pathLength = GetModuleFileNameA(module, modulePath, MAX_PATH);
      FreeLibrary(module);
      probe.available = true;
      probe.detail = std::string("available:") + candidate;
      probe.candidates.push_back({candidate, "available", pathLength > 0 ? std::string(modulePath, pathLength) : "LoadLibraryA succeeded"});
      return probe;
    }
    const auto error = GetLastError();
    probe.candidates.push_back({candidate, "unavailable", "LoadLibraryA failed with Win32 error " + std::to_string(error)});
  }
  probe.detail = "missing:avformat-62.dll,avformat-61.dll,avformat-60.dll,avformat-59.dll,avformat.dll";
#else
  const char* candidates[] = {"libavformat.so.62", "libavformat.so.61", "libavformat.so.60", "libavformat.so.59", "libavformat.so"};
  for (const auto* candidate : candidates) {
    if (void* module = dlopen(candidate, RTLD_LAZY | RTLD_LOCAL)) {
      dlclose(module);
      probe.available = true;
      probe.detail = std::string("available:") + candidate;
      probe.candidates.push_back({candidate, "available", "dlopen succeeded"});
      return probe;
    }
    const char* error = dlerror();
    probe.candidates.push_back({candidate, "unavailable", error ? error : "dlopen failed"});
  }
  probe.detail = "missing:libavformat.so.62,libavformat.so.61,libavformat.so.60,libavformat.so.59,libavformat.so";
#endif
  return probe;
}

std::string trimTrailingSlash(std::string value) {
  while (!value.empty() && (value.back() == '/' || value.back() == '\\')) {
    value.pop_back();
  }
  return value;
}

std::string lowercaseAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

std::string normalizeVideoCodec(std::string value) {
  value = lowercaseAscii(std::move(value));
  if (value == "hevc") {
    return "h265";
  }
  if (value == "h264" || value == "h265" || value == "av1") {
    return value;
  }
  return "h264";
}

std::string normalizeEncoderMode(std::string value) {
  value = lowercaseAscii(std::move(value));
  if (value == "nvenc" || value == "qsv" || value == "amf" || value == "cpu") {
    return value;
  }
  return "auto";
}

std::string buildRtmpEndpoint(const OutputDestinationSettings& settings) {
  std::string endpoint = trimTrailingSlash(settings.url);
  if (!settings.streamKey.empty()) {
    endpoint += "/" + settings.streamKey;
  }
  return endpoint;
}

std::string redactedEndpoint(const std::string& endpoint, const std::string& streamKey) {
  if (streamKey.empty()) {
    return endpoint;
  }
  const auto position = endpoint.rfind(streamKey);
  if (position == std::string::npos) {
    return endpoint;
  }
  return endpoint.substr(0, position) + "<stream-key>";
}

const OutputDestinationSettings* findRtmpSettings(const std::vector<OutputDestinationSettings>& destinationSettings) {
  for (const auto& settings : destinationSettings) {
    if (settings.id == "rtmp" || settings.protocol == "rtmp" || settings.protocol == "rtmps") {
      return &settings;
    }
  }
  return nullptr;
}

bool fileExists(const std::filesystem::path& path) {
  std::error_code error;
  return std::filesystem::is_regular_file(path, error);
}

std::string resolveFfmpegExecutable(const std::string& configuredBinDirectory) {
#if defined(_WIN32)
  constexpr const char* executableName = "ffmpeg.exe";
#else
  constexpr const char* executableName = "ffmpeg";
#endif
  std::vector<std::filesystem::path> candidates;
  if (!configuredBinDirectory.empty()) {
    candidates.emplace_back(std::filesystem::path(configuredBinDirectory) / executableName);
  }
  if (const char* env = std::getenv("COREVIDEO_FFMPEG_BIN_DIR"); env && *env) {
    candidates.emplace_back(std::filesystem::path(env) / executableName);
  }
  if (const char* env = std::getenv("FFMPEG_BIN_DIR"); env && *env) {
    candidates.emplace_back(std::filesystem::path(env) / executableName);
  }
  candidates.emplace_back(std::filesystem::current_path() / executableName);

  for (const auto& candidate : candidates) {
    if (fileExists(candidate)) {
      return candidate.string();
    }
  }
#if defined(_WIN32)
  char resolved[MAX_PATH] = {};
  if (SearchPathA(nullptr, executableName, nullptr, MAX_PATH, resolved, nullptr) > 0) {
    return resolved;
  }
#else
  if (const char* pathEnv = std::getenv("PATH"); pathEnv && *pathEnv) {
    std::stringstream pathStream(pathEnv);
    std::string entry;
    while (std::getline(pathStream, entry, ':')) {
      const auto candidate = std::filesystem::path(entry) / executableName;
      if (fileExists(candidate)) {
        return candidate.string();
      }
    }
  }
#endif
  return {};
}

RuntimeProbe probeFfmpegRuntime(const std::string& configuredBinDirectory) {
  RuntimeProbe probe = probeLibavformatRuntime();
  const auto executable = resolveFfmpegExecutable(configuredBinDirectory);
  if (!executable.empty()) {
    probe.available = true;
    probe.ffmpegExecutable = executable;
    probe.detail = "available:" + executable;
    probe.candidates.push_back({"ffmpeg", "available", executable});
  } else {
    probe.candidates.push_back({"ffmpeg", "unavailable", "ffmpeg executable was not found in configured bin directory, app directory, or PATH"});
    probe.available = false;
    probe.detail = "missing:ffmpeg executable";
  }
  return probe;
}

int64_t estimatedFrameBytes(double bitrateMbps) {
  return static_cast<int64_t>((bitrateMbps * 1000000.0 / 8.0 / 30.0) + 0.5);
}

std::string quoteArgument(const std::string& value) {
  std::string quoted = "\"";
  for (const char ch : value) {
    if (ch == '"') {
      quoted += "\\\"";
    } else {
      quoted += ch;
    }
  }
  quoted += "\"";
  return quoted;
}

std::string ffmpegVideoEncoderFor(const std::string& codec, const std::string& encoderMode) {
  const auto normalizedCodec = normalizeVideoCodec(codec);
  const auto normalizedMode = normalizeEncoderMode(encoderMode);
  if (normalizedMode == "nvenc") {
    if (normalizedCodec == "h265") {
      return "hevc_nvenc";
    }
    if (normalizedCodec == "av1") {
      return "av1_nvenc";
    }
    return "h264_nvenc";
  }
  if (normalizedMode == "qsv") {
    if (normalizedCodec == "h265") {
      return "hevc_qsv";
    }
    if (normalizedCodec == "av1") {
      return "av1_qsv";
    }
    return "h264_qsv";
  }
  if (normalizedMode == "amf") {
    if (normalizedCodec == "h265") {
      return "hevc_amf";
    }
    if (normalizedCodec == "av1") {
      return "av1_amf";
    }
    return "h264_amf";
  }
  if (normalizedCodec == "h265") {
    return "libx265";
  }
  if (normalizedCodec == "av1") {
    return "libsvtav1";
  }
  return "libx264";
}

std::string encoderSpecificArguments(const std::string& encoderName) {
  if (encoderName.find("_nvenc") != std::string::npos) {
    return " -preset p4 -tune ll -rc cbr";
  }
  if (encoderName.find("_qsv") != std::string::npos) {
    return " -preset veryfast";
  }
  if (encoderName.find("_amf") != std::string::npos) {
    return " -quality speed -rc cbr";
  }
  if (encoderName == "libsvtav1") {
    return " -preset 8";
  }
  if (encoderName == "libx265") {
    return " -preset veryfast -tune zerolatency";
  }
  return " -preset veryfast -tune zerolatency";
}

class RtmpOutputSender final : public IOutputSender {
 public:
  explicit RtmpOutputSender(RuntimeProbe runtimeProbe)
      : runtimeProbe_(std::move(runtimeProbe)), runtimeDetail_(runtimeProbe_.detail), runtimeAvailable_(runtimeProbe_.available) {}

  ~RtmpOutputSender() override { stopFfmpegProcess(); }

  OutputSenderSession sync(
      const std::vector<std::string>& destinations,
      const ProgramFrame* frame,
      double elapsedMs,
      const std::vector<OutputDestinationSettings>& destinationSettings = {}) override {
    const bool wantsRtmp = std::find(destinations.begin(), destinations.end(), "rtmp") != destinations.end();
    if (!wantsRtmp) {
      stopFfmpegProcess();
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
    const auto* settings = findRtmpSettings(destinationSettings);
    configuredEndpoint_ = settings ? buildRtmpEndpoint(*settings) : configuredEndpoint_;
    configuredStreamKey_ = settings ? settings->streamKey : configuredStreamKey_;
    configuredFfmpegBinDirectory_ = settings ? settings->ffmpegBinDirectory : configuredFfmpegBinDirectory_;
    configuredFps_ = settings ? (std::max)(1, settings->fps) : configuredFps_;
    configuredVideoCodec_ = settings ? normalizeVideoCodec(settings->videoCodec) : configuredVideoCodec_;
    configuredEncoderMode_ = settings ? normalizeEncoderMode(settings->encoderMode) : configuredEncoderMode_;
    sender_.bitrateMbps = settings ? (std::max)(0.5, settings->targetBitrateMbps) : sender_.bitrateMbps;
    runtimeProbe_ = probeFfmpegRuntime(configuredFfmpegBinDirectory_);
    runtimeDetail_ = runtimeProbe_.detail;
    // Surface a codec/container compatibility note (e.g. H.265 -> H.264 fallback)
    // so the operator sees why the on-air codec may differ from the request.
    const auto codecCompatibility = resolveRtmpCompatibility(configuredVideoCodec_, /*allowEnhancedRtmp=*/false);
    if (!codecCompatibility.warning.empty()) {
      runtimeDetail_ += (runtimeDetail_.empty() ? "" : " ") + codecCompatibility.warning;
    }
    runtimeAvailable_ = runtimeProbe_.available;
    if (!runtimeProbe_.ffmpegExecutable.empty()) {
      ffmpegExecutable_ = runtimeProbe_.ffmpegExecutable;
    }
    sender_.runtimeDetail = runtimeDetail_;
    openSendProofIfNeeded();
    if (configuredEndpoint_.empty()) {
      sender_.status = "warning";
      sender_.warning = "RTMP sender needs a configured RTMP/RTMPS server URL and stream key.";
      sender_.destinationHealth = "warning";
      sender_.lastResultCode = "endpoint-missing";
      sender_.lastError = sender_.warning;
      appendSendProof(nullptr, "endpoint-missing");
      return snapshot();
    }
    if (!runtimeAvailable_) {
      sender_.status = "warning";
      sender_.warning = "RTMP sender requires FFmpeg runtime on this machine (" + runtimeDetail_ + ").";
      sender_.runtimeDetail = runtimeDetail_;
      sender_.destinationHealth = "warning";
      sender_.lastResultCode = "runtime-missing";
      sender_.lastError = sender_.warning;
      appendSendProof(nullptr, "runtime-missing");
      return snapshot();
    }
    if (!frame || frame->frameNumber == 0) {
      sender_.status = "starting";
      sender_.warning = "RTMP sender is waiting for a program frame.";
      sender_.destinationHealth = "starting";
      sender_.lastResultCode = "waiting-for-frame";
      appendSendProof(frame, "waiting-for-frame");
      return snapshot();
    }
    if (frame->preview.width <= 0 || frame->preview.height <= 0 || frame->preview.bgra.empty()) {
      sender_.status = "warning";
      sender_.warning = "RTMP sender is waiting for composed BGRA program pixels.";
      sender_.destinationHealth = "warning";
      sender_.lastResultCode = "frame-pixels-missing";
      sender_.lastError = sender_.warning;
      appendSendProof(frame, "frame-pixels-missing");
      return snapshot();
    }

    if (!ensureFfmpegProcess(*frame, elapsedMs)) {
      appendSendProof(frame, "ffmpeg-start-failed");
      return snapshot();
    }

    if (!writeFrameToFfmpeg(*frame)) {
      sender_.status = "failed";
      ++sender_.retryCount;
      sender_.warning = "FFmpeg stdin write failed; the RTMP process stopped or rejected frames.";
      sender_.destinationHealth = "failed";
      sender_.lastResultCode = "ffmpeg-write-failed";
      sender_.lastError = sender_.warning;
      stopFfmpegProcess();
      appendSendProof(frame, "ffmpeg-write-failed");
      return snapshot();
    }

    sender_.status = "live";
    sender_.warning.clear();
    sender_.runtimeDetail = runtimeDetail_;
    sender_.lastFrameNumber = frame->frameNumber;
    ++sender_.framesSent;
    sender_.bytesSent += estimatedFrameBytes(sender_.bitrateMbps);
    sender_.destinationHealth = "ok";
    sender_.lastResultCode = "ok";
    appendSendProof(frame, "sent");
    return snapshot();
  }

  OutputSenderSession fail(const std::string& destination, const std::string& message, double elapsedMs) override {
    if (destination != "rtmp") {
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
    if (destination != "rtmp") {
      return snapshot();
    }
    stopFfmpegProcess();
    runtimeProbe_ = probeFfmpegRuntime(configuredFfmpegBinDirectory_);
    runtimeDetail_ = runtimeProbe_.detail;
    runtimeAvailable_ = runtimeProbe_.available;
    ensureSender(elapsedMs);
    sender_.status = runtimeAvailable_ ? "starting" : "warning";
    sender_.startedAtMs = elapsedMs;
    sender_.stoppedAtMs = 0;
    sender_.warning = reason.empty() ? "RTMP sender recovered." : reason;
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
    sender_.senderId = "rtmp:program";
    sender_.destination = "rtmp";
    sender_.status = "starting";
    sender_.startedAtMs = elapsedMs;
    sender_.latencyMs = 2100;
    sender_.bitrateMbps = 6.0;
    sender_.runtimeDetail = runtimeDetail_;
    sender_.destinationHealth = "starting";
    sender_.lastResultCode = "waiting-for-frame";
  }

  bool ensureFfmpegProcess(const ProgramFrame& frame, double elapsedMs) {
    const bool sizeChanged = frame.preview.width != ffmpegFrameWidth_ || frame.preview.height != ffmpegFrameHeight_;
    const bool endpointChanged = configuredEndpoint_ != activeEndpoint_;
    const bool executableChanged = ffmpegExecutable_ != activeFfmpegExecutable_;
    const bool fpsChanged = configuredFps_ != activeFps_;
    const bool bitrateChanged = sender_.bitrateMbps != activeBitrateMbps_;
    const bool codecChanged = configuredVideoCodec_ != activeVideoCodec_;
    const bool encoderModeChanged = configuredEncoderMode_ != activeEncoderMode_;
    if (ffmpegRunning_ && !sizeChanged && !endpointChanged && !executableChanged && !fpsChanged && !bitrateChanged && !codecChanged && !encoderModeChanged) {
      return true;
    }

    stopFfmpegProcess();
    ffmpegFrameWidth_ = frame.preview.width;
    ffmpegFrameHeight_ = frame.preview.height;
    activeEndpoint_ = configuredEndpoint_;
    activeFfmpegExecutable_ = ffmpegExecutable_;
    activeFps_ = configuredFps_;
    activeBitrateMbps_ = sender_.bitrateMbps;
    activeVideoCodec_ = configuredVideoCodec_;
    activeEncoderMode_ = configuredEncoderMode_;
    if (startFfmpegProcess(frame.preview.width, frame.preview.height)) {
      sender_.startedAtMs = elapsedMs;
      sender_.destinationHealth = "starting";
      sender_.lastResultCode = "ffmpeg-started";
      return true;
    }
    return false;
  }

  std::string buildFfmpegArguments(int width, int height) const {
    const int bitrateKbps = static_cast<int>((std::max)(1000.0, sender_.bitrateMbps * 1000.0));
    const int bufferKbps = bitrateKbps * 2;
    std::ostringstream args;
    const int fps = (std::max)(1, configuredFps_);
    // Resolve the requested codec to an RTMP-compatible one (H.265/AV1 fall back
    // to H.264 unless enhanced-RTMP is enabled) so the encoded stream always
    // matches what the FLV transport can carry.
    const auto compatibility = resolveRtmpCompatibility(configuredVideoCodec_, /*allowEnhancedRtmp=*/false);
    const auto videoEncoder = ffmpegVideoEncoderFor(compatibility.videoCodec, configuredEncoderMode_);
    args << " -hide_banner -loglevel warning"
         << " -f rawvideo -pix_fmt bgra -s " << width << "x" << height
         << " -r " << fps << " -i pipe:0"
         << " -f lavfi -i anullsrc=channel_layout=stereo:sample_rate=48000"
         << " -map 0:v:0 -map 1:a:0"
         << " -c:v " << videoEncoder << encoderSpecificArguments(videoEncoder)
         << " -b:v " << bitrateKbps << "k -maxrate " << bitrateKbps << "k -bufsize " << bufferKbps << "k"
         << " -g " << (fps * 2) << " -pix_fmt yuv420p"
         << " -c:a aac -b:a 160k -ar 48000"
         << " -f flv " << quoteArgument(configuredEndpoint_);
    return args.str();
  }

  bool startFfmpegProcess(int width, int height) {
    if (ffmpegExecutable_.empty()) {
      sender_.status = "warning";
      sender_.warning = "FFmpeg executable was not found.";
      sender_.destinationHealth = "warning";
      sender_.lastResultCode = "ffmpeg-missing";
      sender_.lastError = sender_.warning;
      return false;
    }
#if defined(_WIN32)
    SECURITY_ATTRIBUTES securityAttributes{};
    securityAttributes.nLength = sizeof(securityAttributes);
    securityAttributes.bInheritHandle = TRUE;

    HANDLE childStdinRead = nullptr;
    HANDLE childStdinWrite = nullptr;
    if (!CreatePipe(&childStdinRead, &childStdinWrite, &securityAttributes, 0)) {
      sender_.status = "failed";
      sender_.warning = "Could not create FFmpeg stdin pipe.";
      sender_.destinationHealth = "failed";
      sender_.lastResultCode = "ffmpeg-pipe-failed";
      sender_.lastError = sender_.warning;
      return false;
    }
    SetHandleInformation(childStdinWrite, HANDLE_FLAG_INHERIT, 0);

    HANDLE nullHandle = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &securityAttributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nullHandle == INVALID_HANDLE_VALUE) {
      CloseHandle(childStdinRead);
      CloseHandle(childStdinWrite);
      sender_.status = "failed";
      sender_.warning = "Could not open NUL for FFmpeg output redirection.";
      sender_.destinationHealth = "failed";
      sender_.lastResultCode = "ffmpeg-redirect-failed";
      sender_.lastError = sender_.warning;
      return false;
    }

    STARTUPINFOA startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.hStdInput = childStdinRead;
    startupInfo.hStdOutput = nullHandle;
    startupInfo.hStdError = nullHandle;

    PROCESS_INFORMATION processInfo{};
    std::string commandLine = quoteArgument(ffmpegExecutable_) + buildFfmpegArguments(width, height);
    std::vector<char> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back('\0');

    const BOOL started = CreateProcessA(
        ffmpegExecutable_.c_str(),
        mutableCommandLine.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &startupInfo,
        &processInfo);
    CloseHandle(childStdinRead);
    CloseHandle(nullHandle);
    if (!started) {
      CloseHandle(childStdinWrite);
      sender_.status = "failed";
      sender_.warning = "Failed to start FFmpeg process. Win32 error " + std::to_string(GetLastError()) + ".";
      sender_.destinationHealth = "failed";
      sender_.lastResultCode = "ffmpeg-start-failed";
      sender_.lastError = sender_.warning;
      return false;
    }

    ffmpegStdin_ = childStdinWrite;
    ffmpegProcess_ = processInfo.hProcess;
    CloseHandle(processInfo.hThread);
    ffmpegRunning_ = true;
    sender_.runtimeDetail = "ffmpeg:" + ffmpegExecutable_;
    writeLine("{\"type\":\"ffmpeg-process-start\",\"destination\":\"rtmp\",\"width\":" + std::to_string(width) +
              ",\"height\":" + std::to_string(height) +
              ",\"endpoint\":" + jsonString(redactedEndpoint(configuredEndpoint_, configuredStreamKey_)) +
              ",\"ffmpegExecutable\":" + jsonString(ffmpegExecutable_) +
              ",\"videoCodec\":" + jsonString(configuredVideoCodec_) +
              ",\"encoderMode\":" + jsonString(configuredEncoderMode_) +
              ",\"ffmpegVideoEncoder\":" + jsonString(ffmpegVideoEncoderFor(configuredVideoCodec_, configuredEncoderMode_)) + "}");
    return true;
#else
    (void)width;
    (void)height;
    sender_.status = "warning";
    sender_.warning = "FFmpeg RTMP process output is currently implemented for Windows dev/runtime builds.";
    sender_.destinationHealth = "warning";
    sender_.lastResultCode = "ffmpeg-platform-disabled";
    sender_.lastError = sender_.warning;
    return false;
#endif
  }

  bool writeFrameToFfmpeg(const ProgramFrame& frame) {
#if defined(_WIN32)
    if (!ffmpegRunning_ || !ffmpegStdin_) {
      return false;
    }
    DWORD exitCode = 0;
    if (ffmpegProcess_ && GetExitCodeProcess(ffmpegProcess_, &exitCode) && exitCode != STILL_ACTIVE) {
      return false;
    }
    DWORD written = 0;
    const auto* data = frame.preview.bgra.data();
    size_t remaining = frame.preview.bgra.size();
    while (remaining > 0) {
      const DWORD chunk = static_cast<DWORD>((std::min)(remaining, static_cast<size_t>(1) << 20));
      if (!WriteFile(ffmpegStdin_, data, chunk, &written, nullptr) || written == 0) {
        return false;
      }
      data += written;
      remaining -= written;
    }
    return true;
#else
    (void)frame;
    return false;
#endif
  }

  void stopFfmpegProcess() {
#if defined(_WIN32)
    if (ffmpegStdin_) {
      CloseHandle(ffmpegStdin_);
      ffmpegStdin_ = nullptr;
    }
    if (ffmpegProcess_) {
      DWORD exitCode = 0;
      if (GetExitCodeProcess(ffmpegProcess_, &exitCode) && exitCode == STILL_ACTIVE) {
        WaitForSingleObject(ffmpegProcess_, 500);
      }
      CloseHandle(ffmpegProcess_);
      ffmpegProcess_ = nullptr;
    }
#endif
    ffmpegRunning_ = false;
  }

  void openSendProofIfNeeded() {
    if (sendProof_.is_open() || !sender_.sendArtifactPath.empty()) {
      return;
    }
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const auto path = std::filesystem::temp_directory_path() / ("corevideo-rtmp-send-proof-" + std::to_string(now) + ".jsonl");
    sendProof_.open(path, std::ios::out | std::ios::trunc);
    if (!sendProof_) {
      sender_.warning = "RTMP send proof file could not be opened.";
      return;
    }
    sender_.sendArtifactPath = path.string();
    writeLine("{\"type\":\"rtmp-send-proof-start\",\"destination\":\"rtmp\",\"endpointConfigured\":" +
              std::string(configuredEndpoint_.empty() ? "false" : "true") +
              ",\"endpoint\":" + jsonString(redactedEndpoint(configuredEndpoint_, configuredStreamKey_)) +
              ",\"endpointMode\":\"ffmpeg-process\","
              "\"testMode\":false,\"muxingMode\":\"ffmpeg-process\",\"runtimeLibrary\":\"ffmpeg\",\"runtimeAvailable\":" +
              std::string(runtimeAvailable_ ? "true" : "false") +
              ",\"runtimeDetail\":" + jsonString(runtimeDetail_) +
              ",\"runtimeCandidates\":" + runtimeCandidatesJson(runtimeProbe_.candidates) +
              ",\"videoCodec\":" + jsonString(configuredVideoCodec_) +
              ",\"encoderMode\":" + jsonString(configuredEncoderMode_) +
              ",\"ffmpegVideoEncoder\":" + jsonString(ffmpegVideoEncoderFor(configuredVideoCodec_, configuredEncoderMode_)) +
              ",\"packagingSignal\":\"sync-ffmpeg-runtime-to-app.ps1 stages ffmpeg.exe and corevideo-ffmpeg-runtime.json when FFmpeg is available or unavailable\"}");
  }

  void appendSendProof(const ProgramFrame* frame, const std::string& status) {
    if (!sendProof_.is_open()) {
      return;
    }
    std::string line = "{\"type\":\"rtmp-send-attempt\",\"destination\":\"rtmp\",\"endpointMode\":\"ffmpeg-process\",\"status\":" + jsonString(status);
    if (frame) {
      line += ",\"frameNumber\":" + std::to_string(frame->frameNumber) +
              ",\"width\":" + std::to_string(frame->width) +
              ",\"height\":" + std::to_string(frame->height) +
              ",\"renderPlanId\":" + jsonString(frame->renderPlanId) +
              ",\"videoCodec\":" + jsonString(configuredVideoCodec_) +
              ",\"encoderMode\":" + jsonString(configuredEncoderMode_) +
              ",\"ffmpegVideoEncoder\":" + jsonString(ffmpegVideoEncoderFor(configuredVideoCodec_, configuredEncoderMode_));
    }
    line += "}";
    writeLine(line);
  }

  void writeLine(const std::string& line) {
    sendProof_ << line << '\n';
    sendProof_.flush();
    sender_.sendBytesWritten += static_cast<int64_t>(line.size() + 1);
  }

  OutputSenderSession snapshot() const {
    OutputSenderSession session;
    if (!sender_.senderId.empty()) {
      session.senders.push_back(sender_);
      if (sender_.status == "live" || sender_.status == "warning" || sender_.status == "starting") {
        session.activeSenderCount = 1;
      }
      if (!sender_.warning.empty()) {
        session.warnings.push_back(sender_.warning);
      }
      session.status = sender_.status == "failed" ? "failed" : !sender_.warning.empty() || sender_.status == "warning" ? "warning" : session.activeSenderCount > 0 ? "live" : "idle";
    }
    return session;
  }

  RuntimeProbe runtimeProbe_;
  std::string runtimeDetail_;
  bool runtimeAvailable_ = false;
  std::string configuredEndpoint_;
  std::string configuredStreamKey_;
  std::string configuredFfmpegBinDirectory_;
  std::string configuredVideoCodec_ = "h264";
  std::string configuredEncoderMode_ = "auto";
  std::string ffmpegExecutable_;
  std::string activeEndpoint_;
  std::string activeFfmpegExecutable_;
  std::string activeVideoCodec_;
  std::string activeEncoderMode_;
  int ffmpegFrameWidth_ = 0;
  int ffmpegFrameHeight_ = 0;
  int configuredFps_ = 30;
  int activeFps_ = 0;
  double activeBitrateMbps_ = 0;
  bool ffmpegRunning_ = false;
#if defined(_WIN32)
  HANDLE ffmpegProcess_ = nullptr;
  HANDLE ffmpegStdin_ = nullptr;
#endif
  OutputSender sender_;
  std::ofstream sendProof_;
};
#endif

}  // namespace

std::unique_ptr<IOutputSender> createRtmpOutputSender() {
#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_RTMP_OUTPUT
  // REQUIRES DEV MACHINE: real RTMP packet muxing belongs behind this libavformat
  // sender. The scaffold verifies runtime availability without affecting stubs.
  return std::make_unique<RtmpOutputSender>(probeFfmpegRuntime(""));
#else
  return nullptr;
#endif
}

}  // namespace corevideo::modules
