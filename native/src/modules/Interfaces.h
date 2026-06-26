#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace corevideo::modules {

struct VideoFrame {
  std::string participantId;
  int width = 0;
  int height = 0;
  // Natural uncropped source dimensions. These can differ from pixelWidth /
  // pixelHeight when a capture or preview path delivers a downscaled frame.
  // Framing and pan/zoom use these dimensions so source XY is resolved against
  // the original feed, not a preview-sized intermediate crop.
  int naturalWidth = 0;
  int naturalHeight = 0;
  int64_t timestampMs = 0;
  // Optional decoded pixel payload. When present, `pixels` holds tightly packed
  // BGRA bytes for a `pixelWidth` x `pixelHeight` image with `pixelStride` bytes
  // per row. The buffer is shared so VideoFrame stays cheap to copy as it flows
  // through pollVideoFrames -> render plan -> compositor. When empty, callers
  // fall back to the synthetic solid-color slate keyed by participantId.
  std::shared_ptr<const std::vector<uint8_t>> pixels;
  int pixelWidth = 0;
  int pixelHeight = 0;
  int pixelStride = 0;
  int64_t frameId = 0;
  [[nodiscard]] bool hasPixels() const {
    return pixels && pixelWidth > 0 && pixelHeight > 0 && pixelStride >= pixelWidth * 4 &&
           pixels->size() >= static_cast<size_t>(pixelStride) * static_cast<size_t>(pixelHeight);
  }
};

struct AudioFrame {
  std::string participantId;
  int sampleRate = 48000;
  int channels = 1;
  int64_t timestampMs = 0;
  int sampleCount = 960;
  double rmsLevel = 0.0;
  double peakLevel = 0.0;
  double noiseFloorDb = -60.0;
  bool voiceActive = true;
  // Optional interleaved float PCM payload in full-scale range [-1, 1] with
  // `channels` channels (so `pcm.size()` is `sampleCount * channels` when
  // present). When non-empty, the audio DSP core measures real RMS/peak from
  // these samples; when empty, callers fall back to the `rmsLevel`/`peakLevel`
  // metadata above. Defaulted empty so every existing producer stays valid.
  std::vector<float> pcm;
};

struct AudioParticipantMixMetrics {
  std::string participantId;
  int inputLevel = 0;
  int outputLevel = 0;
  int gainDb = 0;
  double rmsLevel = 0.0;
  double peakLevel = 0.0;
  double noiseFloorDb = -60.0;
  bool noiseSuppressionActive = false;
  bool limiterActive = false;
  bool underrunDetected = false;
  bool clippingDetected = false;
  bool silenceDetected = false;
  bool muted = false;
  int64_t avSyncOffsetMs = 0;
  int64_t timingDriftMs = 0;
  int64_t framesMixed = 0;
  std::string status = "idle";
};

struct AudioMixMetrics {
  std::string status = "idle";
  int masterLevel = 0;
  double loudnessLufs = -60.0;
  bool limiterActive = false;
  int64_t mixedFrameCount = 0;
  int participantCount = 0;
  int underrunCount = 0;
  int clippingCount = 0;
  int silenceCount = 0;
  int64_t maxAbsAvSyncOffsetMs = 0;
  std::vector<AudioParticipantMixMetrics> participants;
  std::vector<std::string> warnings;
  std::string summary = "Audio mix idle.";
};

struct ProgramFramePreviewPixels {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> bgra;
};

struct ProgramFrameSharedTexture {
  std::string sharedHandleHex;
  int width = 0;
  int height = 0;
  std::string format = "B8G8R8A8_UNORM";
  int64_t frameNumber = 0;
};

struct ProgramFrame {
  int width = 1920;
  int height = 1080;
  int layerCount = 0;
  int64_t frameNumber = 0;
  std::string renderPlanId;
  std::string renderer = "software";
  bool gpuComposed = false;
  std::string health = "live";
  uint32_t programPixelSignature = 0;
  uint32_t renderPlanSignature = 0;
  std::vector<std::string> warnings;
  ProgramFramePreviewPixels preview;
  ProgramFrameSharedTexture sharedTexture;
};

struct CompositorLayerRect {
  float x = 0.f;
  float y = 0.f;
  float width = 1.f;
  float height = 1.f;
};

