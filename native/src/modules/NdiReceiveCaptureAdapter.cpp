#include "modules/Interfaces.h"
#include "modules/NdiReceiveFramePolicy.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_NDI_INGEST && defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdlib>
#include <filesystem>
#endif

namespace corevideo::modules {
namespace {

#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_NDI_INGEST && defined(_WIN32)

// The same v5 ABI used by NdiOutputSenderAdapter. Resolve symbols at runtime so
// the stub and machines without the NDI runtime never acquire an SDK dependency.
struct NdiSource { const char* name; const char* url; };
struct NdiFindCreate { bool showLocal; const char* groups; const char* extraIps; };
struct NdiRecvCreate {
  NdiSource source;
  int colorFormat;
  int bandwidth;
  bool allowFields;
  const char* receiverName;
};
struct NdiVideo {
  int width, height, fourcc, fpsNumerator, fpsDenominator;
  float aspect;
  int frameFormat;
  int64_t timecode;
  const uint8_t* data;
  int stride;
  const char* metadata;
  int64_t timestamp;
};
struct NdiAudio {
  int sampleRate, channels, samples;
  int64_t timecode;
  const float* data;
  int channelStrideBytes;
  const char* metadata;
  int64_t timestamp;
};
static_assert(sizeof(NdiSource) == 16 && sizeof(NdiRecvCreate) == 40);
static_assert(sizeof(NdiVideo) == 72 && sizeof(NdiAudio) == 56);

struct NdiReceiveApi {
  bool (*initialize)() = nullptr;
  void (*destroy)() = nullptr;
  void* (*findCreate)(const NdiFindCreate*) = nullptr;
  void (*findDestroy)(void*) = nullptr;
  bool (*findWait)(void*, uint32_t) = nullptr;
  const NdiSource* (*findSources)(void*, uint32_t*) = nullptr;
  void* (*recvCreate)(const NdiRecvCreate*) = nullptr;
  void (*recvDestroy)(void*) = nullptr;
  int (*recvCapture)(void*, NdiVideo*, NdiAudio*, void*, uint32_t) = nullptr;
  void (*freeVideo)(void*, const NdiVideo*) = nullptr;
  void (*freeAudio)(void*, const NdiAudio*) = nullptr;
  bool complete() const {
    return initialize && destroy && findCreate && findDestroy && findWait && findSources && recvCreate &&
           recvDestroy && recvCapture && freeVideo && freeAudio;
  }
};

template <typename T> T symbol(HMODULE library, const char* name) {
  return reinterpret_cast<T>(::GetProcAddress(library, name));
}

HMODULE loadNdiRuntime() {
  std::vector<std::filesystem::path> candidates = {"Processing.NDI.Lib.x64.dll"};
  if (const char* root = std::getenv("PROGRAMFILES")) {
    const std::filesystem::path base(root);
    candidates.push_back(base / "NDI/NDI 6 Tools/Runtime/Processing.NDI.Lib.x64.dll");
    candidates.push_back(base / "NDI/NDI 6 Runtime/v6/Bin/x64/Processing.NDI.Lib.x64.dll");
    candidates.push_back(base / "NDI/NDI 5 Runtime/v5/Bin/x64/Processing.NDI.Lib.x64.dll");
  }
  if (const char* root = std::getenv("NDI_RUNTIME_DIR")) {
    candidates.push_back(std::filesystem::path(root) / "Processing.NDI.Lib.x64.dll");
  }
  for (const auto& candidate : candidates) {
    if (auto* library = ::LoadLibraryW(candidate.wstring().c_str())) return library;
  }
  return nullptr;
}

NdiReceiveApi loadApi(HMODULE library) {
  NdiReceiveApi api;
  api.initialize = symbol<decltype(api.initialize)>(library, "NDIlib_initialize");
  api.destroy = symbol<decltype(api.destroy)>(library, "NDIlib_destroy");
  api.findCreate = symbol<decltype(api.findCreate)>(library, "NDIlib_find_create_v2");
  api.findDestroy = symbol<decltype(api.findDestroy)>(library, "NDIlib_find_destroy");
  api.findWait = symbol<decltype(api.findWait)>(library, "NDIlib_find_wait_for_sources");
  api.findSources = symbol<decltype(api.findSources)>(library, "NDIlib_find_get_current_sources");
  api.recvCreate = symbol<decltype(api.recvCreate)>(library, "NDIlib_recv_create_v3");
  api.recvDestroy = symbol<decltype(api.recvDestroy)>(library, "NDIlib_recv_destroy");
  api.recvCapture = symbol<decltype(api.recvCapture)>(library, "NDIlib_recv_capture_v2");
  api.freeVideo = symbol<decltype(api.freeVideo)>(library, "NDIlib_recv_free_video_v2");
  api.freeAudio = symbol<decltype(api.freeAudio)>(library, "NDIlib_recv_free_audio_v2");
  return api;
}

int64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string deviceIdFor(const std::string& name) {
  uint64_t hash = 14695981039346656037ull;
  for (unsigned char byte : name) {
    hash ^= byte;
    hash *= 1099511628211ull;
  }
  char suffix[17]{};
  std::snprintf(suffix, sizeof(suffix), "%016llx", static_cast<unsigned long long>(hash));
  return std::string("ndi-") + suffix;
}

struct NdiChannel {
  std::string id;
  std::string name;
  std::string url;
  std::atomic<bool> running{true};
  std::thread thread;
  std::mutex mutex;
  std::shared_ptr<const std::vector<uint8_t>> latest;
  std::vector<AudioFrame> audio;
  int width = 0, height = 0, fps = 0;
  int64_t frameId = 0, lastFrameMs = 0, audioSamples = 0, dropped = 0;
  bool failed = false;
  std::string warning;
};

class NdiReceiveCaptureDevice final : public ICaptureDevice {
 public:
  NdiReceiveCaptureDevice(HMODULE library, NdiReceiveApi api, void* finder)
      : library_(library), api_(api), finder_(finder) {
    discovery_ = std::thread([this] { discoverLoop(); });
  }
  ~NdiReceiveCaptureDevice() override {
    discovering_.store(false);
    if (discovery_.joinable()) discovery_.join();
    std::vector<std::shared_ptr<NdiChannel>> channels;
    {
      std::lock_guard lock(mutex_);
      for (auto& [_, channel] : channels_) channels.push_back(channel);
      channels_.clear();
    }
    for (auto& channel : channels) stop(channel);
    api_.findDestroy(finder_);
    api_.destroy();
    ::FreeLibrary(library_);
  }

