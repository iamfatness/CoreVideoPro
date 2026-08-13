#include "modules/AsyncEncoderSink.h"
#include "modules/AsyncOutputSender.h"
#include "modules/AudioDsp.h"
#include "modules/Interfaces.h"
#include "modules/ProgramFramePreview.h"
#include "modules/RealZoomCaptureSource.h"
#include "modules/WinUiCaptureDeviceAdapter.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <functional>
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
    VideoFrame first;
    first.participantId = "synthetic-speaker-1";
    first.width = 1280;
    first.height = 720;
    first.naturalWidth = 1280;
    first.naturalHeight = 720;
    first.timestampMs = frameNumber_ * 16;

    VideoFrame second;
    second.participantId = "synthetic-speaker-2";
    second.width = 1280;
    second.height = 720;
    second.naturalWidth = 1280;
    second.naturalHeight = 720;
    second.timestampMs = frameNumber_ * 16;
    return {first, second};
  }

  std::vector<AudioFrame> pollAudioFrames() override {
    // Fallback video keeps the UI renderable without a meeting. Audio must stay
    // silent here so meters and monitor output only represent real routed PCM.
    return {};
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

bool isFiniteRect(const CompositorLayerRect& rect) {
  return std::isfinite(rect.x) && std::isfinite(rect.y) && std::isfinite(rect.width) && std::isfinite(rect.height);
}

bool isKnownLayerKind(const std::string& kind) {
  return kind.empty() || kind == "participant-video" || kind == "screen-share" || kind == "media-video" || kind == "media-background" ||
         kind == "overlay" || kind == "chroma-key";
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
    buildMonitorBus(frames);
    return mixedFrameCount_;
  }

  AudioMixMetrics session() const override { return session_; }

  const std::vector<float>& monitorBusPcm() const override { return monitorBusPcm_; }
  int monitorBusSampleRate() const override { return kMonitorBusSampleRate; }
  int monitorBusChannels() const override { return kMonitorBusChannels; }

 private:
  // Sum every participant frame that carries real PCM into a stereo monitor bus
  // and brickwall-limit it to -1 dBFS so the summed signal never clips the
  // render device. Mono sources fan out to both channels; the first two
  // channels of multichannel sources feed L/R. Frames without PCM (metadata
  // only) contribute nothing, leaving the bus empty so the monitor stays armed
  // but silent rather than playing a fabricated tone.
  void buildMonitorBus(const std::vector<AudioFrame>& frames) {
    monitorBusPcm_.clear();
    size_t busFrames = 0;
    bool anyPcm = false;
    for (const auto& frame : frames) {
      if (frame.pcm.empty() || frame.channels <= 0) {
        continue;
      }
      anyPcm = true;
      busFrames = std::max(busFrames, frame.pcm.size() / static_cast<size_t>(frame.channels));
    }
    if (!anyPcm || busFrames == 0) {
      return;
    }
    monitorBusPcm_.assign(busFrames * static_cast<size_t>(kMonitorBusChannels), 0.0f);
    for (const auto& frame : frames) {
      if (frame.pcm.empty() || frame.channels <= 0) {
        continue;
      }
      const size_t sourceFrames = frame.pcm.size() / static_cast<size_t>(frame.channels);
      for (size_t index = 0; index < sourceFrames; ++index) {
        const float left = frame.pcm[index * static_cast<size_t>(frame.channels)];
        const float right = frame.channels == 1
                                ? left
                                : frame.pcm[index * static_cast<size_t>(frame.channels) + 1];
        monitorBusPcm_[index * kMonitorBusChannels] += left;
        monitorBusPcm_[index * kMonitorBusChannels + 1] += right;
      }
    }
    applyPeakLimiter(monitorBusPcm_.data(), monitorBusPcm_.size(), -1.0);
  }

  static constexpr int kMonitorBusSampleRate = 48000;
  static constexpr int kMonitorBusChannels = 2;
  int64_t mixedFrameCount_ = 0;
  std::map<std::string, int64_t> lastParticipantTimestamps_;
  AudioMixMetrics session_;
  std::vector<float> monitorBusPcm_;
};