struct CompositorColorGrade {
  float exposure = 0.f;
  float contrast = 0.f;
  float saturation = 0.f;
  float temperature = 0.f;
};

// Overlay-raster payload for an overlay/lower-third/caption layer. These fields
// are internal to the compositor render plan (built in MediaCore, consumed by
// the compositor adapters) and are NOT serialized over the wire, so they do not
// have a TS protocol mirror. The source command fields (set-overlay-asset /
// push-caption-cue / set-brand-kit) already exist in both protocol mirrors.
struct CompositorOverlayContent {
  std::string title;        // Lower-third title line (e.g. speaker name).
  std::string org;          // Lower-third secondary line (e.g. organization).
  std::string text;         // Free-form overlay text / caption body.
  std::string speaker;      // Caption speaker attribution.
  std::string imageUri;     // Image overlay source (decoded via WIC on Windows).
  std::string keyPosition = "lower-left";  // lower-left | upper-left.
  // Animation phase: hidden | building-in | on-air | building-out. Drives the
  // animated transform/alpha keying applied per frame.
  std::string keyPhase = "on-air";
  // Keyer placement relative to the program: upstream composites under the
  // sources, downstream composites on top of them.
  std::string keyer = "downstream";
  // Normalized [0,1] animation progress within the current keyPhase, advanced
  // by the compositor's animation clock. 0 = phase just entered, 1 = settled.
  float keyProgress = 1.f;
  bool isCaption = false;   // True for caption layers (lower band styling).
  // BrandKit styling resolved at plan-build time (#RRGGBB / #RRGGBBAA).
  std::string brandColor = "#44c1a1";
  std::string brandAccentColor = "#f0a85c";
  std::string brandBackgroundColor = "#0c1118";
  std::string fontFamily = "Inter";
};

struct CompositorRenderPlanLayer {
  std::string layerId;
  std::string kind;
  std::string sourceId;
  std::string participantId;
  int order = 0;
  CompositorLayerRect rect;
  float opacity = 1.f;
  std::string fitMode = "fill";
  std::string borderStyle = "accent";
  std::string borderColor = "#44C1A1";
  float borderThickness = 2.f;
  float sourceScale = 1.f;
  float sourceOffsetX = 0.f;
  float sourceOffsetY = 0.f;
  std::string mediaAssetId;
  std::string mediaAssetName;
  std::string mediaAssetKind;
  std::string mediaAssetPath;
  std::string mediaPlaybackKey;
  bool mediaAssetPlaying = false;
  bool hasColorGrade = false;
  CompositorColorGrade colorGrade;
  // Set for overlay/lower-third/caption layers. Empty (default) for video
  // sources, which leaves overlay rendering on the prior solid-fill fallback.
  bool hasOverlayContent = false;
  CompositorOverlayContent overlay;
};

struct CompositorRenderPlan {
  std::string renderPlanId;
  std::string sceneId;
  int width = 1920;
  int height = 1080;
  int fps = 30;
  CompositorColorGrade colorGrade;
  std::vector<CompositorRenderPlanLayer> layers;
  std::vector<std::string> warnings;
};

struct OutputSession {
  bool active = false;
  std::vector<std::string> destinations;
  std::vector<std::string> isoParticipantIds;
  int64_t encodedFrameCount = 0;
  std::string encoderName = "software-counting";
  std::string codec = "h264";
  int targetBitrateMbps = 10;
  bool hardwareAccelerated = false;
  std::string recordingSessionId;
  std::string recordingStatus = "idle";
  std::string recordingTargetFolder;
  std::string recordingFilenamePrefix;
  std::string recordingFormat;
  std::string recordingQuality;
  std::string recordingArtifactPath;
  int64_t recordingBytesWritten = 0;
  int64_t recordingDurationMs = 0;
  int64_t recordingVideoFrameCount = 0;
  int64_t recordingLastFrameNumber = 0;
  int recordingWidth = 0;
  int recordingHeight = 0;
  int recordingFps = 0;
  std::string recordingContainerFormat;
  std::string recordingVideoCodec;
  std::string recordingAudioCodec;
  int64_t recordingAudioPacketCount = 0;
  int64_t recordingAudioSampleCount = 0;
  int recordingAudioChannels = 0;
  int recordingAudioSampleRate = 0;
  bool recordingMetadataValid = false;
  std::string recordingWarning;
  std::string recordingError;
};

