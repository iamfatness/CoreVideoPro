#include "modules/Interfaces.h"

#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_MF_ENCODER

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <objbase.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace corevideo::modules {
namespace {

using Microsoft::WRL::ComPtr;

constexpr int kDefaultWidth = 1920;
constexpr int kDefaultHeight = 1080;
constexpr int kDefaultFps = 30;

std::string hresultString(HRESULT result) {
  std::ostringstream stream;
  stream << "0x" << std::hex << std::uppercase << static_cast<unsigned long>(result);
  return stream.str();
}

// Map a recording-command video codec name to an MF output subtype. Anything we
// do not explicitly recognize falls back to H.264, which is the guaranteed-
// compatible MP4 baseline.
GUID videoSubtypeForCodec(const std::string& codec) {
  if (codec == "hevc" || codec == "h265" || codec == "hvc1") {
    return MFVideoFormat_HEVC;
  }
  return MFVideoFormat_H264;
}

std::string canonicalVideoCodecName(const std::string& codec) {
  if (codec == "hevc" || codec == "h265" || codec == "hvc1") {
    return "hevc";
  }
  return "h264";
}

// A single MP4 sink writer (program or one ISO participant). Owns its own video
// + audio stream indices and per-stream timing, so program and ISO writers stay
// independent and individually finalizable.
class Mp4Writer {
 public:
  bool open(const std::filesystem::path& path, int width, int height, int fps, int bitrateMbps,
            const std::string& codec, std::string& errorOut) {
    path_ = path;
    width_ = std::max(2, width);
    height_ = std::max(2, height);
    fps_ = std::max(1, fps);
    frameDuration100ns_ = 10'000'000LL / fps_;
    videoCodec_ = canonicalVideoCodecName(codec);

    ComPtr<IMFAttributes> attributes;
    HRESULT result = MFCreateAttributes(&attributes, 1);
    if (FAILED(result)) {
      errorOut = "create Sink Writer attributes: " + hresultString(result);
      return false;
    }
    attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);

    result = MFCreateSinkWriterFromURL(path_.wstring().c_str(), nullptr, attributes.Get(), &sinkWriter_);
    if (FAILED(result)) {
      errorOut = "create MP4 Sink Writer: " + hresultString(result);
      return false;
    }

    // ---- Video stream ----
    ComPtr<IMFMediaType> outputType;
    result = MFCreateMediaType(&outputType);
    if (FAILED(result)) {
      errorOut = "create video output type: " + hresultString(result);
      return false;
    }
    outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outputType->SetGUID(MF_MT_SUBTYPE, videoSubtypeForCodec(codec));
    outputType->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(std::max(1, bitrateMbps) * 1'000'000));
    outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(width_), static_cast<UINT32>(height_));
    MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(fps_), 1);
    MFSetAttributeRatio(outputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    result = sinkWriter_->AddStream(outputType.Get(), &videoStreamIndex_);
    if (FAILED(result)) {
      errorOut = "add video stream: " + hresultString(result);
      return false;
    }

    ComPtr<IMFMediaType> inputType;
    result = MFCreateMediaType(&inputType);
    if (FAILED(result)) {
      errorOut = "create RGB32 input type: " + hresultString(result);
      return false;
    }
    inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    // Top-down BGRA: negative stride so row 0 is the top scanline, matching the
    // ProgramFrame preview/shared-texture layout.
    inputType->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(-(width_ * 4)));
    MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(width_), static_cast<UINT32>(height_));
    MFSetAttributeRatio(inputType.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(fps_), 1);
    MFSetAttributeRatio(inputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    result = sinkWriter_->SetInputMediaType(videoStreamIndex_, inputType.Get(), nullptr);
    if (FAILED(result)) {
      errorOut = "set RGB32 input type: " + hresultString(result);
      return false;
    }

    open_ = true;
    return true;
  }

  // Lazily configures the AAC audio stream the first time real audio arrives.
  // BeginWriting is deferred until both streams are configured, so the audio
  // stream can still be added after the first video frames.
  bool ensureAudioStream(int channels, int sampleRate, std::string& errorOut) {
    if (!open_ || audioConfigured_) {
      return audioConfigured_;
    }
    audioChannels_ = std::clamp(channels, 1, 2);
    audioSampleRate_ = sampleRate > 0 ? sampleRate : 48000;

    ComPtr<IMFMediaType> outputType;
    HRESULT result = MFCreateMediaType(&outputType);
    if (FAILED(result)) {
      errorOut = "create AAC output type: " + hresultString(result);
      return false;
    }
    outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    outputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    outputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    outputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, static_cast<UINT32>(audioSampleRate_));
    outputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, static_cast<UINT32>(audioChannels_));
    outputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 16000);  // 128 kbps AAC.
    outputType->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);

    result = sinkWriter_->AddStream(outputType.Get(), &audioStreamIndex_);
    if (FAILED(result)) {
      errorOut = "add AAC stream: " + hresultString(result);
      return false;
    }

    ComPtr<IMFMediaType> inputType;
    result = MFCreateMediaType(&inputType);
    if (FAILED(result)) {
      errorOut = "create PCM input type: " + hresultString(result);
      return false;
    }
    inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    inputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    inputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    inputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, static_cast<UINT32>(audioSampleRate_));
    inputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, static_cast<UINT32>(audioChannels_));
    inputType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, static_cast<UINT32>(audioChannels_ * 2));
    inputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                         static_cast<UINT32>(audioSampleRate_ * audioChannels_ * 2));

    result = sinkWriter_->SetInputMediaType(audioStreamIndex_, inputType.Get(), nullptr);
    if (FAILED(result)) {
      errorOut = "set PCM input type: " + hresultString(result);
      return false;
    }

    audioConfigured_ = true;
    return true;
  }

  bool beginWriting(std::string& errorOut) {
    if (!open_ || writing_) {
      return open_;
    }
    const HRESULT result = sinkWriter_->BeginWriting();
    if (FAILED(result)) {
      errorOut = "begin MP4 writing: " + hresultString(result);
      return false;
    }
    writing_ = true;
    return true;
  }

  // Writes one BGRA frame. `bgra` is tightly packed top-down at `srcStride`
  // bytes per row; pixels outside the configured size are letterbox/cropped to
  // fit so a resolution mismatch never corrupts the stream.
  bool writeVideo(const uint8_t* bgra, int srcWidth, int srcHeight, int srcStride, std::string& errorOut) {
    if (!writing_ || bgra == nullptr) {
      return false;
    }
    const int dstStride = width_ * 4;
    const DWORD byteCount = static_cast<DWORD>(dstStride) * static_cast<DWORD>(height_);

    ComPtr<IMFMediaBuffer> buffer;
    HRESULT result = MFCreateMemoryBuffer(byteCount, &buffer);
    if (FAILED(result)) {
      errorOut = "allocate video buffer: " + hresultString(result);
      return false;
    }
    BYTE* locked = nullptr;
    DWORD maxLength = 0;
    DWORD currentLength = 0;
    result = buffer->Lock(&locked, &maxLength, &currentLength);
    if (FAILED(result)) {
      errorOut = "lock video buffer: " + hresultString(result);
      return false;
    }
    const int copyWidth = std::min(srcWidth, width_) * 4;
    const int copyRows = std::min(srcHeight, height_);
    for (int y = 0; y < height_; ++y) {
      BYTE* dstRow = locked + static_cast<size_t>(y) * dstStride;
      if (y < copyRows) {
        const uint8_t* srcRow = bgra + static_cast<size_t>(y) * srcStride;
        std::memcpy(dstRow, srcRow, static_cast<size_t>(copyWidth));
        if (copyWidth < dstStride) {
          std::memset(dstRow + copyWidth, 0, static_cast<size_t>(dstStride - copyWidth));
        }
      } else {
        std::memset(dstRow, 0, static_cast<size_t>(dstStride));
      }
    }
    buffer->Unlock();
    buffer->SetCurrentLength(byteCount);

    ComPtr<IMFSample> sample;
    result = MFCreateSample(&sample);
    if (FAILED(result)) {
      errorOut = "create video sample: " + hresultString(result);
      return false;
    }
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(videoTime100ns_);
    sample->SetSampleDuration(frameDuration100ns_);

    result = sinkWriter_->WriteSample(videoStreamIndex_, sample.Get());
    if (FAILED(result)) {
      errorOut = "write video sample: " + hresultString(result);
      return false;
    }
    videoTime100ns_ += frameDuration100ns_;
    ++videoFrameCount_;
    bytesWritten_ += byteCount;
    return true;
  }

  // Converts interleaved float PCM [-1,1] to 16-bit and writes one audio sample.
  bool writeAudio(const float* interleaved, int frameCount, std::string& errorOut) {
    if (!writing_ || !audioConfigured_ || interleaved == nullptr || frameCount <= 0) {
      return false;
    }
    pcm16_.resize(static_cast<size_t>(frameCount) * audioChannels_);
    for (size_t i = 0; i < pcm16_.size(); ++i) {
      const float clamped = std::clamp(interleaved[i], -1.0f, 1.0f);
      pcm16_[i] = static_cast<int16_t>(std::lround(clamped * 32767.0f));
    }
    const DWORD byteCount = static_cast<DWORD>(pcm16_.size() * sizeof(int16_t));

    ComPtr<IMFMediaBuffer> buffer;
    HRESULT result = MFCreateMemoryBuffer(byteCount, &buffer);
    if (FAILED(result)) {
      errorOut = "allocate audio buffer: " + hresultString(result);
      return false;
    }
    BYTE* locked = nullptr;
    DWORD maxLength = 0;
    DWORD currentLength = 0;
    result = buffer->Lock(&locked, &maxLength, &currentLength);
    if (FAILED(result)) {
      errorOut = "lock audio buffer: " + hresultString(result);
      return false;
    }
    std::memcpy(locked, pcm16_.data(), byteCount);
    buffer->Unlock();
    buffer->SetCurrentLength(byteCount);

    const LONGLONG duration100ns =
        static_cast<LONGLONG>(frameCount) * 10'000'000LL / std::max(1, audioSampleRate_);
    ComPtr<IMFSample> sample;
    result = MFCreateSample(&sample);
    if (FAILED(result)) {
      errorOut = "create audio sample: " + hresultString(result);
      return false;
    }
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(audioTime100ns_);
    sample->SetSampleDuration(duration100ns);

    result = sinkWriter_->WriteSample(audioStreamIndex_, sample.Get());
    if (FAILED(result)) {
      errorOut = "write audio sample: " + hresultString(result);
      return false;
    }
    audioTime100ns_ += duration100ns;
    ++audioPacketCount_;
    audioSampleCount_ += frameCount;
    bytesWritten_ += byteCount;
    return true;
  }

  void finalize() {
    if (writing_ && sinkWriter_) {
      sinkWriter_->Finalize();
    }
    writing_ = false;
    open_ = false;
    sinkWriter_.Reset();
    // Prefer the on-disk size once the container is finalized.
    if (!path_.empty() && std::filesystem::exists(path_)) {
      std::error_code error;
      const auto size = std::filesystem::file_size(path_, error);
      if (!error) {
        bytesWritten_ = static_cast<int64_t>(size);
      }
    }
  }

  bool writing() const { return writing_; }
  bool audioConfigured() const { return audioConfigured_; }
  int width() const { return width_; }
  int height() const { return height_; }
  int fps() const { return fps_; }
  const std::string& videoCodec() const { return videoCodec_; }
  int64_t videoFrameCount() const { return videoFrameCount_; }
  int64_t audioPacketCount() const { return audioPacketCount_; }
  int64_t audioSampleCount() const { return audioSampleCount_; }
  int audioChannels() const { return audioChannels_; }
  int audioSampleRate() const { return audioSampleRate_; }
  int64_t bytesWritten() const { return bytesWritten_; }
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
  ComPtr<IMFSinkWriter> sinkWriter_;
  DWORD videoStreamIndex_ = 0;
  DWORD audioStreamIndex_ = 0;
  int width_ = kDefaultWidth;
  int height_ = kDefaultHeight;
  int fps_ = kDefaultFps;
  LONGLONG frameDuration100ns_ = 10'000'000LL / kDefaultFps;
  LONGLONG videoTime100ns_ = 0;
  LONGLONG audioTime100ns_ = 0;
  int audioChannels_ = 2;
  int audioSampleRate_ = 48000;
  std::string videoCodec_ = "h264";
  int64_t videoFrameCount_ = 0;
  int64_t audioPacketCount_ = 0;
  int64_t audioSampleCount_ = 0;
  int64_t bytesWritten_ = 0;
  bool open_ = false;
  bool writing_ = false;
  bool audioConfigured_ = false;
  std::vector<int16_t> pcm16_;
};

