#include "core/BoundedAsyncLog.h"
#include "core/StreamBackpressurePolicy.h"
#include "modules/Interfaces.h"
#include "modules/RtmpCompatibility.h"
#include "modules/RtmpFfmpegArgs.h"
#include "modules/GpuVideoEncoder.h"
#include "modules/HevcTransportStream.h"
#include "modules/MediaFoundationGpuVideoEncoder.h"
#include "modules/EncoderCapacityProbe.h"
#include "modules/EncoderPolicy.h"
#include "modules/StreamStartAdmission.h"
#include "modules/OutputDestinationSupervisorPolicy.h"
#include "modules/BitstreamQueueOverflow.h"
#include "modules/FfmpegSenderDiagnostics.h"
#include "modules/SrtFfmpegArgs.h"

#include <algorithm>
#include <array>
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

#if defined(_WIN32)
// NO ORPHANS. Same rule the SRT ingest decoder already follows (see
// ingestJobObject() in SrtIngestCaptureAdapter.cpp): an FFmpeg spawned by the
// core must not outlive it. For OUTPUT the stakes are higher than for ingest --
// an orphaned egress FFmpeg keeps PUBLISHING to a live destination after the
// app is gone, so the audience keeps seeing a stream nobody is driving.
// KILL_ON_JOB_CLOSE makes the OS do the cleanup when the last handle to the job
// closes, which happens when the core process dies for any reason, including a
// kill that runs no destructors.
//
// SEPARATE JOB FROM INGEST, deliberately. Ingest's job is a file-local static in
// another translation unit's anonymous namespace, so sharing it would mean
// exporting a new cross-module handle for no gain. Keeping outputs in their own
// job also means egress children can later be terminated as a group without
// tearing down ingest decoders, which have an unrelated lifetime.
//
// RESTART IS UNAFFECTED. The job handle is a leaked process-lifetime static, so
// it never closes while we are alive; nothing here kills a child. An intentional
// restart still works the way it always did -- stopFfmpegProcess() terminates
// the old child, and the replacement is simply assigned to the same job.
HANDLE outputJobObject() {
  static HANDLE job = [] {
    HANDLE created = ::CreateJobObjectW(nullptr, nullptr);
    if (created == nullptr) {
      ::corevideo::core::nativeLogf(
          "[rtmp] WARNING: CreateJobObject failed (win32 %lu); output FFmpeg children will NOT be "
          "killed automatically if the core dies\n",
          static_cast<unsigned long>(::GetLastError()));
      return created;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!::SetInformationJobObject(created, JobObjectExtendedLimitInformation, &limits,
                                   sizeof(limits))) {
      ::corevideo::core::nativeLogf(
          "[rtmp] WARNING: SetInformationJobObject(KILL_ON_JOB_CLOSE) failed (win32 %lu); output "
          "FFmpeg children may be orphaned if the core dies\n",
          static_cast<unsigned long>(::GetLastError()));
    }
    return created;
  }();
  return job;
}

// Best effort by design: a stream must never fail to start because the OS would
// not hand us a job object. Degrade loudly, keep publishing.
void adoptOutputChild(HANDLE process, const char* protocolTag) {
  HANDLE job = outputJobObject();
  if (job == nullptr) {
    return;  // already logged once at creation
  }
  if (!::AssignProcessToJobObject(job, process)) {
    ::corevideo::core::nativeLogf(
        "[%s] WARNING: AssignProcessToJobObject failed (win32 %lu); this FFmpeg child will survive "
        "an abnormal core exit and keep publishing\n",
        protocolTag, static_cast<unsigned long>(::GetLastError()));
  }
}
#endif

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
  // The probe is short-lived and always reaped below, but a core killed inside
  // that 5s window would still leave it behind. Same job, same rule.
  adoptOutputChild(processInfo.hProcess, "rtmp-probe");
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
        runtimeAvailable_(runtimeProbe_.available),
        gpuEncoderFactory_(&createMediaFoundationGpuVideoEncoder) {}

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
      // THE ONE UNFLOORED ROUTE, AND THE INVARIANT IT RESTS ON (round 2, item 4).
      // Stream off drops the streak AND the floor, which is right: switching a
      // destination off and on again is the operator re-arming it. It is safe
      // ONLY because `outputDestinations_` is COMMAND state (MediaCore's
      // start/stop-program-output), so this branch is entered on an operator
      // action and not once per tick. A producer that made the destination list
      // tick-derived - or that alternated it - would reset both ladders every
      // tick and re-open the #597 storm through this door. If destinations ever
      // become derived state, this clear() has to become an edge-triggered
      // operator signal, not a per-sync observation.
      restartFloor_.clear();
      backoffProofWritten_ = false;
      startRefusedInadmissible_ = false;  // Stream off/on re-evaluates the refusal
      // #597 Lever A: A STOPPED DESTINATION HAS NO QUEUE. observeStreamBackpressure()
      // lives far below this return, so without these two lines the stopped record
      // keeps publishing whatever divisor it last reached, MediaCore keeps taking it
      // as the max, and the compositor stays throttled with nothing streaming - and
      // renderVideoOutputTick stops calling sync() once the last destination goes,
      // so nothing would ever correct it. Resetting the policy object (rather than
      // only clearing the published value) is what stops the NEXT stream opening at
      // 15 fps on an empty queue; its cumulative counters are per stream RUN.
      // #597 Lever B: the discard counter is per-stream-run state exactly like
      // the policy object - it is reset here too, or the next run's Task 6
      // telemetry would report the PREVIOUS show's discards as its own.
      // #597 Task 6 fix round 1, finding 11: a new run gets a new identity, so
      // a consumer that reads across this reset without observing the node's
      // momentary absence can still tell the counters were RESET, not merely
      // decreased. All of it lives in resetBackpressureForNewRun(), which the
      // reopen path shares (final-review finding 2).
      resetBackpressureForNewRun();
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
    const std::string requestedEndpoint = protocol_.isSrt
                                              ? buildSrtUrl(srtEndpointConfigFrom(*settings)).url
                                              : buildRtmpEndpoint(*settings);
    // THE ONE RE-EVALUATION TRIGGER for a latched configuration refusal. This
    // block re-applies desired state on EVERY tick (the repeating sync channel),
    // so the latch is cleared only when an input the verdict actually depends on
    // has CHANGED - clearing it on every apply would re-attempt and re-log the
    // refusal at frame rate, which is exactly what the latch exists to stop.
    if (configuredEndpoint_ != requestedEndpoint ||
        configuredVideoCodec_ != normalizeVideoCodec(settings->videoCodec) ||
        configuredEncoderMode_ != normalizeEncoderMode(settings->encoderMode) ||
        configuredAllowEnhancedRtmp_ != settings->allowEnhancedRtmp) {
      startRefusedInadmissible_ = false;
    }
    configuredEndpoint_ = requestedEndpoint;
    configuredStreamKey_ = settings->streamKey;
    // Held ONLY so the stderr tail can be scrubbed of it before it reaches
    // lastError (and from there /snapshot and the support bundle). The SRT
    // endpoint carries the passphrase in its query string, so FFmpeg echoes it.
    configuredPassphrase_ = settings->passphrase;
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
    // Surface the codec/container compatibility note: for an admitted E-RTMP
    // stream it is the advisory that the ingest must support it, and for a
    // refused one it is the sentence the start-time admission repeats.
    const auto codecCompatibility = resolveCompatibility();
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

    // #597 Lever A: skip program frames BEFORE the encoder. Compressed frames
    // cannot be dropped individually, so the only safe throttle is upstream. The
    // encoder's declared frame rate is unchanged, so bits-per-frame - and with
    // it per-frame quality - holds while the data rate falls. Observed once per
    // sync(), here, where the sender already holds the frame and before any
    // path that can return early on a configuration refusal.
    //
    // THE POLICY THEREFORE TICKS AT THE RENDER RATE, NOT THE CONFIGURED STREAM
    // FPS - this site is above videoFramePacer_. So kEnterAfterOverWaterTicks
    // (30) is ~0.5 s on the 60 Hz video tick (~0.6 s on the ~50 Hz direct path)
    // whether the stream is declared at 30 or 60 fps, and kRecoverAfterHealthyTicks
    // (600) is ~10 s. A reader of StreamBackpressurePolicy.h will reasonably
    // assume one tick == one submitted frame; here it does not. This is safe
    // because the SIGNAL the policy acts on (bufferedMs) is wall-clock by
    // construction - see that header - so only the streak lengths are rate
    // dependent, not the thresholds.
    //
    // observeStreamBackpressure() (and discardBacklogToNextKeyframe() beneath
    // it) live inside the #if defined(_WIN32) bitstream-queue block, because
    // the queue itself is Windows-only - guard the call here too, or a POSIX
    // build with COREVIDEO_WITH_RTMP_OUTPUT=ON fails to compile.
#if defined(_WIN32)
    observeStreamBackpressure(elapsedMs);
