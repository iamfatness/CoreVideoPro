#include "modules/Interfaces.h"

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
#include <cctype>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
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

std::wstring widenUtf8(const std::string& value) {
  if (value.empty()) {
    return {};
  }
  const int count = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
  if (count <= 0) {
    return {};
  }
  std::wstring result(static_cast<size_t>(count - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), count);
  return result;
}

std::string lowercaseAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

std::string normalizeMediaPath(std::string path) {
  constexpr const char* prefix = "file:///";
  if (path.rfind(prefix, 0) == 0) {
    path = path.substr(std::strlen(prefix));
    std::replace(path.begin(), path.end(), '/', '\\');
  }
  return path;
}

bool isStillImagePath(const std::string& path) {
  const auto lower = lowercaseAscii(path);
  return lower.ends_with(".png") || lower.ends_with(".jpg") || lower.ends_with(".jpeg") ||
         lower.ends_with(".bmp") || lower.ends_with(".tif") || lower.ends_with(".tiff");
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

class MediaFoundationMediaFrameSource final : public IMediaFrameSource {
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
    states_.clear();
    wicFactory_ = {};
    if (mfStarted_) {
      MFShutdown();
    }
    if (comInitialized_) {
      CoUninitialize();
    }
  }

  std::vector<VideoFrame> pollMediaFrames(const std::vector<CompositorRenderPlanLayer>& layers, int64_t timestampMs) override {
    std::vector<VideoFrame> frames;
    std::set<std::string> seen;
    for (const auto& layer : layers) {
      if (layer.mediaAssetId.empty() || layer.mediaAssetPath.empty()) {
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
    int64_t frameId = 0;
    VideoFrame lastFrame;
    ComPtrLite<IMFSourceReader> reader;
    ComPtrLite<IMFSourceReader> audioReader;
    bool audioEnded = false;
    std::string audioPlaybackKey;
  };

  bool decodeLayer(const CompositorRenderPlanLayer& layer, int64_t timestampMs, VideoFrame& frame) {
    const std::string frameSourceId = mediaFrameSourceId(layer);
    auto& state = states_[frameSourceId];
    const auto path = normalizeMediaPath(layer.mediaAssetPath);
    if (state.path != path) {
      state = {};
      state.path = path;
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
      state.wasPlaying = false;
      state.playbackKey = playbackKey;
      if (state.lastFrame.hasPixels()) {
        frame = state.lastFrame;
        frame.participantId = frameSourceId;
        frame.timestampMs = timestampMs;
        return true;
      }
      return false;
    }
    if (!state.wasPlaying || state.playbackKey != playbackKey) {
      state.reader = {};
      state.ended = false;
      state.frameId = 0;
    }
    state.wasPlaying = true;
    state.playbackKey = playbackKey;
    if (!state.reader && !openVideoReader(path, state)) {
      warnings_.push_back("Media asset " + layer.mediaAssetId + " could not be opened for Program playback.");
      return false;
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
    if (!readNextVideoFrame(layer.mediaAssetId, frameSourceId, state, timestampMs)) {
      return false;
    }
    frame = state.lastFrame;
    return frame.hasPixels();
  }

  AudioFrame decodeLayerAudio(const CompositorRenderPlanLayer& layer, int64_t timestampMs) {
    AudioFrame frame;
    const std::string frameSourceId = mediaFrameSourceId(layer);
    auto& state = states_[frameSourceId];
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
    if (!state.audioReader && !openAudioReader(path, state)) {
      warnings_.push_back("Media asset " + layer.mediaAssetId + " could not be opened for Program audio.");
      return frame;
    }
    if (state.audioEnded) {
      return frame;
    }
    if (!readNextAudioFrame(layer.mediaAssetId, state, timestampMs, frame)) {
      return {};
    }
    return frame;
  }

  bool openVideoReader(const std::string& path, AssetState& state) {
    if (!mfStarted_) {
      return false;
    }
    if (FAILED(MFCreateSourceReaderFromURL(widenUtf8(path).c_str(), nullptr, state.reader.put()))) {
      return false;
    }
    ComPtrLite<IMFMediaType> mediaType;
    if (FAILED(MFCreateMediaType(mediaType.put())) ||
        FAILED(mediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
        FAILED(mediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32)) ||
        FAILED(state.reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, mediaType.get()))) {
      state.reader = {};
      return false;
    }
    return true;
  }

  bool openAudioReader(const std::string& path, AssetState& state) {
    if (!mfStarted_) {
      return false;
    }
    if (FAILED(MFCreateSourceReaderFromURL(widenUtf8(path).c_str(), nullptr, state.audioReader.put()))) {
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
        FAILED(mediaType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 48000 * 8)) ||
        FAILED(state.audioReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, mediaType.get()))) {
      state.audioReader = {};
      return false;
    }
    return true;
  }

  bool readNextVideoFrame(const std::string& assetId, const std::string& frameSourceId, AssetState& state, int64_t timestampMs) {
    if (!state.reader) {
      return false;
    }
    DWORD streamIndex = 0;
    DWORD flags = 0;
    LONGLONG sampleTime = 0;
    ComPtrLite<IMFSample> sample;
    const HRESULT read = state.reader->ReadSample(
        MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &streamIndex, &flags, &sampleTime, sample.put());
    if (FAILED(read)) {
      warnings_.push_back("Media asset " + assetId + " failed while decoding a Program frame.");
      return false;
    }
    if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
      state.ended = true;
      return state.lastFrame.hasPixels();
    }
    if (!sample) {
      return state.lastFrame.hasPixels();
    }
    ComPtrLite<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(buffer.put()))) {
      return false;
    }
    BYTE* data = nullptr;
    DWORD maxLength = 0;
    DWORD currentLength = 0;
    if (FAILED(buffer->Lock(&data, &maxLength, &currentLength)) || !data || currentLength == 0) {
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
      return false;
    }
    const int stride = static_cast<int>(width) * 4;
    auto pixels = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(stride) * static_cast<size_t>(height));
    std::memcpy(pixels->data(), data, pixels->size());
    buffer->Unlock();

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
    const HRESULT read = state.audioReader->ReadSample(
        MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &streamIndex, &flags, &sampleTime, sample.put());
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

    frame.participantId = "media";
    frame.sampleRate = static_cast<int>(sampleRate);
    frame.channels = static_cast<int>(channels);
    frame.timestampMs = timestampMs;
    frame.sampleCount = static_cast<int>(floatCount / static_cast<size_t>(channels));
    frame.voiceActive = true;
    frame.pcm = std::move(pcm);
    return frame.sampleCount > 0;
  }

  bool comInitialized_ = false;
  bool mfStarted_ = false;
  ComPtrLite<IWICImagingFactory> wicFactory_;
  std::map<std::string, AssetState> states_;
  std::vector<std::string> warnings_;
};

}  // namespace

std::unique_ptr<IMediaFrameSource> createMediaFoundationMediaFrameSource() {
  return std::make_unique<MediaFoundationMediaFrameSource>();
}

}  // namespace corevideo::modules

#else

namespace corevideo::modules {

std::unique_ptr<IMediaFrameSource> createMediaFoundationMediaFrameSource() {
  return nullptr;
}

}  // namespace corevideo::modules

#endif
