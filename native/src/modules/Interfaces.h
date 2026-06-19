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
  int64_t timestampMs = 0;
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

struct CompositorRenderPlanLayer {
  std::string layerId;
  std::string kind;
  std::string sourceId;
  std::string participantId;
  int order = 0;
  CompositorLayerRect rect;
  float opacity = 1.f;
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

class IZoomCaptureSource {
 public:
  virtual ~IZoomCaptureSource() = default;
  virtual std::vector<VideoFrame> pollVideoFrames() = 0;
  virtual std::vector<AudioFrame> pollAudioFrames() = 0;
};

class ICompositor {
 public:
  virtual ~ICompositor() = default;
  virtual std::string rendererName() const = 0;
  virtual ProgramFrame render(const CompositorRenderPlan& renderPlan, const std::vector<VideoFrame>& frames) = 0;
};

class IAudioMixer {
 public:
  virtual ~IAudioMixer() = default;
  virtual int64_t mix(const std::vector<AudioFrame>& frames) = 0;
  virtual AudioMixMetrics session() const = 0;
};

class IEncoderSink {
 public:
  virtual ~IEncoderSink() = default;
  virtual void configureRecording(const RecordingSessionRequest& request) = 0;
  virtual OutputSession start(const std::vector<std::string>& destinations, const std::vector<std::string>& isoParticipantIds) = 0;
  virtual void submit(const ProgramFrame& frame) = 0;
  virtual OutputSession session() const = 0;
};

class IOutputSender {
 public:
  virtual ~IOutputSender() = default;
  virtual OutputSenderSession sync(const std::vector<std::string>& destinations, const ProgramFrame* frame, double elapsedMs) = 0;
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
};

struct ModuleSet {
  std::unique_ptr<IZoomCaptureSource> zoom;
  std::unique_ptr<ICompositor> compositor;
  std::unique_ptr<IAudioMixer> mixer;
  std::unique_ptr<IEncoderSink> encoder;
  std::unique_ptr<IOutputSender> outputSender;
  std::unique_ptr<ICaptureDevice> captureDevice;
};

ModuleSet createDefaultModules();
ModuleSet createStubModules();
std::unique_ptr<ICompositor> createD3D11Compositor();
std::unique_ptr<IEncoderSink> createStubRecordingEncoderSink();
std::unique_ptr<IEncoderSink> createMediaFoundationEncoderSink();
std::unique_ptr<IOutputSender> createRtmpOutputSender();
std::unique_ptr<ICaptureDevice> createDeckLinkCaptureDevice();
std::unique_ptr<ICaptureDevice> createAjaCaptureDevice();

}  // namespace corevideo::modules
