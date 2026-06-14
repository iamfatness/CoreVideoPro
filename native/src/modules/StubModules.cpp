#include "modules/Interfaces.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <utility>

namespace corevideo::modules {
namespace {

class SyntheticZoomCaptureSource final : public IZoomCaptureSource {
 public:
  std::vector<VideoFrame> pollVideoFrames() override {
    ++frameNumber_;
    return {
        {"synthetic-speaker-1", 1280, 720, frameNumber_ * 16},
        {"synthetic-speaker-2", 1280, 720, frameNumber_ * 16},
    };
  }

  std::vector<AudioFrame> pollAudioFrames() override {
    return {
        {"synthetic-speaker-1", 48000, 1, frameNumber_ * 16},
        {"synthetic-speaker-2", 48000, 1, frameNumber_ * 16},
    };
  }

 private:
  int64_t frameNumber_ = 0;
};

class CpuNoopCompositor final : public ICompositor {
 public:
  std::string rendererName() const override { return "software"; }

  ProgramFrame render(const CompositorRenderPlan& renderPlan, const std::vector<VideoFrame>& frames) override {
    ++frameNumber_;
    const int layerCount = renderPlan.layers.empty() ? static_cast<int>(frames.size()) : static_cast<int>(renderPlan.layers.size());
    return {renderPlan.width, renderPlan.height, layerCount, frameNumber_, renderPlan.renderPlanId, "software"};
  }

 private:
  int64_t frameNumber_ = 0;
};

class PassthroughAudioMixer final : public IAudioMixer {
 public:
  int64_t mix(const std::vector<AudioFrame>& frames) override {
    mixedFrameCount_ += static_cast<int64_t>(frames.size());
    return mixedFrameCount_;
  }

 private:
  int64_t mixedFrameCount_ = 0;
};

class CountingEncoderSink final : public IEncoderSink {
 public:
  OutputSession start(const std::vector<std::string>& destinations, const std::vector<std::string>& isoParticipantIds) override {
    session_.active = true;
    session_.destinations = destinations;
    session_.isoParticipantIds = isoParticipantIds;
    session_.encoderName = "software-counting";
    session_.codec = "h264";
    session_.targetBitrateMbps = 10;
    session_.hardwareAccelerated = false;
    return session_;
  }

  void submit(const ProgramFrame&) override {
    if (session_.active) {
      ++session_.encodedFrameCount;
    }
  }

  OutputSession session() const override { return session_; }

 private:
  OutputSession session_;
};

bool isNetworkDestination(const std::string& destination) {
  return destination == "rtmp" || destination == "ndi" || destination == "srt" || destination == "webrtc";
}

int latencyFor(const std::string& destination) {
  if (destination == "ndi") {
    return 80;
  }
  if (destination == "webrtc") {
    return 220;
  }
  if (destination == "srt") {
    return 420;
  }
  return 2100;
}

double bitrateFor(const std::string& destination) {
  if (destination == "ndi") {
    return 9.6;
  }
  if (destination == "webrtc") {
    return 4.8;
  }
  return 6.0;
}

class SyntheticOutputSender final : public IOutputSender {
 public:
  OutputSenderSession sync(const std::vector<std::string>& destinations, const ProgramFrame* frame, double elapsedMs) override {
    std::vector<std::string> activeDestinations;
    for (const auto& destination : destinations) {
      if (isNetworkDestination(destination) && std::find(activeDestinations.begin(), activeDestinations.end(), destination) == activeDestinations.end()) {
        activeDestinations.push_back(destination);
      }
    }

    for (auto& [destination, sender] : senders_) {
      if (std::find(activeDestinations.begin(), activeDestinations.end(), destination) == activeDestinations.end() && sender.status != "stopped") {
        sender.status = "stopped";
        sender.stoppedAtMs = elapsedMs;
        sender.warning.clear();
      }
    }

    for (const auto& destination : activeDestinations) {
      auto& sender = senders_[destination];
      if (sender.senderId.empty()) {
        sender.senderId = destination + ":program";
        sender.destination = destination;
        sender.startedAtMs = elapsedMs;
        sender.latencyMs = latencyFor(destination);
        sender.bitrateMbps = bitrateFor(destination);
      }

      if (sender.status == "failed") {
        continue;
      }

      if (!frame || frame->frameNumber == 0) {
        sender.status = "starting";
        sender.warning = uppercase(destination) + " sender is waiting for a program frame.";
        continue;
      }

      sender.status = "live";
      sender.warning.clear();
      sender.lastFrameNumber = frame->frameNumber;
      ++sender.framesSent;
    }

    return snapshot();
  }

