#include "modules/Interfaces.h"

#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_MF_ENCODER

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <objbase.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
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

constexpr int kRecordingWidth = 1920;
constexpr int kRecordingHeight = 1080;
constexpr int kRecordingFps = 30;
constexpr LONGLONG kFrameDuration100ns = 10'000'000 / kRecordingFps;

std::string hresultString(HRESULT result) {
  std::ostringstream stream;
  stream << "0x" << std::hex << std::uppercase << static_cast<unsigned long>(result);
  return stream.str();
}

void fillSyntheticBgraFrame(std::vector<unsigned char>& pixels, const ProgramFrame& frame) {
  const auto frameSeed = static_cast<unsigned int>(frame.frameNumber);
  const auto layerSeed = static_cast<unsigned int>(std::max(0, frame.layerCount));
  for (int y = 0; y < kRecordingHeight; ++y) {
    for (int x = 0; x < kRecordingWidth; ++x) {
      const size_t offset = static_cast<size_t>((y * kRecordingWidth + x) * 4);
      const unsigned char red = static_cast<unsigned char>((x + frameSeed * 7) % 256);
      const unsigned char green = static_cast<unsigned char>((y + layerSeed * 31) % 256);
      const unsigned char blue = static_cast<unsigned char>(((x / 8) + (y / 8) + frameSeed * 13) % 256);
      pixels[offset + 0] = blue;
      pixels[offset + 1] = green;
      pixels[offset + 2] = red;
      pixels[offset + 3] = 0xFF;
    }
  }
}

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
    closeRecordingWriter();
    MFShutdown();
    if (comInitialized_) {
      CoUninitialize();
    }
  }

  OutputSession start(const std::vector<std::string>& destinations, const std::vector<std::string>& isoParticipantIds) override {
    closeRecordingWriter();
    session_.active = true;
    session_.destinations = destinations;
    session_.isoParticipantIds = isoParticipantIds;
    session_.recordingArtifactPath.clear();
    session_.recordingBytesWritten = 0;
    session_.recordingDurationMs = 0;
    session_.recordingVideoFrameCount = 0;
    session_.recordingWidth = 0;
    session_.recordingHeight = 0;
    session_.recordingFps = 0;
    session_.recordingContainerFormat.clear();
    session_.recordingVideoCodec.clear();
    session_.recordingAudioCodec.clear();
    session_.recordingMetadataValid = false;
    session_.recordingWarning.clear();
    if (std::find(destinations.begin(), destinations.end(), "recording") != destinations.end()) {
      openRecordingWriter();
    }
    return session_;
  }

  void submit(const ProgramFrame& frame) override {
    if (session_.active) {
      ++session_.encodedFrameCount;
      writeSyntheticProgramSample(frame);
    }
  }

  OutputSession session() const override { return session_; }

 private:
  void openRecordingWriter() {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    recordingPath_ = std::filesystem::temp_directory_path() / ("corevideo-mf-recording-" + std::to_string(now) + ".mp4");

    ComPtr<IMFAttributes> attributes;
    HRESULT result = MFCreateAttributes(&attributes, 1);
    if (FAILED(result)) {
      setRecordingFailure("Media Foundation could not create Sink Writer attributes", result);
      return;
    }
    attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);

    result = MFCreateSinkWriterFromURL(recordingPath_.wstring().c_str(), nullptr, attributes.Get(), &sinkWriter_);
    if (FAILED(result)) {
      setRecordingFailure("Media Foundation could not create MP4 Sink Writer", result);
      return;
    }

    ComPtr<IMFMediaType> outputType;
    result = MFCreateMediaType(&outputType);
    if (FAILED(result)) {
      setRecordingFailure("Media Foundation could not create H.264 output type", result);
      return;
    }
    outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    outputType->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(session_.targetBitrateMbps * 1'000'000));
    outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, kRecordingWidth, kRecordingHeight);
    MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE, kRecordingFps, 1);
    MFSetAttributeRatio(outputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    result = sinkWriter_->AddStream(outputType.Get(), &streamIndex_);
    if (FAILED(result)) {
      setRecordingFailure("Media Foundation could not add H.264 output stream", result);
      return;
    }

    ComPtr<IMFMediaType> inputType;
    result = MFCreateMediaType(&inputType);
    if (FAILED(result)) {
      setRecordingFailure("Media Foundation could not create RGB32 input type", result);
      return;
    }
    inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, kRecordingWidth, kRecordingHeight);
    MFSetAttributeRatio(inputType.Get(), MF_MT_FRAME_RATE, kRecordingFps, 1);
    MFSetAttributeRatio(inputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    result = sinkWriter_->SetInputMediaType(streamIndex_, inputType.Get(), nullptr);
    if (FAILED(result)) {
      setRecordingFailure("Media Foundation could not set RGB32 input type", result);
      return;
    }

    result = sinkWriter_->BeginWriting();
    if (FAILED(result)) {
      setRecordingFailure("Media Foundation could not begin MP4 writing", result);
      return;
    }

    writerOpen_ = true;
    frameTime100ns_ = 0;
    session_.recordingArtifactPath = recordingPath_.string();
    session_.recordingWidth = kRecordingWidth;
    session_.recordingHeight = kRecordingHeight;
    session_.recordingFps = kRecordingFps;
    session_.recordingContainerFormat = "mp4";
    session_.recordingVideoCodec = session_.codec;
    session_.recordingAudioCodec = "aac";
    session_.recordingMetadataValid = true;
  }

  void writeSyntheticProgramSample(const ProgramFrame& frame) {
    if (!writerOpen_ || !sinkWriter_) {
      return;
    }

    constexpr DWORD frameByteCount = static_cast<DWORD>(kRecordingWidth * kRecordingHeight * 4);
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT result = MFCreateMemoryBuffer(frameByteCount, &buffer);
    if (FAILED(result)) {
      setRecordingFailure("Media Foundation could not allocate sample buffer", result);
      return;
    }

    BYTE* locked = nullptr;
    DWORD maxLength = 0;
    DWORD currentLength = 0;
    result = buffer->Lock(&locked, &maxLength, &currentLength);
    if (FAILED(result)) {
      setRecordingFailure("Media Foundation could not lock sample buffer", result);
      return;
    }

    scratchPixels_.resize(frameByteCount);
    fillSyntheticBgraFrame(scratchPixels_, frame);
    std::copy(scratchPixels_.begin(), scratchPixels_.end(), locked);
    buffer->Unlock();
    buffer->SetCurrentLength(frameByteCount);

    ComPtr<IMFSample> sample;
    result = MFCreateSample(&sample);
    if (FAILED(result)) {
      setRecordingFailure("Media Foundation could not create sample", result);
      return;
    }
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(frameTime100ns_);
    sample->SetSampleDuration(kFrameDuration100ns);

    result = sinkWriter_->WriteSample(streamIndex_, sample.Get());
    if (FAILED(result)) {
      setRecordingFailure("Media Foundation could not write sample", result);
      return;
    }
    frameTime100ns_ += kFrameDuration100ns;
    ++session_.recordingVideoFrameCount;
    session_.recordingDurationMs = (session_.recordingVideoFrameCount * 1000) / kRecordingFps;
    session_.recordingBytesWritten += frameByteCount;
  }

  void closeRecordingWriter() {
    if (writerOpen_ && sinkWriter_) {
      const HRESULT result = sinkWriter_->Finalize();
      if (FAILED(result) && session_.recordingWarning.empty()) {
        session_.recordingWarning = "Media Foundation could not finalize MP4 recording: " + hresultString(result) + ".";
      }
    }
    writerOpen_ = false;
    sinkWriter_.Reset();
    if (!recordingPath_.empty() && std::filesystem::exists(recordingPath_)) {
      std::error_code error;
      const auto size = std::filesystem::file_size(recordingPath_, error);
      if (!error) {
        session_.recordingBytesWritten = static_cast<int64_t>(size);
      }
    }
  }

  void setRecordingFailure(const std::string& message, HRESULT result) {
    session_.recordingWarning = message + ": " + hresultString(result) + ".";
    writerOpen_ = false;
    sinkWriter_.Reset();
  }

  OutputSession session_;
  ComPtr<IMFSinkWriter> sinkWriter_;
  DWORD streamIndex_ = 0;
  LONGLONG frameTime100ns_ = 0;
  bool writerOpen_ = false;
  bool comInitialized_ = false;
  std::filesystem::path recordingPath_;
  std::vector<unsigned char> scratchPixels_;
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
