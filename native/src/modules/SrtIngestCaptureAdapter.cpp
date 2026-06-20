#include "modules/Interfaces.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_SRT_INGEST && COREVIDEO_HAS_LIBSRT
#if __has_include(<srt/srt.h>)
#include <srt/srt.h>
#define COREVIDEO_SRT_INGEST_CAN_RECEIVE 1
#elif __has_include(<srt.h>)
#include <srt.h>
#define COREVIDEO_SRT_INGEST_CAN_RECEIVE 1
#else
#define COREVIDEO_SRT_INGEST_CAN_RECEIVE 0
#endif
#else
#define COREVIDEO_SRT_INGEST_CAN_RECEIVE 0
#endif

#if COREVIDEO_SRT_INGEST_CAN_RECEIVE
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif
#endif

namespace corevideo::modules {
namespace {

class SrtIngestCaptureDevice final : public ICaptureDevice {
 public:
  ~SrtIngestCaptureDevice() override { stopReceiver(); }

  std::vector<CaptureDeviceInfo> enumerate() const override {
    std::lock_guard lock(mutex_);
    return enumerateUnlocked();
  }

  std::vector<CaptureDeviceInfo> selectInput(const std::string& deviceId, const std::string& inputId) override {
    std::lock_guard lock(mutex_);
    if (auto* state = findUnlocked(deviceId); state && state->config.id == inputId) {
      state->selectedInputId = inputId;
    }
    return enumerateUnlocked();
  }

  std::vector<CaptureDeviceInfo> setAudioSyncOffset(const std::string& deviceId, int offsetMs) override {
    std::lock_guard lock(mutex_);
    if (auto* state = findUnlocked(deviceId)) {
      state->audioSyncOffsetMs = std::max(-500, std::min(500, offsetMs));
    }
    return enumerateUnlocked();
  }

  std::vector<CaptureDeviceInfo> connect(const std::string& deviceId) override {
    {
      std::lock_guard lock(mutex_);
      if (auto* state = findUnlocked(deviceId)) {
        state->connectionState = "connecting";
        state->warning = COREVIDEO_SRT_INGEST_CAN_RECEIVE
                             ? "SRT receiver armed; waiting for encoded transport packets."
                             : "SRT ingest is running in synthetic mode. Enable COREVIDEO_WITH_SRT_INGEST with libsrt for real transport.";
      }
    }
    startReceiver();
    return enumerate();
  }

  std::vector<CaptureDeviceInfo> configureSrtIngestSources(const std::vector<SrtIngestSourceConfig>& configs) override {
    std::lock_guard lock(mutex_);
    std::map<std::string, SourceState> next;
    for (const auto& config : configs) {
      if (config.deviceId.empty()) {
        continue;
      }
      auto prior = sources_.find(config.deviceId);
      SourceState state = prior == sources_.end() ? SourceState{} : std::move(prior->second);
      state.config = config;
      state.selectedInputId = config.id;
      if (state.connectionState.empty()) {
        state.connectionState = "detected";
      }
      next.emplace(config.deviceId, std::move(state));
    }
    sources_ = std::move(next);
    return enumerateUnlocked();
  }

  std::vector<VideoFrame> pollVideoFrames(int64_t timestampMs) override {
    std::lock_guard lock(mutex_);
    std::vector<VideoFrame> frames;
    for (auto& [deviceId, state] : sources_) {
      if (state.connectionState != "connected" && state.connectionState != "receiving" && state.connectionState != "connecting") {
        continue;
      }
      state.connectionState = COREVIDEO_SRT_INGEST_CAN_RECEIVE && state.bytesReceived > 0 ? "receiving" : "connected";
      state.signalPresent = true;
      ++state.frameId;
      frames.push_back(VideoFrame{
          "capture:" + deviceId,
          state.width,
          state.height,
          timestampMs,
          {},
          0,
          0,
          0,
          state.frameId,
      });
    }
    return frames;
  }

