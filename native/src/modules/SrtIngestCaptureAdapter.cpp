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
  // Embedded contribution audio, drained by pollAudioFrames each tick. Interleaved
  // 48k stereo float, guarded by `mutex` above.
  std::vector<float> audioPending;
  int64_t audioSamplesReceived = 0;

#ifdef _WIN32
  std::atomic<HANDLE> process{nullptr};
  std::atomic<HANDLE> readPipe{nullptr};
  std::atomic<HANDLE> audioPipe{nullptr};
#else
  std::atomic<int> pid{-1};
  std::atomic<int> readFd{-1};
  std::atomic<int> audioFd{-1};
  std::string audioFifoPath;
#endif
  std::thread thread;
  std::thread audioThread;

  void setStatus(const std::string& state, const std::string& note) {
    std::lock_guard lock(mutex);
    connectionState = state;
    warning = note;
  }
};

#ifdef _WIN32
// CreateProcess does NOT use a shell, so Windows was never exposed to command
// injection here — but it parses lpCommandLine itself, so an unquoted url
// containing a quote could still inject extra FFMPEG arguments (an attacker-
// chosen output file, say). Quote every argument by the documented argv rules
// and pass lpApplicationName explicitly so the image cannot be re-targeted.
inline std::string quoteWindowsArgument(const std::string& argument) {
  if (!argument.empty() &&
      argument.find_first_of(" \t\n\v\"") == std::string::npos) {
    return argument;
  }
  std::string quoted = "\"";
  for (auto it = argument.begin();; ++it) {
    std::size_t backslashes = 0;
    while (it != argument.end() && *it == '\\') {
      ++it;
      ++backslashes;
    }
    if (it == argument.end()) {
      quoted.append(backslashes * 2, '\\');
      break;
    }
    if (*it == '"') {
      quoted.append(backslashes * 2 + 1, '\\');
      quoted.push_back('"');
    } else {
      quoted.append(backslashes, '\\');
      quoted.push_back(*it);
    }
  }
  quoted.push_back('"');
  return quoted;
}
#endif

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

  // The feed's embedded audio, keyed "capture:<deviceId>" — the SAME id as its
  // video — so it lands in the existing routing, metering and ISO paths with no
  // special-casing, exactly like a paired capture input.
  std::vector<AudioFrame> pollAudioFrames(int64_t timestampMs) override {
    std::vector<std::shared_ptr<ReaderChannel>> channels;
    {
      std::lock_guard lock(mutex_);
      channels.reserve(channels_.size());
      for (auto& [_, channel] : channels_) {
        channels.push_back(channel);
      }
    }

    std::vector<AudioFrame> frames;
    for (auto& channel : channels) {
      std::vector<float> pcm;
      {
        std::lock_guard lock(channel->mutex);
        if (channel->audioPending.empty()) {
          continue;
        }
        pcm.swap(channel->audioPending);
      }
      // Whole stereo pairs only: a trailing half-pair would swap left and right
      // for the rest of the stream.
      const std::size_t frameCount = pcm.size() / 2;
      if (frameCount == 0) {
        continue;
      }
      pcm.resize(frameCount * 2);
      AudioFrame frame;
      frame.participantId = "capture:" + channel->config.deviceId;
      frame.sampleRate = 48000;
      frame.channels = 2;
      frame.timestampMs = timestampMs;
      frame.sampleCount = static_cast<int>(frameCount);
      frame.pcm = std::move(pcm);
      frames.push_back(std::move(frame));
    }
    return frames;
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
      // The transport carries the guest's audio inline; it is decoded and emitted
      // keyed "capture:<deviceId>" by pollAudioFrames.
      device.inputHasEmbeddedAudio = {true};
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
    if (HANDLE pipe = channel.audioPipe.exchange(nullptr); pipe != nullptr) {
      // CancelIoEx first: the audio thread may be parked in ConnectNamedPipe or
      // ReadFile, and closing the handle under it is not enough to unblock.
      ::CancelIoEx(pipe, nullptr);
      ::DisconnectNamedPipe(pipe);
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
    if (const int fd = channel.audioFd.exchange(-1); fd >= 0) {
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

      // Embedded audio rides its own blocking reader: the video loop below must
      // never stall waiting on audio (or vice versa), and the two arrive at
      // completely different rates.
      if (channel->audioThread.joinable()) {
        channel->audioThread.join();  // previous generation, already unblocked
      }
      channel->audioThread = std::thread([channel] { audioLoop(channel); });

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

      killProcess(*channel);  // also cancels/closes the audio pipe, unblocking it
      if (channel->audioThread.joinable()) {
        channel->audioThread.join();
      }
      if (!channel->running.load()) {
        break;
      }
      channel->setStatus(sawFrame ? "connecting" : "connecting",
                         sawFrame ? "SRT publisher disconnected; waiting for it to return."
                                  : "No SRT publisher yet.");
      std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
      backoffMs = std::min(backoffMs * 2, 10000);
    }
    if (channel->audioThread.joinable()) {
      channel->audioThread.join();
    }
    channel->setStatus("detected", "");
  }

  // Drain the decoder's second output: the feed's embedded audio as interleaved
  // 48k stereo float. Blocking by design and on its own thread, so a silent or
  // audio-less contributor never holds up video.
  static void audioLoop(const std::shared_ptr<ReaderChannel> channel) {
#ifdef _WIN32
    HANDLE pipe = channel->audioPipe.load();
    if (pipe == nullptr) {
      return;
    }
    // ERROR_PIPE_CONNECTED means ffmpeg opened it before we got here — success.
    if (::ConnectNamedPipe(pipe, nullptr) == 0 && ::GetLastError() != ERROR_PIPE_CONNECTED) {
      return;
    }
    std::vector<float> buffer(4096);
    while (channel->running.load()) {
      DWORD read = 0;
      if (::ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size() * sizeof(float)), &read,
                     nullptr) == 0 ||
          read == 0) {
        break;  // decoder exited or the pipe was torn down
      }
      appendAudio(*channel, buffer.data(), read / sizeof(float));
    }
