#include "modules/Interfaces.h"
#include "modules/StillMediaFrameCache.h"
#include "modules/MediaPlaybackTimeline.h"
#include "modules/OwnedMediaFrameSource.h"

#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_MF_ENCODER

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
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
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

 private:
  void reset() {
    if (value_) {
      value_->Release();
      value_ = nullptr;
    }
  }
  T* value_ = nullptr;
};

// One outstanding request and one retained result per reader. The callback
// owns no adapter pointer, so a retired generation cannot touch its successor.
class MediaReadCallback final : public IMFSourceReaderCallback {
 public:
  explicit MediaReadCallback(std::function<void()> wake) : wake_(std::move(wake)) {}
  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFSourceReaderCallback)) {
      *out = static_cast<IMFSourceReaderCallback*>(this); AddRef(); return S_OK;
    }
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
  STDMETHODIMP_(ULONG) Release() override { const auto count = --references_; if (!count) delete this; return count; }
  STDMETHODIMP OnReadSample(HRESULT status, DWORD, DWORD flags, LONGLONG pts, IMFSample* sample) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (retired_) return S_OK;
      pending_ = false; ready_ = true; status_ = status; flags_ = flags; pts_ = pts;
      if (sample) sample->AddRef();
      *sample_.put() = sample;
    }
    if (wake_) wake_(); // No callback mutex held across the worker notification.
    return S_OK;
  }
  STDMETHODIMP OnFlush(DWORD) override { return S_OK; }
  STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }
  void cancel() { std::lock_guard<std::mutex> lock(mutex_); retired_ = true; sample_ = {}; }
  HRESULT take(IMFSourceReader* reader, DWORD stream, DWORD* flags, LONGLONG* pts, IMFSample** sample) {
    HRESULT deliveredStatus = S_FALSE;
    bool delivered = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (retired_) return MF_E_SHUTDOWN;
      if (ready_) {
        ready_ = false; *flags = flags_; *pts = pts_;
        if (sample_) { sample_->AddRef(); *sample = sample_.get(); sample_ = {}; }
        delivered = true; deliveredStatus = status_;
        if (FAILED(status_) || (flags_ & MF_SOURCE_READERF_ENDOFSTREAM)) return status_;
      } else if (pending_) return S_FALSE;
      pending_ = true;
    }
    // Prime the next async request before the caller converts/copies this
    // sample. Waiting for another worker iteration serializes decode with
    // polling sleep and can reduce a60fps source to~54fps under load.
    const auto status = reader->ReadSample(stream, 0, nullptr, nullptr, nullptr, nullptr);
    if (FAILED(status)) {
      std::lock_guard<std::mutex> lock(mutex_);
      pending_ = false;
      if (delivered && !ready_) { ready_ = true; status_ = status; flags_ = 0; sample_ = {}; }
    }
    return delivered ? deliveredStatus : (FAILED(status) ? status : S_FALSE);
  }

 private:
  std::function<void()> wake_;
  std::atomic<ULONG> references_{1};
  std::mutex mutex_;
  bool ready_ = false, pending_ = false, retired_ = false;
  HRESULT status_ = S_OK;
  DWORD flags_ = 0;
  LONGLONG pts_ = 0;
  ComPtrLite<IMFSample> sample_;
};

std::wstring widenUtf8(const std::string& value) {
  if (value.empty()) {
    return {};
  }
  const int count = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
  if (count <= 0) {
    return {};
  }
  // `count` includes the trailing NUL. Allocate it before asking Win32 to
  // write `count` wchar_t values, then remove it from the C++ string. The old
  // count-1 allocation was a one-wchar heap overwrite on every media path.
  std::wstring result(static_cast<size_t>(count), L'\0');
  if (MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), count) <= 0) {
    return {};
  }
  result.pop_back();
  return result;
}

std::string lowercaseAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

// Path normalization + still classification are shared with the still-media
// cache (StillMediaFrameCache.h: normalizeMediaAssetPath / isStillImagePath /
// isStillImageMediaAsset) so both sides key and classify files identically.
std::string normalizeMediaPath(std::string path) {
  return normalizeMediaAssetPath(std::move(path));
}

std::string mediaFrameSourceId(const CompositorRenderPlanLayer& layer) {
  return layer.sourceId.empty() ? "media:" + layer.mediaAssetId : layer.sourceId;
}

std::string mediaLayerPlaybackKey(const CompositorRenderPlanLayer& layer) {
  return layer.mediaPlaybackKey.empty()
             ? layer.mediaAssetId + ":" + (layer.mediaAssetPlaying ? "playing" : "paused")
             : layer.mediaPlaybackKey;
}

std::string mediaLayerStateKey(const CompositorRenderPlanLayer& layer) {
  return mediaFrameSourceId(layer) + "|" + normalizeMediaPath(layer.mediaAssetPath) + "|" + mediaLayerPlaybackKey(layer);
}