// Default monitor output: a safe, host-independent stand-in that "accepts" the
// monitor bus without touching any audio hardware. Keeps MediaCore's monitor
// path exercisable in the stub build and in CI; the dev-gated WASAPI adapter
// (createWasapiMonitorOutput) replaces it on a real Windows rig.
class StubAudioMonitorOutput final : public IAudioMonitorOutput {
 public:
  bool start(const std::string& deviceId, int sampleRate, int channels) override {
    deviceId_ = deviceId;
    sampleRate_ = sampleRate;
    channels_ = channels;
    deviceName_ = deviceId.empty() ? "Stub Monitor (system default)" : "Stub Monitor (" + deviceId + ")";
    active_ = true;
    return true;
  }

  void stop() override { active_ = false; }

  bool render(const float* interleaved, int frameCount, int channels, double volume) override {
    if (!active_ || interleaved == nullptr || frameCount <= 0 || channels <= 0) {
      return false;
    }
    framesRendered_ += static_cast<int64_t>(frameCount);
    lastVolume_ = volume;
    return true;
  }

  bool active() const override { return active_; }
  std::string deviceName() const override { return deviceName_; }
  std::vector<std::string> warnings() const override { return {}; }

  // Test/introspection accessors (not part of the interface).
  int64_t framesRendered() const { return framesRendered_; }
  double lastVolume() const { return lastVolume_; }

 private:
  bool active_ = false;
  std::string deviceId_;
  std::string deviceName_;
  int sampleRate_ = 48000;
  int channels_ = 2;
  int64_t framesRendered_ = 0;
  double lastVolume_ = 0.0;
};

std::string participantIdForCaptureAudioSource(const CaptureAudioSourceConfig& source) {
  if (source.captureDeviceId == "local-machine-audio") {
    return source.captureDeviceId;
  }
  return "capture:" + source.captureDeviceId;
}

bool captureAudioSourceEnabled(const CaptureAudioSourceConfig& source) {
  return !source.captureDeviceId.empty() && source.audioSourceKind != "none";
}

double frequencyForCaptureAudioSource(const CaptureAudioSourceConfig& source) {
  std::hash<std::string> hasher;
  const auto seed = hasher(source.captureDeviceId + ":" + source.audioDeviceId + ":" + source.audioSourceKind);
  return 176.0 + static_cast<double>(seed % 220);
}

class StubAudioCaptureSource final : public IAudioCaptureSource {
 public:
  void configure(const std::vector<CaptureAudioSourceConfig>& sources) override {
    sources_.clear();
    for (const auto& source : sources) {
      if (captureAudioSourceEnabled(source)) {
        sources_.push_back(source);
      }
    }
  }

  std::vector<AudioFrame> pollAudioFrames(int64_t timestampMs) override {
    std::vector<AudioFrame> frames;
    frames.reserve(sources_.size());
    for (const auto& source : sources_) {
      frames.push_back(makeFrame(source, timestampMs));
    }
    ++tick_;
    return frames;
  }

  std::vector<CaptureAudioSourceMetrics> metrics() const override {
    std::vector<CaptureAudioSourceMetrics> metrics;
    metrics.reserve(sources_.size());
    for (const auto& source : sources_) {
      metrics.push_back(CaptureAudioSourceMetrics{
          source.captureDeviceId,
          participantIdForCaptureAudioSource(source),
          source.audioSourceKind,
          true,
          tick_ * 480,
          0,
          48000,
          2,
          source.nativeAudioDeviceId,
          source.audioDeviceName,
          {},
          {},
          -20.0,
          -23.0,
          true,
          tick_ * 480,
          0,
          0});
    }
    return metrics;
  }

 private:
  AudioFrame makeFrame(const CaptureAudioSourceConfig& source, int64_t timestampMs) const {
    AudioFrame frame;
    frame.participantId = participantIdForCaptureAudioSource(source);
    frame.sampleRate = 48000;
    frame.channels = 2;
    frame.timestampMs = timestampMs + source.audioSyncOffsetMs;
    frame.sampleCount = 480;
    frame.pcm.resize(static_cast<size_t>(frame.sampleCount) * static_cast<size_t>(frame.channels));

    const double frequencyHz = frequencyForCaptureAudioSource(source);
    const double amplitude = source.embedded ? 0.14 : 0.10;
    const int64_t baseSample = tick_ * frame.sampleCount;
    for (int index = 0; index < frame.sampleCount; ++index) {
      const double phase = 2.0 * kAudioPi * frequencyHz * static_cast<double>(baseSample + index) / frame.sampleRate;
      const auto sample = static_cast<float>(amplitude * std::sin(phase));
      frame.pcm[static_cast<size_t>(index) * 2] = sample;
      frame.pcm[static_cast<size_t>(index) * 2 + 1] = sample;
    }
    return frame;
  }