#else
    const int fd = channel->audioFd.load();
    if (fd < 0) {
      return;
    }
    std::vector<float> buffer(4096);
    while (channel->running.load()) {
      const auto read = ::read(fd, buffer.data(), buffer.size() * sizeof(float));
      if (read <= 0) {
        break;
      }
      appendAudio(*channel, buffer.data(), static_cast<std::size_t>(read) / sizeof(float));
    }
#endif
  }

  static void appendAudio(ReaderChannel& channel, const float* samples, std::size_t count) {
    if (samples == nullptr || count == 0) {
      return;
    }
    // ~1s of 48k stereo. A consumer that stops draining (recording stopped, core
    // stalled) must not grow this without bound; dropping the OLDEST keeps the
    // feed live rather than replaying stale audio when it resumes.
    constexpr std::size_t kMaxBufferedFloats = 48000 * 2;
    std::lock_guard lock(channel.mutex);
    channel.audioPending.insert(channel.audioPending.end(), samples, samples + count);
    channel.audioSamplesReceived += static_cast<int64_t>(count);
    if (channel.audioPending.size() > kMaxBufferedFloats) {
      const std::size_t excess = channel.audioPending.size() - kMaxBufferedFloats;
      channel.audioPending.erase(channel.audioPending.begin(),
                                 channel.audioPending.begin() + static_cast<long>(excess));
    }
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
#ifdef _WIN32
    // The audio sink must EXIST before ffmpeg opens it, so the named pipe is
    // created here and the server end is what the reader thread accepts on.
    // Windows ffmpeg cannot write to an inherited numeric fd the way POSIX can,
    // so a named pipe is the portable-enough second output on this platform.
    static std::atomic<uint64_t> audioPipeSerial{0};
    const std::string audioSink =
        std::string("\\\\.\\pipe\\corevideo-srt-ingest-audio-") +
        std::to_string(::GetCurrentProcessId()) + "-" +
        std::to_string(audioPipeSerial.fetch_add(1));
    HANDLE audioServer = ::CreateNamedPipeA(
        audioSink.c_str(), PIPE_ACCESS_INBOUND, PIPE_TYPE_BYTE | PIPE_WAIT, 1,
        1 << 20, 1 << 20, 0, nullptr);
    if (audioServer == INVALID_HANDLE_VALUE) {
      audioServer = nullptr;
    }
    const std::vector<std::string> args =
        buildSrtIngestArgv(executable, url, channel.width, channel.height, channel.frameRate,
                           audioServer != nullptr ? audioSink : std::string());
#else
    // POSIX: hand the child an inherited fd and let ffmpeg write to "pipe:3".
    // No FIFO file to create, name, or clean up. NOT verified on hardware from
    // here — the Windows path is the rig-proven one.
    constexpr int kAudioChildFd = 3;
    int audioFds[2] = {-1, -1};
    const bool audioReady = ::pipe(audioFds) == 0;
    const std::vector<std::string> args =
        buildSrtIngestArgv(executable, url, channel.width, channel.height, channel.frameRate,
                           audioReady ? std::string("pipe:") + std::to_string(kAudioChildFd)
                                      : std::string());
#endif
#ifdef _WIN32
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!::CreatePipe(&readPipe, &writePipe, &security, 1 << 20)) {
      if (audioServer != nullptr) {
        ::CloseHandle(audioServer);
      }
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
    std::string commandLine;
    for (const auto& argument : args) {
      if (!commandLine.empty()) {
        commandLine.push_back(' ');
      }
      commandLine += quoteWindowsArgument(argument);
    }
    std::string mutableCommand = commandLine;
    // lpApplicationName pinned: the image cannot be re-targeted by the url.
    const BOOL created = ::CreateProcessA(executable.c_str(), mutableCommand.data(), nullptr,
                                          nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                                          &startup, &process);
    ::CloseHandle(writePipe);  // our copy; the child holds its own
    if (!created) {
      ::CloseHandle(readPipe);
      if (audioServer != nullptr) {
        ::CloseHandle(audioServer);
      }
      return false;
    }
    ::CloseHandle(process.hThread);
    if (HANDLE job = ingestJobObject(); job != nullptr) {
      ::AssignProcessToJobObject(job, process.hProcess);
    }
    channel.process.store(process.hProcess);
    channel.readPipe.store(readPipe);
    channel.audioPipe.store(audioServer);
    return true;
#else
    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0) {
      if (audioReady) {
        ::close(audioFds[0]);
        ::close(audioFds[1]);
      }
      return false;
    }
    const pid_t pid = ::fork();
    if (pid < 0) {
      ::close(fds[0]);
      ::close(fds[1]);
      if (audioReady) {
        ::close(audioFds[0]);
        ::close(audioFds[1]);
      }
      return false;
    }
    if (pid == 0) {
      ::dup2(fds[1], STDOUT_FILENO);
      if (audioReady) {
        // The child writes audio to fd 3; dup2 clears FD_CLOEXEC on the target.
        ::dup2(audioFds[1], kAudioChildFd);
        ::close(audioFds[0]);
        if (audioFds[1] != kAudioChildFd) {
          ::close(audioFds[1]);
        }
      }
      ::close(fds[0]);
      ::close(fds[1]);
      // NO SHELL. argv elements are passed verbatim, so a url containing
      // quotes, semicolons or $(...) is just a (useless) filename to ffmpeg.
      std::vector<char*> argv;
      argv.reserve(args.size() + 1);
      for (const auto& argument : args) {
        argv.push_back(const_cast<char*>(argument.c_str()));
      }
      argv.push_back(nullptr);
      ::execvp(executable.c_str(), argv.data());
      ::_exit(127);
    }
    ::close(fds[1]);
    if (audioReady) {
      ::close(audioFds[1]);  // our copy of the write end; the child holds its own
      channel.audioFd.store(audioFds[0]);
    }
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