std::wstring quoteWindowsArgument(const std::wstring& value) {
  return L"\"" + value + L"\"";
}

std::wstring ffmpegExecutablePath() {
  const auto fromDirectory = [](const char* variable) -> std::wstring {
    const char* value = std::getenv(variable);
    if (!value || !*value) {
      return {};
    }
    const auto candidate = std::filesystem::path(value) / L"ffmpeg.exe";
    std::error_code error;
    return std::filesystem::exists(candidate, error) ? candidate.wstring() : std::wstring{};
  };
  if (auto configured = fromDirectory("COREVIDEO_FFMPEG_BIN_DIR"); !configured.empty()) {
    return configured;
  }
  if (auto configured = fromDirectory("FFMPEG_BIN_DIR"); !configured.empty()) {
    return configured;
  }

  wchar_t found[MAX_PATH]{};
  const DWORD length = SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH, found, nullptr);
  return length > 0 && length < MAX_PATH ? std::wstring(found, length) : std::wstring{};
}

// Media Foundation does not decode common production MOV profiles such as
// Apple ProRes HQ/4444. This worker is a compatibility decoder behind the same
// IMediaFrameSource boundary: FFmpeg performs the codec + BT.709 range conversion
// off the render thread and publishes only the latest 1080p BGRA frame.
class FfmpegVideoDecoder {
 public:
  static constexpr int kOutputWidth = 1920;
  static constexpr int kOutputHeight = 1080;

  ~FfmpegVideoDecoder() { stop(); }
  FfmpegVideoDecoder(const FfmpegVideoDecoder&) = delete;
  FfmpegVideoDecoder& operator=(const FfmpegVideoDecoder&) = delete;

  static std::unique_ptr<FfmpegVideoDecoder> start(const std::string& path,
                                                   bool posterFrame,
                                                   bool loop,
                                                   std::string& error) {
    auto decoder = std::unique_ptr<FfmpegVideoDecoder>(new FfmpegVideoDecoder());
    if (!decoder->launch(path, posterFrame, loop, error)) {
      return nullptr;
    }
    return decoder;
  }

  bool latest(std::shared_ptr<std::vector<std::uint8_t>>& pixels,
              std::int64_t& frameId, bool& ended) const {
    std::lock_guard<std::mutex> lock(mutex_);
    pixels = latestPixels_;
    frameId = latestFrameId_;
    ended = ended_;
    return static_cast<bool>(pixels);
  }

 private:
  FfmpegVideoDecoder() = default;