struct RecordingSessionRequest {
  std::string sessionId = "native-recording-session";
  std::string targetFolder = "Recordings/CoreVideo Pro/native-core";
  std::string filenamePrefix = "program";
  std::string format = "mp4";
  std::string quality = "high";
  std::vector<std::string> isoParticipantIds;
  int width = 1920;
  int height = 1080;
  int fps = 30;
  std::string videoCodec = "h264";
  std::string audioCodec = "aac";
  int targetBitrateMbps = 10;
};

struct OutputSender {
  std::string senderId;
  std::string destination;
  std::string status = "idle";
  double startedAtMs = 0;
  double stoppedAtMs = 0;
  int64_t lastFrameNumber = 0;
  int64_t framesSent = 0;
  int retryCount = 0;
  int latencyMs = 2100;
  double bitrateMbps = 6.0;
  std::string warning;
  std::string destinationHealth = "starting";
  std::string lastResultCode = "waiting-for-frame";
  std::string lastError;
  int64_t bytesSent = 0;
  int64_t audioFramesSent = 0;
  int64_t audioBytesSent = 0;
  int audioChannels = 0;
  int audioSampleRate = 0;
  std::string sendArtifactPath;
  int64_t sendBytesWritten = 0;
  std::string runtimeDetail;
};

struct OutputSenderSession {
  std::string status = "idle";
  int activeSenderCount = 0;
  std::vector<OutputSender> senders;
  std::vector<std::string> warnings;
};

struct OutputDestinationSettings {
  std::string id;
  std::string label;
  std::string protocol;
  std::string url;
  std::string streamKey;
  std::string ffmpegBinDirectory;
  std::string mode;
  std::string host;
  int port = 0;
  int latencyMs = 0;
  int latencyUs = 0;
  std::string passphrase;
  int keyLength = 0;
  std::string streamId;
  std::string ndiName;
  std::string ndiGroup;
  int fps = 30;
  double targetBitrateMbps = 6.0;
  std::string videoCodec = "h264";
  std::string encoderMode = "auto";
  // Opt in to enhanced-RTMP (E-RTMP) so H.265/AV1 can ride the FLV transport
  // instead of being downgraded to H.264. Defaulted off so the guaranteed
  // H.264 + AAC baseline stays the safe default.
  bool allowEnhancedRtmp = false;
};

struct CaptureDeviceInfo {
  std::string id;
  std::string name;
  std::string kind;
  std::string vendor;
  std::vector<std::string> inputIds;
  std::vector<std::string> inputLabels;
  std::vector<bool> inputHasEmbeddedAudio;
  std::string selectedInputId;
  int width = 1920;
  int height = 1080;
  int frameRate = 30;
  std::string connectionState = "detected";
  bool signalPresent = false;
  int64_t droppedFrames = 0;
  int audioSyncOffsetMs = 0;
  std::string warning;
};

struct CaptureAudioSourceConfig {
  std::string captureDeviceId;
  std::string audioDeviceId;
  std::string audioDeviceName;
  std::string audioSourceKind = "none";
  std::string nativeAudioDeviceId;
  std::string audioDriverName;
  int audioSyncOffsetMs = 0;
  bool embedded = false;
};

struct CaptureAudioSourceMetrics {
  std::string captureDeviceId;
  std::string sourceId;
  std::string audioSourceKind;
  bool streaming = false;
  int64_t framesReceived = 0;
  int64_t emptyPacketPolls = 0;
  int sampleRate = 0;
  int channels = 0;
  std::string endpointId;
  std::string endpointName;
  std::string lastError;
  std::string warning;
  double peakDbfs = -120.0;
  double rmsDbfs = -120.0;
  bool signalPresent = false;
  int64_t framesRendered = 0;
  int64_t queuedFrames = 0;
  int64_t underrunCount = 0;
};

struct SrtIngestSourceConfig {
  std::string id;
  std::string deviceId;
  std::string name;
  std::string mode = "listener";
  std::string host = "0.0.0.0";
  int port = 10000;
  int latencyMs = 120;
  std::string streamId;
  std::string passphrase;
};