  OutputSenderSession fail(const std::string& destination, const std::string& message, double elapsedMs) override {
    if (!isNetworkDestination(destination)) {
      return snapshot();
    }
    auto& sender = senders_[destination];
    if (sender.senderId.empty()) {
      sender.senderId = destination + ":program";
      sender.destination = destination;
      sender.latencyMs = latencyFor(destination);
      sender.bitrateMbps = 0;
    }
    sender.status = "failed";
    sender.stoppedAtMs = elapsedMs;
    ++sender.retryCount;
    sender.warning = message;
    return snapshot();
  }

  OutputSenderSession recover(const std::string& destination, double elapsedMs, const std::string& reason) override {
    if (!isNetworkDestination(destination)) {
      return snapshot();
    }
    auto& sender = senders_[destination];
    if (sender.senderId.empty()) {
      sender.senderId = destination + ":program";
      sender.destination = destination;
      sender.latencyMs = latencyFor(destination);
      sender.bitrateMbps = bitrateFor(destination);
    }
    sender.status = "starting";
    sender.startedAtMs = elapsedMs;
    sender.stoppedAtMs = 0;
    sender.warning = reason.empty() ? uppercase(destination) + " sender recovered." : reason;
    return snapshot();
  }

  OutputSenderSession session() const override { return snapshot(); }

 private:
  static std::string uppercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return value;
  }

  OutputSenderSession snapshot() const {
    OutputSenderSession session;
    bool hasFailure = false;
    bool hasWarning = false;
    for (const auto& [_, sender] : senders_) {
      session.senders.push_back(sender);
      if (sender.status == "live" || sender.status == "warning" || sender.status == "starting") {
        ++session.activeSenderCount;
      }
      hasFailure = hasFailure || sender.status == "failed";
      hasWarning = hasWarning || sender.status == "warning" || !sender.warning.empty();
      if (!sender.warning.empty()) {
        session.warnings.push_back(sender.warning);
      }
    }
    session.status = hasFailure ? "failed" : hasWarning ? "warning" : session.activeSenderCount > 0 ? "live" : "idle";
    return session;
  }

  std::map<std::string, OutputSender> senders_;
};

class FakeCaptureDevice final : public ICaptureDevice {
 public:
  std::vector<CaptureDeviceInfo> enumerate() const override {
    return {
        {"fake-camera-1", "CoreVideo Synthetic Camera", "video"},
        {"fake-microphone-1", "CoreVideo Synthetic Microphone", "audio"},
    };
  }
};

}  // namespace

ModuleSet createStubModules() {
  ModuleSet modules;
  modules.zoom = std::make_unique<SyntheticZoomCaptureSource>();
  modules.compositor = std::make_unique<CpuNoopCompositor>();
  modules.mixer = std::make_unique<PassthroughAudioMixer>();
  modules.encoder = std::make_unique<CountingEncoderSink>();
  modules.outputSender = std::make_unique<SyntheticOutputSender>();
  modules.captureDevice = std::make_unique<FakeCaptureDevice>();
  return modules;
}

ModuleSet createDefaultModules() {
  auto modules = createStubModules();
  if (auto compositor = createD3D11Compositor()) {
    modules.compositor = std::move(compositor);
  }
  if (auto encoder = createMediaFoundationEncoderSink()) {
    modules.encoder = std::move(encoder);
  }
  if (auto outputSender = createRtmpOutputSender()) {
    modules.outputSender = std::move(outputSender);
  }
  return modules;
}

}  // namespace corevideo::modules