  bool launch(const std::string& path, bool posterFrame, bool loop, std::string& error) {
    const auto executable = ffmpegExecutablePath();
    if (executable.empty()) {
      error = "FFmpeg was not found in the configured runtime or PATH.";
      return false;
    }

    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE childOutputRead = nullptr;
    HANDLE childOutputWrite = nullptr;
    if (!CreatePipe(&childOutputRead, &childOutputWrite, &security, 0) ||
        !SetHandleInformation(childOutputRead, HANDLE_FLAG_INHERIT, 0)) {
      if (childOutputRead) CloseHandle(childOutputRead);
      if (childOutputWrite) CloseHandle(childOutputWrite);
      error = "Could not create the FFmpeg video pipe.";
      return false;
    }

    HANDLE nullOutput = CreateFileW(L"NUL", GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = childOutputWrite;
    startup.hStdError = nullOutput != INVALID_HANDLE_VALUE ? nullOutput : childOutputWrite;

    const std::wstring filter =
        L"scale=1920:1080:force_original_aspect_ratio=decrease,"
        L"pad=1920:1080:(ow-iw)/2:(oh-ih)/2:black";
    std::wstring command = quoteWindowsArgument(executable) +
        L" -nostdin -hide_banner -loglevel error" +
        (loop && !posterFrame ? L" -stream_loop -1" : L"") +
        L" -re -i " + quoteWindowsArgument(widenUtf8(path)) +
        L" -map 0:v:0 -an -sn -dn -vf " + quoteWindowsArgument(filter) +
        (posterFrame ? L" -frames:v 1" : L"") +
        L" -pix_fmt bgra -f rawvideo pipe:1";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(
        executable.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    CloseHandle(childOutputWrite);
    if (nullOutput != INVALID_HANDLE_VALUE) {
      CloseHandle(nullOutput);
    }
    if (!created) {
      CloseHandle(childOutputRead);
      error = "FFmpeg could not be started (Win32 " + std::to_string(GetLastError()) + ").";
      return false;
    }

    CloseHandle(process.hThread);
    process_ = process.hProcess;
    outputRead_ = childOutputRead;
    reader_ = std::thread([this] { readLoop(); });
    return true;
  }

  void readLoop() {
    constexpr std::size_t frameBytes =
        static_cast<std::size_t>(kOutputWidth) * kOutputHeight * 4u;
    while (!stopRequested_.load(std::memory_order_acquire)) {
      auto frame = std::make_shared<std::vector<std::uint8_t>>(frameBytes);
      std::size_t offset = 0;
      while (offset < frameBytes && !stopRequested_.load(std::memory_order_acquire)) {
        DWORD read = 0;
        const DWORD chunk = static_cast<DWORD>((std::min)(frameBytes - offset,
                                                          static_cast<std::size_t>(1u << 20)));
        if (!ReadFile(outputRead_, frame->data() + offset, chunk, &read, nullptr) || read == 0) {
          offset = 0;
          break;
        }
        offset += read;
      }
      if (offset != frameBytes) {
        break;
      }
      std::lock_guard<std::mutex> lock(mutex_);
      latestPixels_ = std::move(frame);
      ++latestFrameId_;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    ended_ = !stopRequested_.load(std::memory_order_acquire);
  }

  void stop() {
    stopRequested_.store(true, std::memory_order_release);
    if (process_) {
      DWORD exitCode = 0;
      if (GetExitCodeProcess(process_, &exitCode) && exitCode == STILL_ACTIVE) {
        TerminateProcess(process_, 0);
      }
    }
    if (reader_.joinable()) {
      reader_.join();
    }
    if (outputRead_) {
      CloseHandle(outputRead_);
      outputRead_ = nullptr;
    }
    if (process_) {
      CloseHandle(process_);
      process_ = nullptr;
    }
  }

  mutable std::mutex mutex_;
  std::shared_ptr<std::vector<std::uint8_t>> latestPixels_;
  std::int64_t latestFrameId_ = 0;
  bool ended_ = false;
  std::atomic<bool> stopRequested_{false};
  HANDLE process_ = nullptr;
  HANDLE outputRead_ = nullptr;
  std::thread reader_;
};

bool copyWicImageToFrame(IWICImagingFactory* factory, const std::string& path, VideoFrame& frame) {
  if (!factory) {
    return false;
  }
  ComPtrLite<IWICBitmapDecoder> decoder;
  if (FAILED(factory->CreateDecoderFromFilename(
          widenUtf8(path).c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, decoder.put()))) {
    return false;
  }
  ComPtrLite<IWICBitmapFrameDecode> sourceFrame;
  if (FAILED(decoder->GetFrame(0, sourceFrame.put()))) {
    return false;
  }
  UINT width = 0;
  UINT height = 0;
  if (FAILED(sourceFrame->GetSize(&width, &height)) || width == 0 || height == 0) {
    return false;
  }
  ComPtrLite<IWICFormatConverter> converter;
  if (FAILED(factory->CreateFormatConverter(converter.put()))) {
    return false;
  }
  if (FAILED(converter->Initialize(
          sourceFrame.get(),
          GUID_WICPixelFormat32bppBGRA,
          WICBitmapDitherTypeNone,
          nullptr,
          0.f,
          WICBitmapPaletteTypeCustom))) {
    return false;
  }
  const int stride = static_cast<int>(width) * 4;
  auto pixels = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(stride) * static_cast<size_t>(height));
  if (FAILED(converter->CopyPixels(nullptr, static_cast<UINT>(stride), static_cast<UINT>(pixels->size()), pixels->data()))) {
    return false;
  }
  frame.pixelWidth = static_cast<int>(width);
  frame.pixelHeight = static_cast<int>(height);
  frame.width = frame.pixelWidth;
  frame.height = frame.pixelHeight;
  frame.naturalWidth = frame.pixelWidth;
  frame.naturalHeight = frame.pixelHeight;
  frame.pixelStride = stride;
  frame.pixels = std::move(pixels);
  return true;
}

class MediaFoundationMediaFrameSource final : public IMediaFrameSource, public IMediaVideoPrefetch {
 public:
  MediaFoundationMediaFrameSource() {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    comInitialized_ = SUCCEEDED(co);
    mfStarted_ = SUCCEEDED(MFStartup(MF_VERSION));
    if (comInitialized_) {
      CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wicFactory_.put()));
    }
  }

  ~MediaFoundationMediaFrameSource() override {
    for (auto& [id, state] : states_) cancelReaders(state);
    states_.clear();
    wicFactory_ = {};
    if (mfStarted_) {
      MFShutdown();
    }
    if (comInitialized_) {
      CoUninitialize();
    }
  }

  void setMediaWakeCallback(std::function<void()> callback) override { mediaWake_ = std::move(callback); }

  std::vector<ScheduledMediaVideo> prefetchMediaVideo(
      const std::vector<CompositorRenderPlanLayer>& layers, int64_t nowMs) override {
    prefetchingVideo_ = true;
    const auto frames = pollMediaFrames(layers, nowMs);
    prefetchingVideo_ = false;
    std::vector<ScheduledMediaVideo> result;
    for (const auto& frame : frames) {
      const auto found = states_.find(frame.participantId);
      if (found == states_.end()) continue;
      const auto& state = found->second;
      const auto due = state.wasPlaying && !state.ffmpegVideo && !state.imageLoaded
          ? state.clock.epoch100ns() + state.videoLoopOffset + state.decodedVideoPts : nowMs * 10000;
      result.push_back({frame, due});
    }
    return result;
  }