class IZoomCaptureSource {
 public:
  virtual ~IZoomCaptureSource() = default;
  virtual std::vector<VideoFrame> pollVideoFrames() = 0;
  virtual std::vector<AudioFrame> pollAudioFrames() = 0;
};

class IAudioCaptureSource {
 public:
  virtual ~IAudioCaptureSource() = default;
  virtual void configure(const std::vector<CaptureAudioSourceConfig>& sources) = 0;
  virtual std::vector<AudioFrame> pollAudioFrames(int64_t timestampMs) = 0;
  [[nodiscard]] virtual std::vector<std::string> warnings() const { return {}; }
  [[nodiscard]] virtual std::vector<CaptureAudioSourceMetrics> metrics() const { return {}; }
};

class ICompositor {
 public:
  virtual ~ICompositor() = default;
  virtual std::string rendererName() const = 0;
  virtual ProgramFrame render(const CompositorRenderPlan& renderPlan, const std::vector<VideoFrame>& frames) = 0;
};

class IMediaFrameSource {
 public:
  virtual ~IMediaFrameSource() = default;
  virtual std::vector<VideoFrame> pollMediaFrames(const std::vector<CompositorRenderPlanLayer>& layers, int64_t timestampMs) = 0;
  [[nodiscard]] virtual std::vector<std::string> warnings() const { return {}; }
};

class IAudioMixer {
 public:
  virtual ~IAudioMixer() = default;
  virtual int64_t mix(const std::vector<AudioFrame>& frames) = 0;
  virtual AudioMixMetrics session() const = 0;
  // Most recent monitor (MON) bus as interleaved float PCM in [-1, 1] with
  // `monitorBusChannels()` channels at `monitorBusSampleRate()` Hz. Empty when
  // the last mix carried no real PCM signal (e.g. metadata-only frames), in
  // which case the monitor output stays armed but silent. Defaulted here so
  // mixers that have not yet grown a real bus tap stay valid.
  [[nodiscard]] virtual const std::vector<float>& monitorBusPcm() const {
    static const std::vector<float> kEmptyBus;
    return kEmptyBus;
  }
  [[nodiscard]] virtual int monitorBusSampleRate() const { return 48000; }
  [[nodiscard]] virtual int monitorBusChannels() const { return 2; }
};

// Real-time playout of the monitor (MON) bus to a host render device. The
// renderer never touches this directly; MediaCore opens it from the
// `sync-audio-monitor` command and pushes the mixer's monitor bus each tick.
// The default build wires a safe in-memory stub; a dev-gated WASAPI adapter
// drives a real Windows endpoint (createWasapiMonitorOutput, see below).
class IAudioMonitorOutput {
 public:
  virtual ~IAudioMonitorOutput() = default;
  // Opens (or re-targets) the render endpoint for `deviceId` ("" = system
  // default). `sampleRate`/`channels` describe the source bus; the adapter
  // converts to the device's own mix format. Returns true when a render
  // endpoint is ready. Idempotent for an already-open identical device.
  virtual bool start(const std::string& deviceId, int sampleRate, int channels) = 0;
  virtual void stop() = 0;
  // Submits `frameCount` interleaved sample-frames of `channels` float samples
  // in [-1, 1], scaled by linear `volume`. Returns true when the samples were
  // accepted by the endpoint. Real-time: may drop overflow rather than block.
  virtual bool render(const float* interleaved, int frameCount, int channels, double volume) = 0;
  [[nodiscard]] virtual bool active() const = 0;
  [[nodiscard]] virtual bool hardwareOutput() const { return false; }
  [[nodiscard]] virtual std::string deviceName() const = 0;
  [[nodiscard]] virtual std::vector<std::string> warnings() const = 0;
};

class IEncoderSink {
 public:
  virtual ~IEncoderSink() = default;
  virtual void configureRecording(const RecordingSessionRequest& request) = 0;
  virtual OutputSession start(const std::vector<std::string>& destinations, const std::vector<std::string>& isoParticipantIds) = 0;
  virtual void submit(const ProgramFrame& frame) = 0;
  // Mux real program-audio PCM alongside the video frames. `interleaved` holds
  // `frameCount` sample-frames of `channels` float samples in [-1, 1] at
  // `sampleRate` Hz. Default no-op so encoders that don't yet handle audio stay
  // valid; the stub recording sink counts the muxed packets/samples so the
  // recording proof can assert real audio, not a synthetic frame counter.
  virtual void submitAudio(const float* interleaved, int frameCount, int channels, int sampleRate) {
    (void)interleaved;
    (void)frameCount;
    (void)channels;
    (void)sampleRate;
  }
  virtual OutputSession session() const = 0;
};

