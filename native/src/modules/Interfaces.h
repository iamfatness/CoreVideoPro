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
  // Optional I420 (YUV 4:2:0 planar, full-range BT.709) payload. When present,
  // the GPU compositor uploads the Y/U/V planes to single-channel textures and
  // converts to RGB in the pixel shader (no CPU per-pixel I420->BGRA convert).
  // Planes are tightly packed: Y (i420Width*i420Height) then U then V
  // ((i420Width/2)*(i420Height/2) each). i420Width/i420Height are the (even)
  // luma dimensions. Carried alongside `pixels` above; a frame may carry either
  // representation (Zoom participants now flow through as I420 so the convert
  // happens on the GPU).
  std::shared_ptr<const std::vector<uint8_t>> i420;
  int i420Width = 0;
  int i420Height = 0;
  // Color-space hints for the GPU YUV->RGB conversion of the I420 payload.
  // Defaults preserve the historical Zoom behavior (full-range BT.709). The
  // native UVC capture adapter sets these from the negotiated Media Foundation
  // media type (cameras typically deliver limited-range/studio-swing YUV, and
  // SD devices are BT.601) so the compositor shader expands the range and picks
  // the matrix per frame instead of washing out camera blacks.
  bool i420FullRange = true;
  bool i420Bt601 = false;
  [[nodiscard]] bool hasPixels() const {
    return pixels && pixelWidth > 0 && pixelHeight > 0 && pixelStride >= pixelWidth * 4 &&
           pixels->size() >= static_cast<size_t>(pixelStride) * static_cast<size_t>(pixelHeight);
  }
  [[nodiscard]] bool hasI420() const {
    if (!i420 || i420Width <= 0 || i420Height <= 0) {
      return false;
    }
    if ((i420Width & 1) != 0 || (i420Height & 1) != 0) {
      return false;
    }
    const size_t yLen = static_cast<size_t>(i420Width) * static_cast<size_t>(i420Height);
    return i420->size() >= yLen + (yLen / 4) * 2;
  }
};

// One selected ISO source's video for a single encoder tick (ISO-1). Carries a
// zero-copy VideoFrame (the frame's I420/BGRA payload is a shared_ptr, so this
// whole struct is cheap to copy across the coreMutex → audio worker → async
// encoder thread hops — NO pixel copy under any lock). `sourceId` is the
// canonical ISO id (`zoom:<pid>` today; `capture:<id>` in ISO-3); `displayName`
// is informational (the authoritative roster name + file path are assigned at
// recording start on the RecordingSessionRequest).
struct IsoSourceVideoFrame {
  std::string sourceId;
  std::string displayName;
  VideoFrame frame;
};

// One selected ISO source's RAW-STEM audio for a single encoder tick (ISO-2).
// `pcm` is the source's isolated, interleaved float PCM in [-1, 1] tapped
// PRE-channel-strip-DSP and PRE-bus-mix (owner decision: raw stems for post) —
// resampled to the recording bus rate at gather but otherwise untouched. When
// the source delivered NO audio this tick (Zoom gates non-active speakers), the
// entry is submitted with an EMPTY `pcm`/`frameCount == 0`: the sink then
// silence-fills that stem to its epoch-anchored expected sample position so the
// stem stays time-aligned to program and never drifts (spec §2c). `sourceId`
// maps to the same per-source ISO writer as the video (`zoom:<pid>`).
struct IsoSourceAudio {
  std::string sourceId;
  std::vector<float> pcm;  // interleaved; size == frameCount * channels (may be empty)
  int frameCount = 0;
  int channels = 0;
  int sampleRate = 48000;
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
  // Windows: a keyed-mutex DXGI shared HANDLE in hex. Empty on macOS.
  std::string sharedHandleHex;
  // macOS: the global IOSurface ID of the IOSurface backing the compositor's
  // render target (IOSurfaceGetID; the shell resolves it with IOSurfaceLookup).
  // 0 on Windows. A texture is "present" when EITHER identifier is set — the
  // two are platform-exclusive siblings, never both set.
  uint32_t iosurfaceId = 0;
  int width = 0;
  int height = 0;
  std::string format = "B8G8R8A8_UNORM";
  int64_t frameNumber = 0;
};