  std::vector<VideoFrame> pollMediaFrames(const std::vector<CompositorRenderPlanLayer>& layers, int64_t timestampMs) override {
    warnings_.clear();
    std::vector<VideoFrame> frames;
    std::set<std::string> seen;
    for (const auto& layer : layers) {
      if (layer.mediaAssetId.empty() || layer.mediaAssetPath.empty()) {
        continue;
      }
      // Still-image ROUTE layers are served by MediaCore's StillMediaFrameCache
      // (decoded once on its background thread, injected into the frame set
      // before this poll) — decoding them here would repeat the work on the
      // render thread. Background still layers keep the in-place WIC path.
      if (layer.kind == "media-video" &&
          isStillImageMediaAsset(layer.mediaAssetKind, layer.mediaAssetPath)) {
        continue;
      }
      if (!seen.insert(mediaLayerStateKey(layer)).second) {
        continue;
      }
      VideoFrame frame;
      frame.participantId = mediaFrameSourceId(layer);
      frame.timestampMs = timestampMs;
      if (decodeLayer(layer, timestampMs, frame) && frame.hasPixels()) {
        frames.push_back(std::move(frame));
      }
    }
    return frames;
  }

  std::vector<AudioFrame> pollMediaAudioFrames(const std::vector<CompositorRenderPlanLayer>& layers, int64_t timestampMs) override {
    warnings_.clear();
    std::vector<AudioFrame> frames;
    std::set<std::string> seen;
    for (const auto& layer : layers) {
      if (layer.kind != "media-video" || layer.mediaAssetId.empty() || layer.mediaAssetPath.empty()) {
        continue;
      }
      if (!layer.mediaAssetPlaying || isStillImagePath(normalizeMediaPath(layer.mediaAssetPath))) {
        continue;
      }
      if (!seen.insert(mediaLayerStateKey(layer)).second) {
        continue;
      }
      if (auto frame = decodeLayerAudio(layer, timestampMs); frame.sampleCount > 0 && !frame.pcm.empty()) {
        frames.push_back(std::move(frame));
      }
    }
    return frames;
  }

  std::vector<std::string> warnings() const override { return warnings_; }

 private:
  struct AssetState {
    std::string path;
    std::string playbackKey;
    bool imageLoaded = false;
    bool ended = false;
    bool wasPlaying = false;
    bool audioWasPlaying = false;
    bool loop = false;
    int64_t frameId = 0;
    VideoFrame lastFrame;
    ComPtrLite<MediaReadCallback> videoCallback;
    ComPtrLite<IMFSourceReader> reader;
    std::unique_ptr<FfmpegVideoDecoder> ffmpegVideo;
    std::int64_t ffmpegPublishedFrameId = 0;
    std::string videoDecoderError;
    ComPtrLite<MediaReadCallback> audioCallback;
    ComPtrLite<IMFSourceReader> audioReader;
    bool audioEnded = false;
    std::string audioPlaybackKey;
    MediaPlaybackTimeline clock;
    MediaAudioWindows audioWindows;
    std::string generationIdentity;
    VideoFrame presentedFrame;
    bool videoPending = false;
    int64_t decodedVideoPts = 0, decodedVideoDuration = 0, videoLoopOffset = 0;
    int64_t decodedAudioPts = 0, audioLoopOffset = 0, audioEndPts = 0;
  };

  static void cancelReaders(AssetState& state) {
    if (state.videoCallback) state.videoCallback->cancel();
    if (state.audioCallback) state.audioCallback->cancel();
    // Readers were created with ASYNC_CALLBACK: Flush cancels outstanding
    // requests asynchronously. No synchronous ReadSample wait exists to join.
    if (state.reader) state.reader->Flush(MF_SOURCE_READER_ALL_STREAMS);
    if (state.audioReader) state.audioReader->Flush(MF_SOURCE_READER_ALL_STREAMS);
  }

  AssetState& stateFor(const CompositorRenderPlanLayer& layer, int64_t timestampMs) {
    const auto key = mediaFrameSourceId(layer);
    const auto identity = normalizeMediaPath(layer.mediaAssetPath) + "|" + mediaLayerPlaybackKey(layer) +
        (layer.mediaAssetPlaying ? "|playing" : "|paused") + (layer.mediaAssetLoop ? "|loop" : "|once");
    auto& state = states_[key];
    if (state.generationIdentity != identity) {
      cancelReaders(state);
      state = {};
      state.generationIdentity = identity;
      state.path = normalizeMediaPath(layer.mediaAssetPath);
      state.loop = layer.mediaAssetLoop;
      state.clock.configure(identity, layer.mediaAssetPlaying, timestampMs * 10000);
    }
    return state;
  }

