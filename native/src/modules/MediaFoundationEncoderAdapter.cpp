#include "modules/Interfaces.h"

#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_MF_ENCODER

#include <mfapi.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>

namespace corevideo::modules {
namespace {

class MediaFoundationEncoderSink final : public IEncoderSink {
 public:
  MediaFoundationEncoderSink() {
    session_.encoderName = "media-foundation";
    session_.codec = "h264";
    session_.targetBitrateMbps = 18;
    session_.hardwareAccelerated = true;
  }

  ~MediaFoundationEncoderSink() override { MFShutdown(); }

  OutputSession start(const std::vector<std::string>& destinations, const std::vector<std::string>& isoParticipantIds) override {
    session_.active = true;
    session_.destinations = destinations;
    session_.isoParticipantIds = isoParticipantIds;
    session_.recordingArtifactPath.clear();
    session_.recordingBytesWritten = 0;
    session_.recordingWarning.clear();
    if (std::find(destinations.begin(), destinations.end(), "recording") != destinations.end()) {
      openRecordingProof();
    }
    return session_;
  }

  void submit(const ProgramFrame& frame) override {
    if (session_.active) {
      ++session_.encodedFrameCount;
      appendRecordingProof(frame);
    }
  }

  OutputSession session() const override { return session_; }

 private:
  void openRecordingProof() {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const auto path = std::filesystem::temp_directory_path() / ("corevideo-mf-recording-proof-" + std::to_string(now) + ".jsonl");
    recordingProof_.open(path, std::ios::out | std::ios::trunc);
    if (!recordingProof_) {
      session_.recordingWarning = "Media Foundation recording proof file could not be opened.";
      return;
    }
    session_.recordingArtifactPath = path.string();
    writeLine("{\"type\":\"recording-proof-start\",\"encoder\":\"media-foundation\",\"codec\":\"h264\"}");
  }

  void appendRecordingProof(const ProgramFrame& frame) {
    if (!recordingProof_.is_open()) {
      return;
    }
    writeLine("{\"type\":\"program-frame\",\"frameNumber\":" + std::to_string(frame.frameNumber) +
              ",\"width\":" + std::to_string(frame.width) +
              ",\"height\":" + std::to_string(frame.height) +
              ",\"layerCount\":" + std::to_string(frame.layerCount) +
              ",\"renderPlanId\":\"" + frame.renderPlanId + "\"}");
  }

  void writeLine(const std::string& line) {
    recordingProof_ << line << '\n';
    recordingProof_.flush();
    session_.recordingBytesWritten += static_cast<int64_t>(line.size() + 1);
  }

  OutputSession session_;
  std::ofstream recordingProof_;
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
