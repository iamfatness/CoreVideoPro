#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace corevideo::modules {

// ===========================================================================
// VIDEO frame transport (F1 — real frame-pixel transport)
// ===========================================================================

struct VideoFrame {
  std::string participantId;
  int width = 0;
  int height = 0;
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

// Bounded, ref-counted BGRA frame pool. High-rate sources (test pattern today,
// UVC/DeckLink/SRT later) acquire a recycled buffer per frame instead of
// allocating, and back-pressure surfaces as a dropped-frame counter when every
// buffer is still referenced downstream.
//
// A buffer handed out by `acquire()` is wrapped in a shared_ptr whose deleter
// returns it to the free list, so the buffer can be aliased directly into a
// VideoFrame::pixels (which is `shared_ptr<const std::vector<uint8_t>>`). The
// pool stays alive as long as any outstanding buffer references it.
class FramePool : public std::enable_shared_from_this<FramePool> {
 public:
  static std::shared_ptr<FramePool> create(size_t capacity, size_t bytesPerBuffer) {
    // enable_shared_from_this requires construction through a shared_ptr.
    return std::shared_ptr<FramePool>(new FramePool(capacity, bytesPerBuffer));
  }

  // Acquire a recycled buffer sized to at least `bytes`. Returns nullptr when
  // the pool is exhausted (all `capacity` buffers are still referenced); the
  // caller treats that as a dropped frame. The returned buffer's contents are
  // not cleared — the caller fully overwrites it.
  std::shared_ptr<std::vector<uint8_t>> acquire(size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<uint8_t>* raw = nullptr;
    if (!free_.empty()) {
      raw = free_.back();
      free_.pop_back();
    } else if (allocated_ < capacity_) {
      raw = new std::vector<uint8_t>();
      ++allocated_;
    } else {
      droppedFrames_.fetch_add(1, std::memory_order_relaxed);
      return nullptr;
    }
    if (raw->size() < bytes) {
      raw->resize(bytes);
    }
    ++outstanding_;
    auto self = shared_from_this();
    return std::shared_ptr<std::vector<uint8_t>>(raw, [self](std::vector<uint8_t>* buffer) {
      self->release(buffer);
    });
  }

  [[nodiscard]] int64_t droppedFrames() const { return droppedFrames_.load(std::memory_order_relaxed); }
  [[nodiscard]] size_t capacity() const { return capacity_; }
  [[nodiscard]] size_t outstanding() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return outstanding_;
  }

 private:
  FramePool(size_t capacity, size_t bytesPerBuffer)
      : capacity_(capacity == 0 ? 1 : capacity), bytesPerBuffer_(bytesPerBuffer) {}

  void release(std::vector<uint8_t>* buffer) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (outstanding_ > 0) {
      --outstanding_;
    }
    // Keep capacity-bounded buffers warm; never grow the free list past the
    // pool capacity so memory stays bounded.
    if (free_.size() < capacity_) {
      free_.push_back(buffer);
    } else {
      delete buffer;
      if (allocated_ > 0) {
        --allocated_;
      }
    }
  }

  mutable std::mutex mutex_;
  const size_t capacity_;
  const size_t bytesPerBuffer_;
  size_t allocated_ = 0;
  size_t outstanding_ = 0;
  std::vector<std::vector<uint8_t>*> free_;
  std::atomic<int64_t> droppedFrames_{0};
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
  // Measured-from-signal master-chain metrics (F2). Populated when the program
  // audio bus produced real PCM this session; otherwise carry the idle floors.
  bool programTapPresent = false;        // real program PCM flowed
  int64_t programTapSampleCount = 0;     // stereo frames in the latest program tap
  double truePeakDbfs = -120.0;          // measured program true-peak
  double programRmsDbfs = -120.0;        // measured program RMS
  double momentaryLufs = -120.0;         // BS.1770 momentary (400 ms)
  double shortTermLufs = -120.0;         // BS.1770 short-term (3 s)
  double integratedLufs = -120.0;        // BS.1770 gated integrated
  double gainReductionDb = 0.0;          // limiter reduction applied to program
};

