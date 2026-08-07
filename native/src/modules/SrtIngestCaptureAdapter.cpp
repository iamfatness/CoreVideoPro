#include "modules/Interfaces.h"
#include "modules/SrtFfmpegArgs.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// windows.h defines min/max macros that break std::min/std::max below.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace corevideo::modules {
namespace {

// SRT INGEST — remote contribution feeds arriving over SRT.
//
// WHAT THIS REPLACED: the previous implementation opened a libsrt socket and
// then THREW THE PACKETS AWAY — `pumpSource` read into a 1316-byte buffer and
// only incremented counters, with a comment claiming "decode is handled by the
// decoder stage". No decoder stage existed, and pollVideoFrames emitted frames
// carrying width/height and NO PIXELS. Even with libsrt linked and the build
// flags on, an SRT source would have shown nothing.
//
// WHY FFMPEG: receiving SRT is the easy half — the hard half is demuxing MPEG-TS
// and decoding H.264/HEVC, which is a whole media stack. FFmpeg does all of it,
// the staged binary is built with libsrt, and SRT DELIVERY already goes out the
// same way, so ingest and egress share one dependency and one mental model. One
// FFmpeg per source decodes into raw BGRA on stdout; this adapter reads fixed
// size frames and publishes them as ordinary capture pixels.
//
// DECODE ACCEPTS H.264 AND HEVC deliberately, even though this product does not
// ENCODE HEVC (see EncoderPolicy.h). Field encoders commonly send HEVC, and
// refusing to decode it would reject real contribution feeds; decode exposure is
// a far lighter question than distribution.

constexpr int kBytesPerPixel = 4;  // BGRA

#ifdef _WIN32
// NO ORPHANS. A decoder spawned by the core must not outlive it: if the core is
// killed rather than shut down cleanly, its destructors never run and every
// FFmpeg child is left holding a bound SRT port forever (observed exactly that
// while building this). A job object with KILL_ON_JOB_CLOSE makes the OS do the
// cleanup unconditionally — the same "no orphan hosts" rule the browser-source
// host already follows.
HANDLE ingestJobObject() {
  static HANDLE job = [] {
    HANDLE created = ::CreateJobObjectW(nullptr, nullptr);
    if (created != nullptr) {
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
      limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      ::SetInformationJobObject(created, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }
    return created;
  }();
  return job;
}
#endif

std::string resolveFfmpegExecutable() {
#ifdef _WIN32
  constexpr const char* kExeName = "ffmpeg.exe";
#else
  constexpr const char* kExeName = "ffmpeg";
#endif
  // Explicit override first, then the location the app stages beside itself, then
  // PATH. Kept deliberately small: this is the same binary the RTMP/SRT senders
  // already depend on.
  if (const char* dir = std::getenv("COREVIDEO_FFMPEG_DIR"); dir && dir[0] != '\0') {
    std::filesystem::path candidate = std::filesystem::path(dir) / kExeName;
    std::error_code ec;
    if (std::filesystem::is_regular_file(candidate, ec)) {
      return candidate.string();
    }
  }
#ifdef _WIN32
  for (const char* root : {"C:\\ffmpeg\\bin"}) {
    std::filesystem::path candidate = std::filesystem::path(root) / kExeName;
    std::error_code ec;
    if (std::filesystem::is_regular_file(candidate, ec)) {
      return candidate.string();
    }
  }
#endif
  return kExeName;  // rely on PATH
}

// One decoder process per source. Held by shared_ptr so the reader thread keeps
// it alive even if the source is reconfigured out of the map mid-read.
struct ReaderChannel {
  SrtIngestSourceConfig config;
  int width = 1920;
  int height = 1080;
  int frameRate = 60;

  std::atomic_bool running{true};
  std::mutex mutex;                                       // guards the fields below
  std::shared_ptr<const std::vector<uint8_t>> latest;     // tightly packed BGRA
  std::string connectionState = "connecting";
  std::string warning;
  int64_t framesReceived = 0;
  int64_t droppedFrames = 0;

#ifdef _WIN32
  std::atomic<HANDLE> process{nullptr};
  std::atomic<HANDLE> readPipe{nullptr};
#else
  std::atomic<int> pid{-1};
  std::atomic<int> readFd{-1};
#endif
  std::thread thread;

  void setStatus(const std::string& state, const std::string& note) {
    std::lock_guard lock(mutex);
    connectionState = state;
    warning = note;
  }
};

// Build the FFmpeg command that decodes one SRT source into raw BGRA on stdout.
// Output is forced to a fixed size so the reader can consume whole frames without
// negotiating format mid-stream; whatever resolution the contributor sends is
// scaled to the source's configured geometry.
std::string buildIngestCommand(const std::string& executable, const ReaderChannel& channel,
                               const std::string& url) {
  std::ostringstream cmd;
  cmd << "\"" << executable << "\""
      << " -hide_banner -loglevel error"
      // Contribution feeds are live: do not buffer ahead of real time.
      << " -fflags nobuffer -flags low_delay"
      << " -i \"" << url << "\""
      // Video only for now; embedded contribution audio is a follow-up.
      << " -an"
      << " -f rawvideo -pix_fmt bgra"
      << " -s " << channel.width << "x" << channel.height
      << " -r " << channel.frameRate
      << " pipe:1";
  return cmd.str();
}

class SrtIngestCaptureDevice final : public ICaptureDevice {
 public:
  ~SrtIngestCaptureDevice() override { stopAll(); }

  std::vector<CaptureDeviceInfo> enumerate() const override {
    std::lock_guard lock(mutex_);
    return enumerateUnlocked();
  }

  std::vector<CaptureDeviceInfo> selectInput(const std::string& deviceId, const std::string& inputId) override {
    std::lock_guard lock(mutex_);
    if (auto found = channels_.find(deviceId); found != channels_.end() && found->second->config.id == inputId) {
      selectedInputs_[deviceId] = inputId;
    }
    return enumerateUnlocked();
  }

  std::vector<CaptureDeviceInfo> setAudioSyncOffset(const std::string& deviceId, int offsetMs) override {
    std::lock_guard lock(mutex_);
    audioSyncOffsets_[deviceId] = std::max(-500, std::min(500, offsetMs));
    return enumerateUnlocked();
  }

  std::vector<CaptureDeviceInfo> connect(const std::string& deviceId) override {
    std::shared_ptr<ReaderChannel> channel;
    {
      std::lock_guard lock(mutex_);
      if (auto found = channels_.find(deviceId); found != channels_.end()) {
        channel = found->second;
      }
    }
    if (channel) {
      startChannel(channel);
    }
    return enumerate();
  }

  std::vector<CaptureDeviceInfo> configureSrtIngestSources(
      const std::vector<SrtIngestSourceConfig>& configs) override {
    std::map<std::string, std::shared_ptr<ReaderChannel>> next;
    std::vector<std::shared_ptr<ReaderChannel>> retired;
    {
      std::lock_guard lock(mutex_);
      for (const auto& config : configs) {
        if (config.deviceId.empty()) {
          continue;
        }
        auto prior = channels_.find(config.deviceId);
        // Reuse a running channel only when its endpoint is unchanged; anything
        // else has to be torn down and respawned against the new endpoint.
        if (prior != channels_.end() && sameEndpoint(prior->second->config, config)) {
          prior->second->config = config;
          next.emplace(config.deviceId, prior->second);
          channels_.erase(prior);
          continue;
        }
        auto channel = std::make_shared<ReaderChannel>();
        channel->config = config;
        next.emplace(config.deviceId, std::move(channel));
      }
      // Whatever is left in channels_ is no longer configured.
      for (auto& [_, channel] : channels_) {
        retired.push_back(channel);
      }
      channels_ = std::move(next);
    }
    for (auto& channel : retired) {
      stopChannel(channel);
    }
    return enumerate();
  }

  std::vector<VideoFrame> pollVideoFrames(int64_t timestampMs) override {
    std::vector<std::shared_ptr<ReaderChannel>> channels;
    {
      std::lock_guard lock(mutex_);
      channels.reserve(channels_.size());
      for (auto& [_, channel] : channels_) {
        channels.push_back(channel);
      }
    }

    std::vector<VideoFrame> frames;
    frames.reserve(channels.size());
    for (auto& channel : channels) {
      std::shared_ptr<const std::vector<uint8_t>> pixels;
      int64_t frameId = 0;
      {
        std::lock_guard lock(channel->mutex);
        pixels = channel->latest;
        frameId = channel->framesReceived;
      }
      if (!pixels || pixels->empty()) {
        continue;  // nothing decoded yet: no frame rather than an empty one
      }
      VideoFrame frame;
      frame.participantId = "capture:" + channel->config.deviceId;
      frame.width = channel->width;
      frame.height = channel->height;
      frame.naturalWidth = channel->width;
      frame.naturalHeight = channel->height;
      frame.timestampMs = timestampMs;
      frame.pixels = std::move(pixels);
      frame.pixelWidth = channel->width;
      frame.pixelHeight = channel->height;
      frame.pixelStride = channel->width * kBytesPerPixel;
      frame.frameId = frameId;
      frames.push_back(std::move(frame));
    }
    return frames;
  }

 private:
  static bool sameEndpoint(const SrtIngestSourceConfig& a, const SrtIngestSourceConfig& b) {
    return a.host == b.host && a.port == b.port && a.mode == b.mode &&
           a.passphrase == b.passphrase && a.streamId == b.streamId && a.latencyMs == b.latencyMs;
  }

  std::vector<CaptureDeviceInfo> enumerateUnlocked() const {
    std::vector<CaptureDeviceInfo> result;
    result.reserve(channels_.size());
    for (const auto& [deviceId, channel] : channels_) {
      CaptureDeviceInfo device;
      device.id = channel->config.deviceId;
      device.name = channel->config.name + " - " + channel->config.mode + " " +
                    channel->config.host + ":" + std::to_string(channel->config.port);
      device.kind = "video";
      device.vendor = "srt";
      device.inputIds = {channel->config.id};
      device.inputLabels = {channel->config.name};
      device.inputHasEmbeddedAudio = {false};  // ingest audio is a follow-up
      const auto selected = selectedInputs_.find(deviceId);
      device.selectedInputId = selected == selectedInputs_.end() ? channel->config.id : selected->second;
      device.width = channel->width;
      device.height = channel->height;
      device.frameRate = channel->frameRate;
      {
        std::lock_guard lock(channel->mutex);
        device.connectionState = channel->connectionState;
        device.signalPresent = channel->framesReceived > 0;
        device.droppedFrames = channel->droppedFrames;
        device.warning = channel->warning;
      }
      const auto offset = audioSyncOffsets_.find(deviceId);
      device.audioSyncOffsetMs = offset == audioSyncOffsets_.end() ? 0 : offset->second;
      result.push_back(std::move(device));
    }
    return result;
  }

  void startChannel(const std::shared_ptr<ReaderChannel>& channel) {
    if (channel->thread.joinable()) {
      return;  // already running
    }
    channel->running.store(true);
    channel->thread = std::thread([channel] { readerLoop(channel); });
  }

  static void stopChannel(const std::shared_ptr<ReaderChannel>& channel) {
    channel->running.store(false);
    // Killing the decoder is what unblocks the reader's pending read.
    killProcess(*channel);
    if (channel->thread.joinable()) {
      channel->thread.join();
    }
  }

  void stopAll() {
    std::vector<std::shared_ptr<ReaderChannel>> channels;
    {
      std::lock_guard lock(mutex_);
      for (auto& [_, channel] : channels_) {
        channels.push_back(channel);
      }
      channels_.clear();
    }
    for (auto& channel : channels) {
      stopChannel(channel);
    }
  }

  static void killProcess(ReaderChannel& channel) {
#ifdef _WIN32
    if (HANDLE process = channel.process.exchange(nullptr); process != nullptr) {
      ::TerminateProcess(process, 0);
      ::CloseHandle(process);
    }
    if (HANDLE pipe = channel.readPipe.exchange(nullptr); pipe != nullptr) {
      ::CloseHandle(pipe);
    }
#else
    if (const int pid = channel.pid.exchange(-1); pid > 0) {
      ::kill(pid, SIGKILL);
      int status = 0;
      ::waitpid(pid, &status, 0);
    }
    if (const int fd = channel.readFd.exchange(-1); fd >= 0) {
      ::close(fd);
    }
#endif
  }

  // Spawn the decoder and stream whole BGRA frames until it dies or we stop.
  static void readerLoop(const std::shared_ptr<ReaderChannel> channel) {
    const std::string executable = resolveFfmpegExecutable();
    int backoffMs = 500;
    while (channel->running.load()) {
      SrtEndpointConfig endpoint;
      endpoint.host = channel->config.host.empty() ? std::string("0.0.0.0") : channel->config.host;
      endpoint.port = channel->config.port;
      endpoint.mode = channel->config.mode.empty() ? std::string("listener") : channel->config.mode;
      endpoint.latencyMs = channel->config.latencyMs;
      endpoint.passphrase = channel->config.passphrase;
      endpoint.streamId = channel->config.streamId;
      const auto url = buildSrtUrl(endpoint);
      if (!url.valid) {
        // A configuration error will not fix itself by retrying in a tight loop.
        channel->setStatus("failed", url.error);
        std::this_thread::sleep_for(std::chrono::seconds(2));
        continue;
      }

      channel->setStatus("connecting",
                         "Waiting for an SRT publisher on " + endpoint.host + ":" +
                             std::to_string(endpoint.port) + ".");
      if (!spawnDecoder(*channel, executable, url.url)) {
        channel->setStatus("failed", "Could not start the FFmpeg decoder for this SRT source.");
        std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
        backoffMs = std::min(backoffMs * 2, 10000);
        continue;
      }

      const std::size_t frameBytes = static_cast<std::size_t>(channel->width) *
                                     static_cast<std::size_t>(channel->height) * kBytesPerPixel;
      std::vector<uint8_t> buffer(frameBytes);
      bool sawFrame = false;
      while (channel->running.load()) {
        if (!readExactly(*channel, buffer.data(), frameBytes)) {
          break;  // decoder exited or the publisher went away
        }
        auto published = std::make_shared<std::vector<uint8_t>>(buffer);
        {
          std::lock_guard lock(channel->mutex);
          channel->latest = std::move(published);
          ++channel->framesReceived;
          channel->connectionState = "receiving";
          channel->warning.clear();
        }
        sawFrame = true;
        backoffMs = 500;  // a healthy connection resets the backoff
      }

      killProcess(*channel);
      if (!channel->running.load()) {
        break;
      }
      channel->setStatus(sawFrame ? "connecting" : "connecting",
                         sawFrame ? "SRT publisher disconnected; waiting for it to return."
                                  : "No SRT publisher yet.");
      std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
      backoffMs = std::min(backoffMs * 2, 10000);
    }
    channel->setStatus("detected", "");
  }

  // Read exactly `bytes` or fail. A partial read would desynchronise every
  // subsequent frame, turning a healthy stream into permanent garbage.
  static bool readExactly(ReaderChannel& channel, uint8_t* out, std::size_t bytes) {
    std::size_t filled = 0;
    while (filled < bytes) {
#ifdef _WIN32
      HANDLE pipe = channel.readPipe.load();
      if (pipe == nullptr) {
        return false;
      }
      DWORD read = 0;
      if (!::ReadFile(pipe, out + filled, static_cast<DWORD>(bytes - filled), &read, nullptr) || read == 0) {
        return false;
      }
      filled += read;
#else
      const int fd = channel.readFd.load();
      if (fd < 0) {
        return false;
      }
      const auto read = ::read(fd, out + filled, bytes - filled);
      if (read <= 0) {
        return false;
      }
      filled += static_cast<std::size_t>(read);
#endif
    }
    return true;
  }

  static bool spawnDecoder(ReaderChannel& channel, const std::string& executable,
                           const std::string& url) {
    const std::string command = buildIngestCommand(executable, channel, url);
#ifdef _WIN32
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!::CreatePipe(&readPipe, &writePipe, &security, 1 << 20)) {
      return false;
    }
    // The child must not inherit our read end, or the pipe never reports EOF.
    ::SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writePipe;
    startup.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);
    startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    std::string mutableCommand = command;
    const BOOL created = ::CreateProcessA(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE,
                                          CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    ::CloseHandle(writePipe);  // our copy; the child holds its own
    if (!created) {
      ::CloseHandle(readPipe);
      return false;
    }
    ::CloseHandle(process.hThread);
    if (HANDLE job = ingestJobObject(); job != nullptr) {
      ::AssignProcessToJobObject(job, process.hProcess);
    }
    channel.process.store(process.hProcess);
    channel.readPipe.store(readPipe);
    return true;
#else
    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0) {
      return false;
    }
    const pid_t pid = ::fork();
    if (pid < 0) {
      ::close(fds[0]);
      ::close(fds[1]);
      return false;
    }
    if (pid == 0) {
      ::dup2(fds[1], STDOUT_FILENO);
      ::close(fds[0]);
      ::close(fds[1]);
      ::execl("/bin/sh", "sh", "-c", command.c_str(), nullptr);
      ::_exit(127);
    }
    ::close(fds[1]);
    channel.pid.store(pid);
    channel.readFd.store(fds[0]);
    return true;
#endif
  }

  mutable std::mutex mutex_;
  std::map<std::string, std::shared_ptr<ReaderChannel>> channels_;
  std::map<std::string, std::string> selectedInputs_;
  std::map<std::string, int> audioSyncOffsets_;
};

}  // namespace

std::unique_ptr<ICaptureDevice> createSrtIngestCaptureDevice() {
  return std::make_unique<SrtIngestCaptureDevice>();
}

}  // namespace corevideo::modules