  bool decodeLayer(const CompositorRenderPlanLayer& layer, int64_t timestampMs, VideoFrame& frame) {
    const std::string frameSourceId = mediaFrameSourceId(layer);
    auto& state = stateFor(layer, timestampMs);
    const auto path = normalizeMediaPath(layer.mediaAssetPath);
    if (state.path != path || state.loop != layer.mediaAssetLoop) {
      state = {};
      state.path = path;
      state.loop = layer.mediaAssetLoop;
    }
    const std::string playbackKey = mediaLayerPlaybackKey(layer);
    if (isStillImagePath(path)) {
      if (!state.imageLoaded) {
        state.lastFrame.participantId = frameSourceId;
        if (!copyWicImageToFrame(wicFactory_.get(), path, state.lastFrame)) {
          warnings_.push_back("Media asset " + layer.mediaAssetId + " could not be decoded as an image.");
          return false;
        }
        state.imageLoaded = true;
      }
      frame = state.lastFrame;
      frame.participantId = frameSourceId;
      frame.timestampMs = timestampMs;
      return true;
    }
    if (!layer.mediaAssetPlaying) {
      if (state.playbackKey != playbackKey) {
        state.reader = {};
        state.ffmpegVideo = {};
        state.ffmpegPublishedFrameId = 0;
        state.ended = false;
        state.frameId = 0;
        state.lastFrame = {};
      }
      state.wasPlaying = false;
      state.playbackKey = playbackKey;
      // A Preview cue is intentionally paused, but it must still display a
      // real poster frame. Decode exactly one frame and retain it until Take.
      if (!state.lastFrame.hasPixels()) {
        if (!state.reader && !state.ffmpegVideo &&
            !openVideoReader(path, state, true, layer.mediaAssetLoop)) {
          warnings_.push_back("Media asset " + layer.mediaAssetId +
                              " could not be opened for Preview cueing" +
                              (state.videoDecoderError.empty() ? "." : ": " + state.videoDecoderError));
          return false;
        }
        if (!readNextVideoFrame(layer.mediaAssetId, frameSourceId, state, timestampMs)) {
          return false;
        }
      }
      frame = state.lastFrame;
      frame.participantId = frameSourceId;
      frame.timestampMs = timestampMs;
      return frame.hasPixels();
    }
    if (!state.wasPlaying || state.playbackKey != playbackKey) {
      state.reader = {};
      state.ffmpegVideo = {};
      state.ffmpegPublishedFrameId = 0;
      state.ended = false;
      state.frameId = 0;
    }
    state.wasPlaying = true;
    state.playbackKey = playbackKey;
    if (!state.reader && !state.ffmpegVideo &&
        !openVideoReader(path, state, false, layer.mediaAssetLoop)) {
      warnings_.push_back("Media asset " + layer.mediaAssetId +
                          " could not be opened for Program playback" +
                          (state.videoDecoderError.empty() ? "." : ": " + state.videoDecoderError));
      return false;
    }
    if (state.ended && layer.mediaAssetLoop) {
      state.videoLoopOffset += state.decodedVideoPts + state.decodedVideoDuration;
      state.videoPending = false;
      // Media Foundation is the fast path for compatible clips but does not
      // have FFmpeg's -stream_loop input option. Reopen at EOS while retaining
      // the last good frame; the replacement decoder can warm without a flash.
      state.reader = {};
      state.ffmpegVideo = {};
      state.ffmpegPublishedFrameId = 0;
      state.ended = false;
      if (!openVideoReader(path, state, false, true)) {
        warnings_.push_back("Media background " + layer.mediaAssetId +
                            " could not restart its loop" +
                            (state.videoDecoderError.empty() ? "." : ": " + state.videoDecoderError));
        return state.lastFrame.hasPixels();
      }
    }
    if (state.ended) {
      if (state.lastFrame.hasPixels()) {
        frame = state.lastFrame;
        frame.participantId = frameSourceId;
        frame.timestampMs = timestampMs;
        return true;
      }
      return false;
    }
    if (state.ffmpegVideo) {
      if (!readNextVideoFrame(layer.mediaAssetId, frameSourceId, state, timestampMs)) return false;
      frame = state.lastFrame;
      return frame.hasPixels();
    }
    if (prefetchingVideo_) {
      const auto previousId = state.frameId;
      if (!readNextVideoFrame(layer.mediaAssetId, frameSourceId, state, timestampMs) || state.frameId == previousId) return false;
      frame = state.lastFrame;
      return frame.hasPixels();
    }
    // One future sample is enough lookahead. Repeated render polls hold the
    // current image; irregular polls select by media PTS, not decoder count.
    for (int decoded = 0; decoded < 4; ++decoded) {
      if (!state.videoPending) {
        const auto previousId = state.frameId;
        if (!readNextVideoFrame(layer.mediaAssetId, frameSourceId, state, timestampMs) || state.frameId == previousId) break;
        state.videoPending = true;
      }
      if (!state.clock.videoDue(state.videoLoopOffset + state.decodedVideoPts, timestampMs * 10000)) break;
      state.presentedFrame = state.lastFrame;
      state.videoPending = false;
    }
    frame = state.presentedFrame;
    return frame.hasPixels();
  }