// A stereo PCM tap exposed by the mixer for outputs/recording/monitor. `pcm` is
// interleaved float in [-1, 1]; size == frames * channels. Empty when no real
// program audio is flowing.
struct AudioProgramTap {
  std::vector<float> programPcm;  // interleaved stereo program mix
  std::vector<float> monitorPcm;  // interleaved stereo MON bus
  int frames = 0;
  int sampleRate = 48000;
  int channels = 2;
  // Per-ISO taps keyed by bus id ("iso-1".."iso-8"); only present buses appear.
  std::vector<std::pair<std::string, std::vector<float>>> isoPcm;
  [[nodiscard]] bool present() const { return frames > 0 && !programPcm.empty(); }
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
  std::string fitMode = "fill";
  std::string borderStyle = "accent";
  std::string borderColor = "#44C1A1";
  float borderThickness = 2.f;
  float sourceScale = 1.f;
  float sourceOffsetX = 0.f;
  float sourceOffsetY = 0.f;
  bool hasColorGrade = false;
  CompositorColorGrade colorGrade;
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

struct OutputDestinationSettings {
  std::string id;
  std::string label;
  std::string protocol;
  std::string url;
  std::string streamKey;
  std::string ffmpegBinDirectory;
  int fps = 30;
  double targetBitrateMbps = 6.0;
  std::string videoCodec = "h264";
  std::string encoderMode = "auto";
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
  // Real per-source frame counter (F1): the number of populated-pixel frames
  // this device has published through pollVideoFrames(). Stays 0 for
  // metadata-only / probe-only devices until a real adapter is producing data.
  int64_t framesIngested = 0;
  int audioSyncOffsetMs = 0;
  std::string warning;
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

// ---------------------------------------------------------------------------
// Decoder-stage seam (F1).
//
// Compressed sources (SRT ingest, NDI receive) deliver encoded packets, not
// pixels. This is the *seam only*: an interface that turns codec-tagged packets
// into populated-pixel VideoFrames so those sources can join the same F1 frame
// path the test pattern uses. No codec is implemented here — a real FFmpeg
// libavcodec decoder lives behind COREVIDEO_ENABLE_DEV_ADAPTERS +
// COREVIDEO_WITH_FFMPEG_DECODE and is wired in later (Phase B). The default
// build resolves the factory to nullptr, so callers must treat a missing
// decoder as "no pixels yet" and fall back to the synthetic slate.
// ---------------------------------------------------------------------------

struct EncodedVideoPacket {
  std::string participantId;
  // Lowercase codec id, e.g. "h264", "hevc", "av1". Empty means unknown; a real
  // decoder rejects packets whose codec it was not opened for.
  std::string codec;
  int64_t timestampMs = 0;
  int64_t frameId = 0;
  bool keyframe = false;
  std::vector<uint8_t> data;
};

class IVideoDecoder {
 public:
  virtual ~IVideoDecoder() = default;
  // Lowercase codec id this decoder instance was opened for.
  [[nodiscard]] virtual std::string codec() const = 0;
  // True only once a real decode backend is built and initialized. The default
  // build has no decoder, so this is the honesty gate the capture sources and
  // profileCapabilities() consult before claiming decoded pixels.
  [[nodiscard]] virtual bool ready() const = 0;
  // Decode one packet. Returns false (and leaves `out` untouched) when the
  // decoder is not ready, the codec does not match, or the backend produced no
  // frame for this packet (e.g. a packet that only advances reference state).
  virtual bool decode(const EncodedVideoPacket& packet, VideoFrame& out) = 0;
};

class ICompositor {
 public:
  virtual ~ICompositor() = default;
  virtual std::string rendererName() const = 0;
  virtual ProgramFrame render(const CompositorRenderPlan& renderPlan, const std::vector<VideoFrame>& frames) = 0;
};

// One participant channel-strip configuration pushed into the mixer before the
// next mix() block. Mirrors the renderer's sync-participant-audio-mix command.
struct AudioChannelStrip {
  std::string participantId;
  double gainDb = 0.0;
  double pan = 0.0;
  bool muted = false;
  bool solo = false;
  bool noiseSuppression = false;
  bool hasInsert = false;
};

// One routing-matrix crosspoint pushed into the mixer (sync-audio-routing-matrix).
struct AudioMixCrosspoint {
  std::string sourceId;
  std::string busId;
  double gainDb = 0.0;
};

class IAudioMixer {
 public:
  virtual ~IAudioMixer() = default;
  virtual int64_t mix(const std::vector<AudioFrame>& frames) = 0;
  virtual AudioMixMetrics session() const = 0;