// Per-participant GPU shared texture for the multiview tiles — same keyed-mutex
// shared-handle mechanism as the program texture, but one per participant so the
// WinUI tiles present on the GPU instead of decoding base64 thumbnails on the UI
// thread. (Legacy per-tile path; superseded by the core-composited multiview.)
struct ParticipantSharedTexture {
  std::string participantId;
  std::string sharedHandleHex;
  int width = 0;
  int height = 0;
  std::string format = "B8G8R8A8_UNORM";
  int64_t frameNumber = 0;
};

// One tile of the core-composited multiview grid. Geometry (x,y,w,h) is
// normalized [0,1] against the multiview canvas, mirroring the program layer
// rects, so the WinUI consumer can place click targets through the same
// letterbox transform the single multiview swap chain uses. `slot` is the
// ordered position from the layout command; `activeSpeaker` is baked into the
// texture border in-core (no consumer churn) and surfaced here informationally.
struct MultiviewTileRect {
  std::string sourceId;
  std::string participantId;
  int slot = 0;
  float x = 0.f;
  float y = 0.f;
  float w = 0.f;
  float h = 0.f;
  bool activeSpeaker = false;
  std::string label;
  // Tile semantics for the user-selectable multiviewer layouts. `role` is the
  // cell kind ("pgm" | "pvw" | "source"); `tally` is the source's live status
  // ("pgm" if routed to the program scene, "pvw" if in preview, else "none").
  // The pgm/pvw cells carry role/tally "pgm"/"pvw" and label "Program"/"Preview".
  std::string role = "source";
  std::string tally = "none";
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
  // Full-resolution program BGRA (width x height) for the virtual camera, so it
  // serves native 1080p with no upscale. Populated only when the render plan
  // sets fullProgramReadback (vcam enabled); empty otherwise. `preview` above
  // stays the small 320x180 UI thumbnail.
  ProgramFramePreviewPixels programFullBgra;
  // Latest full-program GPU tap converted to NV12 on the compositor's
  // dedicated device/thread. Network senders consume this directly so live
  // output is 1080p program video, not the 320x180 UI thumbnail.
  int programNv12Width = 0;
  int programNv12Height = 0;
  std::vector<uint8_t> programNv12;
  ProgramFrameSharedTexture sharedTexture;
  std::vector<ParticipantSharedTexture> participantSharedTextures;
  // Core-composited multiview grid: one keyed-mutex DXGI shared texture holding
  // the whole grid (mirrors `sharedTexture`), plus the per-tile rects and the
  // canvas dimensions the rects are normalized against. Empty handle when no
  // multiview layout is set.
  ProgramFrameSharedTexture multiviewSharedTexture;
  std::vector<MultiviewTileRect> multiviewTiles;
  int multiviewWidth = 0;
  int multiviewHeight = 0;
  // Core-composited PREVIEW bus: the full previewed scene (routes + overlays +
  // background + grade) composited into its OWN keyed-mutex DXGI shared texture,
  // mirroring `sharedTexture` (program). Empty handle when no preview scene is set,
  // in which case the WinUI falls back to the single-source preview path.
  ProgramFrameSharedTexture previewSharedTexture;
  int previewWidth = 0;
  int previewHeight = 0;
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
  // "none" by default: borders are opt-in styling (multiview tiles set theirs
  // explicitly); a defaulted layer must never bake chrome into PROGRAM.
  std::string borderStyle = "none";
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
  // Optional hard viewport boundary for a remapped composite. Multiview PGM/PVW
  // layers use this so animated overlays cannot escape their monitor cell.
  bool hasClipRect = false;
  CompositorLayerRect clipRect;
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
  // When true, the compositor skips the blocking GPU->CPU readbacks (base64
  // preview + pixel signature) and only updates the GPU shared texture. Set for
  // the light ~60fps display tick; those readbacks (a CPU Map that stalls the
  // pipeline every frame) are only needed for the throttled base64 thumbnail.
  bool skipCpuReadback = false;
  // When true, the compositor also reads back the program at FULL resolution
  // (BGRA) into ProgramFrame::programFullBgra. Set when the virtual camera is
  // enabled so it publishes native 1080p instead of upscaling the 320x180 UI
  // thumbnail. Independent of skipCpuReadback (which only gates the small
  // preview + pixel signature).
  bool fullProgramReadback = false;
};