  AudioFrame decodeLayerAudio(const CompositorRenderPlanLayer& layer, int64_t timestampMs) {
    AudioFrame frame;
    const std::string frameSourceId = mediaFrameSourceId(layer);
    auto& state = stateFor(layer, timestampMs);
    const auto path = normalizeMediaPath(layer.mediaAssetPath);
    if (state.path != path) {
      state = {};
      state.path = path;
    }
    const std::string playbackKey = mediaLayerPlaybackKey(layer);
    if (!state.audioWasPlaying || state.audioPlaybackKey != playbackKey) {
      state.audioReader = {};
      state.audioEnded = false;
    }
    state.audioWasPlaying = true;
    state.audioPlaybackKey = playbackKey;
    if (state.audioEnded && state.audioReader && layer.mediaAssetLoop) {
      state.audioLoopOffset = state.audioEndPts;
      if (state.audioCallback) state.audioCallback->cancel();
      state.audioReader->Flush(MF_SOURCE_READER_ALL_STREAMS);
      state.audioReader = {}; state.audioEnded = false;
    }
    if (state.audioEnded && !state.audioReader) return {};
    currentAudioRequestMs_ = timestampMs;
    if (!state.audioReader && !openAudioReader(path, state)) {
      warnings_.push_back("Media asset " + layer.mediaAssetId + " could not be opened for Program audio.");
      return frame;
    }
    if (!state.audioReader || (state.audioEnded && state.audioWindows.bufferedThrough() <= state.audioWindows.cursor())) return {};
    constexpr int windowFrames = 960;
    const auto targetSample = state.clock.elapsed100ns(timestampMs * 10000) * 48000 / 10000000;
    state.audioWindows.seek(targetSample);
    for (int chunks = 0; chunks < 8 && !state.audioEnded &&
         state.audioWindows.bufferedThrough() < state.audioWindows.cursor() + windowFrames; ++chunks) {
      AudioFrame decoded;
      if (!readNextAudioFrame(layer.mediaAssetId, state, timestampMs, decoded)) break;
      state.audioWindows.append(state.decodedAudioPts, std::move(decoded.pcm));
    }
    if (state.audioWindows.bufferedThrough() <= state.audioWindows.cursor() ||
        (!state.audioEnded && state.audioWindows.bufferedThrough() < state.audioWindows.cursor() + windowFrames)) return {};
    frame.participantId = frameSourceId;
    frame.sampleRate = 48000; frame.channels = 2; frame.sampleCount = windowFrames;
    frame.timestampMs = timestampMs; frame.voiceActive = true;
    frame.pcm = state.audioWindows.take(windowFrames);
    return frame;
  }