#endif

    // Skip BEFORE ensureFfmpegProcess: that call pins FFmpeg's -s geometry from
    // this frame, so letting a preview-sized frame through here is what
    // restarted the encoder mid-stream.
    if (!videoSourceUsable(*frame)) {
      sender_.status = hasWrittenVideo_ ? "live" : "starting";
      sender_.destinationHealth = hasWrittenVideo_ ? "ok" : "starting";
      sender_.lastResultCode = "awaiting-full-res-frame";
      appendSendProof(frame, "awaiting-full-res-frame");
      return snapshot();
    }

    if (!ensureFfmpegProcess(*frame, elapsedMs)) {
      // A latched refusal already wrote its one proof line with the named code;
      // appending per frame would flood the proof file for the rest of the show.
      //
      // A SERVED BACKOFF IS LATCHED THE SAME WAY (round 2, item 6). This runs on
      // every program frame, so a rung is 60 Hz x its length of flushed lines -
      // and round 1 raised the rungs from 1-30 s to 5-60 s, which would have
      // made a rung-5 backoff ~3,600 lines saying the identical thing. One line
      // per backoff WINDOW: the latch is cleared wherever the floor state can
      // next change (a real open, a fresh failure, or a clear), so every
      // distinct window still gets its line.
      const bool servingBackoff = sender_.lastResultCode == "ffmpeg-retry-backoff";
      if (!startRefusedInadmissible_ && !(servingBackoff && backoffProofWritten_)) {
        appendSendProof(frame, servingBackoff ? "ffmpeg-retry-backoff" : "ffmpeg-start-failed");
      }
      backoffProofWritten_ = servingBackoff;
      return snapshot();
    }

    // Audio follows the 20 ms output-worker cadence. Video does not: pace it to
    // the configured stream fps so a 50 Hz worker cannot overfill FFmpeg's raw
    // 4K input pipe (the 2026-07-14 live freeze reproduced at 42 frames/0.84 s).
    writeAudioToFfmpeg();
    if (!videoFramePacer_.shouldWrite(elapsedMs, configuredFps_)) {
      sender_.status = hasWrittenVideo_ ? "live" : "starting";
      sender_.warning.clear();
      sender_.runtimeDetail = runtimeDetail_;
      sender_.audioChannels = activeAudioPresent_ ? activeAudioChannels_ : 0;
      sender_.audioSampleRate = activeAudioPresent_ ? activeAudioSampleRate_ : 0;
      sender_.destinationHealth = hasWrittenVideo_ ? "ok" : "starting";
      sender_.lastResultCode = hasWrittenVideo_ ? "encoder-input-accepted" : "waiting-for-frame";
      return snapshot();
    }

    // GPU-direct: hand the compositor's encoder texture to the hardware encoder,
    // whose sink writes the bitstream to FFmpeg. Raw path writes NV12/BGRA itself.
    const bool videoWriteOk =
        useGpuDirect_ ? submitFrameToGpuEncoder(*frame) : writeFrameToFfmpeg(*frame);
    if (!videoWriteOk) {
      sender_.status = "failed";
      ++sender_.retryCount;
      bool queueOverflow = false;
      bool invalidTiming = false;
#if defined(_WIN32)
      queueOverflow = useGpuDirect_ && bitstreamFailure_.reason() == BitstreamFailure::QueueOverflow;
      invalidTiming = useGpuDirect_ && bitstreamFailure_.reason() == BitstreamFailure::InvalidTiming;
#endif
      const auto genericFailure = invalidTiming
          ? std::string("Hardware encoder returned missing or non-monotonic packet timestamps.")
          : queueOverflow
          ? std::string("Compressed-video queue overflow; the stream transport could not drain encoded video fast enough.")
          : sender_.lastResultCode == "ffmpeg-exited" && !sender_.lastError.empty()
              ? sender_.lastError
              : std::string("FFmpeg stdin write failed; the ") + protocol_.destination +
                    " process stopped or rejected frames.";
      sender_.destinationHealth = "failed";
      if (invalidTiming) {
        sender_.lastResultCode = "encoder-timestamp-invalid";
      } else if (queueOverflow) {
        sender_.lastResultCode = "bitstream-queue-overflow";
      } else if (sender_.lastResultCode != "ffmpeg-exited") {
        sender_.lastResultCode = "ffmpeg-write-failed";
      }
      const auto proofStatus = sender_.lastResultCode;
      // #597: climb the ladder. This is the site the incident hammered — a
      // queue overflow, a 1 s wait, a rebuild, one accepted frame, and round
      // again at 2.5–3.5 s. The streak now survives that one frame.
      restartFloor_.noteFailure(static_cast<std::int64_t>(elapsedMs));
      backoffProofWritten_ = false;
      // STOP FIRST, THEN READ THE STDERR. A failed stdin write is observed the
      // instant the pipe breaks, which is BEFORE FFmpeg has flushed the line that
      // says why - measured live 2026-09-12 against a refusing endpoint, where the
      // tail read here held only "Guessed Channel Layout: stereo" and the whole
      // 72-byte file never gained the "I/O error" line, because we killed the
      // process first. stopFfmpegProcess() closes stdin (FFmpeg's EOF) and waits
      // for exit, so the file is COMPLETE once it returns.
      stopFfmpegProcess();
      // Carry FFmpeg's own reason. Without it this sentence is undiagnosable and
      // reads to an operator as a credential problem (2026-09-12).
      const auto detailedFailure = describeFfmpegSenderFailure(genericFailure, ffmpegStderrTail());
      sender_.warning = detailedFailure;
      sender_.lastError = sender_.warning;
      appendSendProof(frame, proofStatus);
      return snapshot();
    }

    hasWrittenVideo_ = true;
    sender_.status = "live";
    sender_.warning.clear();
    sender_.runtimeDetail = runtimeDetail_;
    sender_.lastFrameNumber = frame->frameNumber;
    // These counters prove local FFmpeg input acceptance, not destination receipt.
    ++sender_.framesSent;
    sender_.bytesSent += estimatedFrameBytes(sender_.bitrateMbps);
    sender_.audioChannels = activeAudioPresent_ ? activeAudioChannels_ : 0;
    sender_.audioSampleRate = activeAudioPresent_ ? activeAudioSampleRate_ : 0;
    sender_.destinationHealth = "ok";
    sender_.lastResultCode = "encoder-input-accepted";
    // NOT clear(): a single accepted frame is a launch, not a healthy run. The
    // budget comes back only after kHealthyRunMs of this run accepting output —
    // the same rule OutputDestinationSupervisorPolicy states for the supervisor.
    restartFloor_.noteAccepted(static_cast<std::int64_t>(elapsedMs));
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

  // THE OPERATOR re-arming this destination. Clears the floor: the house rule is
  // that an operator action always clears give-up, and making them wait out a
  // ladder they just overrode is the opposite of that.
  OutputSenderSession recover(const std::string& destination, double elapsedMs, const std::string& reason) override {
    return reopen(destination, elapsedMs, reason, /*clearRestartFloor=*/true);
  }

  // THE SUPERVISOR restarting this destination automatically. Identical in every
  // respect EXCEPT that it KEEPS the floor (#597 task 7 round 1, finding 1).
  // Round 0 routed this through recover(), so every supervisor restart wiped the
  // adapter's floor and the composed path had no backstop at either level.
  OutputSenderSession restartForSupervisor(const std::string& destination, double elapsedMs,
                                           const std::string& reason) override {
    return reopen(destination, elapsedMs, reason, /*clearRestartFloor=*/false);
  }

  OutputSenderSession reopen(const std::string& destination, double elapsedMs, const std::string& reason,
                             bool clearRestartFloor) {
    if (destination != protocol_.destination) {
      return snapshot();
    }
    stopFfmpegProcess();
    if (clearRestartFloor) restartFloor_.clear();
    // Final-review finding 2: a reopened transport starts from an EMPTY queue,
    // so it must start from an unthrottled policy too. See
    // resetBackpressureForNewRun() for the full reasoning.
    resetBackpressureForNewRun();
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
    sender_.lastError.clear();
    sender_.framesSent = sender_.audioFramesSent = 0;
    sender_.bytesSent = sender_.audioBytesSent = 0;
    return snapshot();
  }

  OutputSenderSession session() const override { return snapshot(); }

  // TEST-ONLY (see the declaration on IOutputSender for the structural guard).
  bool wouldRestartForEncodePathForTest(const ProgramFrame& frame) override {
    const bool desiredGpuDirect =
        resolveGpuEncodePath(frame, videoWidth(frame), videoHeight(frame)) == GpuEncodePath::GpuDirect;
    const bool gpuPathChanged = desiredGpuDirect != activeUseGpuDirect_;
    activeUseGpuDirect_ = desiredGpuDirect;  // exactly what a real restart records
    return gpuPathChanged;
  }

  // TEST-ONLY (see the declaration on IOutputSender for the structural guard).
  void setBackpressureObservationForTest(std::int64_t bufferedMs, bool keyframeInQueue) override {
    backpressureBufferedMsForTest_.store(bufferedMs, std::memory_order_relaxed);
    backpressureKeyframeForTest_.store(keyframeInQueue, std::memory_order_relaxed);
  }

  // TEST-ONLY (see the declaration on IOutputSender for the structural guard).
  // #597 Lever B seam: pushes straight onto bitstreamQueue_ so discard
  // correctness can be exercised with no hardware encoder.
  void enqueueBitstreamChunkForTest(std::size_t bytes, bool keyframe) override {
#if defined(_WIN32)
    GpuEncodedChunk metadata;
    metadata.keyframe = keyframe;
    std::lock_guard<std::mutex> lock(bitstreamQueueMutex_);
    bitstreamQueue_.push_back(
        QueuedBitstream{std::vector<uint8_t>(bytes, 0), metadata, std::chrono::steady_clock::now()});
    bitstreamQueuedBytes_ += bytes;
    republishQueueTelemetryLocked();
#else
    (void)bytes;
    (void)keyframe;
#endif
  }

  // TEST-ONLY (see the declaration on IOutputSender for the structural guard).
  // #597 Task 8b seam: offers a chunk through the REAL enqueueBitstream(), so
  // the overflow path (and the GOP-tail discard that now guards it) can be
  // exercised with no hardware encoder. bitstreamWriterStop_ defaults to true
  // until a stream actually starts its writer thread, and enqueueBitstream()
  // correctly refuses to queue anything in that state, so the seam clears it
  // for exactly the duration of the call and puts it back - it must never
  // leave a stopped sender looking started.
  void offerBitstreamChunkForTest(std::size_t bytes, bool keyframe) override {
#if defined(_WIN32)
    std::vector<uint8_t> payload(bytes, 0);
    GpuEncodedChunk chunk;
    chunk.data = payload.data();
    chunk.size = payload.size();
    chunk.keyframe = keyframe;
    const bool previousStop = bitstreamWriterStop_.exchange(false);
    enqueueBitstream(chunk);
    bitstreamWriterStop_.store(previousStop);
#else
    (void)bytes;
    (void)keyframe;
#endif
  }

  // TEST-ONLY (see the declaration on IOutputSender for the structural guard).
  // ONE read of the queue's true state - depth, keyframe presence, and the
  // real bitstreamBufferedMs() - so a call-site test needs one call instead
  // of three, and none of them go through the setBackpressureObservationForTest
  // override.
  BitstreamQueueSnapshotForTest bitstreamQueueSnapshotForTest() const override {
#if defined(_WIN32)
    return BitstreamQueueSnapshotForTest{
        static_cast<std::size_t>(bitstreamQueuedChunks_.load(std::memory_order_relaxed)),
        bitstreamQueueHasKeyframe_.load(std::memory_order_relaxed), bitstreamBufferedMs(),
        bitstreamFailure_.failed() &&
            bitstreamFailure_.reason() == BitstreamFailure::QueueOverflow};
#else
    return BitstreamQueueSnapshotForTest{};
#endif
  }

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
    return frame.programNv12Bytes().size() >= required;
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
      return frame.programNv12Bytes();
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
          ::corevideo::core::nativeLogf("[rtmp] full-res tap missed %lld of %lld ticks\n",
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
    // A LATCHED CONFIGURATION REFUSAL DOES NOT RE-ATTEMPT (2026-09-20). This runs
    // on every program frame, and an inadmissible configuration cannot change by
    // itself, so re-running the admission here would re-decide identically at ~60
    // Hz, churn stopFfmpegProcess()/stopGpuEncoder(), and flood the 128-entry
    // BoundedAsyncLog with one refusal line per frame. The already-published
    // status / warning / lastResultCode / lastError stand unchanged, so the
    // operator keeps seeing the named reason; the latch is cleared only by a
    // settings apply that changes an input the verdict depends on, or by Stream
    // being switched off (see sync()).
    if (startRefusedInadmissible_) {
      return false;
    }
    const int width = videoWidth(frame);
    const int height = videoHeight(frame);
    const auto pixelFormat = videoPixelFormat(frame);
    // GPU-direct vs raw is decided at process start; a change (env toggle, codec
    // change, encoder texture appearing/disappearing) restarts FFmpeg with the
    // matching argument list.
    const bool desiredGpuDirect = resolveGpuEncodePath(frame, width, height) == GpuEncodePath::GpuDirect;
    const bool gpuPathChanged = desiredGpuDirect != activeUseGpuDirect_;
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
    if (ffmpegRunning_ && !sizeChanged && !pixelFormatChanged && !endpointChanged && !executableChanged && !fpsChanged && !bitrateChanged && !audioBitrateChanged && !codecChanged && !encoderModeChanged && !keyframeChanged && !rateControlChanged && !h264ProfileChanged && !bFramesChanged && !enhancedChanged && !audioChanged && !gpuPathChanged) {
      return true;
    }

    // #597 THE RESTART FLOOR. This is the ONE gate every re-open of this
    // destination's transport passes through, and until 2026-09-23 it was the
    // adapter's own private ladder: first rung ONE second (below the house
    // ladder's five), and cleared by the first accepted frame. A destination
    // that came up, took a frame and died was therefore rebuilt every ~3 s
    // forever - the rebuild storm of the #597 incident, five of whose seven
    // starts the supervisor never decided. It now holds the house ladder
    // (TransportRestartFloor), so no two opens of one destination are closer
    // together than the ladder's current rung.
    //
    // THE ONE EXEMPTION IS A SETTINGS APPLY, NOT "the transport is running"
    // (round 1, finding 2). Round 0 exempted every restart taken while
    // ffmpegRunning_ was true and justified it as operator intent - but that
    // branch is also reached by `gpuPathChanged` (the encoder texture appearing
    // or disappearing, which under exactly this incident's load can flap at tick
    // rate), by `sizeChanged`/`pixelFormatChanged`, and by `audioChanged`. None
    // of those comes from an operator, and each was an unfloored rebuild per
    // flap. The floor now applies to every restart NOT caused by a change in the
    // operator's OutputDestinationSettings. A resolution change that arrives
    // while the floor is armed does wait a rung - correct, not a regression: an
    // armed floor means this destination is already failing repeatedly.
    const bool settingsApplied = endpointChanged || executableChanged || fpsChanged || bitrateChanged ||
                                 audioBitrateChanged || codecChanged || encoderModeChanged ||
                                 keyframeChanged || rateControlChanged || h264ProfileChanged ||
                                 bFramesChanged || enhancedChanged;
    if (!settingsApplied && !restartFloor_.mayOpenAt(static_cast<std::int64_t>(elapsedMs))) {
      // A REFUSED REBUILD LEAVES NO LINGERING CHILD (round 2, item 1). Round 0's
      // gate could only be reached with the transport already down, so falling
      // straight to `return false` was safe. Round 1's gate can be reached with
      // FFmpeg STILL RUNNING - the floor is armed from an earlier failure, a
      // settings apply opened a new transport through the exemption inside that
      // window, and a NON-settings change (gpuPathChanged, sizeChanged,
      // pixelFormatChanged, audioChanged) then arrives. Without this stop the
      // destination spends up to a full rung with a live child being fed
      // nothing while it publishes `failed`.
      //
      // On RTMP that is untidy. On SRT it is a show-killer and this repo has
      // already been bitten by it: an SRT listener accepts exactly ONE caller,
      // so a child that outlives its own teardown holds the slot and the
      // reconnect at the end of the rung is REFUSED - the stream never comes
      // back. stopFfmpegProcess() closes stdin, waits for exit and releases the
      // slot, so the rung is served with the transport genuinely down and
      // `failed` is then a true statement rather than a description of a child
      // that is still connected.
      if (ffmpegRunning_) {
        stopFfmpegProcess();
        // And SAY it stopped. `stoppedAtMs` is how the rest of the system reads
        // "this transport is down"; leaving it unset would publish `failed` for
        // a destination whose child had just been killed without recording when.
        sender_.stoppedAtMs = elapsedMs;
      }
      const auto retryMs = restartFloor_.remainingMs(static_cast<std::int64_t>(elapsedMs));
      sender_.status = "failed";
      sender_.destinationHealth = "failed";
      sender_.lastResultCode = "ffmpeg-retry-backoff";
      // Say what is actually true: not delivering, a retry IS scheduled, and
      // this is rung N of a bounded ladder - not a destination we have given up
      // on. Give-up is the supervisor's word and reads `supervisor-gave-up`.
      // NOT "attempt K of 5". The streak CLAMPS at kMaxConsecutiveFailures, so
      // an "of 5" form publishes "attempt 6 of 5" forever once the ladder tops
      // out - and it implies a give-up this floor never performs. The floor
      // retries at the top rung indefinitely; give-up belongs to the supervisor
      // and has its own word (`supervisor-gave-up`). Say the rung, not a fake
      // countdown.
      sender_.warning = sender_.lastError + " Retry paused for " +
                        std::to_string((std::max)(int64_t{1}, retryMs / 1000 + 1)) +
                        "s (consecutive failures: " +
                        std::to_string(restartFloor_.consecutiveFailures()) + ").";
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
    useGpuDirect_ = desiredGpuDirect;  // startFfmpegProcess may downgrade if the encoder fails to start
    if (startFfmpegProcess(width, height, pixelFormat)) {
      activeUseGpuDirect_ = useGpuDirect_;
      // Final re-review, still-open 1: the THIRD restart door. The operator stop
      // and the supervisor restart both reset the policy; the adapter's OWN
      // re-open at the end of a floor rung did not, so a self-reopened
      // transport resumed at its PRE-FAILURE divisor against an empty queue and
      // needed ~30s of health to climb back. Quality, never stream-off - but a
      // new run is a new run by every other measure this adapter keeps.
      resetBackpressureForNewRun();
      // The healthy-RUN window opens here, not at the first accepted frame.
      restartFloor_.noteOpened(static_cast<std::int64_t>(elapsedMs));
      backoffProofWritten_ = false;
      sender_.startedAtMs = elapsedMs;
      sender_.destinationHealth = "starting";
      sender_.lastResultCode = "ffmpeg-started";
      return true;
    }
    activeUseGpuDirect_ = false;
    if (startRefusedInadmissible_) {
      // A configuration refusal is not a transient failure: leave the named code
      // and sentence standing instead of burying them under ffmpeg-retry-backoff.
      restartFloor_.clear();
    } else {
      restartFloor_.noteFailure(static_cast<std::int64_t>(elapsedMs));
    }
    backoffProofWritten_ = false;
    return false;
  }

  // Never emit a raw endpoint: RTMP carries the stream key in the path and SRT
  // carries the passphrase in the query string.
  std::string redactedSenderEndpoint() const {
    return protocol_.isSrt ? redactedSrtUrl(configuredEndpoint_)
                           : redactedEndpoint(configuredEndpoint_, configuredStreamKey_);
  }

  // FFmpeg's own last words, redacted and bounded.
  //
  // This is the line that was missing on 2026-09-12: FFmpeg wrote "Error opening
  // output rtmp://<host>/<key>: I/O error" - the destination refusing the
  // connection - and the operator was shown "Check the server URL, stream key,
  // and network." instead, so a correct key was re-entered for two minutes.
  // Read on a FAILURE path only, never per frame.
  std::string ffmpegStderrTail() const {
    if (ffmpegStderrPath_.empty()) {
      return "";
    }
    std::error_code ec;
    const auto size = std::filesystem::file_size(ffmpegStderrPath_, ec);
    if (ec) {
      return "";
    }
    std::ifstream in(ffmpegStderrPath_, std::ios::binary);
    if (!in) {
      return "";
    }
    constexpr std::uintmax_t kMaxTailBytes = 2048;
    if (size > kMaxTailBytes) {
      in.seekg(static_cast<std::streamoff>(size - kMaxTailBytes), std::ios::beg);
    }
    std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return redactFfmpegDiagnostics(flattenFfmpegStderrTail(raw, kMaxTailBytes),
                                   configuredStreamKey_, configuredPassphrase_);
  }

  std::string buildFfmpegArguments(int width, int height, const std::string& audioInput, const std::string& videoInputPixelFormat) const {
    // Resolve the requested codec against THIS protocol's transport. On RTMP,
    // H.265/AV1 ride enhanced-RTMP when the operator enabled it; without it the
    // start is REFUSED (startFfmpegProcess), never downgraded. On SRT the
    // MPEG-TS container carries H.265 natively and no opt-in applies (see
    // resolveCompatibility). Either way these arguments describe the codec
    // actually sent.
    const auto compatibility = resolveCompatibility();
    RtmpFfmpegArgsConfig config;
    config.width = width;
    config.height = height;
    config.fps = (std::max)(1, configuredFps_);
    config.bitrateKbps = static_cast<int>((std::max)(500.0, sender_.bitrateMbps * 1000.0));
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
    // GPU-direct HEVC carries encoder timestamps in an internal TS envelope;
    // other codecs retain their elementary input. FFmpeg copies the video.
    config.videoBitstreamInput = useGpuDirect_;
    config.videoBitstreamCodec = gpuEncodeSentCodec_;
    config.timestampedHevcInput = useGpuDirect_ && gpuEncodeSentCodec_ == "hevc";
    return buildRtmpFfmpegArguments(config);
  }

  bool realAudioEnabledForProcess() const {
    const char* value = std::getenv("COREVIDEO_RTMP_DISABLE_REAL_AUDIO");
    return activeAudioPresent_ && !(value && std::string(value) == "1");
  }

  // One place a refused stream start becomes operator-visible state.
  //
  // An INADMISSIBLE CONFIGURATION DOES NOT RIDE THE RETRY LADDER (2026-09-20).
  // enhanced-rtmp-required and no-hardware-encoder are decided from settings and
  // this machine, so retrying re-decides them identically forever - and the retry
  // branch in ensureFfmpegProcess would overwrite lastResultCode with
  // ffmpeg-retry-backoff within one tick, making the named code unobservable.
  // The SAME predicate the output supervisor uses (isTerminalResultCode) decides
  // it, so the two cannot drift: a terminal code stands until the operator changes
  // settings, which re-syncs and re-decides. gpu-encoder-start-failed is NOT
  // terminal - a start fault can be transient - so it keeps the ladder.
  // ONE codec/transport resolution for every call site in this adapter.
  //
  // THE ENHANCED-RTMP CLAUSE IS AN RTMP/FLV CONCEPT AND MUST NOT REACH SRT
  // (2026-09-20). This class serves BOTH egress protocols through one
  // OutputDestinationSettings struct, so the unguarded matrix refused an SRT
  // operator who picked H.265 without ticking "Enhanced RTMP" — and told them to
  // enable an RTMP setting for a stream that never touches FLV. SRT carries
  // MPEG-TS, which takes H.265 natively: no opt-in exists, none is needed, and
  // the advisory warning that rides runtimeDetail_ would be a false claim too.
  // So for SRT the matrix is bypassed entirely and the requested codec stands.
  //
  // AV1 IS STILL REFUSED ON SRT: that defect is in our own hardware encoder
  // (near-empty access units, issue #565 -> `codec-not-deliverable`) and is
  // protocol-independent, so it is decided in startFfmpegProcess, not here.
  // The no-hardware-encoder / gpu-encoder-start-failed clauses are likewise
  // untouched for every protocol.
  RtmpCompatibilityResult resolveCompatibility() const {
    if (protocol_.isSrt) {
      RtmpCompatibilityResult result;
      result.requestedVideoCodec = normalizeRtmpVideoCodec(configuredVideoCodec_);
      result.videoCodec = result.requestedVideoCodec;
      result.container = protocol_.container;  // mpegts, not flv
      return result;
    }
    return resolveRtmpCompatibility(configuredVideoCodec_, configuredAllowEnhancedRtmp_);
  }

  bool refuseStreamStart(const StreamStartAdmission& verdict, const std::string& requestedCodec) {
    sender_.status = "warning";
    sender_.warning = verdict.message;
    sender_.destinationHealth = "warning";
    sender_.lastResultCode = verdict.resultCode;
    sender_.lastError = verdict.message;
    appendSendProof(nullptr, verdict.resultCode);
    startRefusedInadmissible_ = isTerminalResultCode(verdict.resultCode);
    ::corevideo::core::nativeLogf("[gpu-encode] stream start REFUSED code=%s codec=%s reason=%s :: %s\n",
                                 verdict.resultCode.c_str(), requestedCodec.c_str(),
                                 gpuEncodePathReason_.c_str(), verdict.message.c_str());
    return false;
  }

  bool startFfmpegProcess(int width, int height, const std::string& videoInputPixelFormat) {
    startRefusedInadmissible_ = false;
    if (ffmpegExecutable_.empty()) {
      sender_.status = "warning";
      sender_.warning = "FFmpeg executable was not found.";
      sender_.destinationHealth = "warning";
      sender_.lastResultCode = "ffmpeg-missing";
      sender_.lastError = sender_.warning;
      return false;
    }
    const auto compatibility = resolveCompatibility();
    selectedFfmpegVideoEncoder_ = selectFfmpegVideoEncoder(
        ffmpegExecutable_, compatibility.videoCodec, configuredEncoderMode_);
    // REFUSE, NEVER DOWNGRADE (2026-09-20). The operator's codec either goes out
    // on a path that can carry it or the stream does not start, with a stable
    // code the shell renders. H.264 on the CPU fallback is admitted exactly as
    // before, so every machine that streams today keeps streaming.
    //
    // The hardware / start-failure half of the admission needs the start attempt
    // and therefore sits BELOW startGpuEncoderIfChosen.
    {
      StreamStartAdmissionInputs admission;
      admission.requestedCodec = compatibility.requestedVideoCodec;
      admission.compatibilityRefused = compatibility.refused;
      admission.compatibilityReason = compatibility.reason;
      // GPU-direct AV1 is not deliverable on this path (2026-09-20). See
      // StreamStartAdmission.h and docs/superpowers/specs/2026-09-20-gpu-direct-hevc-av1-stream-design.md.
      // Revisit when the AV1 near-empty-payload defect is understood; the gate
      // (scripts/validate-gpu-encode.mjs --codec av1) is what flips this back.
      admission.codecKnownNotDeliverable = (compatibility.requestedVideoCodec == "av1");
      admission.notDeliverableDetail = "near-empty access units, ~18 kbit/s against the configured bitrate";
      // Only clauses 1 and 2 can fire here: the hardware / start-failure half of
      // the admission needs the start attempt and sits below
      // startGpuEncoderIfChosen. Refusing a not-deliverable codec HERE is
      // deliberate — it never binds the MFT for a codec we already know cannot
      // deliver, so no `path=gpu-direct` is ever logged for it.
      const auto verdict = admitStreamStart(admission);
      if (verdict.refused) {
        return refuseStreamStart(verdict, compatibility.requestedVideoCodec);
      }
    }
    // Start the GPU encoder BEFORE FFmpeg so start() is the real capability gate:
    // on failure it clears useGpuDirect_, and the admission below either lets the
    // raw path carry H.264 or refuses the start outright.
    if (startGpuEncoderIfChosen(width, height)) {
      // The proof must name what is actually encoded: on this path FFmpeg is a
      // pure -c:v copy muxer, so a reader can no longer see h264_nvenc on an
      // HEVC stream.
      selectedFfmpegVideoEncoder_ = "gpu-direct-" + gpuEncodeSentCodec_;
    }
    if (!useGpuDirect_) {
      ::corevideo::core::nativeLogf("[gpu-encode] path=cpu-fallback reason=%s\n",
                                   gpuEncodePathReason_.c_str());
    }
    {
      StreamStartAdmissionInputs admission;
      admission.requestedCodec = compatibility.requestedVideoCodec;
      admission.codecHasHardwareEncoder = codecHasSupportedHardwareEncoder(compatibility.requestedVideoCodec);
      // The GPU path WAS chosen when the encoder was started and start() failed:
      // startGpuEncoderIfChosen already cleared useGpuDirect_. Without this the
      // "HEVC/AV1 off the GPU path" clause fires first and an encoder fault is
      // misreported as "this machine has no hardware encoder", discarding the
      // detail the encoder recorded.
      admission.gpuPathChosen = useGpuDirect_ || gpuEncoderStartFailed_;
      admission.gpuPathReason = gpuEncodePathReason_.c_str();
      admission.gpuEncoderStartFailed = gpuEncoderStartFailed_;
      // Never render empty parentheses at the operator.
      admission.gpuEncoderFailureDetail =
          gpuEncoderFailureDetail_.empty() ? std::string("unknown failure") : gpuEncoderFailureDetail_;
      const auto verdict = admitStreamStart(admission);
      if (verdict.refused) {
        stopGpuEncoder();
        useGpuDirect_ = false;
        return refuseStreamStart(verdict, compatibility.requestedVideoCodec);
      }
    }
#if defined(_WIN32)
    SECURITY_ATTRIBUTES securityAttributes{};
    securityAttributes.nLength = sizeof(securityAttributes);
    securityAttributes.bInheritHandle = TRUE;

    HANDLE childStdinRead = nullptr;
    HANDLE childStdinWrite = nullptr;
    // A compressed access unit can exceed the default anonymous-pipe buffer.
    // Let FFmpeg consume complete bursts without repeatedly blocking the MFT
    // event thread. Bounded at 1 MiB; the raw fallback retains its existing size.
    const DWORD videoPipeBufferBytes = useGpuDirect_ ? (1u << 20) : 0;
    if (!CreatePipe(&childStdinRead, &childStdinWrite, &securityAttributes, videoPipeBufferBytes)) {
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

    // Adopt before anything else can go wrong: from here on the OS owns the
    // cleanup even if we are killed without running a single destructor.
    adoptOutputChild(processInfo.hProcess, protocol_.destination.c_str());

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
        ::corevideo::core::nativeLogf("[rtmp] ffmpeg %s\n", argString.c_str());
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

  // -------- GPU-direct encode path (#521 slice 1) --------

  bool gpuForcedOffByEnv() const {
    const char* v = std::getenv("COREVIDEO_GPU_ENCODE");
    return v && std::string(v) == "0";
  }

  // A hardware encoder session for THIS codec is (probably) available. Never
  // REFUSE on a pending/unknown probe (the TESTER RULE) — encoder->start() is the
  // real gate. `codec` is the RtmpCompatibility spelling; the probe key is canonical.
  bool gpuEncoderProbeAllows(const std::string& codec, int width, int height) const {
    const auto cap = EncoderCapacityCache::instance().lookup(
        EncoderProbeKey{canonicalProbeCodec(codec), width, height, (std::max)(1, configuredFps_)});
    if (!cap.probed) return true;
    return cap.hardwareAvailable && cap.hardwareSessionCeiling > 0;
  }

  // The start-time path decision, using the exact pure policy the unit tests pin.
  GpuEncodePath resolveGpuEncodePath(const ProgramFrame& frame, int width, int height) {
    GpuEncodePathInputs in;
    in.platformSupported = static_cast<bool>(gpuEncoderFactory_);
    in.forcedOffByEnv = gpuForcedOffByEnv();
    const auto compatibility = resolveCompatibility();
    const std::string sentCodec = canonicalProbeCodec(compatibility.videoCodec);  // "h264"|"hevc"|"av1"
    const bool probeAllows = gpuEncoderProbeAllows(compatibility.videoCodec, width, height);
    in.hardwareEncoderAvailable = in.platformSupported && probeAllows;
    in.sessionAvailable = probeAllows;
    const bool codecHasGpuEncoder =
        codecHasSupportedHardwareEncoder(normalizeVideoCodec(compatibility.videoCodec)) && probeAllows;
    const bool frameHasEncoderTexture = !frame.encoderSharedTexture.sharedHandleHex.empty();
    const char* reason = "cpu-fallback";
    const auto path = chooseStreamEncodePath(in, sentCodec, codecHasGpuEncoder, frameHasEncoderTexture, &reason);
    gpuEncodePathReason_ = reason;
    gpuEncodeSentCodec_ = sentCodec;
    return path;
  }

  // Called from startFfmpegProcess BEFORE FFmpeg is launched: if we mean to run
  // GPU-direct, actually start the encoder here so start() is the real capability
  // gate. On failure we downgrade to raw so FFmpeg is never launched in bitstream
  // mode with no encoder feeding it. The sink writes the compressed bitstream to
  // FFmpeg's stdin (populated right after this returns).
  bool startGpuEncoderIfChosen(int width, int height) {
    gpuEncoderStartFailed_ = false;
    gpuEncoderFailureDetail_.clear();
    if (!useGpuDirect_) return false;
    gpuEncoder_ = gpuEncoderFactory_ ? gpuEncoderFactory_() : nullptr;
    if (!gpuEncoder_) {
      useGpuDirect_ = false;
      gpuEncodePathReason_ = "encoder-create-failed";
      return false;
    }
    GpuVideoEncoderConfig cfg;
    cfg.width = width;
    cfg.height = height;
    cfg.fps = (std::max)(1, configuredFps_);
    cfg.bitrateKbps = static_cast<int>((std::max)(500.0, sender_.bitrateMbps * 1000.0));
    cfg.keyframeIntervalSeconds = configuredKeyframeIntervalSeconds_;
    cfg.rateControl = configuredRateControl_;
    cfg.h264Profile = configuredH264Profile_.empty() ? "high" : configuredH264Profile_;
    cfg.codec = gpuEncodeSentCodec_;
#if defined(_WIN32)
    bitstreamFailure_.reset();
    bitstreamWriterStop_.store(false);
#endif
    const bool ok = gpuEncoder_->start(cfg, [this](const GpuEncodedChunk& chunk) {
#if defined(_WIN32)
      enqueueBitstream(chunk);
#else
      writeBitstreamToFfmpeg(chunk.data, chunk.size);
#endif
    });
    if (!ok) {
      gpuEncoderFailureDetail_ = gpuEncoder_->lastFailure();
      gpuEncoder_.reset();
      useGpuDirect_ = false;
      gpuEncodePathReason_ = "encoder-start-failed";
      gpuEncoderStartFailed_ = true;
      return false;  // startFfmpegProcess refuses, or logs the cpu-fallback line
    }
#if defined(_WIN32)
    bitstreamWriterExited_.store(false);
    bitstreamWriterThread_ = std::thread([this] { bitstreamWriterLoop(); });
#endif
    ::corevideo::core::nativeLogf("[gpu-encode] path=gpu-direct codec=%s %dx%d@%d bitrate=%.1fMbps\n",
                                 cfg.codec.c_str(), width, height, cfg.fps, sender_.bitrateMbps);
    return true;
  }

  void stopGpuEncoder() {
    if (gpuEncoder_) {
      gpuEncoder_->stop();  // joins the encoder thread before we close FFmpeg's stdin
      gpuEncoder_.reset();
    }
#if defined(_WIN32)
    bitstreamWriterStop_.store(true);
    bitstreamQueueCv_.notify_all();
    if (bitstreamWriterThread_.joinable()) {
      // Repeat cancellation to cover a write entering after the first cancel.
      while (!bitstreamWriterExited_.load()) {
        CancelSynchronousIo(static_cast<HANDLE>(bitstreamWriterThread_.native_handle()));
        Sleep(1);
      }
      bitstreamWriterThread_.join();
    }
    std::lock_guard<std::mutex> lock(bitstreamQueueMutex_);
    bitstreamQueue_.clear();
    bitstreamQueuedBytes_ = 0;
    republishQueueTelemetryLocked();
    writeTraceNext_ = 0;
    lastWriteTraceLog_ = {};
#endif
  }

#if defined(_WIN32)
  // Caller must hold bitstreamQueueMutex_.
  void republishQueueTelemetryLocked() {
    bitstreamQueuedChunks_.store(static_cast<std::int64_t>(bitstreamQueue_.size()),
                                 std::memory_order_relaxed);
    bitstreamHeadEnqueuedNs_.store(
        bitstreamQueue_.empty()
            ? 0
            : bitstreamQueue_.front().enqueuedAt.time_since_epoch().count(),
        std::memory_order_relaxed);
    bool keyframe = false;
    for (const auto& q : bitstreamQueue_) {
      if (q.metadata.keyframe) { keyframe = true; break; }
    }
    bitstreamQueueHasKeyframe_.store(keyframe, std::memory_order_relaxed);
  }

  // #597 Lever B. Lever A stops the queue growing; it never clears what is
  // already in it, so a stream can stabilise a full second behind and stay
  // there. Discarding every chunk AHEAD of the next queued keyframe recovers
  // that latency as a clean skip. Dropping an arbitrary chunk instead would
  // corrupt every frame until the next keyframe. Our HEVC/AV1 encoders run with
  // B-frames disabled (the low-latency work), so there are no non-reference
  // frames to drop cheaply and the GOP tail is the only safe unit.
  //
  // The DECISION (how many chunks are safe to drop) is the pure
  // corevideo::core::discardableGopTailLength() in StreamBackpressurePolicy.h,
  // unit-tested there with no seam of any kind - not even a fake queue. This
  // is just the locked mutation: pop that many, keep the byte accounting
  // exact, and republish telemetry so bitstreamBufferedMs() cannot keep
  // reporting the age of a chunk that no longer exists.
  //
  // Task 8b split this in two. The mutation is the ...Locked form, because the
  // OVERFLOW path in enqueueBitstream() now runs the same discard while it
  // ALREADY holds bitstreamQueueMutex_ - std::mutex is not recursive, so
  // calling the locking form from there would self-deadlock the encoder's
  // event loop, which is strictly worse than the defect being fixed. The
  // locking wrapper stays for observeStreamBackpressure(), which calls it from
  // the sync thread holding nothing.
  std::size_t discardBacklogToNextKeyframe() {
    std::lock_guard<std::mutex> lock(bitstreamQueueMutex_);
    return discardBacklogToNextKeyframeLocked();
  }

  // Caller must hold bitstreamQueueMutex_. `cutPoint` is the Task 8b fix-round-1
  // split BY SITE: Lever B's ordinary discard cuts to the NEAREST keyframe (the
  // smallest clean skip that recovers latency), the overflow path cuts to the
  // LAST (the most room a safe cut can free, because its only alternative is
  // failing the sender and rebuilding the encoder). The choice is a parameter
  // on the pure policy, never a second copy of the logic.
  std::size_t discardBacklogToNextKeyframeLocked(
      corevideo::core::GopCutPoint cutPoint = corevideo::core::GopCutPoint::Nearest,
      bool arrivalIsKeyframe = false) {
    const std::size_t dropped = corevideo::core::discardableBacklogForArrival(
        bitstreamQueue_, [](const QueuedBitstream& chunk) { return chunk.metadata.keyframe; },
        arrivalIsKeyframe, cutPoint);
    for (std::size_t i = 0; i < dropped; ++i) {
      bitstreamQueuedBytes_ -= bitstreamQueue_.front().bytes.size();
      bitstreamQueue_.pop_front();
    }
    if (dropped > 0) republishQueueTelemetryLocked();
    return dropped;
  }

  // #597 THE POLICY AND ITS PER-RUN COUNTERS ARE ONE UNIT OF LIFETIME.
  //
  // FINAL-REVIEW FINDING 2, and it is a CROSS-TASK defect no per-task review
  // could see: only the operator-stop path (`!wantsRtmp`) used to reconstruct
  // `backpressure_`. `reopen()` - both `recover()` and
  // `restartForSupervisor()` - stopped FFmpeg and cleared the floor but left
  // the policy holding whatever divisor it had reached before the fault. So
  // after ANY fault and reopen the destination republished, say, divisor 4
  // against an EMPTY queue, and `kRecoverAfterHealthyTicks = 600` meant about
  // ten seconds per step and roughly thirty seconds of healthy streaming
  // before 60 fps returned - while the compositor visibly snapped 4 -> 1 (the
  // node goes absent, so applyEncoderExportDivisor correctly pops to 1) -> 4
  // across the outage, with `lastReason` still reading
  // `buffered-above-threshold` from before the fault. Task 7 owned the restart
  // and Tasks 4/6 owned the policy lifetime, which is exactly the seam it fell
  // through.
  //
  // A reopen IS a new run by every other measure this adapter keeps -
  // `framesSent`, `bytesSent` and `startedAtMs` are all reset there - so the
  // per-run backpressure counters reset with them and the run identity
  // advances, which is what lets a consumer tell a reset from a decrease.
  void resetBackpressureForNewRun() {
    backpressure_ = corevideo::core::StreamBackpressurePolicy{};
    backpressureDiscardedChunks_ = 0;
    overflowDiscardedChunks_.store(0, std::memory_order_relaxed);
#if defined(_WIN32)
    pipeWriteStartedSteadyMs_.store(0, std::memory_order_relaxed);
    pipeWriteMaxMs_.store(0, std::memory_order_relaxed);
    pipeSlowWriteCount_.store(0, std::memory_order_relaxed);
#endif
    ++backpressureRunId_;
    sender_.backpressure.reset();
  }

  // #597 Lever A. Observe this destination's own outgoing queue once per sync
  // and publish the input divisor the policy asks for. MediaCore reads
  // OutputSender::backpressure and drives ICompositor::setEncoderExportDivisor
  // with the MAX across active GPU-direct senders - one encoder texture feeds
  // them all, so the divisor cannot be per destination here (Lever B, the
  // GOP-tail discard, is).
  //
  // A NEGATIVE bufferedMs is "no evidence" and the policy ignores the tick
  // entirely: the bitstream queue only exists on the GPU-direct path, and the
  // raw CPU path already drops stale frames and is deliberately untouched by
  // this lever. Absent backpressure is therefore NOT "healthy" - it is
  // "nothing to observe here".
  //
  // Task 6 fix round 1, finding 1: gated on `activeUseGpuDirect_` (the path
  // baked into the RUNNING FFmpeg args), never `useGpuDirect_` (only the
  // DESIRED path for the next/running process). `ensureFfmpegProcess` sets
  // `useGpuDirect_` BEFORE it knows whether FFmpeg will actually start
  // (`RtmpOutputSenderAdapter.cpp` near `startFfmpegProcess`), and only sets
  // `activeUseGpuDirect_` once that start genuinely SUCCEEDED. Gating on the
  // desired field published a pristine `divisor 1 / bufferedMs 0 /
  // queuedChunks 0` node - the textbook healthy reading - for a destination
  // whose FFmpeg failed to start and is retrying with no encoder, no
  // process and no queue at all.
  void observeStreamBackpressure(double elapsedMs) {
    const std::int64_t injected = backpressureBufferedMsForTest_.load(std::memory_order_relaxed);
    corevideo::core::StreamBackpressureObservation observation;
    if (injected >= 0) {
      observation.bufferedMs = injected;
      observation.keyframeInQueue = backpressureKeyframeForTest_.load(std::memory_order_relaxed);
    } else if (activeUseGpuDirect_) {
      observation.bufferedMs = bitstreamBufferedMs();
      observation.keyframeInQueue = bitstreamQueueHasKeyframe_.load(std::memory_order_relaxed);
    } else {
      sender_.backpressure.reset();
      return;
    }
    const auto decision = backpressure_.observe(observation);
    if (decision.transition != corevideo::core::StreamBackpressureTransition::None) {
      ::corevideo::core::nativeLogf(
          "[stream-backpressure] %s divisor=%d buffered=%lldms\n",
          corevideo::core::StreamBackpressurePolicy::transitionName(decision.transition),
          backpressure_.divisor(), static_cast<long long>(observation.bufferedMs));
    }
    // Task 6 fix round 1, finding 2: `publishedBufferedMs` starts as the
    // pre-discard observation that DROVE this tick's decision, but on a tick
    // where Lever B actually fires it is re-read AFTER the discard - the same
    // discipline the adjacent Task 5 test already demands of
    // bitstreamBufferedMs() itself ("the buffered measure must republish
    // against the NEW head"). Without this, the published node paired a
    // pre-discard bufferedMs with a post-discard queuedChunks on exactly the
    // tick worth inspecting, describing two different instants at once.
    std::int64_t publishedBufferedMs = observation.bufferedMs;
    if (decision.discardBacklog) {
      const auto dropped = discardBacklogToNextKeyframe();
      if (dropped > 0) {
        backpressureDiscardedChunks_ = (std::min)(
            backpressureDiscardedChunks_ + static_cast<std::int64_t>(dropped),
            kBackpressureDiscardedChunksCeiling);
        ::corevideo::core::nativeLogf(
            "[stream-backpressure] discard dropped=%zu divisor=%d buffered=%lldms\n",
            dropped, backpressure_.divisor(), static_cast<long long>(observation.bufferedMs));
        publishedBufferedMs = bitstreamBufferedMs();
      }
    }
    OutputBackpressureState state;
    state.divisor = backpressure_.divisor();
    state.level = backpressure_.level();
    state.bufferedMs = publishedBufferedMs;
    state.queuedChunks = static_cast<std::int64_t>(
        bitstreamQueuedChunks_.load(std::memory_order_relaxed));
#if defined(_WIN32)
    const auto writeStartedMs = pipeWriteStartedSteadyMs_.load(std::memory_order_relaxed);
    if (writeStartedMs > 0) {
      const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count();
      state.inFlightWriteMs = (std::max)(std::int64_t{0}, nowMs - writeStartedMs);
    }
    state.maxWriteMs = pipeWriteMaxMs_.load(std::memory_order_relaxed);
    state.slowWriteCount = pipeSlowWriteCount_.load(std::memory_order_relaxed);
#endif
    state.enteredCount = backpressure_.enteredCount();
    // Per-stream-run, reset alongside backpressure_ on the !wantsRtmp stop
    // path above (see backpressureDiscardedChunks_).
    // Task 8b: the overflow path discards too, from the ENCODER thread, so its
    // count lives in its own atomic and is summed here rather than mutating
    // backpressureDiscardedChunks_ (owned by this sync thread) off-thread.
    // Both are per stream run and both are reset together on the stop path.
    //
    // THE TWO COUNTERS DELIBERATELY COUNT DIFFERENT POPULATIONS (fix round 2,
    // item 3), and a reader must not diagnose that as a bug. `discardedChunks`
    // counts chunks dropped by BOTH discard sites - Lever B's policy-driven
    // discard AND the queue's last-resort overflow discard. `discardEvents`
    // counts only the POLICY's decisions (StreamBackpressurePolicy::observe
    // returning discardBacklog), because that is what the policy's own
    // hysteresis and cooldown are keyed on. THE REASON IS THE DATA RACE, NOT
    // THE COOLDOWN (fix round 3, item 3 - the earlier justification here was
    // wrong in detail and the reviewer was right to check it): discardEvents_
    // is a pure OUTPUT and feeds no decision, so counting overflow discards
    // into it would not corrupt any policy behaviour. What it WOULD do is
    // mutate the non-atomic StreamBackpressurePolicy object from the encoder
    // thread while the sync thread reads and writes it - an actual data race
    // on an object with no lock of its own. That is why the two counters stay
    // separate.
    // So `discardedChunks > 0` with `discardEvents == 0` is the SIGNATURE of a
    // run that hit the hard cap without the policy ever asking for a discard -
    // exactly the burst-gate case - and is a real reading, not a counter bug.
    // The `overflow-discard` log line is what names that site explicitly.
    state.discardedChunks = (std::min)(
        backpressureDiscardedChunks_ + overflowDiscardedChunks_.load(std::memory_order_relaxed),
        kBackpressureDiscardedChunksCeiling);
    state.discardEvents = backpressure_.discardEvents();
    // Fix round 3, item 3: say it ON THE WIRE, not only in a C++ comment. A
    // reader of the snapshot meets `discardedChunks` above zero next to
    // `discardEvents` at zero and has nothing telling them that is a real
    // reading rather than a broken counter.
    state.discardCounterNote =
        "discardedChunks counts BOTH discard sites (the policy's Lever B discard and the "
        "queue's last-resort overflow discard); discardEvents counts only the policy's own "
        "decisions. discardedChunks > 0 with discardEvents == 0 means the hard cap was hit "
        "without the policy ever asking for a discard - a real reading, not a counter bug.";
    state.lastReason = backpressure_.lastReason();
    state.lastTransitionBufferedMs = backpressure_.lastTransitionBufferedMs();
    state.runId = backpressureRunId_;
    state.observedAtMs = elapsedMs;
    sender_.backpressure = state;
  }

  // 0 when the queue is empty. Lock-free: reads the head's enqueue time and ages
  // it against now, so the number keeps rising while the writer is blocked.
  [[nodiscard]] std::int64_t bitstreamBufferedMs() const {
    const auto head = bitstreamHeadEnqueuedNs_.load(std::memory_order_relaxed);
    if (head == 0) return 0;
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto ageNs = now - head;
    return ageNs <= 0 ? 0 : static_cast<std::int64_t>(ageNs / 1'000'000);
  }

  // Never block the MFT event loop on network I/O, and never grow latency
  // without bound: the queue is capped at 60 chunks / 2 MiB.
  //
  // #597 TASK 8B - WHAT THIS PATH USED TO DO, AND WHY IT CHANGED. An overrun
  // used to FAIL the sender on purpose so its supervisor would restart it. A
  // supervisor restart rebuilds the encoder, so backpressure's own last-resort
  // bound was itself a caller of the restart storm this sub-project exists to
  // remove - and the spec is explicit that a destination fault must never
  // rebuild the encoder. The acceptance gate measured it: the queue crossed 22
  // chunks to the 60-chunk cap inside one second, the overflow path failed the
  // sender, and the encoder was rebuilt (1 of 3 congested 240 s runs).
  //
  // So on overflow we spend LEVER B first - the GOP-tail discard, at the one
  // moment it matters most - and accept the arriving chunk if that freed room.
  // Failing is reserved for the case where the discard frees NOTHING - which,
  // since fix round 1 cut this site to the LAST keyframe and let the ARRIVING
  // chunk be a cut point too, means "no keyframe anywhere, queued or
  // arriving", the one case where
  // dropping anything would corrupt the stream until the next keyframe. (Cutting
  // to the FIRST keyframe left a second, avoidable failure: a full queue whose
  // only keyframe sat at the HEAD freed nothing and failed the sender anyway,
  // and with the GOP about the size of the queue that position is a rolling
  // coin flip.) The cap is NOT raised: an unbounded queue is unbounded latency,
  // the defect this all exists to remove.
  //
  // This also removes by construction the divisor > 1 race the gate named:
  // Lever B's ordinary trigger cannot fire until Lever A has stepped (~0.5 s),
  // and this path does not consult the divisor at all.
  void enqueueBitstream(const GpuEncodedChunk& chunk) {
    if (!chunk.data || !chunk.size || bitstreamWriterStop_.load() || bitstreamFailure_.failed()) return;
    {
      std::lock_guard<std::mutex> lock(bitstreamQueueMutex_);
      constexpr size_t maxBytes = kMaxQueuedBytes;
      // A single chunk larger than the whole byte budget can never fit, no
      // matter what is dropped - discarding a GOP tail for it would cost a
      // visible skip and still fail.
      if (chunk.size > maxBytes) {
        bitstreamFailure_.record(BitstreamFailure::QueueOverflow);
        // Final-review finding 6: composed in modules/BitstreamQueueOverflow.h
        // so this branch carries kQueueOverflowSenderFailedMarker too. It used
        // to be the only one of the three sender-failing overflow branches with
        // a hand-written string and no grepped marker at all.
        ::corevideo::core::nativeLogf(
            "%s", corevideo::modules::describeQueueOverflowOversizedChunk(chunk.size, maxBytes)
                      .c_str());
        return;
      }
      const auto full = [&] {
        return bitstreamQueuedBytes_ > maxBytes - chunk.size ||
               bitstreamQueue_.size() >= kMaxQueuedChunks;
      };
      if (full()) {
        // Already holding bitstreamQueueMutex_ - hence the ...Locked form (see
        // the deadlock note on discardBacklogToNextKeyframe).
        //
        // A KEYFRAME ARRIVAL DROPS THE WHOLE BACKLOG, even when cutting to a
        // queued keyframe would have freed enough room (fix round 2, item 5).
        // That is deliberate and it is a LATENCY-FIRST choice: this is the last
        // resort, the queue here is by definition a full second or more behind
        // real time, and a freshly arrived IDR lets the stream resume at NOW
        // instead of at a cut point that is itself already stale. The cost is
        // picture that would have survived a narrower cut; the benefit is that
        // the destination stops being behind rather than merely less behind.
        const std::size_t dropped = discardBacklogToNextKeyframeLocked(
            corevideo::core::GopCutPoint::Last, /*arrivalIsKeyframe=*/chunk.keyframe);
        if (dropped > 0) {
          overflowDiscardedChunks_.fetch_add(static_cast<std::int64_t>(dropped),
                                             std::memory_order_relaxed);
          // Rate-limited: at the cap this can fire once per frame, and a log
          // line per frame is how a diagnostic becomes the incident.
          const auto now = std::chrono::steady_clock::now();
          if (lastOverflowDiscardLog_ == std::chrono::steady_clock::time_point{} ||
              now - lastOverflowDiscardLog_ >= std::chrono::seconds(1)) {
            lastOverflowDiscardLog_ = now;
            // Composed in modules/BitstreamQueueOverflow.h, which the gate
            // greps and BitstreamQueueOverflowTest.cpp pins - see the header.
            ::corevideo::core::nativeLogf(
                "%s", corevideo::modules::describeQueueOverflowDiscard(
                          dropped, bitstreamQueue_.size(), bitstreamQueuedBytes_).c_str());
          }
        }
        if (full()) {
          bitstreamFailure_.record(BitstreamFailure::QueueOverflow);
          // Composed in modules/BitstreamQueueOverflow.h. Fix round 2 reworded
          // this message in the same commit that tightened the gate's grep for
          // it, which disarmed that assertion silently; both branches are now
          // pinned by BitstreamQueueOverflowTest.cpp - see the header.
          ::corevideo::core::nativeLogf(
              "%s", corevideo::modules::describeQueueOverflowFailure(
                        dropped, bitstreamQueuedBytes_, bitstreamQueue_.size(), chunk.size,
                        kMaxQueuedChunks, kMaxQueuedBytes).c_str());
          return;
        }
      }
      bitstreamQueue_.push_back({std::vector<uint8_t>(chunk.data, chunk.data + chunk.size), chunk,
                                 std::chrono::steady_clock::now()});
      bitstreamQueuedBytes_ += chunk.size;
      republishQueueTelemetryLocked();
    }
    bitstreamQueueCv_.notify_one();
  }

  void bitstreamWriterLoop() {
    HevcTransportStream transport;
    std::vector<uint8_t> wire;
    while (!bitstreamWriterStop_.load() && !bitstreamFailure_.failed()) {
      QueuedBitstream packet;
      {
        std::unique_lock<std::mutex> lock(bitstreamQueueMutex_);
        bitstreamQueueCv_.wait(lock, [this] { return bitstreamWriterStop_.load() || !bitstreamQueue_.empty(); });
        if (bitstreamWriterStop_.load()) break;
        packet = std::move(bitstreamQueue_.front());
        bitstreamQueue_.pop_front();
        bitstreamQueuedBytes_ -= packet.bytes.size();
        republishQueueTelemetryLocked();
      }
      packet.metadata.data = packet.bytes.data();
      if (gpuEncodeSentCodec_ == "hevc") {
        if (!transport.packetize(packet.metadata, wire)) {
          bitstreamFailure_.record(BitstreamFailure::InvalidTiming);
          ::corevideo::core::nativeLogf("[gpu-encode] invalid encoder packet timing pts=%lld dts=%lld valid=%d\n",
              static_cast<long long>(packet.metadata.pts100ns), static_cast<long long>(packet.metadata.dts100ns),
              packet.metadata.timingValid ? 1 : 0);
          break;
        }
        writeBitstreamToFfmpeg(wire.data(), wire.size(), &packet.metadata, packet.enqueuedAt);
      } else {
        writeBitstreamToFfmpeg(packet.bytes.data(), packet.bytes.size(), &packet.metadata, packet.enqueuedAt);
      }
    }
    bitstreamWriterExited_.store(true);
  }
#endif

  // Write one encoded chunk to FFmpeg's stdin. Windows runs this on its
  // transport worker; stopGpuEncoder joins both workers before stdin is closed.
  void writeBitstreamToFfmpeg(const uint8_t* data, size_t size,
                             const GpuEncodedChunk* metadata = nullptr,
                             std::chrono::steady_clock::time_point enqueuedAt = {}) {
    if (!data || size == 0) return;
#if defined(_WIN32)
    if (!ffmpegRunning_ || !ffmpegStdin_) {
      if (!firstBitstreamLogged_) {
        firstBitstreamLogged_ = true;
        ::corevideo::core::nativeLogf("[gpu-encode] bitstream dropped: ffmpegRunning=%d stdin=%p size=%zu\n",
                                     ffmpegRunning_ ? 1 : 0, (void*)ffmpegStdin_, size);
      }
      return;
    }
    const auto packetWriteStarted = std::chrono::steady_clock::now();
    // Writer-owned, fixed-size history. Retain timing without logging on the
    // encoder or render thread and without retaining encoded media.
    const size_t traceSlot = writeTraceNext_++ % writeTrace_.size();
    auto& trace = writeTrace_[traceSlot];
    trace = {metadata ? metadata->frameNumber : -1,
             metadata ? metadata->pts100ns : 0,
             metadata ? metadata->dts100ns : 0,
             metadata ? metadata->keyframe : false,
             metadata ? metadata->timingValid : false,
             size,
             enqueuedAt == std::chrono::steady_clock::time_point{} ? -1
                 : std::chrono::duration_cast<std::chrono::milliseconds>(packetWriteStarted - enqueuedAt).count(),
             std::chrono::duration_cast<std::chrono::milliseconds>(packetWriteStarted.time_since_epoch()).count(),
             -1};
    size_t remaining = size;
    while (remaining > 0) {
      if (bitstreamWriterStop_.load()) return;
      DWORD written = 0;
      const DWORD chunk = static_cast<DWORD>((std::min)(remaining, static_cast<size_t>(1) << 20));
      const auto writeStarted = std::chrono::steady_clock::now();
      pipeWriteStartedSteadyMs_.store(
          std::chrono::duration_cast<std::chrono::milliseconds>(writeStarted.time_since_epoch()).count(),
          std::memory_order_relaxed);
      const BOOL writeSucceeded = WriteFile(ffmpegStdin_, data, chunk, &written, nullptr);
      const DWORD writeError = writeSucceeded ? ERROR_SUCCESS : GetLastError();
      pipeWriteStartedSteadyMs_.store(0, std::memory_order_relaxed);
      const auto writeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - writeStarted).count();
      if (writeMs > pipeWriteMaxMs_.load(std::memory_order_relaxed)) {
        pipeWriteMaxMs_.store(writeMs, std::memory_order_relaxed);
      }
      if (writeMs >= 100) {
        pipeSlowWriteCount_.fetch_add(1, std::memory_order_relaxed);
        ::corevideo::core::nativeLogf(
            "[ffmpeg-pipe] video slow-write elapsedMs=%lld requested=%lu written=%lu error=%lu stopping=%d\n",
            static_cast<long long>(writeMs), static_cast<unsigned long>(chunk),
            static_cast<unsigned long>(written), static_cast<unsigned long>(writeError),
            bitstreamWriterStop_.load() ? 1 : 0);
        const auto now = std::chrono::steady_clock::now();
        if (lastWriteTraceLog_ == std::chrono::steady_clock::time_point{} ||
            now - lastWriteTraceLog_ >= std::chrono::seconds(1)) {
          lastWriteTraceLog_ = now;
          for (size_t back = 0; back < (std::min)(writeTraceNext_, writeTrace_.size()); ++back) {
            const auto& prior = writeTrace_[(writeTraceNext_ - 1 - back) % writeTrace_.size()];
            ::corevideo::core::nativeLogf(
                "[ffmpeg-pipe-history] back=%zu frame=%lld pts100ns=%lld dts100ns=%lld valid=%d key=%d bytes=%zu queueAgeMs=%lld writeStartSteadyMs=%lld writeMs=%lld\n",
                back, static_cast<long long>(prior.frameNumber), static_cast<long long>(prior.pts100ns),
                static_cast<long long>(prior.dts100ns), prior.timingValid ? 1 : 0,
                prior.keyframe ? 1 : 0, prior.bytes, static_cast<long long>(prior.queueAgeMs),
                static_cast<long long>(prior.writeStartSteadyMs),
                static_cast<long long>(back == 0 ? writeMs : prior.writeMs));
          }
        }
      }
      if (!writeSucceeded || written == 0) {
        if (bitstreamWriterStop_.load()) return;
        bitstreamFailure_.record(BitstreamFailure::PipeWrite);
        ::corevideo::core::nativeLogf("[gpu-encode] bitstream WriteFile failed err=%lu (encoder->ffmpeg pipe broke)\n",
                                     static_cast<unsigned long>(writeError));
        return;
      }
      data += written;
      remaining -= written;
    }
    trace.writeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - packetWriteStarted).count();
    if (!firstBitstreamLogged_) {
      firstBitstreamLogged_ = true;
      ::corevideo::core::nativeLogf("[gpu-encode] first bitstream write to ffmpeg size=%zu\n", size);
    }
#else
    if (!ffmpegRunning_ || ffmpegStdinFd_ < 0) return;
    size_t remaining = size;
    while (remaining > 0) {
      const ssize_t written = ::write(ffmpegStdinFd_, data, remaining);
      if (written <= 0) {
        if (written < 0 && errno == EINTR) continue;
        return;
      }
      data += static_cast<size_t>(written);
      remaining -= static_cast<size_t>(written);
    }
#endif
    hasWrittenVideo_ = true;
  }

  // GPU-direct per-frame path: hand the compositor's dedicated encoder texture to
  // the hardware encoder. A missing texture this tick is not a failure (hold);
  // an unhealthy encoder returns false so the failure path restarts the sender.
  bool submitFrameToGpuEncoder(const ProgramFrame& frame) {
#if defined(_WIN32)
    if (bitstreamFailure_.failed()) return false;
#endif
    if (!gpuEncoder_) return false;
    if (!gpuEncoder_->healthy()) return false;
    if (frame.encoderSharedTexture.sharedHandleHex.empty()) return true;
    GpuVideoEncoderFrame f;
    f.publishedFrameNumber = frame.encoderSharedTexture.publishedFrameNumber;
    f.sharedHandleHex = frame.encoderSharedTexture.sharedHandleHex;
    f.width = frame.encoderSharedTexture.width;
    f.height = frame.encoderSharedTexture.height;
    f.frameNumber = frame.frameNumber;
    return gpuEncoder_->submit(f);
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
      sender_.lastError = describeFfmpegSenderFailure(
          "FFmpeg process exited before accepting program frames. Exit code " + std::to_string(exitCode) + ".",
          ffmpegStderrTail());
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
        std::string exitDetail = "FFmpeg process exited before accepting program frames.";
        if (WIFEXITED(status)) {
          exitDetail = "FFmpeg process exited before accepting program frames. Exit code " + std::to_string(WEXITSTATUS(status)) + ".";
        } else if (WIFSIGNALED(status)) {
          exitDetail = "FFmpeg process exited before accepting program frames. Signal " + std::to_string(WTERMSIG(status)) + ".";
        }
        sender_.lastError = describeFfmpegSenderFailure(exitDetail, ffmpegStderrTail());
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
        ::corevideo::core::nativeLogf("[rtmp] first video write: declared %dx%d %s -> expected %zu bytes, "
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
        const auto writeStarted = std::chrono::steady_clock::now();
        const BOOL writeSucceeded = WriteFile(audioPipeServer_, data, chunk, &written, nullptr);
        const DWORD writeError = writeSucceeded ? ERROR_SUCCESS : GetLastError();
        const auto writeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - writeStarted).count();
        if (writeMs >= 100) {
          ::corevideo::core::nativeLogf(
              "[ffmpeg-pipe] audio slow-write elapsedMs=%lld requested=%lu written=%lu error=%lu\n",
              static_cast<long long>(writeMs), static_cast<unsigned long>(chunk),
              static_cast<unsigned long>(written), static_cast<unsigned long>(writeError));
        }
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
          const DWORD error = writeError;
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
    // Stop the GPU encoder FIRST: its sink writes FFmpeg's stdin on the encoder
    // thread, so joining it here guarantees no write-after-close on the pipe.
    stopGpuEncoder();
    activeUseGpuDirect_ = false;
    hasWrittenVideo_ = false;
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
               ",\"ffmpegVideoEncoder\":" + jsonString(selectedFfmpegVideoEncoder_.empty() ? ffmpegVideoEncoderFor(resolveCompatibility().videoCodec, configuredEncoderMode_) : selectedFfmpegVideoEncoder_) +
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
              ",\"ffmpegVideoEncoder\":" + jsonString(selectedFfmpegVideoEncoder_.empty() ? ffmpegVideoEncoderFor(resolveCompatibility().videoCodec, configuredEncoderMode_) : selectedFfmpegVideoEncoder_);
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
  RuntimeProbe runtimeProbe_;
  std::string runtimeDetail_;
  bool runtimeAvailable_ = false;
  std::string configuredEndpoint_;
  std::string configuredStreamKey_;
  std::string configuredPassphrase_;
  std::string configuredFfmpegBinDirectory_;
  std::string configuredVideoCodec_ = "h264";
  std::string configuredEncoderMode_ = "auto";
  double configuredKeyframeIntervalSeconds_ = 2.0;
  std::string configuredRateControl_ = "cbr";
  std::string configuredH264Profile_ = "high";
  int configuredBFrames_ = 2;
  bool configuredAllowEnhancedRtmp_ = false;
  std::string ffmpegExecutable_;
  // NOTE: where this is empty (FFmpeg never started for this attempt) the proof
  // reports the encoder that WOULD be used, and it must go through
  // resolveRtmpCompatibility first. Reporting the raw configured codec said
  // "av1_nvenc" on a run whose own runtimeDetail said "falling back to H.264" -
  // two fields contradicting each other in the same proof line, sending a reader
  // diagnosing a stream failure to chase an encoder that is never selected
  // (observed live 2026-09-12).
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
  // #597 the restart floor: the house 5/10/20/40/60 s ladder, keyed on the same
  // monotonic `elapsedMs` clock the video pacer already uses.
  TransportRestartFloor restartFloor_;
  // One send-proof line per served backoff WINDOW, not one per program frame.
  bool backoffProofWritten_ = false;
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
  // GPU-direct encode (#521 slice 1). When chosen at process start, the compositor's
  // dedicated keyed-mutex encoder texture is fed to the MF hardware MFT for the sent
  // codec (h264 / hevc / av1) and the
  // ~6 Mbps bitstream is written to FFmpeg (demoted to a -c:v copy muxer). The raw
  // path is the fallback for every non-capable machine and COREVIDEO_GPU_ENCODE=0.
  // The factory is injectable for tests; default is the real MF encoder.
  std::function<std::unique_ptr<GpuVideoEncoder>()> gpuEncoderFactory_;
  std::unique_ptr<GpuVideoEncoder> gpuEncoder_;
  // #597 Lever A. Private to this destination; MediaCore takes the MAX across
  // senders because one encoder texture feeds every GPU-direct destination.
  corevideo::core::StreamBackpressurePolicy backpressure_;
  // #597 Lever B. Per-destination (unlike the divisor above): each sender
  // discards its OWN bitstream backlog, never reaching across senders.
  // Saturates rather than wraps, same discipline as every other counter in
  // this feature (StreamBackpressurePolicy's, encoderExportShedFrames_'s).
  std::int64_t backpressureDiscardedChunks_ = 0;
  // Written by the encoder thread under bitstreamQueueMutex_, read by the sync
  // thread; atomic so the two never race. Summed into the published
  // discardedChunks - see observeStreamBackpressure().
  std::atomic<std::int64_t> overflowDiscardedChunks_{0};
  static constexpr std::int64_t kBackpressureDiscardedChunksCeiling = INT64_C(1) << 62;
  // #597 Task 6 fix round 1, finding 11: bumped every time backpressure_ is
  // RECONSTRUCTED (the `!wantsRtmp` stop path), i.e. every time the per-run
  // counters reset to zero. Published as OutputBackpressureState::runId so a
  // consumer who polls across a stop/restart without observing the node's
  // momentary absence can still tell a genuine reset from a counter going
  // backwards.
  std::int64_t backpressureRunId_ = 0;
  // TEST-ONLY override of the queue measurement (see
  // IOutputSender::setBackpressureObservationForTest). Negative = not set.
  std::atomic<std::int64_t> backpressureBufferedMsForTest_{-1};
  std::atomic<bool> backpressureKeyframeForTest_{false};
  bool useGpuDirect_ = false;        // desired path for the next/running process
  bool activeUseGpuDirect_ = false;  // path baked into the RUNNING FFmpeg args
  std::string gpuEncodePathReason_ = "cpu-fallback";
  // The codec actually sent, canonical ("h264"|"hevc"|"av1"): it rides the
  // encoder config, the muxer demuxer choice and the send proof, so what is
  // encoded, what is muxed and what is reported cannot disagree.
  std::string gpuEncodeSentCodec_ = "h264";
  bool gpuEncoderStartFailed_ = false;
  // Set when the last start was refused with a TERMINAL (configuration) code,
  // so ensureFfmpegProcess does not put it on the FFmpeg retry ladder.
  bool startRefusedInadmissible_ = false;
  std::string gpuEncoderFailureDetail_;
  bool firstBitstreamLogged_ = false;
#if defined(_WIN32)
  struct PipeWriteTrace {
    int64_t frameNumber = -1;
    int64_t pts100ns = 0;
    int64_t dts100ns = 0;
    bool keyframe = false;
    bool timingValid = false;
    size_t bytes = 0;
    int64_t queueAgeMs = -1;
    int64_t writeStartSteadyMs = -1;
    int64_t writeMs = -1;
  };
  // Only the bitstream writer touches these fields. A stall prints at most one
  // 16-entry history per second; normal frames perform no I/O for this trace.
  std::array<PipeWriteTrace, 16> writeTrace_{};
  size_t writeTraceNext_ = 0;
  std::chrono::steady_clock::time_point lastWriteTraceLog_{};
  std::atomic<std::int64_t> pipeWriteStartedSteadyMs_{0};
  std::atomic<std::int64_t> pipeWriteMaxMs_{0};
  std::atomic<std::int64_t> pipeSlowWriteCount_{0};
  std::mutex bitstreamQueueMutex_;
  std::condition_variable bitstreamQueueCv_;
  struct QueuedBitstream {
    std::vector<uint8_t> bytes;
    GpuEncodedChunk metadata;
    // Wall-clock moment this chunk entered the queue. The backpressure signal is
    // the AGE of the head of the queue, which is the only measure that stays
    // honest while the frame rate is being changed underneath it.
    std::chrono::steady_clock::time_point enqueuedAt{};
  };
  std::deque<QueuedBitstream> bitstreamQueue_;
  size_t bitstreamQueuedBytes_ = 0;
  std::thread bitstreamWriterThread_;
  std::atomic<bool> bitstreamWriterStop_{true};
  BitstreamFailureState bitstreamFailure_;
  std::atomic<bool> bitstreamWriterExited_{true};
  // Read by the submit path with NO lock (see bitstreamBufferedMs). Written only
  // under bitstreamQueueMutex_, where the queue is already being mutated.
  std::atomic<std::int64_t> bitstreamHeadEnqueuedNs_{0};  // 0 = empty
  std::atomic<std::int64_t> bitstreamQueuedChunks_{0};
  std::atomic<bool> bitstreamQueueHasKeyframe_{false};
  // #597 fix round 3, item 5. The queue's two bounds, named once so the code
  // that enforces them and the message that describes them cannot disagree.
  // (These are MEMORY bounds standing in for a latency bound - filed as #607,
  // deliberately not changed here.)
  static constexpr std::size_t kMaxQueuedChunks = 60;
  static constexpr std::size_t kMaxQueuedBytes = 2u << 20;
  // Guarded by bitstreamQueueMutex_ (written only inside enqueueBitstream).
  std::chrono::steady_clock::time_point lastOverflowDiscardLog_{};
#endif

  OutputSender sender_;
  RtmpVideoFramePacer videoFramePacer_;
  std::atomic<bool> hasWrittenVideo_{false};
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
