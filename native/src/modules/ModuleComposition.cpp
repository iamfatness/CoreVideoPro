// Production module composition: which real adapters a core holds and how they are
// combined. createDefaultModules() starts from the stub set (StubModules.cpp) and
// replaces each module whose real adapter constructs; createLiveServerModules() adds
// the worker-thread wrappers the live process needs. Nothing in this file is a stub.
#include "modules/AsyncEncoderSink.h"
#include "modules/AsyncOutputSender.h"
#include "modules/AudioDsp.h"
#include "core/TestPattern.h"
#include "modules/Interfaces.h"
#include "modules/ProgramStreamClock.h"
#include "modules/IsolatedOutputSender.h"
#include "modules/OutputDestinationSupervisor.h"
#include "modules/ProgramFramePreview.h"
#include "modules/WinUiCaptureDeviceAdapter.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <utility>

namespace corevideo::modules {
namespace {

bool isNetworkDestination(const std::string& destination) {
  return destination == "rtmp" || destination == "ndi" || destination == "srt" || destination == "webrtc";
}

class CompositeOutputSender final : public IOutputSender {
 public:
  explicit CompositeOutputSender(std::vector<std::unique_ptr<IOutputSender>> senders,
      std::vector<std::string> supported = {})
      : senders_(std::move(senders)), supportedDestinations_(std::move(supported)) {}

  // One writer thread per protocol, and (PR19) one SUPERVISOR per protocol
  // outside it. Order matters and is the isolation argument: the supervisor
  // must sit OUTSIDE the async writer, so every call it makes lands on the
  // non-blocking queue rather than on a wedged FFmpeg pipe or libNDI send. See
  // the header comment in OutputDestinationSupervisor.h.
  void enableIndependentWriters(bool supervised = true) {
    const bool hasNetworkMux = std::any_of(supportedDestinations_.begin(), supportedDestinations_.end(),
        [](const std::string& destination) {
          return destination == "rtmp" || destination == "srt" || destination == "hls";
        });
    sharedAacEnabled_ = hasNetworkMux && sharedAac_.start();
    sharedAacConsumers_.clear();
    for (std::size_t index = 0; index < senders_.size(); ++index) {
      auto& sender = senders_[index];
      sharedAacConsumers_.push_back(sender->acceptsSharedAac());
      sender->setSharedAacEnabled(sharedAacEnabled_);
      sender = std::make_unique<AsyncOutputSender>(std::move(sender));
      if (supervised) {
        // Each supervisor learns the one destination its sender serves (#602);
        // supportedDestinations_ is index-aligned with senders_ (see fail()).
        SupervisedOutputSender::Options options;
        if (index < supportedDestinations_.size()) {
          options.servedDestinations = {supportedDestinations_[index]};
        }
        sender = std::make_unique<SupervisedOutputSender>(std::move(sender), std::move(options));
      }
    }
  }

  OutputSenderSession sync(
      const std::vector<std::string>& destinations,
      const ProgramFrame* frame,
      double elapsedMs,
      const std::vector<OutputDestinationSettings>& destinationSettings = {},
      const std::vector<float>* programAudioPcm = nullptr,
      int audioChannels = 0,
      int audioSampleRate = 0) override {
    if (sharedAacEnabled_) {
      const bool streamSession = std::any_of(destinations.begin(), destinations.end(),
          [](const std::string& destination) {
            return destination == "rtmp" || destination == "srt" || destination == "hls";
          });
      streamClock_.observeProgramFrame(frame ? frame->frameNumber : 0,
                                       frame ? frame->timelineTimestamp100ns : 0, streamSession);
    }
    OutputSenderSession combined;
    for (const auto& sender : senders_) {
      mergeInto(combined, sender->sync(destinations, frame, elapsedMs, destinationSettings, programAudioPcm, audioChannels, audioSampleRate));
    }
    addMissingDestinationWarnings(combined, destinations, elapsedMs);
    finalize(combined);
    { std::lock_guard<std::mutex> lock(sessionMutex_); lastSession_ = combined; }
    return combined;
  }

