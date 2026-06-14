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
    return devices_;
  }

  std::vector<CaptureDeviceInfo> selectInput(const std::string& deviceId, const std::string& inputId) override {
    for (auto& device : devices_) {
      if (device.id != deviceId) {
        continue;
      }
      if (std::find(device.inputIds.begin(), device.inputIds.end(), inputId) != device.inputIds.end()) {
        device.selectedInputId = inputId;
      }
    }
    return enumerate();
  }

  std::vector<CaptureDeviceInfo> setAudioSyncOffset(const std::string& deviceId, int offsetMs) override {
    const int clamped = std::max(-500, std::min(500, offsetMs));
    for (auto& device : devices_) {
      if (device.id == deviceId) {
        device.audioSyncOffsetMs = clamped;
      }
    }
    return enumerate();
  }

  std::vector<CaptureDeviceInfo> connect(const std::string& deviceId) override {
    for (auto& device : devices_) {
      if (device.id == deviceId && device.connectionState != "connected") {
        device.connectionState = "connected";
        device.signalPresent = true;
      }
    }
    return enumerate();
  }

 private:
  std::vector<CaptureDeviceInfo> devices_ = {
      {"decklink-1",
       "DeckLink Mini Recorder 4K",
       "video",
       "blackmagic",
       {"sdi-1", "hdmi-1"},
       {"SDI 1", "HDMI"},
       {true, true},
       "sdi-1",
       1920,
       1080,
       60,
       "connected",
       true,
       0,
       0,
       ""},
      {"aja-io-1",
       "AJA Io 4K Plus",
       "video",
       "aja",
       {"sdi-1", "sdi-2"},
       {"SDI 1", "SDI 2"},
       {true, false},
       "sdi-1",
       1920,
       1080,
       30,
       "detected",
       false,
       0,
       0,
       ""},
  };
};

class CompositeCaptureDevice final : public ICaptureDevice {
 public:
  explicit CompositeCaptureDevice(std::vector<std::unique_ptr<ICaptureDevice>> devices) : devices_(std::move(devices)) {}

  std::vector<CaptureDeviceInfo> enumerate() const override {
    std::vector<CaptureDeviceInfo> result;
    for (const auto& device : devices_) {
      auto next = device->enumerate();
      result.insert(result.end(), next.begin(), next.end());
    }
    return result;
  }

  std::vector<CaptureDeviceInfo> selectInput(const std::string& deviceId, const std::string& inputId) override {
    for (const auto& device : devices_) {
      (void)device->selectInput(deviceId, inputId);
    }
    return enumerate();
  }

  std::vector<CaptureDeviceInfo> setAudioSyncOffset(const std::string& deviceId, int offsetMs) override {
    for (const auto& device : devices_) {
      (void)device->setAudioSyncOffset(deviceId, offsetMs);
    }
    return enumerate();
  }

  std::vector<CaptureDeviceInfo> connect(const std::string& deviceId) override {
    for (const auto& device : devices_) {
      (void)device->connect(deviceId);
    }
    return enumerate();
  }

 private:
  std::vector<std::unique_ptr<ICaptureDevice>> devices_;
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
  std::vector<std::unique_ptr<ICaptureDevice>> hardwareCaptureDevices;
  if (auto deckLink = createDeckLinkCaptureDevice()) {
    hardwareCaptureDevices.push_back(std::move(deckLink));
  }
  if (auto aja = createAjaCaptureDevice()) {
    hardwareCaptureDevices.push_back(std::move(aja));
  }
  if (!hardwareCaptureDevices.empty()) {
    modules.captureDevice = std::make_unique<CompositeCaptureDevice>(std::move(hardwareCaptureDevices));
  }
  return modules;
}

}  // namespace corevideo::modules