  std::vector<CaptureAudioSourceConfig> sources_;
  int64_t tick_ = 0;
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
  OutputSenderSession sync(
      const std::vector<std::string>& destinations,
      const ProgramFrame* frame,
      double elapsedMs,
      const std::vector<OutputDestinationSettings>& = {},
      const std::vector<float>* programAudioPcm = nullptr,
      int audioChannels = 0,
      int audioSampleRate = 0) override {
    const int programAudioFrames =
        programAudioPcm && !programAudioPcm->empty() && audioChannels > 0
            ? static_cast<int>(programAudioPcm->size() / static_cast<size_t>(audioChannels))
            : 0;
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
      if (programAudioFrames > 0) {
        sender.audioFramesSent += programAudioFrames;
        sender.audioBytesSent += static_cast<int64_t>(programAudioPcm->size() * sizeof(float));
        sender.audioChannels = audioChannels;
        sender.audioSampleRate = audioSampleRate;
      }
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

class CompositeOutputSender final : public IOutputSender {
 public:
  explicit CompositeOutputSender(std::vector<std::unique_ptr<IOutputSender>> senders)
      : senders_(std::move(senders)) {}

  OutputSenderSession sync(
      const std::vector<std::string>& destinations,
      const ProgramFrame* frame,
      double elapsedMs,
      const std::vector<OutputDestinationSettings>& destinationSettings = {},
      const std::vector<float>* programAudioPcm = nullptr,
      int audioChannels = 0,
      int audioSampleRate = 0) override {
    OutputSenderSession combined;
    for (const auto& sender : senders_) {
      mergeInto(combined, sender->sync(destinations, frame, elapsedMs, destinationSettings, programAudioPcm, audioChannels, audioSampleRate));
    }
    addMissingDestinationWarnings(combined, destinations, elapsedMs);
    finalize(combined);
    lastSession_ = combined;
    return combined;
  }

  // Fan out on the AUDIO cadence. Senders that carry no audio inherit the
  // no-op default, so this is safe for every member.
  void submitAudio(const std::vector<float>& pcm, int channels, int sampleRate) override {
    for (const auto& sender : senders_) {
      sender->submitAudio(pcm, channels, sampleRate);
    }
  }

  OutputSenderSession fail(const std::string& destination, const std::string& message, double elapsedMs) override {
    OutputSenderSession combined;
    for (const auto& sender : senders_) {
      mergeInto(combined, sender->fail(destination, message, elapsedMs));
    }
    finalize(combined);
    lastSession_ = combined;
    return combined;
  }

  OutputSenderSession recover(const std::string& destination, double elapsedMs, const std::string& reason) override {
    OutputSenderSession combined;
    for (const auto& sender : senders_) {
      mergeInto(combined, sender->recover(destination, elapsedMs, reason));
    }
    finalize(combined);
    lastSession_ = combined;
    return combined;
  }

  OutputSenderSession session() const override {
    OutputSenderSession combined;
    for (const auto& sender : senders_) {
      mergeInto(combined, sender->session());
    }
    if (combined.senders.empty() && !lastSession_.senders.empty()) {
      return lastSession_;
    }
    for (const auto& sender : lastSession_.senders) {
      if (!hasSenderFor(combined, sender.destination)) {
        combined.senders.push_back(sender);
      }
    }
    combined.warnings.insert(combined.warnings.end(), lastSession_.warnings.begin(), lastSession_.warnings.end());
    finalize(combined);
    return combined;
  }

  void interrupt(const std::string& destination) override {
    for (const auto& sender : senders_) {
      sender->interrupt(destination);
    }
  }

 private:
  static void mergeInto(OutputSenderSession& combined, const OutputSenderSession& next) {
    combined.senders.insert(combined.senders.end(), next.senders.begin(), next.senders.end());
    combined.warnings.insert(combined.warnings.end(), next.warnings.begin(), next.warnings.end());
  }

  static bool hasSenderFor(const OutputSenderSession& session, const std::string& destination) {
    return std::any_of(session.senders.begin(), session.senders.end(), [&](const OutputSender& sender) {
      return sender.destination == destination;
    });
  }

  static std::string uppercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return value;
  }

  static void addMissingDestinationWarnings(
      OutputSenderSession& session,
      const std::vector<std::string>& destinations,
      double elapsedMs) {
    for (const auto& destination : destinations) {
      if (!isNetworkDestination(destination) || hasSenderFor(session, destination)) {
        continue;
      }
      OutputSender sender;
      sender.senderId = destination + ":program";
      sender.destination = destination;
      sender.status = "warning";
      sender.startedAtMs = elapsedMs;
      sender.destinationHealth = "warning";
      sender.lastResultCode = destination + "-output-unavailable";
      sender.runtimeDetail = uppercase(destination) + " output sender is not available in this build.";
      sender.warning = uppercase(destination) + " output is selected, but no " + uppercase(destination) +
                       " sender module is available in this build.";
      session.senders.push_back(sender);
      session.warnings.push_back(sender.warning);
    }
  }

  static void finalize(OutputSenderSession& session) {
    bool hasFailure = false;
    bool hasWarning = false;
    session.activeSenderCount = 0;
    for (const auto& sender : session.senders) {
      if (sender.status == "live" || sender.status == "warning" || sender.status == "starting") {
        ++session.activeSenderCount;
      }
      hasFailure = hasFailure || sender.status == "failed";
      hasWarning = hasWarning || sender.status == "warning" || !sender.warning.empty();
    }
    session.status = hasFailure ? "failed" : hasWarning ? "warning" : session.activeSenderCount > 0 ? "live" : "idle";
  }

  std::vector<std::unique_ptr<IOutputSender>> senders_;
  OutputSenderSession lastSession_;
};

// Deterministic 7-bar SMPTE-style BGRA test pattern, shared (immutable) so each
// poll hands out a cheap reference rather than reallocating the buffer.
std::shared_ptr<const std::vector<uint8_t>> makeTestPatternBgra(int width, int height) {
  auto pixels = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
  static const uint8_t bars[7][3] = {
      // B, G, R
      {255, 255, 255},  // white
      {0, 255, 255},    // yellow
      {255, 255, 0},    // cyan
      {0, 255, 0},      // green  (center bar)
      {255, 0, 255},    // magenta
      {0, 0, 255},      // red
      {255, 0, 0},      // blue
  };
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const int bar = std::min(6, x * 7 / std::max(1, width));
      const size_t offset = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4;
      (*pixels)[offset + 0] = bars[bar][0];
      (*pixels)[offset + 1] = bars[bar][1];
      (*pixels)[offset + 2] = bars[bar][2];
      (*pixels)[offset + 3] = 255;
    }
  }
  return pixels;
}