  std::vector<CaptureDeviceInfo> enumerate() const override {
    std::map<std::string, std::pair<std::string, std::string>> sources;
    std::map<std::string, std::shared_ptr<NdiChannel>> channels;
    std::map<std::string, int> offsets;
    {
      std::lock_guard lock(mutex_);
      sources = sources_;
      channels = channels_;
      offsets = offsets_;
    }
    std::vector<CaptureDeviceInfo> result;
    std::set<std::string> ids;
    for (const auto& [id, _] : sources) ids.insert(id);
    for (const auto& [id, _] : channels) ids.insert(id);
    for (const auto& id : ids) {
      CaptureDeviceInfo info;
      info.id = id;
      info.vendor = "ndi";
      info.kind = "video";
      info.connectionState = "detected";
      info.inputHasEmbeddedAudio = {true};
      if (auto found = sources.find(id); found != sources.end()) {
        info.name = found->second.first;
      } else {
        info.name = channels.at(id)->name;
      }
      info.inputIds = {id};
      info.inputLabels = {info.name};
      info.selectedInputId = id;
      if (auto offset = offsets.find(id); offset != offsets.end())
        info.audioSyncOffsetMs = offset->second;
      if (auto found = channels.find(id); found != channels.end()) {
        auto& channel = *found->second;
        std::lock_guard lock(channel.mutex);
        info.width = channel.width;
        info.height = channel.height;
        info.frameRate = channel.fps;
        info.decodedFrames = channel.frameId;
        info.decodedAudioSamples = channel.audioSamples;
        info.droppedFrames = channel.dropped;
        info.lastFrameAgeMs = channel.lastFrameMs ? std::max<int64_t>(0, nowMs() - channel.lastFrameMs) : 0;
        info.signalPresent = channel.lastFrameMs && *info.lastFrameAgeMs <= 1500;
        info.connectionState = channel.failed ? "failed" : info.signalPresent ? "connected"
            : channel.frameId ? "stalled" : "connecting";
        info.warning = channel.warning;
        if (!info.signalPresent && channel.frameId && info.warning.empty())
          info.warning = "NDI video stopped advancing.";
      }
      result.push_back(std::move(info));
    }
    return result;
  }