class IOutputSender {
 public:
  virtual ~IOutputSender() = default;
  // Push the program frame (and, optionally, the real program-audio mix) to the
  // active network destinations each tick. `programAudioPcm` is interleaved
  // float PCM in [-1, 1] with `audioChannels` channels at `audioSampleRate` Hz
  // (the F2 program-audio tap / master bus). Defaulted null so senders that do
  // not carry audio — and existing call sites — stay valid; the RTMP sender
  // muxes it as a real AAC track instead of `anullsrc` silence.
  virtual OutputSenderSession sync(
      const std::vector<std::string>& destinations,
      const ProgramFrame* frame,
      double elapsedMs,
      const std::vector<OutputDestinationSettings>& destinationSettings = {},
      const std::vector<float>* programAudioPcm = nullptr,
      int audioChannels = 0,
      int audioSampleRate = 0) = 0;
  virtual OutputSenderSession fail(const std::string& destination, const std::string& message, double elapsedMs) = 0;
  virtual OutputSenderSession recover(const std::string& destination, double elapsedMs, const std::string& reason) = 0;
  virtual OutputSenderSession session() const = 0;
};

class ICaptureDevice {
 public:
  virtual ~ICaptureDevice() = default;
  virtual std::vector<CaptureDeviceInfo> enumerate() const = 0;
  virtual std::vector<CaptureDeviceInfo> selectInput(const std::string& deviceId, const std::string& inputId) = 0;
  virtual std::vector<CaptureDeviceInfo> setAudioSyncOffset(const std::string& deviceId, int offsetMs) = 0;
  virtual std::vector<CaptureDeviceInfo> connect(const std::string& deviceId) = 0;
  virtual std::vector<CaptureDeviceInfo> configureSrtIngestSources(const std::vector<SrtIngestSourceConfig>&) { return enumerate(); }
  virtual std::vector<VideoFrame> pollVideoFrames(int64_t) { return {}; }
};

struct ModuleSet {
  std::unique_ptr<IZoomCaptureSource> zoom;
  std::unique_ptr<ICompositor> compositor;
  std::unique_ptr<IMediaFrameSource> mediaFrames;
  std::unique_ptr<IAudioMixer> mixer;
  std::unique_ptr<IAudioMonitorOutput> monitorOutput;
  std::unique_ptr<IAudioCaptureSource> audioCapture;
  std::unique_ptr<IEncoderSink> encoder;
  std::unique_ptr<IOutputSender> outputSender;
  std::unique_ptr<ICaptureDevice> captureDevice;
};

ModuleSet createDefaultModules();
ModuleSet createStubModules();
std::unique_ptr<ICompositor> createD3D11Compositor();
std::unique_ptr<IMediaFrameSource> createMediaFoundationMediaFrameSource();
std::unique_ptr<IAudioMonitorOutput> createStubAudioMonitorOutput();
std::unique_ptr<IAudioMonitorOutput> createWasapiMonitorOutput();
std::unique_ptr<IAudioCaptureSource> createStubAudioCaptureSource();
std::unique_ptr<IAudioCaptureSource> createWasapiAudioCaptureSource();
std::unique_ptr<IEncoderSink> createStubRecordingEncoderSink();
std::unique_ptr<IEncoderSink> createMediaFoundationEncoderSink();
std::unique_ptr<IOutputSender> createRtmpOutputSender();
std::unique_ptr<IOutputSender> createSrtOutputSender();
std::unique_ptr<IOutputSender> createNdiOutputSender();
std::unique_ptr<ICaptureDevice> createSrtIngestCaptureDevice();
std::unique_ptr<ICaptureDevice> createDeckLinkCaptureDevice();
std::unique_ptr<ICaptureDevice> createAjaCaptureDevice();

}  // namespace corevideo::modules
