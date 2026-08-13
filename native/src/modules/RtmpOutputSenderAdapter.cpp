#include "modules/Interfaces.h"
#include "modules/RtmpCompatibility.h"
#include "modules/RtmpFfmpegArgs.h"
#include "modules/EncoderPolicy.h"
#include "modules/SrtFfmpegArgs.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_RTMP_OUTPUT
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
extern char** environ;
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
  if (value == "nvenc" || value == "qsv" || value == "amf" || value == "cpu" ||
      value == "videotoolbox") {
    return value;
  }
  return "auto";
}

std::string normalizeRateControl(std::string value) {
  value = lowercaseAscii(std::move(value));
  return value == "vbr" ? "vbr" : "cbr";
}

std::string normalizeH264Profile(std::string value) {
  value = lowercaseAscii(std::move(value));
  if (value == "auto" || value == "baseline" || value == "main" || value == "high") {
    return value;
  }
  return "high";
}

std::string buildRtmpEndpoint(const OutputDestinationSettings& settings) {
  std::string endpoint = trimTrailingSlash(settings.url);
  if (!settings.streamKey.empty()) {
    endpoint += "/" + settings.streamKey;
  }
  return endpoint;
}

bool startsWithScheme(const std::string& value, const std::string& scheme) {
  return value.rfind(scheme, 0) == 0;
}

bool hasWhitespace(const std::string& value) {
  return std::any_of(value.begin(), value.end(), [](unsigned char ch) {
    return std::isspace(ch) != 0;
  });
}

bool hasRtmpHostAndPath(const std::string& url) {
  const std::string separator = "://";
  const auto schemeEnd = url.find(separator);
  if (schemeEnd == std::string::npos) {
    return false;
  }

  const auto hostStart = schemeEnd + separator.size();
  const auto pathStart = url.find('/', hostStart);
  const auto hostEnd = pathStart == std::string::npos ? url.size() : pathStart;
  if (hostStart >= hostEnd) {
    return false;
  }

  if (pathStart == std::string::npos || pathStart + 1 >= url.size()) {
    return false;
  }

  return true;
}