  // Fan out on the AUDIO cadence. Senders that carry no audio inherit the
  // no-op default, so this is safe for every member.
  void submitAudio(const std::vector<float>& pcm, int channels, int sampleRate) override {
    submitAudioAt(pcm, channels, sampleRate, 0);
  }

  void submitAudioAt(const std::vector<float>& pcm, int channels, int sampleRate,
                     int64_t timelineTimestamp100ns) override {
    if (sharedAacEnabled_ && !sharedAacFailed_) {
      std::vector<ProgramAacPacket> packets;
      streamClock_.beginAudioBlock(sharedAac_.acceptedSamples(), timelineTimestamp100ns);
      ProgramStreamClock::AnchorEvidence anchor{};
      if (streamClock_.takeAnchorEvidence(anchor)) {
        ::corevideo::core::nativeLogf(
            "[stream-clock] session anchor frame=%lld sample=%lld offset100ns=%lld frameTimeline100ns=%lld audioTimeline100ns=%lld\n",
            static_cast<long long>(anchor.frame), static_cast<long long>(anchor.sample),
            static_cast<long long>(anchor.offset100ns), static_cast<long long>(anchor.frameTimeline100ns),
            static_cast<long long>(anchor.audioTimeline100ns));
      }
      if (sharedAac_.encode(pcm, channels, sampleRate, packets)) {
        for (auto& packet : packets) {
          streamClock_.stamp(packet);
          for (size_t index = 0; index < senders_.size(); ++index) {
            if (sharedAacConsumers_[index]) senders_[index]->submitEncodedAudio(packet);
          }
        }
      } else if (!sharedAacFailed_) {
        sharedAacFailed_ = true;
        for (size_t index = 0; index < senders_.size(); ++index) {
          if (sharedAacConsumers_[index])
            senders_[index]->fail(supportedDestinations_[index],
                "shared AAC encode failed; expected 48 kHz stereo Program PCM", 0);
        }
      }
    }
    // PCM GOES TO EVERY SENDER, shared-AAC consumers included (#735).
    // "Accepts shared AAC" is a capability; whether a sender USES it is decided per
    // stream, when it picks its encode path. A consumer on the CPU fallback path
    // (no usable hardware encoder, a capacity refusal, COREVIDEO_GPU_ENCODE=0)
    // ignores the encoded packets and has an FFmpeg waiting on a PCM input. Slice 6
    // withheld PCM from every consumer by capability, so that FFmpeg never got audio,
    // stopped reading video after 12 frames, and the stream delivered nothing
    // (measured: 0 bytes at the sink on every build from 2026-09-30).
    // A consumer that IS on the shared path drops this PCM itself
    // (RtmpOutputSenderAdapter::writeAudioToFfmpeg returns first), so sending it
    // costs one small queued copy per tick and cannot double the audio.
    for (size_t index = 0; index < senders_.size(); ++index) {
      senders_[index]->submitAudio(pcm, channels, sampleRate);
    }
  }

  OutputSenderSession fail(const std::string& destination, const std::string& message, double elapsedMs) override {
    OutputSenderSession combined;
    for (const auto& sender : senders_) {
      mergeInto(combined, sender->fail(destination, message, elapsedMs));
    }
    finalize(combined);
    { std::lock_guard<std::mutex> lock(sessionMutex_); lastSession_ = combined; }
    return combined;
  }

  OutputSenderSession recover(const std::string& destination, double elapsedMs, const std::string& reason) override {
    OutputSenderSession combined;
    for (const auto& sender : senders_) {
      mergeInto(combined, sender->recover(destination, elapsedMs, reason));
    }
    finalize(combined);
    { std::lock_guard<std::mutex> lock(sessionMutex_); lastSession_ = combined; }
    return combined;
  }

