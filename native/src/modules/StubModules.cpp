#include "modules/AudioDsp.h"
#include "modules/Interfaces.h"
#include "modules/ProgramFramePreview.h"
#include "modules/RealZoomCaptureSource.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <map>
#include <optional>
#include <set>
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

uint32_t programPreviewSignature(const ProgramFramePreviewPixels& preview) {
  if (preview.width <= 0 || preview.height <= 0 || preview.bgra.empty()) {
    return 0;
  }

  uint32_t hash = 2166136261u;
  const auto mix = [&hash](uint8_t value) {
    hash ^= value;
    hash *= 16777619u;
  };
  mix(static_cast<uint8_t>(preview.width & 0xff));
  mix(static_cast<uint8_t>((preview.width >> 8) & 0xff));
  mix(static_cast<uint8_t>(preview.height & 0xff));
  mix(static_cast<uint8_t>((preview.height >> 8) & 0xff));
  for (const uint8_t value : preview.bgra) {
    mix(value);
  }
  return hash == 0 ? 1u : hash;
}

void mixHash(uint32_t& hash, uint8_t value) {
  hash ^= value;
  hash *= 16777619u;
}

void mixHash(uint32_t& hash, int value) {
  for (int shift = 0; shift < 32; shift += 8) {
    mixHash(hash, static_cast<uint8_t>((static_cast<uint32_t>(value) >> shift) & 0xffu));
  }
}

void mixHash(uint32_t& hash, float value) {
  const int quantized = static_cast<int>(std::lround(value * 10000.f));
  mixHash(hash, quantized);
}

void mixHash(uint32_t& hash, const std::string& value) {
  for (const unsigned char ch : value) {
    mixHash(hash, ch);
  }
  mixHash(hash, static_cast<uint8_t>(0xffu));
}

uint32_t renderPlanSignature(const CompositorRenderPlan& renderPlan) {
  uint32_t hash = 2166136261u;
  mixHash(hash, renderPlan.renderPlanId);
  mixHash(hash, renderPlan.sceneId);
  mixHash(hash, renderPlan.width);
  mixHash(hash, renderPlan.height);
  mixHash(hash, renderPlan.fps);
  for (const auto& layer : renderPlan.layers) {
    mixHash(hash, layer.layerId);
    mixHash(hash, layer.kind);
    mixHash(hash, layer.sourceId);
    mixHash(hash, layer.participantId);
    mixHash(hash, layer.order);
    mixHash(hash, layer.rect.x);
    mixHash(hash, layer.rect.y);
    mixHash(hash, layer.rect.width);
    mixHash(hash, layer.rect.height);
    mixHash(hash, compositorLayerOpacity(layer));
  }
  return hash == 0 ? 1u : hash;
}

bool isFiniteRect(const CompositorLayerRect& rect) {
  return std::isfinite(rect.x) && std::isfinite(rect.y) && std::isfinite(rect.width) && std::isfinite(rect.height);
}

bool isKnownLayerKind(const std::string& kind) {
  return kind.empty() || kind == "participant-video" || kind == "screen-share" || kind == "overlay" || kind == "chroma-key";
}

void addWarning(std::vector<std::string>& warnings, const std::string& warning) {
  if (std::find(warnings.begin(), warnings.end(), warning) == warnings.end()) {
    warnings.push_back(warning);
  }
}