std::string validateRtmpSettings(const OutputDestinationSettings& settings) {
  const std::string protocol = lowercaseAscii(settings.protocol);
  if (protocol != "rtmp" && protocol != "rtmps") {
    return "RTMP sender requires an rtmp or rtmps protocol.";
  }

  if (settings.url.empty()) {
    return "RTMP sender needs a configured RTMP/RTMPS server URL.";
  }

  if (hasWhitespace(settings.url)) {
    return "RTMP sender server URL cannot contain whitespace.";
  }

  const bool schemeMatches = protocol == "rtmp"
                                 ? startsWithScheme(settings.url, "rtmp://")
                                 : startsWithScheme(settings.url, "rtmps://");
  if (!schemeMatches) {
    return "RTMP sender protocol must match the server URL scheme.";
  }

  if (!hasRtmpHostAndPath(settings.url)) {
    return "RTMP sender URL must include a host and application path.";
  }

  if (settings.streamKey.empty()) {
    return "RTMP sender needs a stream key before streaming.";
  }

  if (hasWhitespace(settings.streamKey)) {
    return "RTMP sender stream key cannot contain whitespace.";
  }

  return "";
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

// This sender serves BOTH FFmpeg-transported protocols. RTMP and SRT differ only
// in their endpoint syntax, container and validation - the process pipeline,
// wallclock pacing, NV12 feeding, reconnect/backoff and health reporting are
// identical, so they share one implementation rather than two 1700-line copies.
struct FfmpegSenderProtocol {
  std::string destination = "rtmp";  // the destination name the operator toggles
  std::string container = "flv";     // FLV for RTMP, MPEG-TS for SRT
  bool isSrt = false;
};

inline FfmpegSenderProtocol rtmpProtocol() { return {"rtmp", "flv", false}; }
inline FfmpegSenderProtocol srtProtocol() { return {"srt", "mpegts", true}; }

// SRT settings carry host/port/mode/latency/passphrase rather than a URL and a
// stream key, so they get their own matcher and validator.
const OutputDestinationSettings* findSrtSettings(const std::vector<OutputDestinationSettings>& destinationSettings) {
  for (const auto& settings : destinationSettings) {
    if (settings.id == "srt" || lowercaseAscii(settings.protocol) == "srt") {
      return &settings;
    }
  }
  return nullptr;
}

SrtEndpointConfig srtEndpointConfigFrom(const OutputDestinationSettings& settings) {
  SrtEndpointConfig config;
  // Tolerate the host arriving in either field - operators paste a full
  // "srt://host:port" into the URL box as often as they fill host/port.
  config.host = !settings.host.empty() ? settings.host : settings.url;
  config.port = settings.port;
  config.mode = settings.mode.empty() ? std::string("caller") : lowercaseAscii(settings.mode);
  config.latencyMs = settings.latencyMs;
  config.latencyUs = settings.latencyUs;
  config.passphrase = settings.passphrase;
  config.keyLength = settings.keyLength;
  config.streamId = settings.streamId;
  return config;
}

// Returns "" when the settings can produce a usable srt:// endpoint.
std::string validateSrtSettings(const OutputDestinationSettings& settings) {
  const auto result = buildSrtUrl(srtEndpointConfigFrom(settings));
  return result.valid ? std::string() : result.error;
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

std::filesystem::path currentExecutableDirectory() {
#if defined(_WIN32)
  std::vector<char> buffer(MAX_PATH);
  for (;;) {
    const DWORD length = GetModuleFileNameA(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) {
      return {};
    }
    if (length < buffer.size() - 1) {
      return std::filesystem::path(std::string(buffer.data(), length)).parent_path();
    }
    buffer.resize(buffer.size() * 2);
  }
#else
  return {};
#endif
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
  const auto executableDirectory = currentExecutableDirectory();
  if (!executableDirectory.empty()) {
    candidates.emplace_back(executableDirectory / executableName);
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
  // RTMP output is implemented through the external FFmpeg process below; it
  // does not link to libavformat in-process. Loading and immediately unloading
  // avformat here is both unnecessary and unsafe: current FFmpeg DLLs run
  // teardown code during FreeLibrary, and repeated per-frame probes eventually
  // abort the host process. Probe only the executable we actually launch.
  RuntimeProbe probe;
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

#if !defined(_WIN32)
// Split an FFmpeg command-line string into argv tokens, honoring the double
// quotes that buildFfmpegArguments emits around the endpoint, and stripping
// them. Used by the POSIX posix_spawn path which needs a real argv vector.
std::vector<std::string> tokenizeArguments(const std::string& args) {
  std::vector<std::string> tokens;
  std::string current;
  bool inQuotes = false;
  bool hasToken = false;
  for (size_t i = 0; i < args.size(); ++i) {
    const char ch = args[i];
    if (ch == '"') {
      inQuotes = !inQuotes;
      hasToken = true;
      continue;
    }
    if (ch == '\\' && inQuotes && i + 1 < args.size() && args[i + 1] == '"') {
      current += '"';
      ++i;
      continue;
    }
    if (!inQuotes && (ch == ' ' || ch == '\t')) {
      if (hasToken) {
        tokens.push_back(current);
        current.clear();
        hasToken = false;
      }
      continue;
    }
    current += ch;
    hasToken = true;
  }
  if (hasToken) {
    tokens.push_back(current);
  }
  return tokens;
}
#endif

// Encoder policy (which hardware this product ships, and why) lives in
// EncoderPolicy.h so it is unit-testable and hard to change by accident.
std::string ffmpegVideoEncoderFor(const std::string& codec, const std::string& encoderMode) {
  return preferredEncoderFor(normalizeVideoCodec(codec), normalizeEncoderMode(encoderMode));
}

std::string encoderSpecificArguments(const std::string& encoderName, const std::string& rateControl) {
  const auto rc = normalizeRateControl(rateControl);
  if (encoderName.find("_nvenc") != std::string::npos) {
    return " -preset p4 -tune ll -rc " + rc;
  }
  if (encoderName.find("_qsv") != std::string::npos) {
    return " -preset veryfast";
  }
  if (encoderName.find("_amf") != std::string::npos) {
    return " -quality speed -rc " + rc;
  }
  if (encoderName.find("_videotoolbox") != std::string::npos) {
    // VideoToolbox rejects libx264 preset/tune flags; realtime keeps the
    // hardware encoder in its low-latency path and prioritize_speed matches.
    return " -realtime 1 -prio_speed 1";
  }
  if (encoderName == "libsvtav1") {
    return " -preset 8";
  }
  if (encoderName == "libx265") {
    return " -preset veryfast -tune zerolatency";
  }
  if (encoderName == "libx264") {
    return " -preset veryfast -tune zerolatency";
  }
  // Media Foundation and OpenH264 do not accept libx264's preset/tune flags.
  return "";
}

bool ffmpegEncoderIsAvailable(const std::string& executable, const std::string& encoderName) {
  if (executable.empty() || encoderName.empty()) {
    return false;
  }
#if defined(_WIN32)
  STARTUPINFOA startupInfo{};
  startupInfo.cb = sizeof(startupInfo);
  PROCESS_INFORMATION processInfo{};
  // `ffmpeg -h encoder=<name>` exits with code 0 even when <name> is unknown,
  // so help output is not an availability probe. Encode one tiny synthetic
  // frame instead; this verifies that the encoder is both registered and able
  // to initialize on the current machine before a live stream selects it.
  std::string commandLine = quoteArgument(executable) +
                            " -hide_banner -loglevel quiet -nostdin"
                            " -f lavfi -i color=c=black:s=1280x720:r=30"
                            " -frames:v 30 -an -c:v " + encoderName +
                            " -f null NUL";
  std::vector<char> mutableCommandLine(commandLine.begin(), commandLine.end());
  mutableCommandLine.push_back('\0');
  if (!CreateProcessA(
          executable.c_str(), mutableCommandLine.data(), nullptr, nullptr, FALSE,
          CREATE_NO_WINDOW, nullptr, nullptr, &startupInfo, &processInfo)) {
    return false;
  }
  CloseHandle(processInfo.hThread);
  const DWORD waitResult = WaitForSingleObject(processInfo.hProcess, 5000);
  DWORD exitCode = 1;
  if (waitResult == WAIT_TIMEOUT) {
    TerminateProcess(processInfo.hProcess, 1);
    WaitForSingleObject(processInfo.hProcess, 1000);
  } else {
    GetExitCodeProcess(processInfo.hProcess, &exitCode);
  }
  CloseHandle(processInfo.hProcess);
  return waitResult == WAIT_OBJECT_0 && exitCode == 0;
#else
  // Release packaging on non-Windows supplies the documented software encoders.
  (void)executable;
  (void)encoderName;
  return true;
#endif
}

std::string selectFfmpegVideoEncoder(
    const std::string& executable,
    const std::string& codec,
    const std::string& encoderMode) {
  const auto normalizedCodec = normalizeVideoCodec(codec);
  const auto normalizedMode = normalizeEncoderMode(encoderMode);
  const auto candidates = encoderCandidatesFor(normalizedCodec, normalizedMode);
  for (const auto& candidate : candidates) {
    if (ffmpegEncoderIsAvailable(executable, candidate)) {
      return candidate;
    }
  }
  return candidates.empty() ? preferredEncoderFor(normalizedCodec, normalizedMode) : candidates.front();
}

class RtmpOutputSender final : public IOutputSender {
 public:
  RtmpOutputSender(RuntimeProbe runtimeProbe, FfmpegSenderProtocol protocol = rtmpProtocol())
      : protocol_(std::move(protocol)),
        runtimeProbe_(std::move(runtimeProbe)),
        runtimeDetail_(runtimeProbe_.detail),
        runtimeAvailable_(runtimeProbe_.available) {}

  ~RtmpOutputSender() override { stopFfmpegProcess(); }

  // Program audio on the AUDIO cadence. Writes straight into the same queue the
  // per-tick path uses; the borrow ends inside writeAudioToFfmpeg (it copies into
  // audioQueue_), so holding a caller-owned reference here is safe.
  void submitAudio(const std::vector<float>& pcm, int channels, int sampleRate) override {
    if (pcm.empty() || channels <= 0 || sampleRate <= 0) {
      return;
    }
    pendingAudioPcm_ = &pcm;
    pendingAudioChannels_ = channels;
    pendingAudioSampleRate_ = sampleRate;
    haveRealAudio_ = true;
    writeAudioToFfmpeg();  // no-op until the process is up with a PCM input
    pendingAudioPcm_ = nullptr;
  }

  OutputSenderSession sync(
      const std::vector<std::string>& destinations,
      const ProgramFrame* frame,
      double elapsedMs,
      const std::vector<OutputDestinationSettings>& destinationSettings = {},
      const std::vector<float>* programAudioPcm = nullptr,
      int audioChannels = 0,
      int audioSampleRate = 0) override {
    // Capture the latest real program-audio mix for this tick so the FFmpeg
    // process is configured with (and fed) the second PCM input instead of the
    // `anullsrc` silence source. We treat "audio available" as having a positive
    // channel/sample-rate and non-empty PCM.
    // Audio presence is STICKY (haveRealAudio_), not per-call. Video now arrives
    // on its own 60Hz tick while audio arrives on the 50Hz worker via
    // submitAudio(), so most sync() calls legitimately carry no PCM — and
    // clearing the layout on those would look like "audio disappeared" and
    // restart FFmpeg (the arg list bakes in the audio input) on every tick.
    // A LAYOUT ALONE DECLARES AUDIO — PCM is not required. The FFmpeg argument
    // list bakes in the audio input, so discovering audio only when the first
    // PCM buffer arrives means starting with `anullsrc` and RESTARTING moments
    // later. That restart forces a second SRT connect, and an SRT listener
    // accepts ONE caller: the reconnect is refused ("Connection to srt://...
    // failed: I/O error") and the stream never recovers. Declaring the layout on
    // the first sync gets the process right the first time.
    if (audioChannels > 0 && audioSampleRate > 0) {
      pendingAudioChannels_ = audioChannels;
      pendingAudioSampleRate_ = audioSampleRate;
      haveRealAudio_ = true;
    }
    pendingAudioPcm_ = (programAudioPcm && !programAudioPcm->empty()) ? programAudioPcm : nullptr;
    const bool wantsRtmp =
        std::find(destinations.begin(), destinations.end(), protocol_.destination) != destinations.end();
    if (!wantsRtmp) {
      stopFfmpegProcess();
      videoFramePacer_.reset();
      clearFfmpegRetryBackoff();
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
    const auto* settings = protocol_.isSrt ? findSrtSettings(destinationSettings)
                                           : findRtmpSettings(destinationSettings);
    if (!settings) {
      stopFfmpegProcess();
      configuredEndpoint_.clear();
      configuredStreamKey_.clear();
      sender_.status = "warning";
      sender_.warning = "RTMP sender needs current RTMP destination settings before streaming.";
      sender_.destinationHealth = "warning";
      sender_.lastResultCode = "rtmp-settings-missing";
      sender_.lastError = sender_.warning;
      appendSendProof(frame, "rtmp-settings-missing");
      return snapshot();
    }

    const auto settingsError = protocol_.isSrt ? validateSrtSettings(*settings)
                                               : validateRtmpSettings(*settings);
    if (!settingsError.empty()) {
      stopFfmpegProcess();
      configuredEndpoint_.clear();
      configuredStreamKey_.clear();
      sender_.status = "warning";
      sender_.warning = settingsError;
      sender_.destinationHealth = "warning";
      sender_.lastResultCode = "rtmp-settings-invalid";
      sender_.lastError = sender_.warning;
      appendSendProof(frame, "rtmp-settings-invalid");
      return snapshot();
    }

    const bool ffmpegBinDirectoryChanged = configuredFfmpegBinDirectory_ != settings->ffmpegBinDirectory;
    configuredEndpoint_ = protocol_.isSrt ? buildSrtUrl(srtEndpointConfigFrom(*settings)).url
                                          : buildRtmpEndpoint(*settings);
    configuredStreamKey_ = settings->streamKey;
    configuredFfmpegBinDirectory_ = settings->ffmpegBinDirectory;
    configuredFps_ = (std::max)(1, settings->fps);
    configuredVideoCodec_ = normalizeVideoCodec(settings->videoCodec);
    configuredEncoderMode_ = normalizeEncoderMode(settings->encoderMode);
    configuredKeyframeIntervalSeconds_ = (std::max)(0.5, (std::min)(10.0, settings->keyframeIntervalSeconds));
    configuredRateControl_ = normalizeRateControl(settings->rateControl);
    configuredH264Profile_ = normalizeH264Profile(settings->h264Profile);
    configuredBFrames_ = (std::max)(0, (std::min)(4, settings->bFrames));
    configuredAllowEnhancedRtmp_ = settings->allowEnhancedRtmp;
    configuredAudioBitrateKbps_ = (std::max)(32, (std::min)(512, settings->audioBitrateKbps));
    sender_.bitrateMbps = (std::max)(0.5, settings->targetBitrateMbps);
    // Filesystem/runtime discovery is configuration state, not frame work.
    // Re-probing on every 20 ms sync previously loaded/unloaded FFmpeg DLLs
    // hundreds of times and crashed corevideo-native during a live stream.
    if (ffmpegBinDirectoryChanged || runtimeProbe_.candidates.empty()) {
      runtimeProbe_ = probeFfmpegRuntime(configuredFfmpegBinDirectory_);
    }
    runtimeDetail_ = runtimeProbe_.detail;
    // Surface a codec/container compatibility note (e.g. H.265 -> H.264 fallback)
    // so the operator sees why the on-air codec may differ from the request.
    const auto codecCompatibility = resolveRtmpCompatibility(configuredVideoCodec_, configuredAllowEnhancedRtmp_);
    if (!codecCompatibility.warning.empty()) {
      runtimeDetail_ += (runtimeDetail_.empty() ? "" : " ") + codecCompatibility.warning;
    }
    // LOUD when the requested codec has no supported hardware encoder here.
    // "AV1 selected, silently got H.264" is the same silent-wrong-output class as
    // shipping a thumbnail as the program: the stream looks fine and is not what
    // was asked for. Say which hardware is required instead.
    unsupportedCodecWarning_.clear();
    if (!codecHasSupportedHardwareEncoder(configuredVideoCodec_)) {
      if (configuredVideoCodec_ == "h265") {
        unsupportedCodecWarning_ =
            "HEVC/H.265 encoding is not supported in this build; encoding H.264 instead.";
      } else if (configuredVideoCodec_ == "av1") {
        unsupportedCodecWarning_ =
#if defined(__APPLE__)
            "AV1 encoding needs an NVIDIA Ada (RTX 40-series) GPU; Apple Silicon has no AV1 "
            "encoder. Encoding H.264 instead.";
#else
            "AV1 encoding needs an NVIDIA Ada (RTX 40-series) GPU or newer. Encoding H.264 "
            "instead.";
#endif
      }
      if (!unsupportedCodecWarning_.empty()) {
        runtimeDetail_ += (runtimeDetail_.empty() ? "" : " ") + unsupportedCodecWarning_;
      }
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
    if (!hasProgramNv12(*frame) && !hasProgramFullBgra(*frame) &&
        (frame->preview.width <= 0 || frame->preview.height <= 0 || frame->preview.bgra.empty())) {
      sender_.status = "warning";
      sender_.warning = "RTMP sender is waiting for composed BGRA program pixels.";
      sender_.destinationHealth = "warning";
      sender_.lastResultCode = "frame-pixels-missing";
      sender_.lastError = sender_.warning;
      appendSendProof(frame, "frame-pixels-missing");
      return snapshot();
    }

    // Skip BEFORE ensureFfmpegProcess: that call pins FFmpeg's -s geometry from
    // this frame, so letting a preview-sized frame through here is what
    // restarted the encoder mid-stream.
    if (!videoSourceUsable(*frame)) {
      sender_.status = "live";
      sender_.lastResultCode = "awaiting-full-res-frame";
      appendSendProof(frame, "awaiting-full-res-frame");
      return snapshot();
    }

    if (!ensureFfmpegProcess(*frame, elapsedMs)) {
      appendSendProof(frame, "ffmpeg-start-failed");
      return snapshot();
    }

    // Audio follows the 20 ms output-worker cadence. Video does not: pace it to
    // the configured stream fps so a 50 Hz worker cannot overfill FFmpeg's raw
    // 4K input pipe (the 2026-07-14 live freeze reproduced at 42 frames/0.84 s).
    writeAudioToFfmpeg();
    if (!videoFramePacer_.shouldWrite(elapsedMs, configuredFps_)) {
      sender_.status = "live";
      // A live stream still carries the unsupported-codec notice: the operator
      // asked for AV1/HEVC and is getting H.264, which must not go quiet just
      // because the stream is otherwise healthy.
      sender_.warning = unsupportedCodecWarning_;
      sender_.runtimeDetail = runtimeDetail_;
      sender_.audioChannels = activeAudioPresent_ ? activeAudioChannels_ : 0;
      sender_.audioSampleRate = activeAudioPresent_ ? activeAudioSampleRate_ : 0;
      sender_.destinationHealth = "ok";
      sender_.lastResultCode = "ok";
      return snapshot();
    }

    if (!writeFrameToFfmpeg(*frame)) {
      sender_.status = "failed";
      ++sender_.retryCount;
      const auto detailedFailure =
          sender_.lastResultCode == "ffmpeg-exited" && !sender_.lastError.empty()
              ? sender_.lastError
              : std::string("FFmpeg stdin write failed; the RTMP process stopped or rejected frames.");
      sender_.warning = detailedFailure;
      sender_.destinationHealth = "failed";
      if (sender_.lastResultCode != "ffmpeg-exited") {
        sender_.lastResultCode = "ffmpeg-write-failed";
      }
      sender_.lastError = sender_.warning;
      const auto proofStatus = sender_.lastResultCode == "ffmpeg-exited" ? "ffmpeg-exited" : "ffmpeg-write-failed";
      scheduleFfmpegRetry();
      stopFfmpegProcess();
      appendSendProof(frame, proofStatus);
      return snapshot();
    }

    sender_.status = "live";
    // A live stream still carries the unsupported-codec notice: the operator
    // asked for AV1/HEVC and is getting H.264, which must not go quiet just
    // because the stream is otherwise healthy.
    sender_.warning = unsupportedCodecWarning_;
    sender_.runtimeDetail = runtimeDetail_;
    sender_.lastFrameNumber = frame->frameNumber;
    ++sender_.framesSent;
    sender_.bytesSent += estimatedFrameBytes(sender_.bitrateMbps);
    sender_.audioChannels = activeAudioPresent_ ? activeAudioChannels_ : 0;
    sender_.audioSampleRate = activeAudioPresent_ ? activeAudioSampleRate_ : 0;
    sender_.destinationHealth = "ok";
    sender_.lastResultCode = "ok";
    clearFfmpegRetryBackoff();
    appendSendProof(frame, "sent");
    return snapshot();
  }

  OutputSenderSession fail(const std::string& destination, const std::string& message, double elapsedMs) override {
    if (destination != protocol_.destination) {
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
    if (destination != protocol_.destination) {
      return snapshot();
    }
    stopFfmpegProcess();
    clearFfmpegRetryBackoff();
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

  void interrupt(const std::string& destination) override {
    if (destination != protocol_.destination) {
      return;
    }
#if defined(_WIN32)
    std::lock_guard<std::mutex> lock(ffmpegProcessMutex_);
    if (ffmpegProcess_) {
      DWORD exitCode = 0;
      if (GetExitCodeProcess(ffmpegProcess_, &exitCode) && exitCode == STILL_ACTIVE) {
        TerminateProcess(ffmpegProcess_, 1);
      }
    }
#else
    if (ffmpegPid_ > 0) {
      ::kill(ffmpegPid_, SIGTERM);
    }
#endif
  }

 private:
  static bool hasProgramNv12(const ProgramFrame& frame) {
    if (frame.programNv12Width <= 0 || frame.programNv12Height <= 0) {
      return false;
    }
    const auto required = static_cast<size_t>(frame.programNv12Width) *
                          static_cast<size_t>(frame.programNv12Height) * 3 / 2;
    return frame.programNv12.size() >= required;
  }

  // Full-res BGRA tap (the Metal GPU tap on macOS; fills whenever output is
  // active). Preferred over `preview` — streaming the 320x180 UI thumbnail
  // was the silent-quality failure this ordering exists to prevent.
  static bool hasProgramFullBgra(const ProgramFrame& frame) {
    if (frame.programFullBgra.width <= 0 || frame.programFullBgra.height <= 0) {
      return false;
    }
    const auto required = static_cast<size_t>(frame.programFullBgra.width) *
                          static_cast<size_t>(frame.programFullBgra.height) * 4;
    return frame.programFullBgra.bgra.size() >= required;
  }

  static int videoWidth(const ProgramFrame& frame) {
    if (hasProgramNv12(frame)) {
      return frame.programNv12Width;
    }
    return hasProgramFullBgra(frame) ? frame.programFullBgra.width : frame.preview.width;
  }

  static int videoHeight(const ProgramFrame& frame) {
    if (hasProgramNv12(frame)) {
      return frame.programNv12Height;
    }
    return hasProgramFullBgra(frame) ? frame.programFullBgra.height : frame.preview.height;
  }

  static std::string videoPixelFormat(const ProgramFrame& frame) {
    return hasProgramNv12(frame) ? "nv12" : "bgra";
  }

  static const std::vector<uint8_t>& videoFrameBytes(const ProgramFrame& frame) {
    if (hasProgramNv12(frame)) {
      return frame.programNv12;
    }
    return hasProgramFullBgra(frame) ? frame.programFullBgra.bgra : frame.preview.bgra;
  }

  // Has this sender ever seen a full-resolution program buffer? The full-res tap
  // is ASYNCHRONOUS: it publishes a finished buffer on the ticks where its
  // readback completed, and ProgramFrame::programFullBgra is EMPTY on the rest.
  // The static pickers above then silently fall back to `preview` — a 320x180 UI
  // thumbnail with completely different geometry. FFmpeg is spawned with
  // `-s <declared>` and fed raw frames on pipe:0, so a mid-stream geometry flip
  // makes it either restart continuously or sit stitching thumbnails into a
  // frame that never completes: a video track is advertised and NOTHING
  // decodable ever arrives, while the separate audio pipe keeps flowing. That is
  // exactly the "streams audio, no picture" failure. Once full-res is available,
  // NEVER downgrade — skip the tick instead. FFmpeg paces with -re and simply
  // receives slightly fewer frames, which is always better than a broken stream.
  bool fullResLocked_ = false;

  // Returns false when this tick has no frame at the locked geometry and must be
  // skipped rather than written at the wrong size.
  bool videoSourceUsable(const ProgramFrame& frame) {
    const bool full = hasProgramNv12(frame) || hasProgramFullBgra(frame);
    {
      static long long s_full = 0, s_total = 0;
      static auto s_stamp = std::chrono::steady_clock::now();
      ++s_total;
      if (full) ++s_full;
      const auto now = std::chrono::steady_clock::now();
      // Only report when the tap actually missed — a healthy stream should be
      // silent here, and a sudden run of misses is the signal worth seeing.
      if (std::chrono::duration<double>(now - s_stamp).count() >= 3.0) {
        if (s_full != s_total) {
          std::fprintf(stderr, "[rtmp] full-res tap missed %lld of %lld ticks\n",
                       s_total - s_full, s_total);
        }
        s_full = 0; s_total = 0; s_stamp = now;
      }
    }
    if (full) {
      fullResLocked_ = true;
      return true;
    }
    if (fullResLocked_) {
      return false;  // never downgrade geometry mid-stream
    }
    // STARTUP: the full-res tap is asynchronous, so the first tick or two after
    // arming can arrive before it has published (measured: 1 miss in the first
    // ~123 ticks, then 100%). Starting FFmpeg on that first preview-sized frame
    // pins -s to 320x180 and the very next full-res frame forces a restart —
    // dropping the connection a real ingest has already accepted. Wait briefly
    // for the tap instead; only fall back to preview if it never appears (a
    // build with no tap at all), so the legacy path still works.
    constexpr int kWaitTicksForFullRes = 60;  // ~1.2s at the 50Hz output worker
    return ++previewOnlyTicks_ > kWaitTicksForFullRes;
  }
  int previewOnlyTicks_ = 0;

  void ensureSender(double elapsedMs) {
    if (!sender_.senderId.empty()) {
      return;
    }
    // Identity must follow the PROTOCOL, not the class name. The composite adds a
    // synthetic "<dest> output sender is not available in this build" warning for
    // any network destination with no registered sender, so an SRT instance
    // reporting itself as "rtmp" streams perfectly while the operator is told SRT
    // is unavailable (observed in the first end-to-end SRT proof: 6.4MB of h264
    // delivered, snapshot said no SRT sender module).
    sender_.senderId = protocol_.destination + ":program";
    sender_.destination = protocol_.destination;
    sender_.status = "starting";
    sender_.startedAtMs = elapsedMs;
    sender_.latencyMs = 2100;
    sender_.bitrateMbps = 6.0;
    sender_.runtimeDetail = runtimeDetail_;
    sender_.destinationHealth = "starting";
    sender_.lastResultCode = "waiting-for-frame";
  }

  bool ensureFfmpegProcess(const ProgramFrame& frame, double elapsedMs) {
    const int width = videoWidth(frame);
    const int height = videoHeight(frame);
    const auto pixelFormat = videoPixelFormat(frame);
    const bool sizeChanged = width != ffmpegFrameWidth_ || height != ffmpegFrameHeight_;
    const bool pixelFormatChanged = pixelFormat != ffmpegPixelFormat_;
    const bool endpointChanged = configuredEndpoint_ != activeEndpoint_;
    const bool executableChanged = ffmpegExecutable_ != activeFfmpegExecutable_;
    const bool fpsChanged = configuredFps_ != activeFps_;
    const bool bitrateChanged = sender_.bitrateMbps != activeBitrateMbps_;
    const bool audioBitrateChanged = configuredAudioBitrateKbps_ != activeAudioBitrateKbps_;
    const bool codecChanged = configuredVideoCodec_ != activeVideoCodec_;
    const bool encoderModeChanged = configuredEncoderMode_ != activeEncoderMode_;
    const bool keyframeChanged = configuredKeyframeIntervalSeconds_ != activeKeyframeIntervalSeconds_;
    const bool rateControlChanged = configuredRateControl_ != activeRateControl_;
    const bool h264ProfileChanged = configuredH264Profile_ != activeH264Profile_;
    const bool bFramesChanged = configuredBFrames_ != activeBFrames_;
    const bool enhancedChanged = configuredAllowEnhancedRtmp_ != activeAllowEnhancedRtmp_;
    // The audio input layout is baked into the FFmpeg argument list, so a change
    // in audio presence / channel count / sample rate requires a fresh process.
    // Sticky: whether a real PCM input exists at all, NOT whether this
    // particular call carried a buffer (see the note in sync()).
    const bool audioPresent = haveRealAudio_;
    const bool audioChanged = audioPresent != activeAudioPresent_ ||
                              pendingAudioChannels_ != activeAudioChannels_ ||
                              pendingAudioSampleRate_ != activeAudioSampleRate_;
    if (ffmpegRunning_ && !sizeChanged && !pixelFormatChanged && !endpointChanged && !executableChanged && !fpsChanged && !bitrateChanged && !audioBitrateChanged && !codecChanged && !encoderModeChanged && !keyframeChanged && !rateControlChanged && !h264ProfileChanged && !bFramesChanged && !enhancedChanged && !audioChanged) {
      return true;
    }

    if (!ffmpegRunning_ && ffmpegRetryAfter_ != std::chrono::steady_clock::time_point{} &&
        std::chrono::steady_clock::now() < ffmpegRetryAfter_) {
      const auto retryMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               ffmpegRetryAfter_ - std::chrono::steady_clock::now())
                               .count();
      sender_.status = "failed";
      sender_.destinationHealth = "failed";
      sender_.lastResultCode = "ffmpeg-retry-backoff";
      sender_.warning = sender_.lastError + " Retry paused for " +
                        std::to_string((std::max)(int64_t{1}, retryMs / 1000 + 1)) + "s.";
      return false;
    }

    stopFfmpegProcess();
    ffmpegFrameWidth_ = width;
    ffmpegFrameHeight_ = height;
    ffmpegPixelFormat_ = pixelFormat;
    activeEndpoint_ = configuredEndpoint_;
    activeFfmpegExecutable_ = ffmpegExecutable_;
    activeFps_ = configuredFps_;
    activeBitrateMbps_ = sender_.bitrateMbps;
    activeAudioBitrateKbps_ = configuredAudioBitrateKbps_;
    activeVideoCodec_ = configuredVideoCodec_;
    activeEncoderMode_ = configuredEncoderMode_;
    activeKeyframeIntervalSeconds_ = configuredKeyframeIntervalSeconds_;
    activeRateControl_ = configuredRateControl_;
    activeH264Profile_ = configuredH264Profile_;
    activeBFrames_ = configuredBFrames_;
    activeAllowEnhancedRtmp_ = configuredAllowEnhancedRtmp_;
    activeAudioPresent_ = audioPresent;
    activeAudioChannels_ = pendingAudioChannels_;
    activeAudioSampleRate_ = pendingAudioSampleRate_;
    if (startFfmpegProcess(width, height, pixelFormat)) {
      sender_.startedAtMs = elapsedMs;
      sender_.destinationHealth = "starting";
      sender_.lastResultCode = "ffmpeg-started";
      return true;
    }
    scheduleFfmpegRetry();
    return false;
  }

  void scheduleFfmpegRetry() {
    consecutiveFfmpegFailures_ = (std::min)(consecutiveFfmpegFailures_ + 1, 6);
    const int delaySeconds = (std::min)(30, 1 << (consecutiveFfmpegFailures_ - 1));
    ffmpegRetryAfter_ = std::chrono::steady_clock::now() + std::chrono::seconds(delaySeconds);
  }

  void clearFfmpegRetryBackoff() {
    consecutiveFfmpegFailures_ = 0;
    ffmpegRetryAfter_ = {};
  }

  // Never emit a raw endpoint: RTMP carries the stream key in the path and SRT
  // carries the passphrase in the query string.
  std::string redactedSenderEndpoint() const {
    return protocol_.isSrt ? redactedSrtUrl(configuredEndpoint_)
                           : redactedEndpoint(configuredEndpoint_, configuredStreamKey_);
  }

  std::string buildFfmpegArguments(int width, int height, const std::string& audioInput, const std::string& videoInputPixelFormat) const {
    // Resolve the requested codec to an RTMP-compatible one (H.265/AV1 fall back
    // to H.264 unless enhanced-RTMP is enabled) so the encoded stream always
    // matches what the FLV transport can carry.
    const auto compatibility = resolveRtmpCompatibility(configuredVideoCodec_, configuredAllowEnhancedRtmp_);
    RtmpFfmpegArgsConfig config;
    config.width = width;
    config.height = height;
    config.fps = (std::max)(1, configuredFps_);
    config.bitrateKbps = static_cast<int>((std::max)(1000.0, sender_.bitrateMbps * 1000.0));
    config.videoInputPixelFormat = videoInputPixelFormat;
    config.videoEncoder = selectedFfmpegVideoEncoder_.empty()
                              ? ffmpegVideoEncoderFor(compatibility.videoCodec, configuredEncoderMode_)
                              : selectedFfmpegVideoEncoder_;
    config.videoEncoderExtraArgs = encoderSpecificArguments(config.videoEncoder, configuredRateControl_);
    config.keyframeIntervalSeconds = configuredKeyframeIntervalSeconds_;
    config.rateControl = configuredRateControl_;
    config.h264Profile = compatibility.videoCodec == "h264" ? configuredH264Profile_ : "auto";
    config.bFrames = configuredBFrames_;
    config.endpoint = configuredEndpoint_;
    // When the program-audio tap delivered real PCM this tick, feed it over the
    // second input as raw f32le PCM; otherwise fall back to silent anullsrc.
    config.hasAudio = realAudioEnabledForProcess();
    config.audioChannels = activeAudioPresent_ ? activeAudioChannels_ : 2;
    config.audioSampleRate = activeAudioPresent_ ? activeAudioSampleRate_ : 48000;
    config.audioBitrateKbps = configuredAudioBitrateKbps_;
    config.audioSampleFormat = "f32le";
    config.audioInput = audioInput;
    config.container = protocol_.container;
    return buildRtmpFfmpegArguments(config);
  }

  bool realAudioEnabledForProcess() const {
    const char* value = std::getenv("COREVIDEO_RTMP_DISABLE_REAL_AUDIO");
    return activeAudioPresent_ && !(value && std::string(value) == "1");
  }

  bool startFfmpegProcess(int width, int height, const std::string& videoInputPixelFormat) {
    if (ffmpegExecutable_.empty()) {
      sender_.status = "warning";
      sender_.warning = "FFmpeg executable was not found.";
      sender_.destinationHealth = "warning";
      sender_.lastResultCode = "ffmpeg-missing";
      sender_.lastError = sender_.warning;
      return false;
    }
    const auto compatibility = resolveRtmpCompatibility(configuredVideoCodec_, configuredAllowEnhancedRtmp_);
    selectedFfmpegVideoEncoder_ = selectFfmpegVideoEncoder(
        ffmpegExecutable_, compatibility.videoCodec, configuredEncoderMode_);
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

    // Second input: a named pipe carrying the real program-audio PCM. FFmpeg
    // opens the path as its audio input; we are the server end. The named pipe
    // keeps audio independent of the video stdin stream while letting FFmpeg
    // mux a real AAC track instead of `anullsrc` silence.
    std::string audioPipeName;
    std::string audioInputArg = "pipe:0";  // unused when no audio
    if (realAudioEnabledForProcess()) {
      const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
      audioPipeName = "\\\\.\\pipe\\corevideo-rtmp-audio-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(now);
      audioPipeServer_ = CreateNamedPipeA(
          audioPipeName.c_str(),
          PIPE_ACCESS_OUTBOUND,
          // Audio now has its own writer thread, so a blocking byte pipe is
          // desirable: Windows backpressure paces PCM instead of returning a
          // zero-byte non-blocking write that silently kills the audio feed.
          PIPE_TYPE_BYTE | PIPE_WAIT,
          1,
          1 << 20,
          1 << 20,
          0,
          nullptr);
      if (audioPipeServer_ == INVALID_HANDLE_VALUE) {
        audioPipeServer_ = nullptr;
        CloseHandle(childStdinRead);
        CloseHandle(childStdinWrite);
        sender_.status = "failed";
        sender_.warning = "Could not create FFmpeg audio named pipe.";
        sender_.destinationHealth = "failed";
        sender_.lastResultCode = "ffmpeg-audio-pipe-failed";
        sender_.lastError = sender_.warning;
        return false;
      }
      audioInputArg = audioPipeName;
    }

    HANDLE nullHandle = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &securityAttributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nullHandle == INVALID_HANDLE_VALUE) {
      CloseHandle(childStdinRead);
      CloseHandle(childStdinWrite);
      closeAudioPipe();
      sender_.status = "failed";
      sender_.warning = "Could not open NUL for FFmpeg output redirection.";
      sender_.destinationHealth = "failed";
      sender_.lastResultCode = "ffmpeg-redirect-failed";
      sender_.lastError = sender_.warning;
      return false;
    }

    // Preserve FFmpeg warnings locally. Previously stderr went to NUL, so an
    // ingest rejection and a blocked pipe looked identical in diagnostics.
    const auto diagnosticNow = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count();
    ffmpegStderrPath_ = std::filesystem::temp_directory_path() /
                        ("corevideo-ffmpeg-rtmp-" + std::to_string(GetCurrentProcessId()) + "-" +
                         std::to_string(diagnosticNow) + ".log");
    HANDLE diagnosticHandle = CreateFileA(
        ffmpegStderrPath_.string().c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &securityAttributes, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (diagnosticHandle == INVALID_HANDLE_VALUE) {
      diagnosticHandle = nullptr;
      ffmpegStderrPath_.clear();
    }

    STARTUPINFOA startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.hStdInput = childStdinRead;
    startupInfo.hStdOutput = nullHandle;
    startupInfo.hStdError = diagnosticHandle ? diagnosticHandle : nullHandle;

    PROCESS_INFORMATION processInfo{};
    std::string commandLine = quoteArgument(ffmpegExecutable_) +
                              buildFfmpegArguments(width, height, audioInputArg, videoInputPixelFormat);
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
    if (diagnosticHandle) {
      CloseHandle(diagnosticHandle);
    }
    if (!started) {
      CloseHandle(childStdinWrite);
      closeAudioPipe();
      sender_.status = "failed";
      sender_.warning = "Failed to start FFmpeg process. Win32 error " + std::to_string(GetLastError()) + ".";
      sender_.destinationHealth = "failed";
      sender_.lastResultCode = "ffmpeg-start-failed";
      sender_.lastError = sender_.warning;
      return false;
    }

    ffmpegStdin_ = childStdinWrite;
    {
      std::lock_guard<std::mutex> lock(ffmpegProcessMutex_);
      ffmpegProcess_ = processInfo.hProcess;
    }
    audioPipeConnected_ = false;
    if (audioPipeServer_) {
      startAudioWriterThread();
    }
    CloseHandle(processInfo.hThread);
    ffmpegRunning_ = true;
    sender_.runtimeDetail = "ffmpeg:" + ffmpegExecutable_;
    writeLine("{\"type\":\"ffmpeg-process-start\",\"destination\":\"rtmp\",\"width\":" + std::to_string(width) +
              ",\"height\":" + std::to_string(height) +
              ",\"endpoint\":" + jsonString(redactedSenderEndpoint()) +
              ",\"ffmpegExecutable\":" + jsonString(ffmpegExecutable_) +
              ",\"videoCodec\":" + jsonString(configuredVideoCodec_) +
              ",\"encoderMode\":" + jsonString(configuredEncoderMode_) +
              ",\"audioInput\":" + jsonString(realAudioEnabledForProcess() ? std::string("pcm") : std::string("anullsrc")) +
              ",\"audioChannels\":" + std::to_string(realAudioEnabledForProcess() ? activeAudioChannels_ : 0) +
              ",\"audioSampleRate\":" + std::to_string(realAudioEnabledForProcess() ? activeAudioSampleRate_ : 0) +
              ",\"videoInputPixelFormat\":" + jsonString(videoInputPixelFormat) +
              ",\"ffmpegVideoEncoder\":" + jsonString(selectedFfmpegVideoEncoder_) +
              ",\"ffmpegStderrPath\":" + jsonString(ffmpegStderrPath_.string()) + "}");
    return true;
#else
    // POSIX (macOS/Linux) frame path: posix_spawn FFmpeg with the video pipe on
    // stdin (pipe:0) and, when present, the audio pipe inherited as fd 3
    // (pipe:3). Mirrors the Windows process+pipe path behind the platform guard.
    int videoPipe[2] = {-1, -1};
    if (::pipe(videoPipe) != 0) {
      sender_.status = "failed";
      sender_.warning = std::string("Could not create FFmpeg stdin pipe: ") + std::strerror(errno);
      sender_.destinationHealth = "failed";
      sender_.lastResultCode = "ffmpeg-pipe-failed";
      sender_.lastError = sender_.warning;
      return false;
    }

    int audioPipe[2] = {-1, -1};
    if (activeAudioPresent_) {
      if (::pipe(audioPipe) != 0) {
        ::close(videoPipe[0]);
        ::close(videoPipe[1]);
        sender_.status = "failed";
        sender_.warning = std::string("Could not create FFmpeg audio pipe: ") + std::strerror(errno);
        sender_.destinationHealth = "failed";
        sender_.lastResultCode = "ffmpeg-audio-pipe-failed";
        sender_.lastError = sender_.warning;
        return false;
      }
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    // Video read end -> child stdin (fd 0); /dev/null -> stdout. stderr goes
    // to a temp log (published in the start event, Windows parity) — without
    // it an ingest rejection and a blocked pipe are indistinguishable.
    posix_spawn_file_actions_adddup2(&actions, videoPipe[0], 0);
    posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
    ffmpegStderrPath_.clear();
    {
      const char* tempDir = ::getenv("TMPDIR");
      std::string logPath = std::string(tempDir ? tempDir : "/tmp");
      if (!logPath.empty() && logPath.back() != '/') {
        logPath += '/';
      }
      logPath += "corevideo-rtmp-ffmpeg-" + std::to_string(::getpid()) + "-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                 ".log";
      ffmpegStderrPath_ = logPath;
    }
    posix_spawn_file_actions_addopen(&actions, 2, ffmpegStderrPath_.c_str(),
                                     O_WRONLY | O_CREAT | O_TRUNC, 0644);
    std::string audioInputArg = "pipe:0";
    if (activeAudioPresent_) {
      // Audio read end -> child fd 3 (referenced as pipe:3).
      posix_spawn_file_actions_adddup2(&actions, audioPipe[0], 3);
      audioInputArg = "pipe:3";
    }
    // Close our write ends in the child.
    posix_spawn_file_actions_addclose(&actions, videoPipe[1]);
    if (activeAudioPresent_) {
      posix_spawn_file_actions_addclose(&actions, audioPipe[1]);
    }

    const std::string argString = buildFfmpegArguments(width, height, audioInputArg, videoInputPixelFormat);
    // One-shot: the exact invocation, so a stream that connects but delivers no
    // video can be reproduced by hand instead of inferred.
    {
      static bool s_loggedArgs = false;
      if (!s_loggedArgs) {
        s_loggedArgs = true;
        std::fprintf(stderr, "[rtmp] ffmpeg %s\n", argString.c_str());
      }
    }
    std::vector<std::string> tokens = tokenizeArguments(argString);
    std::vector<char*> argv;
    argv.reserve(tokens.size() + 2);
    argv.push_back(const_cast<char*>(ffmpegExecutable_.c_str()));
    for (auto& token : tokens) {
      argv.push_back(const_cast<char*>(token.c_str()));
    }
    argv.push_back(nullptr);

    pid_t pid = 0;
    const int spawnResult = posix_spawn(&pid, ffmpegExecutable_.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(videoPipe[0]);
    if (activeAudioPresent_) {
      ::close(audioPipe[0]);
    }
    if (spawnResult != 0) {
      ::close(videoPipe[1]);
      if (activeAudioPresent_) {
        ::close(audioPipe[1]);
      }
      sender_.status = "failed";
      sender_.warning = std::string("Failed to start FFmpeg process: ") + std::strerror(spawnResult);
      sender_.destinationHealth = "failed";
      sender_.lastResultCode = "ffmpeg-start-failed";
      sender_.lastError = sender_.warning;
      return false;
    }

    // Avoid SIGPIPE killing us when FFmpeg exits; writes return EPIPE instead.
    ::signal(SIGPIPE, SIG_IGN);
    ffmpegStdinFd_ = videoPipe[1];
    ffmpegAudioFd_ = activeAudioPresent_ ? audioPipe[1] : -1;
    ffmpegPid_ = pid;
    ffmpegRunning_ = true;
    sender_.runtimeDetail = "ffmpeg:" + ffmpegExecutable_;
    writeLine("{\"type\":\"ffmpeg-process-start\",\"destination\":\"rtmp\",\"width\":" + std::to_string(width) +
              ",\"height\":" + std::to_string(height) +
              ",\"endpoint\":" + jsonString(redactedSenderEndpoint()) +
              ",\"ffmpegExecutable\":" + jsonString(ffmpegExecutable_) +
              ",\"videoCodec\":" + jsonString(configuredVideoCodec_) +
              ",\"encoderMode\":" + jsonString(configuredEncoderMode_) +
              ",\"audioInput\":" + jsonString(activeAudioPresent_ ? std::string("pcm") : std::string("anullsrc")) +
              ",\"audioChannels\":" + std::to_string(activeAudioPresent_ ? activeAudioChannels_ : 0) +
              ",\"audioSampleRate\":" + std::to_string(activeAudioPresent_ ? activeAudioSampleRate_ : 0) +
              ",\"videoInputPixelFormat\":" + jsonString(videoInputPixelFormat) +
              ",\"ffmpegVideoEncoder\":" + jsonString(selectedFfmpegVideoEncoder_) +
              ",\"ffmpegStderrPath\":" + jsonString(ffmpegStderrPath_.string()) + "}");
    return true;
#endif
  }

  bool writeFrameToFfmpeg(const ProgramFrame& frame) {
#if defined(_WIN32)
    if (!ffmpegRunning_ || !ffmpegStdin_) {
      return false;
    }
    HANDLE process = nullptr;
    {
      std::lock_guard<std::mutex> lock(ffmpegProcessMutex_);
      process = ffmpegProcess_;
    }
    DWORD exitCode = 0;
    if (process && GetExitCodeProcess(process, &exitCode) && exitCode != STILL_ACTIVE) {
      sender_.lastResultCode = "ffmpeg-exited";
      sender_.lastError = "FFmpeg process exited before accepting program frames. Exit code " + std::to_string(exitCode) + ".";
      return false;
    }
    // Audio is written on every output-worker tick before video pacing is
    // evaluated in sync(). A failed video write stops the process.
    DWORD written = 0;
    const auto& videoBytes = videoFrameBytes(frame);
    const auto* data = videoBytes.data();
    size_t remaining = videoBytes.size();
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
    if (!ffmpegRunning_ || ffmpegStdinFd_ < 0) {
      return false;
    }
    if (ffmpegPid_ > 0) {
      int status = 0;
      const pid_t result = ::waitpid(ffmpegPid_, &status, WNOHANG);
      if (result == ffmpegPid_) {
        ffmpegPid_ = 0;
        sender_.lastResultCode = "ffmpeg-exited";
        if (WIFEXITED(status)) {
          sender_.lastError = "FFmpeg process exited before accepting program frames. Exit code " + std::to_string(WEXITSTATUS(status)) + ".";
        } else if (WIFSIGNALED(status)) {
          sender_.lastError = "FFmpeg process exited before accepting program frames. Signal " + std::to_string(WTERMSIG(status)) + ".";
        } else {
          sender_.lastError = "FFmpeg process exited before accepting program frames.";
        }
        return false;  // FFmpeg exited
      }
    }
    const auto& videoBytes = videoFrameBytes(frame);
    // BOUNDARY EVIDENCE (one-shot): ffmpeg is told `-s WxH -pix_fmt <fmt>` and
    // then fed raw frames on pipe:0. If the byte count does not match that
    // geometry exactly, ffmpeg blocks forever assembling a frame that never
    // completes — no video ever reaches the endpoint while the separate audio
    // pipe keeps flowing. That is precisely the observed symptom, so log what we
    // declared against what we actually write.
    {
      static bool s_logged = false;
      if (!s_logged) {
        s_logged = true;
        const int w = videoWidth(frame);
        const int h = videoHeight(frame);
        const auto fmt = videoPixelFormat(frame);
        const size_t expected = static_cast<size_t>(w) * static_cast<size_t>(h) *
                                (fmt == "nv12" ? 3u : 8u) / 2u;
        std::fprintf(stderr,
                     "[rtmp] first video write: declared %dx%d %s -> expected %zu bytes, "
                     "actual %zu bytes (source=%s)%s\n",
                     w, h, fmt.c_str(), expected, videoBytes.size(),
                     hasProgramNv12(frame) ? "programNv12"
                         : (hasProgramFullBgra(frame) ? "programFullBgra" : "preview"),
                     expected == videoBytes.size() ? "" : "  *** MISMATCH ***");
      }
    }
    const auto* data = reinterpret_cast<const char*>(videoBytes.data());
    size_t remaining = videoBytes.size();
    while (remaining > 0) {
      const ssize_t written = ::write(ffmpegStdinFd_, data, remaining);
      if (written <= 0) {
        if (written < 0 && errno == EINTR) {
          continue;
        }
        return false;
      }
      data += written;
      remaining -= static_cast<size_t>(written);
    }
    return true;
#endif
  }

  // Push this tick's interleaved float PCM down the audio pipe. Non-fatal on
  // error: audio drop should not tear down the live video stream.
  void writeAudioToFfmpeg() {
    if (!activeAudioPresent_ || !pendingAudioPcm_ || pendingAudioPcm_->empty()) {
      return;
    }
    const auto* bytes = reinterpret_cast<const char*>(pendingAudioPcm_->data());
    size_t remaining = pendingAudioPcm_->size() * sizeof(float);
    size_t accepted = 0;
#if defined(_WIN32)
    if (!audioPipeServer_) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(audioQueueMutex_);
      audioQueue_.insert(audioQueue_.end(), pendingAudioPcm_->begin(), pendingAudioPcm_->end());
      const size_t maxSamples = static_cast<size_t>((std::max)(1, activeAudioSampleRate_)) *
                                static_cast<size_t>((std::max)(1, activeAudioChannels_)) * 5;
      while (audioQueue_.size() > maxSamples) {
        audioQueue_.pop_front();
      }
    }
    audioQueueCv_.notify_one();
    sender_.audioBytesSent = audioBytesWritten_.load();
    const int bytesPerFrame = (std::max)(1, activeAudioChannels_) * static_cast<int>(sizeof(float));
    sender_.audioFramesSent = sender_.audioBytesSent / bytesPerFrame;
    return;
#else
    if (ffmpegAudioFd_ < 0) {
      return;
    }
    while (remaining > 0) {
      const ssize_t written = ::write(ffmpegAudioFd_, bytes, remaining);
      if (written <= 0) {
        if (written < 0 && errno == EINTR) {
          continue;
        }
        break;  // non-fatal
      }
      bytes += written;
      remaining -= static_cast<size_t>(written);
      accepted += static_cast<size_t>(written);
    }
#endif
    if (accepted > 0) {
      sender_.audioBytesSent += static_cast<int64_t>(accepted);
      const int bytesPerFrame = (std::max)(1, activeAudioChannels_) * static_cast<int>(sizeof(float));
      sender_.audioFramesSent += static_cast<int64_t>(accepted / static_cast<size_t>(bytesPerFrame));
    }
  }

#if defined(_WIN32)
  void startAudioWriterThread() {
    {
      std::lock_guard<std::mutex> lock(audioQueueMutex_);
      audioWriterStop_ = false;
      audioQueue_.clear();
    }
    audioBytesWritten_.store(0);
    audioWriterThread_ = std::thread([this] { audioWriterLoop(); });
  }

  void audioWriterLoop() {
    while (true) {
      {
        std::unique_lock<std::mutex> lock(audioQueueMutex_);
        if (audioWriterStop_) {
          return;
        }
      }

      if (!audioPipeConnected_) {
        if (ConnectNamedPipe(audioPipeServer_, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
          audioPipeConnected_ = true;
        } else {
          const DWORD error = GetLastError();
          if (error != ERROR_PIPE_LISTENING && error != ERROR_NO_DATA && error != ERROR_PIPE_BUSY) {
            return;
          }
          std::unique_lock<std::mutex> lock(audioQueueMutex_);
          audioQueueCv_.wait_for(lock, std::chrono::milliseconds(2), [&] { return audioWriterStop_; });
          continue;
        }
      }

      std::vector<float> samples;
      {
        std::unique_lock<std::mutex> lock(audioQueueMutex_);
        audioQueueCv_.wait_for(lock, std::chrono::milliseconds(5), [&] {
          return audioWriterStop_ || !audioQueue_.empty();
        });
        if (audioWriterStop_) {
          return;
        }
        const size_t chunkSamples = (std::min)(audioQueue_.size(), static_cast<size_t>(4096));
        samples.reserve(chunkSamples);
        for (size_t index = 0; index < chunkSamples; ++index) {
          samples.push_back(audioQueue_.front());
          audioQueue_.pop_front();
        }
      }
      if (samples.empty()) {
        continue;
      }

      const auto* data = reinterpret_cast<const char*>(samples.data());
      size_t bytesRemaining = samples.size() * sizeof(float);
      while (bytesRemaining > 0) {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>((std::min)(bytesRemaining, static_cast<size_t>(1) << 20));
        const BOOL writeSucceeded = WriteFile(audioPipeServer_, data, chunk, &written, nullptr);
        if (writeSucceeded && written == 0) {
          // A zero-byte success must not end the audio writer; retain the same
          // buffer and let pipe backpressure clear.
          std::unique_lock<std::mutex> lock(audioQueueMutex_);
          audioQueueCv_.wait_for(lock, std::chrono::milliseconds(2), [&] { return audioWriterStop_; });
          if (audioWriterStop_) {
            return;
          }
          continue;
        }
        if (!writeSucceeded) {
          const DWORD error = GetLastError();
          if (error == ERROR_OPERATION_ABORTED && audioWriterStop_) {
            return;
          }
          if (error == ERROR_NO_DATA || error == ERROR_PIPE_LISTENING || error == ERROR_PIPE_BUSY) {
            std::unique_lock<std::mutex> lock(audioQueueMutex_);
            audioQueueCv_.wait_for(lock, std::chrono::milliseconds(2), [&] { return audioWriterStop_; });
            if (audioWriterStop_) {
              return;
            }
            continue;
          }
          return;
        }
        data += written;
        bytesRemaining -= written;
        audioBytesWritten_.fetch_add(written);
      }
    }
  }

  void stopAudioWriterThread() {
    {
      std::lock_guard<std::mutex> lock(audioQueueMutex_);
      audioWriterStop_ = true;
      audioQueue_.clear();
    }
    audioQueueCv_.notify_all();
    if (audioWriterThread_.joinable()) {
      // PIPE_WAIT makes steady-state audio lossless. Explicitly cancel a
      // pending ConnectNamedPipe/WriteFile so stream stop remains bounded.
      CancelSynchronousIo(static_cast<HANDLE>(audioWriterThread_.native_handle()));
      audioWriterThread_.join();
    }
  }

  void closeAudioPipe() {
    stopAudioWriterThread();
    if (audioPipeServer_) {
      CloseHandle(audioPipeServer_);
      audioPipeServer_ = nullptr;
    }
    audioPipeConnected_ = false;
  }
#endif

  void stopFfmpegProcess() {
#if defined(_WIN32)
    if (ffmpegStdin_) {
      CloseHandle(ffmpegStdin_);
      ffmpegStdin_ = nullptr;
    }
    closeAudioPipe();
    HANDLE process = nullptr;
    {
      std::lock_guard<std::mutex> lock(ffmpegProcessMutex_);
      process = ffmpegProcess_;
      ffmpegProcess_ = nullptr;
    }
    if (process) {
      DWORD exitCode = 0;
      if (GetExitCodeProcess(process, &exitCode) && exitCode == STILL_ACTIVE) {
        WaitForSingleObject(process, 500);
      }
      CloseHandle(process);
    }
#else
    if (ffmpegStdinFd_ >= 0) {
      ::close(ffmpegStdinFd_);
      ffmpegStdinFd_ = -1;
    }
    if (ffmpegAudioFd_ >= 0) {
      ::close(ffmpegAudioFd_);
      ffmpegAudioFd_ = -1;
    }
    if (ffmpegPid_ > 0) {
      // Closing the pipes signals EOF; give FFmpeg a brief chance to flush, then
      // reap so we don't leak a zombie.
      int status = 0;
      for (int attempt = 0; attempt < 50; ++attempt) {
        const pid_t result = ::waitpid(ffmpegPid_, &status, WNOHANG);
        if (result == ffmpegPid_ || result < 0) {
          break;
        }
        ::usleep(10000);
      }
      if (::waitpid(ffmpegPid_, &status, WNOHANG) == 0) {
        ::kill(ffmpegPid_, SIGTERM);
        ::waitpid(ffmpegPid_, &status, 0);
      }
      ffmpegPid_ = 0;
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
              ",\"endpoint\":" + jsonString(redactedSenderEndpoint()) +
              ",\"endpointMode\":\"ffmpeg-process\","
              "\"testMode\":false,\"muxingMode\":\"ffmpeg-process\",\"runtimeLibrary\":\"ffmpeg\",\"runtimeAvailable\":" +
              std::string(runtimeAvailable_ ? "true" : "false") +
              ",\"runtimeDetail\":" + jsonString(runtimeDetail_) +
              ",\"runtimeCandidates\":" + runtimeCandidatesJson(runtimeProbe_.candidates) +
              ",\"videoCodec\":" + jsonString(configuredVideoCodec_) +
              ",\"encoderMode\":" + jsonString(configuredEncoderMode_) +
               ",\"ffmpegVideoEncoder\":" + jsonString(selectedFfmpegVideoEncoder_.empty() ? ffmpegVideoEncoderFor(configuredVideoCodec_, configuredEncoderMode_) : selectedFfmpegVideoEncoder_) +
              ",\"packagingSignal\":\"sync-ffmpeg-runtime-to-app.ps1 stages ffmpeg.exe and corevideo-ffmpeg-runtime.json when FFmpeg is available or unavailable\"}");
  }

  void appendSendProof(const ProgramFrame* frame, const std::string& status) {
    if (!sendProof_.is_open()) {
      return;
    }
    std::string line = "{\"type\":\"rtmp-send-attempt\",\"destination\":\"rtmp\",\"endpointMode\":\"ffmpeg-process\",\"status\":" + jsonString(status);
    if (frame) {
      line += ",\"frameNumber\":" + std::to_string(frame->frameNumber) +
              ",\"width\":" + std::to_string(videoWidth(*frame)) +
              ",\"height\":" + std::to_string(videoHeight(*frame)) +
              ",\"videoInputPixelFormat\":" + jsonString(videoPixelFormat(*frame)) +
              ",\"renderPlanId\":" + jsonString(frame->renderPlanId) +
              ",\"videoCodec\":" + jsonString(configuredVideoCodec_) +
              ",\"encoderMode\":" + jsonString(configuredEncoderMode_) +
              ",\"ffmpegVideoEncoder\":" + jsonString(selectedFfmpegVideoEncoder_.empty() ? ffmpegVideoEncoderFor(configuredVideoCodec_, configuredEncoderMode_) : selectedFfmpegVideoEncoder_);
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

  FfmpegSenderProtocol protocol_;
  // Set when the requested codec has no supported hardware encoder here, so the
  // operator is told rather than silently receiving a different codec.
  std::string unsupportedCodecWarning_;
  RuntimeProbe runtimeProbe_;
  std::string runtimeDetail_;
  bool runtimeAvailable_ = false;
  std::string configuredEndpoint_;
  std::string configuredStreamKey_;
  std::string configuredFfmpegBinDirectory_;
  std::string configuredVideoCodec_ = "h264";
  std::string configuredEncoderMode_ = "auto";
  double configuredKeyframeIntervalSeconds_ = 2.0;
  std::string configuredRateControl_ = "cbr";
  std::string configuredH264Profile_ = "high";
  int configuredBFrames_ = 2;
  bool configuredAllowEnhancedRtmp_ = false;
  std::string ffmpegExecutable_;
  std::string selectedFfmpegVideoEncoder_;
  std::filesystem::path ffmpegStderrPath_;
  std::string activeEndpoint_;
  std::string activeFfmpegExecutable_;
  std::string activeVideoCodec_;
  std::string activeEncoderMode_;
  double activeKeyframeIntervalSeconds_ = 0;
  std::string activeRateControl_;
  std::string activeH264Profile_;
  int activeBFrames_ = -1;
  bool activeAllowEnhancedRtmp_ = false;
  int ffmpegFrameWidth_ = 0;
  int ffmpegFrameHeight_ = 0;
  std::string ffmpegPixelFormat_ = "bgra";
  int configuredFps_ = 30;
  int configuredAudioBitrateKbps_ = 160;
  int activeFps_ = 0;
  double activeBitrateMbps_ = 0;
  int activeAudioBitrateKbps_ = 0;
  bool ffmpegRunning_ = false;
  int consecutiveFfmpegFailures_ = 0;
  std::chrono::steady_clock::time_point ffmpegRetryAfter_{};
  // Latest real program-audio mix for this tick (interleaved float PCM), and the
  // audio layout currently baked into the running FFmpeg process. `pending*` is
  // refreshed by sync(); `active*` reflects the live process configuration.
  const std::vector<float>* pendingAudioPcm_ = nullptr;
  int pendingAudioChannels_ = 0;
  int pendingAudioSampleRate_ = 0;
  // Sticky "a real PCM source exists", latched by submitAudio/sync and never
  // cleared by a video-only sync. The FFmpeg arg list bakes in the audio input,
  // so treating a video-only call as audio-absent would restart the encoder.
  bool haveRealAudio_ = false;
  bool activeAudioPresent_ = false;
  int activeAudioChannels_ = 0;
  int activeAudioSampleRate_ = 0;
#if defined(_WIN32)
  mutable std::mutex ffmpegProcessMutex_;
  HANDLE ffmpegProcess_ = nullptr;
  HANDLE ffmpegStdin_ = nullptr;
  HANDLE audioPipeServer_ = nullptr;
  bool audioPipeConnected_ = false;
  std::mutex audioQueueMutex_;
  std::condition_variable audioQueueCv_;
  std::deque<float> audioQueue_;
  std::thread audioWriterThread_;
  bool audioWriterStop_ = true;
  std::atomic<int64_t> audioBytesWritten_{0};
#else
  int ffmpegStdinFd_ = -1;
  int ffmpegAudioFd_ = -1;
  pid_t ffmpegPid_ = 0;
#endif
  OutputSender sender_;
  RtmpVideoFramePacer videoFramePacer_;
  std::ofstream sendProof_;
};
#endif

}  // namespace

std::unique_ptr<IOutputSender> createRtmpOutputSender() {
#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_RTMP_OUTPUT
  // REQUIRES DEV MACHINE: real RTMP packet muxing belongs behind this libavformat
  // sender. The scaffold verifies runtime availability without affecting stubs.
  return std::make_unique<RtmpOutputSender>(probeFfmpegRuntime(""), rtmpProtocol());
#else
  return nullptr;
#endif
}

// SRT DELIVERY. Same FFmpeg process pipeline as RTMP - only the endpoint syntax,
// container (MPEG-TS) and validation differ - so it is the same sender with a
// different protocol profile rather than a second implementation. Gated on the
// RTMP build flag because it IS the RTMP sender; the staged FFmpeg is built with
// libsrt, verified by loopback before this was wired.
std::unique_ptr<IOutputSender> createFfmpegSrtOutputSender() {
#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_RTMP_OUTPUT
  return std::make_unique<RtmpOutputSender>(probeFfmpegRuntime(""), srtProtocol());
#else
  return nullptr;
#endif
}

}  // namespace corevideo::modules