  // F2 program-bus controls. Default no-ops so the stub and legacy mixers stay
  // valid; the real mixing-graph mixer overrides them.
  virtual void configureChannels(const std::vector<AudioChannelStrip>&) {}
  virtual void configureCrosspoints(const std::vector<AudioMixCrosspoint>&) {}
  virtual void setLimiterEnabled(bool) {}
  // The latest program/ISO/MON PCM tap. Empty by default.
  virtual AudioProgramTap programTap() const { return {}; }
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
  virtual OutputSenderSession sync(
      const std::vector<std::string>& destinations,
      const ProgramFrame* frame,
      double elapsedMs,
      const std::vector<OutputDestinationSettings>& destinationSettings = {}) = 0;
  virtual OutputSenderSession fail(const std::string& destination, const std::string& message, double elapsedMs) = 0;
  virtual OutputSenderSession recover(const std::string& destination, double elapsedMs, const std::string& reason) = 0;
  virtual OutputSenderSession session() const = 0;
};

// F2 — a real audio-monitor playback device. Renders interleaved float PCM
// (the MON bus, or program tap fallback) to a system output. The default build
// has no real output; the dev-gated WasapiMonitorOutput plays the MON bus via
// WASAPI shared mode, replacing the legacy Beep()/playMonitorPulse() fallback.
class IAudioMonitorOutput {
 public:
  virtual ~IAudioMonitorOutput() = default;
  // Play one interleaved-stereo block. `frames` is per-channel sample count;
  // `pcm` holds `frames * channels` samples in [-1, 1]. Returns true when the
  // block was accepted by a real device.
  virtual bool play(const float* pcm, int frames, int channels, int sampleRate, double volume) = 0;
};

// Construct the platform monitor output when built with COREVIDEO_WITH_WASAPI_MONITOR;
// returns nullptr otherwise (caller falls back to the Beep pulse).
std::unique_ptr<IAudioMonitorOutput> createWasapiMonitorOutput();

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
std::unique_ptr<IOutputSender> createSrtOutputSender();
std::unique_ptr<ICaptureDevice> createSrtIngestCaptureDevice();
std::unique_ptr<ICaptureDevice> createDeckLinkCaptureDevice();
std::unique_ptr<ICaptureDevice> createAjaCaptureDevice();

// Deterministic test-pattern capture device. Always available (it is part of
// the stub) and emits real BGRA frames through pollVideoFrames() so the F1
// frame path can be exercised end to end without hardware. `id` keys the
// device so it can be routed via capture:<id> into scenes/program.
std::unique_ptr<ICaptureDevice> createTestPatternCaptureDevice(
    std::string id = "test-pattern-1", int width = 1280, int height = 720, int frameRate = 30);

// Decoder-stage seam factory (F1). Returns a real FFmpeg-backed decoder for the
// requested codec only when COREVIDEO_ENABLE_DEV_ADAPTERS +
// COREVIDEO_WITH_FFMPEG_DECODE are built; otherwise returns nullptr so the
// default build never claims decode it cannot do.
std::unique_ptr<IVideoDecoder> createVideoDecoder(const std::string& codec);

}  // namespace corevideo::modules