  std::vector<CaptureDeviceInfo> selectInput(const std::string&, const std::string&) override {
    return enumerate();
  }
  std::vector<CaptureDeviceInfo> setAudioSyncOffset(const std::string& id, int offsetMs) override {
    {
      std::lock_guard lock(mutex_);
      offsets_[id] = std::clamp(offsetMs, -500, 500);
    }
    return enumerate();
  }
  std::vector<CaptureDeviceInfo> connect(const std::string& id) override {
    {
      std::lock_guard lock(mutex_);
      const auto found = sources_.find(id);
      if (auto existing = channels_.find(id); existing != channels_.end()) {
        bool failed = false;
        {
          std::lock_guard channelLock(existing->second->mutex);
          failed = existing->second->failed;
        }
        if (failed) {
          stop(existing->second);
          channels_.erase(existing);
        }
      }
      if (!channels_.count(id) && found != sources_.end()) {
        auto channel = std::make_shared<NdiChannel>();
        channel->id = id;
        channel->name = found->second.first;
        channel->url = found->second.second;
        channel->thread = std::thread([this, channel] { receiveLoop(channel); });
        channels_[id] = channel;
      }
    }
    return enumerate();
  }
  std::vector<CaptureDeviceInfo> disconnect(const std::string& id) override {
    std::shared_ptr<NdiChannel> channel;
    {
      std::lock_guard lock(mutex_);
      if (auto found = channels_.find(id); found != channels_.end()) {
        channel = found->second;
        channels_.erase(found);
      }
    }
    if (channel) stop(channel);
    return enumerate();
  }
  std::vector<std::string> audioSourceIds() const override {
    std::lock_guard lock(mutex_);
    std::vector<std::string> ids;
    for (const auto& [id, _] : channels_) ids.push_back("capture:" + id);
    return ids;
  }
  void captureVideoTick(int64_t timestampMs) override {
    auto channels = activeChannels();
    std::vector<VideoFrame> frames;
    for (const auto& channel : channels) {
      std::lock_guard lock(channel->mutex);
      if (!channel->latest) continue;
      VideoFrame frame;
      frame.participantId = "capture:" + channel->id;
      frame.width = frame.naturalWidth = frame.pixelWidth = channel->width;
      frame.height = frame.naturalHeight = frame.pixelHeight = channel->height;
      frame.pixelStride = channel->width * 4;
      frame.timestampMs = timestampMs;
      frame.frameId = channel->frameId;
      frame.pixels = channel->latest;
      frames.push_back(std::move(frame));
    }
    replaceVideo(std::move(frames));
  }
  void captureAudioTick(int64_t timestampMs) override {
    auto channels = activeChannels();
    for (const auto& channel : channels) {
      std::vector<AudioFrame> frames;
      {
        std::lock_guard lock(channel->mutex);
        frames.swap(channel->audio);
      }
      for (auto& frame : frames) {
        frame.timestampMs = timestampMs;
        postAudio(std::move(frame));
      }
    }
  }