// Per-ISO-writer health (ISO-1). One node per selected ISO source, surfaced in
// the recording snapshot streams[] and folded into recording.warning so a
// video-only-broken ISO is as loud as a video-only program was (#286). Populated
// by the Media Foundation sink from its per-source writers.
struct IsoStreamStatus {
  std::string sourceId;
  std::string displayName;
  std::string path;
  std::string kind = "iso";
  int64_t videoFrameCount = 0;
  int64_t audioSampleCount = 0;  // 0 until ISO-2 muxes per-source audio
  int64_t bytesWritten = 0;
  bool trackOpen = false;        // the writer opened + began writing its video track
  std::string warning;           // per-source open/write failure (empty = healthy)
};

struct OutputSession {
  bool active = false;
  std::vector<std::string> destinations;
  std::vector<std::string> isoParticipantIds;
  // Per-session subfolder + manifest (ISO-1 folder scheme, spec §5). Empty for
  // the stub / non-MF sinks.
  std::string recordingSessionDir;
  std::string recordingManifestPath;
  // Real per-ISO-writer health from the Media Foundation sink (empty on the
  // stub, where the snapshot synthesizes ISO nodes from the selected ids).
  std::vector<IsoStreamStatus> isoStreams;
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
  int recordingAudioBitrateKbps = 0;
  int64_t recordingAudioPacketCount = 0;
  int64_t recordingAudioSampleCount = 0;
  int recordingAudioChannels = 0;
  int recordingAudioSampleRate = 0;
  bool recordingMetadataValid = false;
  std::string recordingWarning;
  std::string recordingError;
};

// One selected ISO source at recording start: the canonical id + the roster
// display name used to sanitize the on-disk file name (spec §5). Resolved in
// MediaCore (roster lookup) so post-production sees `ISO-01-<Name>.mp4`, not a
// per-meeting participant id.
struct IsoSourceSelection {
  std::string sourceId;      // `zoom:<pid>` (ISO-1) or `capture:<id>` (ISO-3)
  std::string displayName;   // roster / device name (sanitized at file-open)
  // ISO-3 capture audio-pairing rule: a Zoom participant always has audio (its
  // `isolate_audio` stem). A `capture:<id>` source has audio ONLY when the
  // operator paired an audio input to that capture device (Elgato-class embedded
  // audio / a mic assigned to the camera via sync-capture-audio-sources); a pure
  // camera with no paired audio is a VIDEO-ONLY ISO (no all-silence AAC track).
  // Resolved in MediaCore from captureAudioSources_ at request-build time. When
  // false, the ISO writer opens WITHOUT an audio stream.
  bool hasAudio = true;
};