CompositorRenderPlan validateRenderPlan(CompositorRenderPlan renderPlan) {
  std::vector<std::string> warnings = renderPlan.warnings;
  if (renderPlan.renderPlanId.empty()) {
    addWarning(warnings, "Render plan id is empty.");
    renderPlan.renderPlanId = "invalid-render-plan";
  }
  if (renderPlan.width <= 0 || renderPlan.height <= 0) {
    addWarning(warnings, "Render plan dimensions must be positive.");
    renderPlan.width = 1920;
    renderPlan.height = 1080;
  }
  if (renderPlan.fps <= 0 || renderPlan.fps > 240) {
    addWarning(warnings, "Render plan fps is outside the supported range.");
    renderPlan.fps = 30;
  }

  std::set<std::string> layerIds;
  for (size_t index = 0; index < renderPlan.layers.size(); ++index) {
    auto& layer = renderPlan.layers[index];
    const std::string layerLabel = layer.layerId.empty() ? "layer " + std::to_string(index) : layer.layerId;
    if (layer.layerId.empty()) {
      addWarning(warnings, "Scene layer " + std::to_string(index) + " is missing layerId.");
      layer.layerId = "invalid-layer:" + std::to_string(index);
    } else if (!layerIds.insert(layer.layerId).second) {
      addWarning(warnings, "Scene layer " + layer.layerId + " is duplicated.");
      layer.layerId += ":" + std::to_string(index);
    }
    if (!isKnownLayerKind(layer.kind)) {
      addWarning(warnings, "Scene layer " + layerLabel + " has unsupported kind " + layer.kind + ".");
      layer.kind = "overlay";
    }
    if (!std::isfinite(layer.opacity)) {
      addWarning(warnings, "Scene layer " + layerLabel + " opacity is not finite.");
      layer.opacity = 1.f;
    }
    if (!isFiniteRect(layer.rect)) {
      addWarning(warnings, "Scene layer " + layerLabel + " rect is not finite.");
      layer.rect = {};
    }
    if (layer.rect.width < 0.f || layer.rect.height < 0.f) {
      addWarning(warnings, "Scene layer " + layerLabel + " rect size is negative.");
      layer.rect.width = 0.f;
      layer.rect.height = 0.f;
    }
    if (!compositorLayerIsOverlay(layer) && layer.participantId.empty() && layer.sourceId.empty()) {
      addWarning(warnings, "Scene layer " + layerLabel + " has no source binding.");
    }
  }
  renderPlan.warnings = std::move(warnings);
  return renderPlan;
}

class CpuNoopCompositor final : public ICompositor {
 public:
  std::string rendererName() const override { return "software"; }

  ProgramFrame render(const CompositorRenderPlan& renderPlan, const std::vector<VideoFrame>& frames) override {
    ++frameNumber_;
    const auto deterministicPlan = sortCompositorRenderPlan(validateRenderPlan(renderPlan));
    const int layerCount = deterministicPlan.layers.empty() ? static_cast<int>(frames.size()) : static_cast<int>(deterministicPlan.layers.size());
    ProgramFrame frame;
    frame.width = deterministicPlan.width;
    frame.height = deterministicPlan.height;
    frame.layerCount = layerCount;
    frame.frameNumber = frameNumber_;
    frame.renderPlanId = deterministicPlan.renderPlanId;
    frame.renderer = "software";
    frame.health = deterministicPlan.warnings.empty() ? "live" : "degraded";
    frame.warnings = deterministicPlan.warnings;
    frame.renderPlanSignature = renderPlanSignature(deterministicPlan);
    fillSyntheticProgramFramePreview(frame.preview, deterministicPlan, frames, frame);
    frame.programPixelSignature = programPreviewSignature(frame.preview);
#if COREVIDEO_STUB
    frame.sharedTexture.sharedHandleHex = "0xFEEDFACE";
    frame.sharedTexture.width = frame.width;
    frame.sharedTexture.height = frame.height;
    frame.sharedTexture.format = "B8G8R8A8_UNORM";
    frame.sharedTexture.frameNumber = frame.frameNumber;
#endif
    return frame;
  }

 private:
  int64_t frameNumber_ = 0;
};