  // THE WRAPPER LAW (#597 task 7 round 1). Inheriting the default here would
  // turn every supervisor restart back into an operator reset for every member.
  OutputSenderSession restartForSupervisor(const std::string& destination, double elapsedMs,
                                           const std::string& reason) override {
    OutputSenderSession combined;
    for (const auto& sender : senders_) {
      mergeInto(combined, sender->restartForSupervisor(destination, elapsedMs, reason));
    }
    finalize(combined);
    { std::lock_guard<std::mutex> lock(sessionMutex_); lastSession_ = combined; }
    return combined;
  }

  OutputSenderSession session() const override {
    OutputSenderSession combined;
    for (const auto& sender : senders_) {
      mergeInto(combined, sender->session());
    }
    std::lock_guard<std::mutex> lock(sessionMutex_);
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

  void addMissingDestinationWarnings(
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
      if (std::find(supportedDestinations_.begin(), supportedDestinations_.end(), destination) != supportedDestinations_.end()) {
        // A child writer has accepted the request but has not published its
        // first snapshot yet. Absence here does not mean the module is missing.
        sender.status = "starting";
        sender.destinationHealth = "starting";
        sender.startedAtMs = elapsedMs;
        session.senders.push_back(sender);
        continue;
      }
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
  ProgramAacEncoder sharedAac_;
  ProgramStreamClock streamClock_;
  bool sharedAacEnabled_ = false;
  bool sharedAacFailed_ = false;
  std::vector<bool> sharedAacConsumers_;
  std::vector<std::string> supportedDestinations_;
  mutable std::mutex sessionMutex_;
  OutputSenderSession lastSession_;
};

// What a non-stub core holds when no capture adapter constructed: nothing to
// enumerate, nothing to connect. MediaCore dereferences captureDevice without a
// null check, so "no devices" must be an object, not a null pointer.
class NoCaptureDevice final : public ICaptureDevice {
 public:
  std::vector<CaptureDeviceInfo> enumerate() const override { return {}; }
  std::vector<CaptureDeviceInfo> selectInput(const std::string&, const std::string&) override { return {}; }
  std::vector<CaptureDeviceInfo> setAudioSyncOffset(const std::string&, int) override { return {}; }
  std::vector<CaptureDeviceInfo> connect(const std::string&) override { return {}; }
};

class CompositeCaptureDevice final : public ICaptureDevice {
 public:
  explicit CompositeCaptureDevice(std::vector<std::unique_ptr<ICaptureDevice>> devices) : devices_(std::move(devices)) {}
  void setVideoConsumerDemand(const std::vector<SourceVideoDemand>& demands) override {
    for (const auto& device : devices_) device->setVideoConsumerDemand(demands);
  }
  std::vector<VideoFrame> takeCpuVideoFrames() override {
    std::vector<VideoFrame> result;
    for (const auto& device : devices_) {
      auto frames = device->takeCpuVideoFrames();
      result.insert(result.end(), std::make_move_iterator(frames.begin()), std::make_move_iterator(frames.end()));
    }
    return result;
  }

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

  std::vector<CaptureDeviceInfo> configureRtmpIngestSources(const std::vector<RtmpIngestSourceConfig>& sources) override {
    for (const auto& device : devices_) {
      (void)device->configureRtmpIngestSources(sources);
    }
    return enumerate();
  }

  void registerCaptureBuffer(const std::string& deviceId, const std::string& shmName, int width, int height) override {
    for (const auto& device : devices_) {
      device->registerCaptureBuffer(deviceId, shmName, width, height);
    }
  }

  void unregisterCaptureBuffer(const std::string& deviceId) override {
    for (const auto& device : devices_) {
      device->unregisterCaptureBuffer(deviceId);
    }
  }

  void captureVideoTick(int64_t timestampMs) override {
    struct Collect final : ICaptureVideoConsumer {
      std::vector<VideoFrame> frames;
      void publish(VideoFrame frame) override { frames.push_back(std::move(frame)); }
      void end(const std::string&) override {}
    } collect;
    for (const auto& device : devices_) device->deliverVideo(collect, timestampMs);
    replaceVideo(std::move(collect.frames));
  }

  void captureAudioTick(int64_t timestampMs) override {
    struct Collect final : ICaptureAudioConsumer {
      CompositeCaptureDevice* self = nullptr;
      void publish(AudioFrame frame) override { self->postAudio(std::move(frame)); }
    } collect;
    collect.self = this;
    for (const auto& device : devices_) device->deliverAudio(collect, timestampMs);
  }

  std::vector<std::string> audioSourceIds() const override {
    std::vector<std::string> result;
    for (const auto& device : devices_) {
      for (auto& id : device->audioSourceIds()) result.push_back(std::move(id));
    }
    return result;
  }

 private:
  std::vector<std::unique_ptr<ICaptureDevice>> devices_;
};

}  // namespace

namespace {
void recordCapability(ModuleSet& modules, const char* name, const char* state,
                      const char* detail = "", const char* failureScope = "") {
  modules.capabilityConstruction[name] = {state, detail, failureScope};
}
}  // namespace

std::unique_ptr<IOutputSender> createIsolatedOutputSender(
    std::vector<std::unique_ptr<IOutputSender>> senders, std::vector<std::string> supportedDestinations) {
  auto composite = std::make_unique<CompositeOutputSender>(std::move(senders), std::move(supportedDestinations));
  composite->enableIndependentWriters();
  return composite;
}

ModuleSet createDefaultModules() {
  auto modules = createStubModules();
  if (auto compositor = createD3D11Compositor()) {
    modules.compositor = std::move(compositor);
  } else if (auto metalCompositor = createMetalCompositor()) {
    modules.compositor = std::move(metalCompositor);
  }
  recordCapability(modules, "gpu-compositor", modules.compositor->rendererName() != "software"
      ? "available" : (COREVIDEO_WITH_D3D11 || COREVIDEO_WITH_METAL)
          ? "failed-to-construct" : "omitted", COREVIDEO_STUB ? "stub-build" : "");
  if (auto mediaDecoderFactory = createMediaFoundationMediaDecoderFactory()) {
    modules.mediaDecoderFactory = std::move(mediaDecoderFactory);
  }
  bool monitorConstructed = false;
  if (auto monitorOutput = createWasapiMonitorOutput()) {
    modules.monitorOutput = std::move(monitorOutput);
    monitorConstructed = true;
  } else if (auto coreAudioMonitor = createCoreAudioMonitorOutput()) {
    modules.monitorOutput = std::move(coreAudioMonitor);
    monitorConstructed = true;
  }
  recordCapability(modules, "audio-monitor-output", monitorConstructed ? "available"
      : (COREVIDEO_WITH_WASAPI_MONITOR || COREVIDEO_WITH_COREAUDIO) ? "failed-to-construct" : "omitted",
      COREVIDEO_STUB ? "stub-build" : "");
  bool audioCaptureConstructed = false;
  if (auto audioCapture = createWasapiAudioCaptureSource()) {
    modules.audioCapture = std::move(audioCapture);
    audioCaptureConstructed = true;
  } else if (auto coreAudioCapture = createCoreAudioCaptureSource()) {
    modules.audioCapture = std::move(coreAudioCapture);
    audioCaptureConstructed = true;
  }
  recordCapability(modules, "local-audio-capture", audioCaptureConstructed ? "available"
      : (COREVIDEO_WITH_WASAPI_CAPTURE || COREVIDEO_WITH_COREAUDIO) ? "failed-to-construct" : "omitted",
      COREVIDEO_STUB ? "stub-build" : "");
  bool encoderConstructed = false;
  if (auto avfEncoder = createAVFoundationEncoderSink()) {
    modules.encoder = std::move(avfEncoder);
    encoderConstructed = true;
  } else if (auto encoder = createMediaFoundationEncoderSink()) {
    modules.encoder = std::move(encoder);
    encoderConstructed = true;
  }
  // A real build whose encoder adapter did not construct keeps the counting stub
  // encoder, which reports a recording that writes no file (#762). Record the
  // failure here, at the factory that tried, so MediaCore can refuse Record and
  // the shell can see why. The stub tier and the stub set leave it "omitted".
  if (!COREVIDEO_STUB && (COREVIDEO_WITH_MF_ENCODER || COREVIDEO_WITH_AVF_ENCODER) && !encoderConstructed) {
    recordCapability(modules, "program-recording", "failed-to-construct", "encoder-adapter-did-not-start");
    recordCapability(modules, "iso-recording", "failed-to-construct", "encoder-adapter-did-not-start");
  }
#if !COREVIDEO_STUB
  modules.permitStubZoomSession = false;
#endif
  std::vector<std::unique_ptr<IOutputSender>> outputSenders;
  std::vector<std::string> supportedOutputDestinations;
  if (auto outputSender = createRtmpOutputSender()) {
    recordCapability(modules, "rtmp-output", outputSender->runtimeAvailableAtConstruction()
        ? "available" : "omitted", outputSender->runtimeAvailableAtConstruction()
        ? "" : "ffmpeg-runtime-missing");
    outputSenders.push_back(std::move(outputSender));
    supportedOutputDestinations.push_back("rtmp");
  } else if (COREVIDEO_WITH_RTMP_OUTPUT) {
    recordCapability(modules, "rtmp-output", "failed-to-construct");
  }
  if (auto srtSender = createSrtOutputSender()) {
    recordCapability(modules, "srt-output", srtSender->runtimeAvailableAtConstruction()
        ? "available" : "omitted", srtSender->runtimeAvailableAtConstruction()
        ? "" : "ffmpeg-runtime-missing");
    outputSenders.push_back(std::move(srtSender));
    supportedOutputDestinations.push_back("srt");
  } else if (COREVIDEO_WITH_RTMP_OUTPUT) {
    recordCapability(modules, "srt-output", "failed-to-construct");
  }
  if (auto hlsSender = createHlsOutputSender()) {
    recordCapability(modules, "hls-output", hlsSender->runtimeAvailableAtConstruction()
        ? "available" : "omitted", hlsSender->runtimeAvailableAtConstruction()
        ? "" : "ffmpeg-runtime-missing");
    outputSenders.push_back(std::move(hlsSender));
    supportedOutputDestinations.push_back("hls");
  } else if (COREVIDEO_WITH_RTMP_OUTPUT) {
    recordCapability(modules, "hls-output", "failed-to-construct");
  }
  if (auto ndiSender = createNdiOutputSender()) {
    recordCapability(modules, "ndi-output", ndiSender->runtimeAvailableAtConstruction()
        ? "available" : "omitted", ndiSender->runtimeAvailableAtConstruction()
        ? "" : "ndi-runtime-missing", "process");
    outputSenders.push_back(std::move(ndiSender));
    supportedOutputDestinations.push_back("ndi");
  } else if (COREVIDEO_WITH_NDI_OUTPUT) {
    recordCapability(modules, "ndi-output", "failed-to-construct", "", "process");
  }
  if (!outputSenders.empty()) {
    modules.outputSender = std::make_unique<CompositeOutputSender>(std::move(outputSenders), std::move(supportedOutputDestinations));
  }
  std::vector<std::unique_ptr<ICaptureDevice>> hardwareCaptureDevices;
#if COREVIDEO_STUB
  // The stub tier has no hardware adapters: its fake DeckLink/AJA pair IS its
  // capture surface, and the stub round-trip tests drive it.
  hardwareCaptureDevices.push_back(std::move(modules.captureDevice));
#else
  // A real core lists real devices only (#739). The fake pair used to ride along
  // as the first member of the composite: a "connected" DeckLink that does not
  // exist, generating a 640x360 test pattern on every capture tick, kept off the
  // operator's screen only by an id-prefix filter in the WinUI shell.
  modules.captureDevice = std::make_unique<NoCaptureDevice>();
#endif
  bool srtIngestConstructed = false;
  if (auto srtIngest = createSrtIngestCaptureDevice()) {
    hardwareCaptureDevices.push_back(std::move(srtIngest));
    srtIngestConstructed = true;
  }
  // Ingest uses the staged FFmpeg decoder, which already carries libsrt. A
  // missing runtime is reported by the source when it attempts to connect.
  recordCapability(modules, "srt-ingest",
      COREVIDEO_STUB || !COREVIDEO_WITH_SRT_INGEST ? "omitted"
          : srtIngestConstructed ? "available" : "failed-to-construct",
      COREVIDEO_STUB ? "stub-build" : !COREVIDEO_WITH_SRT_INGEST ? "build-gate-off"
          : srtIngestConstructed ? "ffmpeg-decoder" : "");
  bool rtmpIngestConstructed = false;
  if (auto rtmpIngest = createRtmpIngestCaptureDevice()) {
    hardwareCaptureDevices.push_back(std::move(rtmpIngest));
    rtmpIngestConstructed = true;
  }
  recordCapability(modules, "rtmp-ingest",
      COREVIDEO_STUB || !COREVIDEO_WITH_RTMP_INGEST ? "omitted"
          : rtmpIngestConstructed ? "available" : "failed-to-construct",
      COREVIDEO_STUB ? "stub-build" : !COREVIDEO_WITH_RTMP_INGEST ? "build-gate-off"
          : rtmpIngestConstructed ? "ffmpeg-decoder" : "");
  bool ndiIngestConstructed = false;
  if (auto ndiIngest = createNdiReceiveCaptureDevice()) {
    hardwareCaptureDevices.push_back(std::move(ndiIngest));
    ndiIngestConstructed = true;
  }
  recordCapability(modules, "ndi-ingest",
      COREVIDEO_STUB || !COREVIDEO_WITH_NDI_INGEST ? "omitted"
          : ndiIngestConstructed ? "available" : "failed-to-construct",
      COREVIDEO_STUB ? "stub-build" : !COREVIDEO_WITH_NDI_INGEST ? "build-gate-off"
          : ndiIngestConstructed ? "ndi-runtime" : "ndi-runtime-missing", "process");
  if (auto deckLink = createDeckLinkCaptureDevice()) {
    hardwareCaptureDevices.push_back(std::move(deckLink));
  }
  if (auto aja = createAjaCaptureDevice()) {
    hardwareCaptureDevices.push_back(std::move(aja));
  }
  // Probe-only hardware adapters are not Program sources until #537 has pixels.
  recordCapability(modules, "decklink-capture", "omitted", COREVIDEO_STUB ? "stub-build" : "no-live-pixels");
  recordCapability(modules, "aja-capture", "omitted", COREVIDEO_STUB ? "stub-build" : "no-live-pixels");
  // Native UVC (Media Foundation) capture: webcams/capture cards enumerated and
  // streamed inside the core, no WinUI shared-memory hop (dev-gated; nullptr in
  // stub builds). The WinUiCaptureDeviceAdapter wrap below still supersedes
  // these frames for a device the shell bridges via shm, so the WinUI path
  // remains the fallback arbiter for the same device id.
  bool cameraConstructed = false;
  if (auto uvc = createUvcCaptureDevice()) {
    hardwareCaptureDevices.push_back(std::move(uvc));
    cameraConstructed = true;
  }
  // macOS camera capture (AVFoundation): the UVC twin, same arbitration rules.
  if (auto avfCameras = createAvfCaptureDevice()) {
    hardwareCaptureDevices.push_back(std::move(avfCameras));
    cameraConstructed = true;
  }
  recordCapability(modules, "uvc-capture", cameraConstructed ? "available"
      : (COREVIDEO_WITH_UVC || COREVIDEO_WITH_AVF_CAPTURE) ? "failed-to-construct" : "omitted",
      COREVIDEO_STUB ? "stub-build" : "");
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
    if (auto* composite = dynamic_cast<CompositeOutputSender*>(modules.outputSender.get())) {
      // One worker per protocol: blocked RTMP cannot stall SRT/NDI acceptance.
      composite->enableIndependentWriters();
    } else {
      modules.outputSender = std::make_unique<AsyncOutputSender>(std::move(modules.outputSender));
    }
  }
  return modules;
}

}  // namespace corevideo::modules