 private:
  struct SourceState {
    SrtIngestSourceConfig config;
    std::string selectedInputId;
    std::string connectionState = "detected";
    bool signalPresent = false;
    int64_t droppedFrames = 0;
    int audioSyncOffsetMs = 0;
    std::string warning;
    int64_t frameId = 0;
    int64_t bytesReceived = 0;
    int64_t packetsReceived = 0;
    int width = 1920;
    int height = 1080;
    int frameRate = 60;
#if COREVIDEO_SRT_INGEST_CAN_RECEIVE
    SRTSOCKET listenSocket = SRT_INVALID_SOCK;
    SRTSOCKET socket = SRT_INVALID_SOCK;
#endif
  };

  SourceState* findUnlocked(const std::string& deviceId) {
    auto found = sources_.find(deviceId);
    return found == sources_.end() ? nullptr : &found->second;
  }

  std::vector<CaptureDeviceInfo> enumerateUnlocked() const {
    std::vector<CaptureDeviceInfo> result;
    result.reserve(sources_.size());
    for (const auto& [_, state] : sources_) {
      result.push_back(toDevice(state));
    }
    return result;
  }

  static CaptureDeviceInfo toDevice(const SourceState& state) {
    CaptureDeviceInfo device;
    device.id = state.config.deviceId;
    device.name = state.config.name + " - " + state.config.mode + " " + state.config.host + ":" + std::to_string(state.config.port);
    device.kind = "video";
    device.vendor = "srt";
    device.inputIds = {state.config.id};
    device.inputLabels = {state.config.name};
    device.inputHasEmbeddedAudio = {true};
    device.selectedInputId = state.selectedInputId.empty() ? state.config.id : state.selectedInputId;
    device.width = state.width;
    device.height = state.height;
    device.frameRate = state.frameRate;
    device.connectionState = state.connectionState;
    device.signalPresent = state.signalPresent;
    device.droppedFrames = state.droppedFrames;
    device.audioSyncOffsetMs = state.audioSyncOffsetMs;
    device.warning = state.warning;
    return device;
  }

  void startReceiver() {
#if COREVIDEO_SRT_INGEST_CAN_RECEIVE
    bool expected = false;
    if (!receiverRunning_.compare_exchange_strong(expected, true)) {
      return;
    }
    receiverThread_ = std::thread([this] { receiverLoop(); });
#endif
  }