class FakeCaptureDevice final : public ICaptureDevice {
 public:
  std::vector<CaptureDeviceInfo> enumerate() const override {
    return devices_;
  }

  // Connected devices with signal flow a real test-pattern frame into the native
  // core as a first-class capture source (participantId "capture:<deviceId>"),
  // so a scene route can composite a real-pixel program frame from a UVC/SDI
  // input exactly like the hardware adapters will.
  std::vector<VideoFrame> pollVideoFrames(int64_t timestampMs) override {
    std::vector<VideoFrame> frames;
    for (const auto& device : devices_) {
      if (device.connectionState != "connected" || !device.signalPresent) {
        continue;
      }
      VideoFrame frame;
      frame.participantId = "capture:" + device.id;
      frame.width = kCaptureWidth;
      frame.height = kCaptureHeight;
      frame.naturalWidth = kCaptureWidth;
      frame.naturalHeight = kCaptureHeight;
      frame.timestampMs = timestampMs;
      frame.pixels = testPattern_;
      frame.pixelWidth = kCaptureWidth;
      frame.pixelHeight = kCaptureHeight;
      frame.pixelStride = kCaptureWidth * 4;
      frame.frameId = ++frameId_;
      frames.push_back(std::move(frame));
    }
    return frames;
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
  static constexpr int kCaptureWidth = 640;
  static constexpr int kCaptureHeight = 360;
  std::shared_ptr<const std::vector<uint8_t>> testPattern_ = makeTestPatternBgra(kCaptureWidth, kCaptureHeight);
  int64_t frameId_ = 0;
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
       "",
       // Stub OS-level identity so the nativeDeviceId JSON contract has stub
       // coverage (mirrors the UVC adapter's symbolic-link field).
       "\\\\?\\stub#decklink-1"},
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