class DevSafeAudioMixer final : public IAudioMixer {
 public:
  int64_t mix(const std::vector<AudioFrame>& frames) override {
    mixedFrameCount_ += static_cast<int64_t>(frames.size());
    std::vector<AudioParticipantMixMetrics> participants;
    participants.reserve(frames.size());
    std::optional<int64_t> mixReferenceTimestamp;
    for (const auto& frame : frames) {
      const auto bounded = boundAudioFrame(frame);
      if (!mixReferenceTimestamp || bounded.timestampMs < *mixReferenceTimestamp) {
        mixReferenceTimestamp = bounded.timestampMs;
      }
    }
    for (const auto& frame : frames) {
      const auto bounded = boundAudioFrame(frame);
      AudioDspTimingReference timing;
      if (const auto previous = lastParticipantTimestamps_.find(bounded.participantId); previous != lastParticipantTimestamps_.end()) {
        timing.hasPreviousTimestamp = true;
        timing.previousTimestampMs = previous->second;
      }
      if (mixReferenceTimestamp) {
        timing.hasMixReferenceTimestamp = true;
        timing.mixReferenceTimestampMs = *mixReferenceTimestamp;
      }
      participants.push_back(analyzeAudioParticipantFrame(frame, timing));
      lastParticipantTimestamps_[bounded.participantId] = bounded.timestampMs;
    }
    session_ = summarizeAudioMixMetrics(std::move(participants), mixedFrameCount_);
    return mixedFrameCount_;
  }

  AudioMixMetrics session() const override { return session_; }

 private:
  int64_t mixedFrameCount_ = 0;
  std::map<std::string, int64_t> lastParticipantTimestamps_;
  AudioMixMetrics session_;
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

int64_t estimatedFrameBytes(double bitrateMbps) {
  return static_cast<int64_t>((bitrateMbps * 1000000.0 / 8.0 / 30.0) + 0.5);
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
        sender.destinationHealth = "stopped";
        sender.lastResultCode = "stopped";
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
        sender.destinationHealth = "starting";
        sender.lastResultCode = "waiting-for-frame";
      }

      if (sender.status == "failed") {
        continue;
      }

      if (!frame || frame->frameNumber == 0) {
        sender.status = "starting";
        sender.warning = uppercase(destination) + " sender is waiting for a program frame.";
        sender.destinationHealth = "starting";
        sender.lastResultCode = "waiting-for-frame";
        continue;
      }

      if (frame->health == "dropped") {
        sender.status = "warning";
        sender.warning = uppercase(destination) + " sender skipped a dropped program frame.";
        sender.destinationHealth = "warning";
        sender.lastResultCode = "dropped-frame";
        sender.lastError = sender.warning;
        ++sender.retryCount;
        continue;
      }

      sender.status = "live";
      sender.warning.clear();
      sender.lastFrameNumber = frame->frameNumber;
      ++sender.framesSent;
      sender.bytesSent += estimatedFrameBytes(sender.bitrateMbps);
      if (frame->health == "degraded") {
        sender.status = "warning";
        sender.warning = uppercase(destination) + " sender is publishing degraded program frames.";
        sender.destinationHealth = "warning";
        sender.lastResultCode = "degraded-frame";
        sender.lastError = sender.warning;
      } else {
        sender.destinationHealth = "ok";
        sender.lastResultCode = "ok";
      }
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
    sender.destinationHealth = "failed";
    sender.lastResultCode = "failed";
    sender.lastError = message;
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
    sender.destinationHealth = "starting";
    sender.lastResultCode = "recovered";
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
  // Real decoded Zoom frames are ingested into RealZoomCaptureSource by the
  // media-core tick. The synthetic source stays wired as the fallback so the
  // program/preview keeps rendering a slate when there is no meeting or no
  // participant video yet.
  modules.zoom = std::make_unique<RealZoomCaptureSource>(std::make_unique<SyntheticZoomCaptureSource>());
  modules.compositor = std::make_unique<CpuNoopCompositor>();
  modules.mixer = std::make_unique<DevSafeAudioMixer>();
  modules.encoder = createStubRecordingEncoderSink();
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