 private:
  std::vector<std::shared_ptr<NdiChannel>> activeChannels() const {
    std::lock_guard lock(mutex_);
    std::vector<std::shared_ptr<NdiChannel>> out;
    for (const auto& [_, channel] : channels_) out.push_back(channel);
    return out;
  }
  static void stop(const std::shared_ptr<NdiChannel>& channel) {
    channel->running.store(false);
    if (channel->thread.joinable()) channel->thread.join();
  }
  void discoverLoop() {
    while (discovering_.load()) {
      api_.findWait(finder_, 250);
      uint32_t count = 0;
      const NdiSource* found = api_.findSources(finder_, &count);
      std::map<std::string, std::pair<std::string, std::string>> next;
      for (uint32_t i = 0; found && i < count && i < 256; ++i) {
        if (!found[i].name || !found[i].name[0]) continue;
        std::string name(found[i].name);
        next.emplace(deviceIdFor(name),
                     std::make_pair(name, found[i].url ? found[i].url : ""));
      }
      std::lock_guard lock(mutex_);
      sources_ = std::move(next);
    }
  }
  void receiveLoop(const std::shared_ptr<NdiChannel>& channel) {
    NdiSource source{channel->name.c_str(), channel->url.empty() ? nullptr : channel->url.c_str()};
    NdiRecvCreate config{source, 0, 100, false, "CoreVideo Pro input"};
    void* receiver = api_.recvCreate(&config);
    if (!receiver) {
      std::lock_guard lock(channel->mutex);
      channel->failed = true;
      channel->warning = "NDI receiver could not connect to this source.";
      return;
    }
    while (channel->running.load()) {
      NdiVideo video{};
      NdiAudio audio{};
      const int type = api_.recvCapture(receiver, &video, &audio, nullptr, 50);
      if (type == 1) {
        const bool opaque = video.fourcc == 0x58524742;  // BGRX
        if (video.fourcc == 0x41524742 || opaque) {      // BGRA
          auto pixels = copyNdiBgra(video.data, video.width, video.height,
                                    video.stride, opaque);
          std::lock_guard lock(channel->mutex);
          if (pixels) {
            channel->latest = std::move(pixels);
            channel->width = video.width;
            channel->height = video.height;
            channel->fps = video.fpsDenominator > 0
                ? video.fpsNumerator / video.fpsDenominator : 0;
            ++channel->frameId;
            channel->lastFrameMs = nowMs();
            channel->warning.clear();
          } else {
            ++channel->dropped;
            channel->warning = "NDI video frame has invalid geometry or stride.";
          }
        } else {
          std::lock_guard lock(channel->mutex);
          ++channel->dropped;
          channel->warning = "NDI video format is unsupported by the BGRA receiver.";
        }
        api_.freeVideo(receiver, &video);
      } else if (type == 2) {
        auto pcm = interleaveNdiAudio(audio.data, audio.channels,
                                      audio.samples, audio.channelStrideBytes);
        if (!pcm.empty() && audio.sampleRate > 0) {
          AudioFrame frame;
          frame.participantId = "capture:" + channel->id;
          frame.sampleRate = audio.sampleRate;
          frame.channels = 2;
          frame.sampleCount = audio.samples;
          frame.pcm = std::move(pcm);
          std::lock_guard lock(channel->mutex);
          if (channel->audio.size() >= 16) channel->audio.erase(channel->audio.begin());
          channel->audioSamples += audio.samples;
          channel->audio.push_back(std::move(frame));
        }
        api_.freeAudio(receiver, &audio);
      } else if (type == 4) {
        std::lock_guard lock(channel->mutex);
        channel->warning = "NDI receiver reported a stream error.";
      }
    }
    api_.recvDestroy(receiver);
  }

  HMODULE library_ = nullptr;
  NdiReceiveApi api_;
  void* finder_ = nullptr;
  std::atomic<bool> discovering_{true};
  std::thread discovery_;
  mutable std::mutex mutex_;
  std::map<std::string, std::pair<std::string, std::string>> sources_;
  std::map<std::string, std::shared_ptr<NdiChannel>> channels_;
  std::map<std::string, int> offsets_;
};

#endif

}  // namespace

std::unique_ptr<ICaptureDevice> createNdiReceiveCaptureDevice() {
#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_NDI_INGEST && defined(_WIN32)
  HMODULE library = loadNdiRuntime();
  if (!library) return nullptr;
  const auto api = loadApi(library);
  if (!api.complete()) {
    ::FreeLibrary(library);
    return nullptr;
  }
  if (!api.initialize()) {
    ::FreeLibrary(library);
    return nullptr;
  }
  NdiFindCreate config{true, nullptr, nullptr};
  void* finder = api.findCreate(&config);
  if (!finder) {
    api.destroy();
    ::FreeLibrary(library);
    return nullptr;
  }
  return std::make_unique<NdiReceiveCaptureDevice>(library, api, finder);
#else
  return nullptr;
#endif
}

}  // namespace corevideo::modules