struct RecordingSessionRequest {
  std::string sessionId = "native-recording-session";
  std::string targetFolder = "Recordings/CoreVideo Pro/native-core";
  std::string filenamePrefix = "program";
  std::string format = "mp4";
  std::string quality = "high";
  // Legacy flat id list (back-compat: bare participant ids). Prefer isoSources
  // below, which carries display names for the ISO-1 folder/name scheme.
  std::vector<std::string> isoParticipantIds;
  std::vector<IsoSourceSelection> isoSources;
  int width = 1920;
  int height = 1080;
  int fps = 30;
  std::string videoCodec = "h264";
  std::string audioCodec = "aac";
  int audioBitrateKbps = 192;
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
  int audioBitrateKbps = 160;
  std::string videoCodec = "h264";
  std::string encoderMode = "auto";
  double keyframeIntervalSeconds = 2.0;
  std::string rateControl = "cbr";
  std::string h264Profile = "high";
  int bFrames = 2;
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
  // The underlying OS device identity (Windows device-interface symbolic link
  // for UVC devices). Lets the WinUI shell correlate a core-enumerated device
  // with its own WinRT enumeration even when the hashed stable ids disagree
  // (e.g. symbolic-link casing differences). Empty for devices that have no
  // OS-level identity (stub/virtual devices).
  std::string nativeDeviceId;
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
  int64_t startedAtMs = 0;
  int64_t lastFrameAtMs = 0;
  int64_t stoppedAtMs = 0;
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

// Cumulative source-pixel upload accounting for the GPU compositor's per-source
// texture cache. A source drawn in several passes (program + multiview + preview
// + participant export) must cost ONE upload per new frame, not one per draw —
// tests and rig telemetry assert that through these counters.
struct CompositorSourceTexStats {
  uint64_t cachedUploads = 0;   // source planes copied into a cached per-source texture
  uint64_t cacheHits = 0;       // draws served from the cache with no upload
  uint64_t textureCreates = 0;  // per-source GPU texture set (re)creations
  uint64_t scratchUploads = 0;  // legacy shared-scratch uploads (frames with no stable source id)
};

class ICompositor {
 public:
  virtual ~ICompositor() = default;
  virtual std::string rendererName() const = 0;
  virtual ProgramFrame render(const CompositorRenderPlan& renderPlan, const std::vector<VideoFrame>& frames) = 0;
  // Virtual camera: the render thread does a cheap GPU->GPU copy of the program
  // into a keyed-mutex shared texture (via the render plan's fullProgramReadback
  // flag); a dedicated device+thread reads it back AND converts it to NV12 off
  // the render/audio hot paths. This hands the caller (the audio/output worker)
  // the latest NV12 frame with a cheap ~3MB copy. Returns false when no new frame
  // is available. Defaulted to no-op so the software/stub compositor stays valid;
  // only the GPU adapter implements it.
  virtual bool takeVcamNv12(std::vector<uint8_t>& outNv12, int& width, int& height) {
    (void)outNv12;
    (void)width;
    (void)height;
    return false;
  }
  // Composites the multiview grid into a second keyed-mutex DXGI shared texture
  // (the whole grid in one texture, the OBS/broadcast-multiviewer model) and
  // returns its handle/dimensions. Defaulted to an empty texture so the
  // software/stub compositor stays valid; only the GPU adapter implements it.
  virtual ProgramFrameSharedTexture renderMultiview(const CompositorRenderPlan& renderPlan, const std::vector<VideoFrame>& frames) {
    (void)renderPlan;
    (void)frames;
    return {};
  }
  // Composites the PREVIEW scene into a third keyed-mutex DXGI shared texture
  // (its own render target, mirroring renderMultiview) and returns its handle/
  // dimensions. Defaulted to an empty texture so the software/stub compositor
  // stays valid; only the GPU adapter implements it.
  virtual ProgramFrameSharedTexture renderPreview(const CompositorRenderPlan& renderPlan, const std::vector<VideoFrame>& frames) {
    (void)renderPlan;
    (void)frames;
    return {};
  }
  // Upload accounting for the per-source texture cache. Defaulted to zeros so
  // the software/stub compositor stays valid; only the GPU adapter tracks it.
  [[nodiscard]] virtual CompositorSourceTexStats sourceTexStats() const { return {}; }
  // True when the encoder should receive the FULL-resolution program
  // (ProgramFrame::programFullBgra) while recording. Default false: on
  // Windows the D3D11 path serves recording from the vcam tap economics and
  // setting fullProgramReadback would spin that tap for nothing. The Metal
  // adapter returns true — on Apple-silicon shared memory the full readback
  // is a cheap getBytes, and it is what lets the recording encoder mux
  // native-resolution program instead of the 320x180 preview.
  [[nodiscard]] virtual bool wantsFullProgramReadbackForRecording() const { return false; }
};

class IMediaFrameSource {
 public:
  virtual ~IMediaFrameSource() = default;
  virtual std::vector<VideoFrame> pollMediaFrames(const std::vector<CompositorRenderPlanLayer>& layers, int64_t timestampMs) = 0;
  virtual std::vector<AudioFrame> pollMediaAudioFrames(const std::vector<CompositorRenderPlanLayer>& layers, int64_t timestampMs) {
    (void)layers;
    (void)timestampMs;
    return {};
  }
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
  // Times the endpoint ran completely dry between fills (shared-mode WASAPI
  // plays SILENCE into the gap — an audible glitch that was previously
  // invisible to telemetry; audio overhaul spec R5). Defaulted for the stub.
  [[nodiscard]] virtual std::int64_t underrunCount() const { return 0; }
  // The RESOLVED endpoint id of the device actually opened (IMMDevice::GetId),
  // which differs from the requested id when "" resolved to the OS default.
  // Compared against loopback capture endpoints to detect the monitor feeding
  // the very endpoint the loopback records (feedback loop — spec R6).
  [[nodiscard]] virtual std::string resolvedEndpointId() const { return {}; }
};

class IEncoderSink {
 public:
  virtual ~IEncoderSink() = default;
  virtual void configureRecording(const RecordingSessionRequest& request) = 0;
  virtual OutputSession start(const std::vector<std::string>& destinations, const std::vector<std::string>& isoParticipantIds) = 0;
  virtual void submit(const ProgramFrame& frame) = 0;
  // ISO-1: mux each selected source's OWN video into its own MP4 (mapped
  // sourceId → per-source writer), alongside the program submit above. The
  // frames carry zero-copy shared_ptr payloads (I420 for Zoom → NV12 encoder
  // input, no CPU color-convert under any lock; any convert happens on the
  // async encoder thread). Default no-op keeps non-recording / stub sinks valid.
  virtual void submitIsoVideo(const std::vector<IsoSourceVideoFrame>& sources) { (void)sources; }
  // ISO-2: mux each selected source's OWN raw-stem audio into its own MP4 (the
  // same sourceId → per-source writer map as submitIsoVideo), so each ISO is a
  // self-contained A+V file. Submitted every tick for every selected source:
  // entries with real PCM mux it; entries with empty PCM silence-fill that
  // stem's timeline to the shared epoch (spec §2c) so a gated guest's ISO has
  // silence exactly where they were not talking, staying sample-aligned to
  // program. Rides AsyncEncoderSink like the video, so a slow disk drops ISO
  // audio (→ silence in the stem, timeline intact), never program audio, never
  // a worker stall. Default no-op keeps non-recording / stub sinks valid.
  virtual void submitIsoAudio(const std::vector<IsoSourceAudio>& sources) { (void)sources; }
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
  // A3 (VST latency compensation): the program-audio content latency added by
  // an active out-of-process plugin, in samples at the program rate. The
  // recording PTS clock latches it at the FIRST audio buffer of a session so
  // the muxed audio timeline reflects the delayed content (A/V stays true).
  // Must be thread-safe (called from the audio worker; implementations store
  // an atomic). Default no-op keeps non-recording sinks valid.
  virtual void setAudioContentLatencySamples(int latencySamples) { (void)latencySamples; }
  // Finalize any open recording container(s) — moov write + writer close — so
  // the artifact on disk is playable the moment the operator stops recording,
  // WITHOUT tearing down the encoder session (streaming destinations keep
  // running). Before this seam existed the MP4 was only finalized by the sink
  // destructor (process exit) or the next start(), leaving a stopped recording
  // unplayable while the app kept running. Default no-op keeps sinks without
  // recording writers valid.
  virtual void stopRecording() {}
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
  // Non-blocking emergency cancellation used by the live async wrapper to
  // release a sender stuck in pipe/network I/O. Implementations should only
  // interrupt the transport here; normal state cleanup remains in sync().
  virtual void interrupt(const std::string&) {}
};

class ICaptureDevice {
 public:
  virtual ~ICaptureDevice() = default;
  virtual std::vector<CaptureDeviceInfo> enumerate() const = 0;
  virtual std::vector<CaptureDeviceInfo> selectInput(const std::string& deviceId, const std::string& inputId) = 0;
  virtual std::vector<CaptureDeviceInfo> setAudioSyncOffset(const std::string& deviceId, int offsetMs) = 0;
  virtual std::vector<CaptureDeviceInfo> connect(const std::string& deviceId) = 0;
  // Connect + emit this device's VideoFrames keyed by `outputSourceId` instead of
  // the adapter's own enumerated id. The shell (WinRT) and the core (Media
  // Foundation) hash the same physical camera to DIFFERENT stable ids, so without
  // this the native frame key never matches the shell's `capture:<shellId>`
  // routing and the tile stays a placeholder. Default: ignore the override and
  // connect normally (used by adapters whose ids already agree with the shell,
  // e.g. screen/window/SRT).
  virtual std::vector<CaptureDeviceInfo> connect(const std::string& deviceId,
                                                 const std::string& outputSourceId) {
    (void)outputSourceId;
    return connect(deviceId);
  }
  // Lifecycle L2: stop the device session and release its resources. Default
  // no-op for adapters without live sessions.
  virtual std::vector<CaptureDeviceInfo> disconnect(const std::string&) { return enumerate(); }
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
// createDefaultModules(), but with the encoder wrapped in AsyncEncoderSink so its
// blocking disk I/O runs on a dedicated writer thread. This is for the LIVE process
// (main.cpp): the audio/output worker submits frames every tick, so a WriteSample
// stall must never collapse the worker or block stop-recording. Tests use
// createDefaultModules directly and keep the synchronous encoder.
ModuleSet createLiveServerModules();
std::unique_ptr<ICompositor> createD3D11Compositor();
// The macOS Metal twin; returns nullptr unless COREVIDEO_WITH_METAL (mirrors
// the D3D11 null factory — each is non-null on at most one platform).
std::unique_ptr<ICompositor> createMetalCompositor();
std::unique_ptr<IMediaFrameSource> createMediaFoundationMediaFrameSource();
std::unique_ptr<IAudioMonitorOutput> createStubAudioMonitorOutput();
// macOS CoreAudio twins; nullptr unless COREVIDEO_WITH_COREAUDIO.
std::unique_ptr<IAudioMonitorOutput> createCoreAudioMonitorOutput();
std::unique_ptr<IAudioCaptureSource> createCoreAudioCaptureSource();
std::unique_ptr<IAudioMonitorOutput> createWasapiMonitorOutput();
std::unique_ptr<IAudioCaptureSource> createStubAudioCaptureSource();
std::unique_ptr<IAudioCaptureSource> createWasapiAudioCaptureSource();
std::unique_ptr<IEncoderSink> createStubRecordingEncoderSink();
// The macOS AVFoundation/VideoToolbox twin; nullptr unless COREVIDEO_WITH_AVF_ENCODER.
std::unique_ptr<IEncoderSink> createAVFoundationEncoderSink();
std::unique_ptr<IEncoderSink> createMediaFoundationEncoderSink();
std::unique_ptr<IOutputSender> createRtmpOutputSender();
std::unique_ptr<IOutputSender> createSrtOutputSender();
std::unique_ptr<IOutputSender> createNdiOutputSender();
std::unique_ptr<ICaptureDevice> createSrtIngestCaptureDevice();
std::unique_ptr<ICaptureDevice> createDeckLinkCaptureDevice();
std::unique_ptr<ICaptureDevice> createAjaCaptureDevice();
// Native UVC webcam/capture-card ingest via Media Foundation (dev-gated behind
// COREVIDEO_WITH_UVC; returns nullptr otherwise). Enumerates VIDCAP devices,
// negotiates 1080p60-targeted formats (NV12/YUY2/MJPG), and delivers I420
// frames keyed "capture:<stableDeviceId>" straight into the compositor — no
// WinUI shared-memory hop. The WinUI bridge remains the fallback path and
// supersedes these frames for the same device id.
std::unique_ptr<ICaptureDevice> createUvcCaptureDevice();

// Screen capture via Windows.Graphics.Capture (docs/capture-sources-spec.md
// SC): monitors enumerate as "screen:<n>" capture devices; frames deliver as
// BGRA keyed "capture:screen:<n>". Dev-gated (COREVIDEO_WITH_WGC); nullptr
// when the flag is off.
std::unique_ptr<ICaptureDevice> createWgcScreenCaptureDevice();

}  // namespace corevideo::modules