  void stopReceiver() {
#if COREVIDEO_SRT_INGEST_CAN_RECEIVE
    receiverRunning_.store(false);
    if (receiverThread_.joinable()) {
      receiverThread_.join();
    }
    std::lock_guard lock(mutex_);
    for (auto& [_, state] : sources_) {
      closeSocket(state.socket);
      closeSocket(state.listenSocket);
    }
#endif
  }

#if COREVIDEO_SRT_INGEST_CAN_RECEIVE
  void receiverLoop() {
    srt_startup();
    while (receiverRunning_.load()) {
      {
        std::lock_guard lock(mutex_);
        for (auto& [_, state] : sources_) {
          if (state.connectionState == "connecting" || state.connectionState == "connected" || state.connectionState == "receiving") {
            pumpSource(state);
          }
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    srt_cleanup();
  }

  static void closeSocket(SRTSOCKET& socket) {
    if (socket != SRT_INVALID_SOCK) {
      srt_close(socket);
      socket = SRT_INVALID_SOCK;
    }
  }

  static bool fillAddress(const SourceState& state, sockaddr_in& address) {
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(std::max(1, std::min(65535, state.config.port))));
    const std::string host = state.config.host.empty() ? "0.0.0.0" : state.config.host;
    return inet_pton(AF_INET, host.c_str(), &address.sin_addr) == 1;
  }

  static void configureSocket(SRTSOCKET socket, const SourceState& state) {
    const int no = 0;
    const int latencyMs = std::max(20, state.config.latencyMs);
    (void)srt_setsockopt(socket, 0, SRTO_RCVSYN, &no, sizeof(no));
    (void)srt_setsockopt(socket, 0, SRTO_SNDSYN, &no, sizeof(no));
    (void)srt_setsockopt(socket, 0, SRTO_LATENCY, &latencyMs, sizeof(latencyMs));
    if (!state.config.streamId.empty()) {
      (void)srt_setsockopt(socket, 0, SRTO_STREAMID, state.config.streamId.c_str(), static_cast<int>(state.config.streamId.size()));
    }
    if (!state.config.passphrase.empty()) {
      constexpr int keyLength = 16;
      (void)srt_setsockopt(socket, 0, SRTO_PASSPHRASE, state.config.passphrase.c_str(), static_cast<int>(state.config.passphrase.size()));
      (void)srt_setsockopt(socket, 0, SRTO_PBKEYLEN, &keyLength, sizeof(keyLength));
    }
  }

  static void openListener(SourceState& state) {
    if (state.listenSocket != SRT_INVALID_SOCK || state.socket != SRT_INVALID_SOCK) {
      return;
    }
    sockaddr_in address;
    if (!fillAddress(state, address)) {
      state.connectionState = "error";
      state.warning = "SRT listener host must be an IPv4 address for the native alpha adapter.";
      return;
    }
    state.listenSocket = srt_create_socket();
    if (state.listenSocket == SRT_INVALID_SOCK) {
      state.connectionState = "error";
      state.warning = "libsrt could not create listener socket.";
      return;
    }
    configureSocket(state.listenSocket, state);
    if (srt_bind(state.listenSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SRT_ERROR ||
        srt_listen(state.listenSocket, 1) == SRT_ERROR) {
      state.connectionState = "error";
      state.warning = "libsrt listener failed on " + state.config.host + ":" + std::to_string(state.config.port) + ".";
      closeSocket(state.listenSocket);
      return;
    }
    state.connectionState = "connected";
    state.warning = "SRT listener ready on " + state.config.host + ":" + std::to_string(state.config.port) + ".";
  }

  static void openCaller(SourceState& state) {
    if (state.socket != SRT_INVALID_SOCK) {
      return;
    }
    sockaddr_in address;
    if (!fillAddress(state, address)) {
      state.connectionState = "error";
      state.warning = "SRT caller host must be an IPv4 address for the native alpha adapter.";
      return;
    }
    state.socket = srt_create_socket();
    if (state.socket == SRT_INVALID_SOCK) {
      state.connectionState = "error";
      state.warning = "libsrt could not create caller socket.";
      return;
    }
    configureSocket(state.socket, state);
    (void)srt_connect(state.socket, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    state.connectionState = "connected";
    state.warning = "SRT caller connecting to " + state.config.host + ":" + std::to_string(state.config.port) + ".";
  }

  static void acceptListener(SourceState& state) {
    if (state.socket != SRT_INVALID_SOCK || state.listenSocket == SRT_INVALID_SOCK) {
      return;
    }
    sockaddr_in remoteAddress;
    int remoteLength = sizeof(remoteAddress);
    const SRTSOCKET accepted = srt_accept(state.listenSocket, reinterpret_cast<sockaddr*>(&remoteAddress), &remoteLength);
    if (accepted != SRT_INVALID_SOCK) {
      state.socket = accepted;
      configureSocket(state.socket, state);
      state.connectionState = "receiving";
      state.warning = "SRT listener accepted an ingest publisher.";
    }
  }

  static void pumpSource(SourceState& state) {
    if (state.config.mode == "caller") {
      openCaller(state);
    } else {
      openListener(state);
      acceptListener(state);
    }
    if (state.socket == SRT_INVALID_SOCK) {
      return;
    }
    char packet[1316];
    const int received = srt_recvmsg(state.socket, packet, static_cast<int>(sizeof(packet)));
    if (received > 0) {
      state.connectionState = "receiving";
      state.signalPresent = true;
      state.bytesReceived += received;
      ++state.packetsReceived;
      state.warning = "SRT transport receiving encoded packets. Decode is handled by the decoder stage.";
    }
  }
#endif

  mutable std::mutex mutex_;
  std::map<std::string, SourceState> sources_;
  std::atomic_bool receiverRunning_{false};
  std::thread receiverThread_;
};

}  // namespace

std::unique_ptr<ICaptureDevice> createSrtIngestCaptureDevice() {
  return std::make_unique<SrtIngestCaptureDevice>();
}

}  // namespace corevideo::modules