  bool openVideoReader(const std::string& path, AssetState& state,
                       bool posterFrame, bool loop) {
    state.videoDecoderError.clear();
    if (!mfStarted_) {
      state.videoDecoderError = "Media Foundation is unavailable.";
    } else {
      // The source reader normally exposes the decoder's native YUV output.
      // Enable Media Foundation video processing so our requested RGB32 output
      // is negotiated through the built-in color converter/scaler.
      ComPtrLite<IMFAttributes> attributes;
      *state.videoCallback.put() = new MediaReadCallback(mediaWake_);
      if (SUCCEEDED(MFCreateAttributes(attributes.put(), 2)) &&
          SUCCEEDED(attributes->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, state.videoCallback.get())) &&
          SUCCEEDED(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE)) &&
          SUCCEEDED(MFCreateSourceReaderFromURL(widenUtf8(path).c_str(), attributes.get(), state.reader.put()))) {
        ComPtrLite<IMFMediaType> mediaType;
        if (SUCCEEDED(MFCreateMediaType(mediaType.put())) &&
            SUCCEEDED(mediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) &&
            SUCCEEDED(mediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32)) &&
            SUCCEEDED(state.reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, mediaType.get()))) {
          return true;
        }
      }
      state.reader = {};
      state.videoDecoderError = "Media Foundation did not provide a compatible video decoder.";
    }

    std::string fallbackError;
    state.ffmpegVideo = FfmpegVideoDecoder::start(path, posterFrame, loop, fallbackError);
    if (!state.ffmpegVideo) {
      state.videoDecoderError += " FFmpeg fallback failed: " + fallbackError;
      return false;
    }
    return true;
  }

  bool openAudioReader(const std::string& path, AssetState& state) {
    if (!mfStarted_) {
      return false;
    }
    ComPtrLite<IMFAttributes> attributes;
    *state.audioCallback.put() = new MediaReadCallback(mediaWake_);
    if (FAILED(MFCreateAttributes(attributes.put(), 1)) ||
        FAILED(attributes->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, state.audioCallback.get()))) return false;
    const HRESULT opened = MFCreateSourceReaderFromURL(
        widenUtf8(path).c_str(), attributes.get(), state.audioReader.put());
    if (FAILED(opened)) {
      return false;
    }
    ComPtrLite<IMFMediaType> mediaType;
    if (FAILED(MFCreateMediaType(mediaType.put())) ||
        FAILED(mediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio)) ||
        FAILED(mediaType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float)) ||
        FAILED(mediaType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000)) ||
        FAILED(mediaType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2)) ||
        FAILED(mediaType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32)) ||
        FAILED(mediaType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 8)) ||
        FAILED(mediaType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 48000 * 8))) {
      state.audioReader = {};
      return false;
    }
    const HRESULT selected = state.audioReader->SetCurrentMediaType(
        MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, mediaType.get());
    if (selected == MF_E_INVALIDSTREAMNUMBER) {
      // Video-only backdrops are normal. Absence of an audio stream is not a
      // decoder failure and must not alarm the operator every render tick.
      state.audioReader = {};
      state.audioEnded = true;
      return true;
    }
    if (FAILED(selected)) {
      state.audioReader = {};
      return false;
    }
    const auto audioPts = state.clock.elapsed100ns(currentAudioRequestMs_ * 10000);
    const auto seekPts = mediaLoopSeek100ns(audioPts, state.audioLoopOffset);
    if (seekPts > 0) {
      PROPVARIANT position{}; position.vt = VT_I8;
      position.hVal.QuadPart = seekPts;
      if (FAILED(state.audioReader->SetCurrentPosition(GUID_NULL, position))) {
        warnings_.push_back("Media audio could not seek to its shared playback clock.");
        return false;
      }
    }
    return true;
  }

  bool readNextVideoFrame(const std::string& assetId, const std::string& frameSourceId, AssetState& state, int64_t timestampMs) {
    if (state.ffmpegVideo) {
      std::shared_ptr<std::vector<std::uint8_t>> pixels;
      std::int64_t decodedFrameId = 0;
      bool ended = false;
      if (!state.ffmpegVideo->latest(pixels, decodedFrameId, ended)) {
        state.ended = ended;
        return state.lastFrame.hasPixels();
      }
      if (decodedFrameId == state.ffmpegPublishedFrameId && state.lastFrame.hasPixels()) {
        state.ended = ended;
        return true;
      }
      VideoFrame decoded;
      decoded.participantId = frameSourceId;
      decoded.width = FfmpegVideoDecoder::kOutputWidth;
      decoded.height = FfmpegVideoDecoder::kOutputHeight;
      decoded.naturalWidth = decoded.width;
      decoded.naturalHeight = decoded.height;
      decoded.timestampMs = timestampMs;
      decoded.pixelWidth = decoded.width;
      decoded.pixelHeight = decoded.height;
      decoded.pixelStride = decoded.width * 4;
      decoded.frameId = decodedFrameId;
      decoded.pixels = std::move(pixels);
      state.lastFrame = std::move(decoded);
      state.ffmpegPublishedFrameId = decodedFrameId;
      state.ended = ended;
      return true;
    }
    if (!state.reader) {
      return false;
    }
    DWORD streamIndex = 0;
    DWORD flags = 0;
    LONGLONG sampleTime = 0;
    ComPtrLite<IMFSample> sample;
    const HRESULT read = state.videoCallback->take(
        state.reader.get(), MF_SOURCE_READER_FIRST_VIDEO_STREAM, &flags, &sampleTime, sample.put());
    if (FAILED(read)) {
      warnings_.push_back("Media asset " + assetId + " failed while decoding a video frame.");
      return false;
    }
    if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
      state.ended = true;
      if (!state.lastFrame.hasPixels()) {
        warnings_.push_back("Media asset " + assetId + " ended before a video frame could be decoded.");
      }
      return state.lastFrame.hasPixels();
    }
    if (!sample) {
      return state.lastFrame.hasPixels();
    }
    ComPtrLite<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(buffer.put()))) {
      warnings_.push_back("Media asset " + assetId + " produced a video sample that could not be copied.");
      return false;
    }
    BYTE* data = nullptr;
    DWORD maxLength = 0;
    DWORD currentLength = 0;
    if (FAILED(buffer->Lock(&data, &maxLength, &currentLength)) || !data || currentLength == 0) {
      warnings_.push_back("Media asset " + assetId + " produced an empty video sample.");
      return false;
    }
    ComPtrLite<IMFMediaType> currentType;
    UINT32 width = 0;
    UINT32 height = 0;
    if (SUCCEEDED(state.reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, currentType.put()))) {
      MFGetAttributeSize(currentType.get(), MF_MT_FRAME_SIZE, &width, &height);
    }
    if (width == 0 || height == 0 || currentLength < width * height * 4u) {
      buffer->Unlock();
      warnings_.push_back("Media asset " + assetId + " produced an invalid RGB32 video frame.");
      return false;
    }
    const int stride = static_cast<int>(width) * 4;
    auto pixels = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(stride) * static_cast<size_t>(height));
    std::memcpy(pixels->data(), data, pixels->size());
    buffer->Unlock();

    // MFVideoFormat_RGB32 is BGRX, not BGRA: the high byte is padding and many
    // H.264 decoders leave it at zero. The compositor samples this buffer as
    // BGRA and uses source alpha, so an untouched BGRX frame decodes correctly
    // but becomes completely transparent over the scene underneath. Video is
    // always opaque at this stage; graphics transparency is handled by the
    // still-image/overlay paths.
    for (size_t offset = 3; offset < pixels->size(); offset += 4) {
      (*pixels)[offset] = 0xff;
    }

    VideoFrame frame;
    frame.participantId = frameSourceId;
    frame.width = static_cast<int>(width);
    frame.height = static_cast<int>(height);
    frame.naturalWidth = frame.width;
    frame.naturalHeight = frame.height;
    frame.timestampMs = timestampMs;
    frame.pixelWidth = frame.width;
    frame.pixelHeight = frame.height;
    frame.pixelStride = stride;
    state.decodedVideoPts = sampleTime;
    LONGLONG duration = 0; sample->GetSampleDuration(&duration);
    state.decodedVideoDuration = duration;
    frame.frameId = ++state.frameId;
    frame.pixels = std::move(pixels);
    state.lastFrame = std::move(frame);
    return true;
  }

  bool readNextAudioFrame(const std::string& assetId, AssetState& state, int64_t timestampMs, AudioFrame& frame) {
    if (!state.audioReader) {
      return false;
    }
    DWORD streamIndex = 0;
    DWORD flags = 0;
    LONGLONG sampleTime = 0;
    ComPtrLite<IMFSample> sample;
    const HRESULT read = state.audioCallback->take(
        state.audioReader.get(), MF_SOURCE_READER_FIRST_AUDIO_STREAM, &flags, &sampleTime, sample.put());
    if (FAILED(read)) {
      warnings_.push_back("Media asset " + assetId + " failed while decoding Program audio.");
      return false;
    }
    if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
      state.audioEnded = true;
      return false;
    }
    if (!sample) {
      return false;
    }

    ComPtrLite<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(buffer.put()))) {
      warnings_.push_back("Media asset " + assetId + " audio sample could not be copied.");
      return false;
    }
    BYTE* data = nullptr;
    DWORD maxLength = 0;
    DWORD currentLength = 0;
    if (FAILED(buffer->Lock(&data, &maxLength, &currentLength)) || !data || currentLength < sizeof(float)) {
      return false;
    }

    ComPtrLite<IMFMediaType> currentType;
    UINT32 sampleRate = 48000;
    UINT32 channels = 2;
    if (SUCCEEDED(state.audioReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, currentType.put()))) {
      UINT32 typeSampleRate = 0;
      UINT32 typeChannels = 0;
      if (SUCCEEDED(currentType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &typeSampleRate)) && typeSampleRate > 0) {
        sampleRate = typeSampleRate;
      }
      if (SUCCEEDED(currentType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &typeChannels)) && typeChannels > 0) {
        channels = typeChannels;
      }
    }
    const size_t floatCount = static_cast<size_t>(currentLength / sizeof(float));
    if (channels == 0 || floatCount < channels) {
      buffer->Unlock();
      return false;
    }
    std::vector<float> pcm(floatCount);
    std::memcpy(pcm.data(), data, floatCount * sizeof(float));
    buffer->Unlock();

    state.decodedAudioPts = sampleTime + state.audioLoopOffset;
    state.audioEndPts = state.decodedAudioPts + static_cast<int64_t>(floatCount / channels) * 10000000 / sampleRate;
    frame.sampleRate = static_cast<int>(sampleRate);
    frame.channels = static_cast<int>(channels);
    frame.timestampMs = timestampMs;
    frame.sampleCount = static_cast<int>(floatCount / static_cast<size_t>(channels));
    frame.voiceActive = true;
    frame.pcm = std::move(pcm);
    return frame.sampleCount > 0;
  }

  std::function<void()> mediaWake_;
  bool prefetchingVideo_ = false;
  int64_t currentAudioRequestMs_ = 0;
  bool comInitialized_ = false;
  bool mfStarted_ = false;
  ComPtrLite<IWICImagingFactory> wicFactory_;
  std::map<std::string, AssetState> states_;
  std::vector<std::string> warnings_;
};

}  // namespace

std::unique_ptr<IMediaFrameSource> createMediaFoundationMediaFrameSource() {
  return std::make_unique<OwnedMediaFrameSource>([] { return std::make_unique<MediaFoundationMediaFrameSource>(); });
}

}  // namespace corevideo::modules

#else

namespace corevideo::modules {

std::unique_ptr<IMediaFrameSource> createMediaFoundationMediaFrameSource() {
  return nullptr;
}

}  // namespace corevideo::modules

#endif
