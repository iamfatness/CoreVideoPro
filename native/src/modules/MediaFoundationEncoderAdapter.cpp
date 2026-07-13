#include "modules/Interfaces.h"
#include "modules/RecordingPtsClock.h"

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
    // RESET ALL per-session state before configuring the new sink writer. This
    // writer object is REUSED across recording sessions (every encoder start()
    // reopens it), and stream indices / audioConfigured_ describe the PREVIOUS
    // IMFSinkWriter. A stale audioConfigured_==true made ensureAudioStream skip
    // AddStream on the new writer, so every audio WriteSample hit the missing
    // stream index and failed with MF_E_INVALIDSTREAMNUMBER (0xC00D36B3) — a
    // video-only MP4 while the master bus carried live program audio (the
    // 2026-07-13 alpha-blocking zero-audio-recording bug: start-program-output
    // opened generation 1, start-recording-session reopened generation 2).
    sinkWriter_.Reset();
    open_ = false;
    writing_ = false;
    audioConfigured_ = false;
    videoStreamIndex_ = 0;
    audioStreamIndex_ = 0;
    videoFrameCount_ = 0;
    audioPacketCount_ = 0;
    audioSampleCount_ = 0;
    audioWriteFailureCount_ = 0;
    bytesWritten_ = 0;
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
  bool ensureAudioStream(int channels, int sampleRate, int audioBitrateKbps, std::string& errorOut) {
    if (!open_ || audioConfigured_) {
      return audioConfigured_;
    }
    audioChannels_ = std::clamp(channels, 1, 2);
    audioSampleRate_ = sampleRate > 0 ? sampleRate : 48000;
    audioBitrateKbps_ = std::clamp(audioBitrateKbps, 32, 512);

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
    outputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, static_cast<UINT32>((audioBitrateKbps_ * 1000) / 8));
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
  bool writeVideo(const uint8_t* bgra, int srcWidth, int srcHeight, int srcStride, LONGLONG pts100ns,
                  std::string& errorOut) {
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
    // Shared-epoch wall-clock PTS from RecordingPtsClock (spec 4.3): frames
    // carry the time they were actually composed, so the video timeline stays
    // aligned with the sample-counted audio track instead of drifting at
    // (muxed fps / nominal fps) of real time. Nominal duration is fine — the
    // container's frame timing comes from the PTS deltas (VFR MP4).
    sample->SetSampleTime(pts100ns);
    sample->SetSampleDuration(frameDuration100ns_);

    result = sinkWriter_->WriteSample(videoStreamIndex_, sample.Get());
    if (FAILED(result)) {
      errorOut = "write video sample: " + hresultString(result);
      return false;
    }
    ++videoFrameCount_;
    bytesWritten_ += byteCount;
    return true;
  }

  // Converts interleaved float PCM [-1,1] to 16-bit and writes one audio sample.
  bool writeAudio(const float* interleaved, int frameCount, LONGLONG pts100ns, std::string& errorOut) {
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
    // Sample-counted PTS with a shared-epoch base offset (RecordingPtsClock):
    // gapless within the track, aligned to the video timeline at its start.
    sample->SetSampleTime(pts100ns);
    sample->SetSampleDuration(duration100ns);

    result = sinkWriter_->WriteSample(audioStreamIndex_, sample.Get());
    if (FAILED(result)) {
      // LOUD failure (rate-limited): a recording quietly muxing zero audio is
      // an alpha-blocking failure mode — log the first drop and every 250th
      // (~every 5s at the 50Hz worker cadence) so it can never hide again.
      ++audioWriteFailureCount_;
      if (audioWriteFailureCount_ == 1 || audioWriteFailureCount_ % 250 == 0) {
        std::fprintf(stderr,
                     "[recording] program audio WriteSample FAILED hr=%s stream=%lu (failure %lld) file=%s\n",
                     hresultString(result).c_str(), static_cast<unsigned long>(audioStreamIndex_),
                     static_cast<long long>(audioWriteFailureCount_), path_.string().c_str());
      }
      errorOut = "write audio sample: " + hresultString(result);
      return false;
    }
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
  int audioBitrateKbps() const { return audioBitrateKbps_; }
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
  int audioChannels_ = 2;
  int audioSampleRate_ = 48000;
  int audioBitrateKbps_ = 192;
  std::string videoCodec_ = "h264";
  int64_t videoFrameCount_ = 0;
  int64_t audioPacketCount_ = 0;
  int64_t audioSampleCount_ = 0;
  int64_t audioWriteFailureCount_ = 0;
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
    session_.recordingAudioBitrateKbps = request.audioBitrateKbps;
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
    session_.recordingAudioBitrateKbps = request_.audioBitrateKbps;
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
    // Shared-epoch clock (spec 4.3): the audio worker re-submits the LATEST
    // program frame every ~20ms tick; the clock dedups by frameNumber so each
    // composed frame is muxed exactly once, timestamped at the wall time it
    // was submitted — keeping the video timeline aligned with the
    // sample-counted audio track instead of drifting monotonically apart.
    const auto pts = recordingClock_.videoPts(now100ns(), frame.frameNumber);
    if (!pts) {
      return;  // this program frame is already in the file
    }
    const int srcStride = preview.width * 4;
    std::string error;
    if (!program_.writeVideo(preview.bgra.data(), preview.width, preview.height, srcStride, *pts, error)) {
      setRecordingFailure("Media Foundation could not write program video", error);
      return;
    }
    // ISO writers mux the same composed frame per selected participant. The
    // IEncoderSink boundary does not (yet) carry per-source pixel buffers, so the
    // composed program frame is the per-participant proxy; each writer is a real,
    // independently playable MP4 with its routed audio.
    for (auto& iso : isoWriters_) {
      iso.writeVideo(preview.bgra.data(), preview.width, preview.height, srcStride, *pts, error);
    }

    ++session_.recordingVideoFrameCount;
    session_.recordingDurationMs = recordingClock_.lastVideoPts100ns() / 10'000;
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
      // The audio stream is configured up front in openRecordingWriters (it
      // cannot be added after BeginWriting); if it is missing here the open
      // already failed and surfaced a recording failure.
      return;
    }
    if (channels != program_.audioChannels() || sampleRate != program_.audioSampleRate()) {
      // Defensive: the program tap is canonically 48kHz stereo. Drop mismatched
      // buffers (with a visible warning) rather than mux garbage or kill the
      // whole recording session.
      if (session_.recordingWarning.empty()) {
        session_.recordingWarning =
            "Program audio format " + std::to_string(sampleRate) + "Hz/" + std::to_string(channels) +
            "ch does not match the recording AAC stream (" + std::to_string(program_.audioSampleRate()) +
            "Hz/" + std::to_string(program_.audioChannels()) + "ch); dropping audio buffers.";
      }
      return;
    }

    // Down/area-mix to the program writer's channel count is handled by MF's PCM
    // input type matching what we declared; we pass channels through unchanged
    // since the program tap is already stereo. Convert/write float -> 16-bit PCM.
    const LONGLONG audioPts = recordingClock_.audioPts(now100ns(), frameCount, sampleRate);
    if (program_.writeAudio(interleaved, frameCount, audioPts, error)) {
      session_.recordingAudioPacketCount = program_.audioPacketCount();
      session_.recordingAudioSampleCount = program_.audioSampleCount();
      session_.recordingAudioChannels = program_.audioChannels();
      session_.recordingAudioSampleRate = program_.audioSampleRate();
      session_.recordingAudioBitrateKbps = program_.audioBitrateKbps();
    } else if (!error.empty() &&
               session_.recordingWarning.rfind("Media Foundation dropped program audio", 0) != 0) {
      // A failed audio WriteSample used to vanish silently (video kept muxing,
      // the finalized MP4 just had no AAC track and nothing was surfaced).
      // Keep the recording alive but make the drop visible — and let it
      // OVERWRITE a benign earlier note (e.g. the format-fallback warning),
      // which previously masked the drop entirely.
      session_.recordingWarning = "Media Foundation dropped program audio: " + error + ".";
    }
    for (auto& iso : isoWriters_) {
      iso.writeAudio(interleaved, frameCount, audioPts, error);
    }
    updateBytesWritten();
  }

  void stopRecording() override {
    // Finalize the program + ISO writers so the MP4s gain their moov box and
    // are playable immediately. closeWriters() also refreshes
    // recordingBytesWritten from the finalized on-disk size. Subsequent
    // submit/submitAudio calls fall through safely (program_.writing() is
    // false), and the next start() reopens fresh writers.
    closeWriters();
    if (session_.recordingStatus == "recording" || session_.recordingStatus == "warning") {
      session_.recordingStatus = "stopped";
    }
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
    const std::string requestedFormat = request_.format.empty() ? "mp4" : request_.format;
    if (requestedFormat != "mp4" && session_.recordingWarning.empty()) {
      session_.recordingWarning = "Media Foundation recorder currently writes MP4 artifacts; requested " +
                                  requestedFormat + " will be recorded as MP4.";
    }

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

    // Configure the AAC audio stream UP FRONT. The MF sink writer rejects
    // AddStream after BeginWriting with MF_E_INVALIDREQUEST (0xC00D36B2), so the
    // previous "add the audio stream lazily on first audio" flow killed every
    // recording ~1 second in as soon as real program audio arrived (found by the
    // 2026-07-02 alpha soak: 10-min recording produced a 1-frame MP4 with
    // "could not add program AAC stream: 0xC00D36B2"). The program audio tap is
    // canonically 48kHz stereo; submitAudio drops mismatched formats with a
    // warning instead of failing the session.
    if (!program_.ensureAudioStream(2, 48000, request_.audioBitrateKbps, error)) {
      setRecordingFailure("Media Foundation could not add program AAC stream", error);
      return;
    }
    for (auto& iso : isoWriters_) {
      std::string isoError;
      iso.ensureAudioStream(2, 48000, request_.audioBitrateKbps, isoError);
    }

    // Begin writing only after BOTH streams are configured.
    if (!program_.beginWriting(error)) {
      setRecordingFailure("Media Foundation could not begin program writing", error);
      return;
    }
    for (auto& iso : isoWriters_) {
      std::string isoError;
      iso.beginWriting(isoError);
    }

    // Fresh shared-epoch clock per recording session: the epoch anchors to the
    // first media submitted after this point (spec 4.3).
    recordingClock_.reset();

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

  // 100ns ticks on the steady clock since sink construction — the time base
  // RecordingPtsClock anchors its shared epoch to.
  [[nodiscard]] LONGLONG now100ns() const {
    return std::chrono::duration_cast<std::chrono::duration<LONGLONG, std::ratio<1, 10'000'000>>>(
               std::chrono::steady_clock::now() - clockOrigin_)
        .count();
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
  // Shared-epoch A/V PTS clock (spec 4.3) + the steady-clock origin its 100ns
  // tick count is measured from. One clock serves program + ISO writers (they
  // mux the same frames on the same timeline).
  RecordingPtsClock recordingClock_;
  std::chrono::steady_clock::time_point clockOrigin_ = std::chrono::steady_clock::now();
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