  std::vector<CaptureDeviceInfo> connect(const std::string& deviceId,
                                         const std::string& outputSourceId) override {
    for (const auto& device : devices_) {
      (void)device->connect(deviceId, outputSourceId);
    }
    return enumerate();
  }

  std::vector<CaptureDeviceInfo> disconnect(const std::string& deviceId) override {
    for (const auto& device : devices_) {
      (void)device->disconnect(deviceId);
    }
    return enumerate();
  }

  std::vector<CaptureDeviceInfo> configureSrtIngestSources(const std::vector<SrtIngestSourceConfig>& sources) override {
    for (const auto& device : devices_) {
      (void)device->configureSrtIngestSources(sources);
    }
    return enumerate();
  }

  std::vector<VideoFrame> pollVideoFrames(int64_t timestampMs) override {
    std::vector<VideoFrame> result;
    for (const auto& device : devices_) {
      auto frames = device->pollVideoFrames(timestampMs);
      result.insert(result.end(), frames.begin(), frames.end());
    }
    return result;
  }

  std::vector<AudioFrame> pollAudioFrames(int64_t timestampMs) override {
    std::vector<AudioFrame> result;
    for (const auto& device : devices_) {
      auto frames = device->pollAudioFrames(timestampMs);
      result.insert(result.end(), frames.begin(), frames.end());
    }
    return result;
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
  modules.mediaFrames = nullptr;
  modules.mixer = std::make_unique<DevSafeAudioMixer>();
  modules.monitorOutput = createStubAudioMonitorOutput();
  modules.audioCapture = createStubAudioCaptureSource();
  modules.encoder = createStubRecordingEncoderSink();
  modules.outputSender = std::make_unique<SyntheticOutputSender>();
  modules.captureDevice = std::make_unique<FakeCaptureDevice>();
  return modules;
}

std::unique_ptr<IAudioMonitorOutput> createStubAudioMonitorOutput() {
  return std::make_unique<StubAudioMonitorOutput>();
}

std::unique_ptr<IAudioCaptureSource> createStubAudioCaptureSource() {
  return std::make_unique<StubAudioCaptureSource>();
}

#if !defined(__APPLE__)
// The Metal factory's translation unit (MetalCompositorAdapter.mm) is only
// compiled on Apple toolchains; every other platform links this null factory
// (the same pattern as the D3D11 null factory on non-Windows).
std::unique_ptr<ICompositor> createMetalCompositor() {
  return nullptr;
}
std::unique_ptr<IEncoderSink> createAVFoundationEncoderSink() {
  return nullptr;
}
std::unique_ptr<ICaptureDevice> createAvfCaptureDevice() {
  return nullptr;
}
std::unique_ptr<ICaptureDevice> createSckScreenCaptureDevice() {
  return nullptr;
}
#endif

ModuleSet createDefaultModules() {
  auto modules = createStubModules();
  if (auto compositor = createD3D11Compositor()) {
    modules.compositor = std::move(compositor);
  } else if (auto metalCompositor = createMetalCompositor()) {
    modules.compositor = std::move(metalCompositor);
  }
  if (auto mediaFrames = createMediaFoundationMediaFrameSource()) {
    modules.mediaFrames = std::move(mediaFrames);
  }
  if (auto monitorOutput = createWasapiMonitorOutput()) {
    modules.monitorOutput = std::move(monitorOutput);
  } else if (auto coreAudioMonitor = createCoreAudioMonitorOutput()) {
    modules.monitorOutput = std::move(coreAudioMonitor);
  }
  if (auto audioCapture = createWasapiAudioCaptureSource()) {
    modules.audioCapture = std::move(audioCapture);
  } else if (auto coreAudioCapture = createCoreAudioCaptureSource()) {
    modules.audioCapture = std::move(coreAudioCapture);
  }
  if (auto avfEncoder = createAVFoundationEncoderSink()) {
    modules.encoder = std::move(avfEncoder);
  } else if (auto encoder = createMediaFoundationEncoderSink()) {
    modules.encoder = std::move(encoder);
  }
  std::vector<std::unique_ptr<IOutputSender>> outputSenders;
  if (auto outputSender = createRtmpOutputSender()) {
    outputSenders.push_back(std::move(outputSender));
  }
  if (auto srtSender = createSrtOutputSender()) {
    outputSenders.push_back(std::move(srtSender));
  }
  if (auto ndiSender = createNdiOutputSender()) {
    outputSenders.push_back(std::move(ndiSender));
  }
  if (!outputSenders.empty()) {
    modules.outputSender = std::make_unique<CompositeOutputSender>(std::move(outputSenders));
  }
  std::vector<std::unique_ptr<ICaptureDevice>> hardwareCaptureDevices;
  hardwareCaptureDevices.push_back(std::move(modules.captureDevice));
  if (auto srtIngest = createSrtIngestCaptureDevice()) {
    hardwareCaptureDevices.push_back(std::move(srtIngest));
  }
  if (auto deckLink = createDeckLinkCaptureDevice()) {
    hardwareCaptureDevices.push_back(std::move(deckLink));
  }
  if (auto aja = createAjaCaptureDevice()) {
    hardwareCaptureDevices.push_back(std::move(aja));
  }
  // Native UVC (Media Foundation) capture: webcams/capture cards enumerated and
  // streamed inside the core, no WinUI shared-memory hop (dev-gated; nullptr in
  // stub builds). The WinUiCaptureDeviceAdapter wrap below still supersedes
  // these frames for a device the shell bridges via shm, so the WinUI path
  // remains the fallback arbiter for the same device id.
  if (auto uvc = createUvcCaptureDevice()) {
    hardwareCaptureDevices.push_back(std::move(uvc));
  }
  // macOS camera capture (AVFoundation): the UVC twin, same arbitration rules.
  if (auto avfCameras = createAvfCaptureDevice()) {
    hardwareCaptureDevices.push_back(std::move(avfCameras));
  }
  // macOS screen/window capture (ScreenCaptureKit): the WGC twin.
  if (auto sckScreens = createSckScreenCaptureDevice()) {
    hardwareCaptureDevices.push_back(std::move(sckScreens));
  }
  // Screen capture (WGC): monitors as sources, same arbitration rules.
  if (auto wgcScreens = createWgcScreenCaptureDevice()) {
    hardwareCaptureDevices.push_back(std::move(wgcScreens));
  }
  if (hardwareCaptureDevices.size() == 1) {
    modules.captureDevice = std::move(hardwareCaptureDevices.front());
  } else if (!hardwareCaptureDevices.empty()) {
    modules.captureDevice = std::make_unique<CompositeCaptureDevice>(std::move(hardwareCaptureDevices));
  }
  // Bridge the WinUI shell's capture-card frames (Game Capture / Elgato / UVC) into
  // the core compositor via shared memory. Wraps the hardware/composite device so
  // metadata (enumerate/connect/...) is unchanged; real BGRA frames keyed by
  // "capture:<deviceId>" now reach the compositor (and thus recording/streaming).
  if (modules.captureDevice) {
    modules.captureDevice = std::make_unique<WinUiCaptureDeviceAdapter>(std::move(modules.captureDevice));
  }
  return modules;
}

ModuleSet createLiveServerModules() {
  auto modules = createDefaultModules();
  if (modules.encoder) {
    // Live-only: the audio/output worker submits a program frame + audio every tick,
    // so a blocking WriteSample (disk stall under load) would collapse the worker and
    // block the operator's stop-recording. AsyncEncoderSink drains the encoder onto a
    // dedicated writer thread (non-blocking submit + drop-to-latest backlog, bounded
    // finalize at teardown). Unit tests use createDefaultModules directly and keep the
    // synchronous encoder so their post-command assertions stay deterministic.
    modules.encoder = std::make_unique<AsyncEncoderSink>(std::move(modules.encoder));
  }
  if (modules.outputSender) {
    // FFmpeg/network backpressure must never run under the native core path.
    // The async sender keeps only the freshest frame and exposes an immediate
    // transport interrupt so Stop remains responsive even if a pipe is wedged.
    modules.outputSender = std::make_unique<AsyncOutputSender>(std::move(modules.outputSender));
  }
  return modules;
}

}  // namespace corevideo::modules