class MediaFoundationEncoderSink final : public IEncoderSink {
 public:
  MediaFoundationEncoderSink() {
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(comResult)) {
      comInitialized_ = true;
    } else if (comResult != RPC_E_CHANGED_MODE) {
      session_.recordingWarning = "Media Foundation COM initialization failed with " + hresultString(comResult) + ".";
    }
    session_.encoderName = "media-foundation";
    session_.codec = "h264";
    session_.targetBitrateMbps = 18;
    session_.hardwareAccelerated = true;
  }

  ~MediaFoundationEncoderSink() override {
    closeWriters();
    MFShutdown();
    if (comInitialized_) {
      CoUninitialize();
    }
  }

  void configureRecording(const RecordingSessionRequest& request) override {
    request_ = request;
    session_.recordingSessionId = request.sessionId;
    session_.recordingTargetFolder = request.targetFolder;
    session_.recordingFilenamePrefix = request.filenamePrefix;
    session_.recordingFormat = request.format;
    session_.recordingQuality = request.quality;
    session_.recordingContainerFormat = request.format;
    session_.recordingVideoCodec = request.videoCodec;
    session_.recordingAudioCodec = request.audioCodec;
    session_.recordingFps = request.fps;
    session_.targetBitrateMbps = request.targetBitrateMbps;
    if (!request.isoParticipantIds.empty()) {
      session_.isoParticipantIds = request.isoParticipantIds;
    }
  }

  OutputSession start(const std::vector<std::string>& destinations, const std::vector<std::string>& isoParticipantIds) override {
    closeWriters();
    session_.active = true;
    session_.destinations = destinations;
    session_.isoParticipantIds = isoParticipantIds.empty() ? request_.isoParticipantIds : isoParticipantIds;
    session_.encodedFrameCount = 0;
    session_.recordingArtifactPath.clear();
    session_.recordingBytesWritten = 0;
    session_.recordingDurationMs = 0;
    session_.recordingVideoFrameCount = 0;
    session_.recordingLastFrameNumber = 0;
    session_.recordingWidth = 0;
    session_.recordingHeight = 0;
    session_.recordingFps = 0;
    session_.recordingContainerFormat.clear();
    session_.recordingVideoCodec.clear();
    session_.recordingAudioCodec.clear();
    session_.recordingAudioPacketCount = 0;
    session_.recordingAudioSampleCount = 0;
    session_.recordingAudioChannels = 0;
    session_.recordingAudioSampleRate = 0;
    session_.recordingMetadataValid = false;
    session_.recordingWarning.clear();
    session_.recordingError.clear();
    session_.recordingSessionId = request_.sessionId;
    session_.recordingTargetFolder = request_.targetFolder;
    session_.recordingFilenamePrefix = request_.filenamePrefix;
    session_.recordingFormat = request_.format;
    session_.recordingQuality = request_.quality;
    session_.recordingStatus = "encoding";
    if (std::find(destinations.begin(), destinations.end(), "recording") != destinations.end()) {
      session_.recordingStatus = "recording";
      openRecordingWriters();
    }
    return session_;
  }

  void submit(const ProgramFrame& frame) override {
    if (!session_.active) {
      return;
    }
    ++session_.encodedFrameCount;
    session_.recordingLastFrameNumber = frame.frameNumber;
    if (!program_.writing()) {
      return;
    }

    // Feed the REAL composed program pixels (F1+F3). The preview carries the
    // tightly packed top-down BGRA the compositor produced this tick; when it is
    // empty (metadata-only tick) we skip the frame rather than synthesize one.
    const auto& preview = frame.preview;
    if (preview.bgra.empty() || preview.width <= 0 || preview.height <= 0) {
      return;
    }
    const int srcStride = preview.width * 4;
    std::string error;
    if (!program_.writeVideo(preview.bgra.data(), preview.width, preview.height, srcStride, error)) {
      setRecordingFailure("Media Foundation could not write program video", error);
      return;
    }
    // ISO writers mux the same composed frame per selected participant. The
    // IEncoderSink boundary does not (yet) carry per-source pixel buffers, so the
    // composed program frame is the per-participant proxy; each writer is a real,
    // independently playable MP4 with its routed audio.
    for (auto& iso : isoWriters_) {
      iso.writeVideo(preview.bgra.data(), preview.width, preview.height, srcStride, error);
    }

    ++session_.recordingVideoFrameCount;
    session_.recordingDurationMs = (session_.recordingVideoFrameCount * 1000) / std::max(1, program_.fps());
    updateBytesWritten();
  }

  void submitAudio(const float* interleaved, int frameCount, int channels, int sampleRate) override {
    if (!session_.active || interleaved == nullptr || frameCount <= 0 || channels <= 0) {
      return;
    }
    if (!program_.writing()) {
      return;
    }
    std::string error;
    if (!program_.audioConfigured()) {
      if (!program_.ensureAudioStream(channels, sampleRate, error)) {
        setRecordingFailure("Media Foundation could not add program AAC stream", error);
        return;
      }
      for (auto& iso : isoWriters_) {
        iso.ensureAudioStream(channels, sampleRate, error);
      }
      // Both video and audio streams are now configured; (re)begin writing.
      if (!program_.beginWriting(error)) {
        setRecordingFailure("Media Foundation could not begin writing", error);
        return;
      }
      for (auto& iso : isoWriters_) {
        iso.beginWriting(error);
      }
      session_.recordingAudioCodec = "aac";
    }

    // Down/area-mix to the program writer's channel count is handled by MF's PCM
    // input type matching what we declared; we pass channels through unchanged
    // since the program tap is already stereo. Convert/write float -> 16-bit PCM.
    if (program_.writeAudio(interleaved, frameCount, error)) {
      session_.recordingAudioPacketCount = program_.audioPacketCount();
      session_.recordingAudioSampleCount = program_.audioSampleCount();
      session_.recordingAudioChannels = program_.audioChannels();
      session_.recordingAudioSampleRate = program_.audioSampleRate();
    }
    for (auto& iso : isoWriters_) {
      iso.writeAudio(interleaved, frameCount, error);
    }
    updateBytesWritten();
  }

  OutputSession session() const override { return session_; }

 private:
  void openRecordingWriters() {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    const int width = request_.width > 0 ? request_.width : kDefaultWidth;
    const int height = request_.height > 0 ? request_.height : kDefaultHeight;
    const int fps = request_.fps > 0 ? request_.fps : kDefaultFps;
    const int bitrate = request_.targetBitrateMbps > 0 ? request_.targetBitrateMbps : 18;
    const std::string codec = request_.videoCodec.empty() ? "h264" : request_.videoCodec;

    const auto baseDir = resolveTargetDir();
    const std::string suffix = "-" + std::to_string(now) + ".mp4";

    const auto programPath = baseDir / (request_.filenamePrefix + "-program" + suffix);
    std::string error;
    if (!program_.open(programPath, width, height, fps, bitrate, codec, error)) {
      setRecordingFailure("Media Foundation could not open program MP4 writer", error);
      return;
    }

    // ISO: one real sink writer per selected participant, driven by the same
    // configured resolution/FPS/codec/bitrate.
    isoWriters_.clear();
    isoWriters_.resize(session_.isoParticipantIds.size());
    for (size_t i = 0; i < session_.isoParticipantIds.size(); ++i) {
      const auto isoPath =
          baseDir / (request_.filenamePrefix + "-iso-" + session_.isoParticipantIds[i] + suffix);
      std::string isoError;
      if (!isoWriters_[i].open(isoPath, width, height, fps, bitrate, codec, isoError) &&
          session_.recordingWarning.empty()) {
        session_.recordingWarning = "Media Foundation could not open ISO MP4 writer for " +
                                    session_.isoParticipantIds[i] + ": " + isoError + ".";
      }
    }

    // Begin writing now so program video flows even before the first audio tick;
    // the audio stream is added lazily and BeginWriting re-runs are guarded.
    if (!program_.beginWriting(error)) {
      setRecordingFailure("Media Foundation could not begin program writing", error);
      return;
    }
    for (auto& iso : isoWriters_) {
      std::string isoError;
      iso.beginWriting(isoError);
    }

    session_.recordingArtifactPath = programPath.string();
    session_.recordingWidth = program_.width();
    session_.recordingHeight = program_.height();
    session_.recordingFps = program_.fps();
    session_.recordingContainerFormat = "mp4";
    session_.recordingVideoCodec = program_.videoCodec();
    session_.codec = program_.videoCodec();
    session_.recordingAudioCodec = "aac";
    session_.recordingMetadataValid = true;
  }

  std::filesystem::path resolveTargetDir() {
    // Prefer the requested target folder when it is usable; otherwise fall back
    // to a temp directory so the writer always has a valid path on a dev rig.
    if (!request_.targetFolder.empty()) {
      std::error_code ec;
      std::filesystem::path target(request_.targetFolder);
      std::filesystem::create_directories(target, ec);
      if (!ec && std::filesystem::is_directory(target)) {
        return target;
      }
    }
    return std::filesystem::temp_directory_path();
  }

  void updateBytesWritten() {
    int64_t total = program_.bytesWritten();
    for (const auto& iso : isoWriters_) {
      total += iso.bytesWritten();
    }
    session_.recordingBytesWritten = total;
  }

  void closeWriters() {
    program_.finalize();
    for (auto& iso : isoWriters_) {
      iso.finalize();
    }
    if (session_.recordingWarning.empty()) {
      // Surface the finalized on-disk size of the program file.
      updateBytesWritten();
    }
    isoWriters_.clear();
  }

  void setRecordingFailure(const std::string& message, const std::string& detail) {
    session_.recordingWarning = message + ": " + detail + ".";
    session_.recordingError = session_.recordingWarning;
    session_.recordingStatus = "warning";
    program_.finalize();
    for (auto& iso : isoWriters_) {
      iso.finalize();
    }
    isoWriters_.clear();
  }

  OutputSession session_;
  RecordingSessionRequest request_;
  Mp4Writer program_;
  std::vector<Mp4Writer> isoWriters_;
  bool comInitialized_ = false;
};

}  // namespace

std::unique_ptr<IEncoderSink> createMediaFoundationEncoderSink() {
  // REQUIRES DEV MACHINE: Media Foundation is the Windows hardware encoder
  // gateway. Concrete NVENC/QuickSync/AMF selection belongs behind this facade.
  if (FAILED(MFStartup(MF_VERSION))) {
    return nullptr;
  }
  return std::make_unique<MediaFoundationEncoderSink>();
}

}  // namespace corevideo::modules

#else

namespace corevideo::modules {

std::unique_ptr<IEncoderSink> createMediaFoundationEncoderSink() {
  return nullptr;
}

}  // namespace corevideo::modules

#endif
