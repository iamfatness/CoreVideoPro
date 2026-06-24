#include "modules/Interfaces.h"

#include <algorithm>
#include <sstream>

namespace corevideo::modules {
namespace {

bool containsDestination(const std::vector<std::string>& destinations, const std::string& destination) {
  return std::find(destinations.begin(), destinations.end(), destination) != destinations.end();
}

std::string recordingProgramPath(const RecordingSessionRequest& request) {
  std::ostringstream path;
  path << request.targetFolder << "/" << request.filenamePrefix << "-program-0." << request.format;
  return path.str();
}

int64_t bytesPerProgramFrame(const RecordingSessionRequest& request) {
  const int64_t pixels = static_cast<int64_t>(std::max(1, request.width)) * static_cast<int64_t>(std::max(1, request.height));
  const int64_t qualityScale = request.quality == "low" ? 6 : request.quality == "medium" ? 8 : 10;
  return std::max<int64_t>(1, (pixels * qualityScale) / 80);
}

class StubRecordingEncoderSink final : public IEncoderSink {
 public:
  void configureRecording(const RecordingSessionRequest& request) override {
    request_ = request;
    if (!request.isoParticipantIds.empty()) {
      session_.isoParticipantIds = request.isoParticipantIds;
    }
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
  }

  OutputSession start(const std::vector<std::string>& destinations, const std::vector<std::string>& isoParticipantIds) override {
    session_.active = true;
    session_.destinations = destinations;
    session_.isoParticipantIds = isoParticipantIds.empty() ? request_.isoParticipantIds : isoParticipantIds;
    session_.encodedFrameCount = 0;
    session_.encoderName = "software-counting";
    session_.codec = request_.videoCodec.empty() ? "h264" : request_.videoCodec;
    session_.targetBitrateMbps = request_.targetBitrateMbps;
    session_.hardwareAccelerated = false;
    session_.recordingSessionId = request_.sessionId;
    session_.recordingTargetFolder = request_.targetFolder;
    session_.recordingFilenamePrefix = request_.filenamePrefix;
    session_.recordingFormat = request_.format;
    session_.recordingQuality = request_.quality;
    session_.recordingArtifactPath.clear();
    session_.recordingBytesWritten = 0;
    session_.recordingDurationMs = 0;
    session_.recordingVideoFrameCount = 0;
    session_.recordingLastFrameNumber = 0;
    session_.recordingWidth = 0;
    session_.recordingHeight = 0;
    session_.recordingFps = request_.fps;
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
    session_.recordingStatus = containsDestination(destinations, "recording") ? "recording" : "encoding";
    if (containsDestination(destinations, "recording")) {
      session_.recordingArtifactPath = recordingProgramPath(request_);
      session_.recordingContainerFormat = request_.format;
      session_.recordingVideoCodec = session_.codec;
      session_.recordingAudioCodec = request_.audioCodec;
      if (request_.format != "mp4") {
        session_.recordingWarning = "Stub recording session accepted non-mp4 format for state tracking only.";
      }
    }
    return session_;
  }

  void submit(const ProgramFrame& frame) override {
    if (!session_.active) {
      return;
    }

    ++session_.encodedFrameCount;
    if (!containsDestination(session_.destinations, "recording")) {
      return;
    }

    ++session_.recordingVideoFrameCount;
    session_.recordingLastFrameNumber = frame.frameNumber;
    session_.recordingDurationMs = (session_.recordingVideoFrameCount * 1000) / std::max(1, request_.fps);
    session_.recordingWidth = request_.width > 0 ? request_.width : frame.width;
    session_.recordingHeight = request_.height > 0 ? request_.height : frame.height;
    session_.recordingBytesWritten = session_.recordingVideoFrameCount * bytesPerProgramFrame(request_);
    session_.recordingMetadataValid = session_.recordingWidth > 0 && session_.recordingHeight > 0 && session_.recordingFps > 0 &&
                                      !session_.recordingContainerFormat.empty() && !session_.recordingVideoCodec.empty() &&
                                      !session_.recordingAudioCodec.empty();
  }

  void submitAudio(const float* interleaved, int frameCount, int channels, int sampleRate) override {
    if (!session_.active || interleaved == nullptr || frameCount <= 0 || channels <= 0) {
      return;
    }
    if (!containsDestination(session_.destinations, "recording")) {
      return;
    }
    // One muxed audio packet per delivered buffer, plus the real sample total,
    // with the muxed PCM payload folded into the recording size. This is the
    // honest "audio packets observed" signal the recording proof checks.
    ++session_.recordingAudioPacketCount;
    session_.recordingAudioSampleCount += static_cast<int64_t>(frameCount);
    session_.recordingAudioChannels = channels;
    session_.recordingAudioSampleRate = sampleRate;
    session_.recordingBytesWritten += static_cast<int64_t>(frameCount) * channels * 2;  // 16-bit muxed PCM
  }

  OutputSession session() const override { return session_; }

 private:
  RecordingSessionRequest request_;
  OutputSession session_;
};

}  // namespace

std::unique_ptr<IEncoderSink> createStubRecordingEncoderSink() {
  return std::make_unique<StubRecordingEncoderSink>();
}

}  // namespace corevideo::modules
