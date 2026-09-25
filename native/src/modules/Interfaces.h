#pragma once

#include "contracts/Lifecycle.h"

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
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
  // Submission time in the same steady-clock/100ns domain as ProgramFrame and
  // IsoSourceAudio. The async writer must stamp media from capture/submission
  // time, never from however long this source waited behind other ISO encoders.
  int64_t timelineTimestamp100ns = 0;
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
  // Capture/submission time on std::chrono::steady_clock, expressed as 100ns
  // ticks since that clock's epoch. The encoder is asynchronous: using the
  // writer-thread processing time turns queue latency into muxed silence and
  // makes ISO files longer than Program under multi-encoder load. Zero is
  // accepted for direct/test callers and makes the sink sample its clock.
  int64_t timelineTimestamp100ns = 0;
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
  // Zoom raw audio arrives as 10 ms SDK packets while the program mixer runs
  // on a 20 ms clock. Those independent clocks need a permanent one-tick
  // cushion, just like Zoom video keeps a one-frame sync cushion. Producers
  // that set this flag opt into full-tick priming in steadyAudioFrameFeed;
  // local/media/capture sources keep their existing zero-latency behavior.
  bool requiresSteadyFeedPriming = false;
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
  // Optional in-process encoder metadata; not serialized to the shell.
  std::shared_ptr<std::atomic<int64_t>> publishedFrameNumber;
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

struct CompositorRenderPlan;

struct ProgramBufferDiagnostics {
  int requestedFrames = 3;
  int activeFrames = 0;
  int capacity = 0;
  int occupancy = 0;
  uint64_t produced = 0, delivered = 0, underruns = 0, overflows = 0;
  uint64_t gpuNotReady = 0, deadlineMisses = 0;
  uint64_t displayUnconsumed = 0, displayBusy = 0;
  uint64_t prepared = 0;
  double lastQueueWaitMs = 0, maxQueueWaitMs = 0;
  double lastPreparationMs = 0, maxPreparationMs = 0;
  uint64_t generation = 0;
  std::string status = "unsupported";
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
  // Zero-copy ownership path for the Windows program tap. ProgramFrame is copied
  // into recorder and sender queues every frame; sharing this immutable 1080p
  // buffer avoids several redundant ~3 MB copies per output. The vector above
  // remains as the compatibility and test path.
  std::shared_ptr<const std::vector<uint8_t>> programNv12Shared;
  [[nodiscard]] const std::vector<uint8_t>& programNv12Bytes() const noexcept {
    return programNv12Shared ? *programNv12Shared : programNv12;
  }
  ProgramFrameSharedTexture sharedTexture;
  // Dedicated GPU keyed-mutex DXGI shared texture for hardware H.264 encode.
  // Separate from sharedTexture so sender encode does not contend with WinUI frame
  // consumption while the sender runs at encoder cadence.
  ProgramFrameSharedTexture encoderSharedTexture;
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
  // Submission time on std::chrono::steady_clock, expressed as 100ns ticks.
  // Kept at the tail to preserve existing positional aggregate initializers.
  // Recording sinks use it instead of writer-thread time so encoder/disk queue
  // latency cannot stretch the Program video timeline away from audio.
  int64_t timelineTimestamp100ns = 0;
  int64_t deliverySequence = 0;
  int64_t producedAt100ns = 0;
  int64_t deliveredAt100ns = 0;
  int64_t productionSlot = -1;
  int64_t productionAnchorNs = 0;
  // Retain GPU resources and attribution for this exact buffered frame.
  std::shared_ptr<const void> gpuOwner;
  std::shared_ptr<const CompositorRenderPlan> renderPlanEvidence;
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

// Per-layer chroma key (the green/blue screen keyer).
//
// The core advertised a "chroma-key" capability — and listed it as REQUIRED —
// while implementing none of it: the only command carrying a chromaKey payload
// discarded it (setParticipantTransform takes an UNNAMED rpc::Json), there were
// no key fields on the render plan, and neither shader had keying math. Anything
// gating on that capability got a true answer that meant nothing.
//
// `similarity` is the chroma distance at which a pixel becomes fully
// transparent; `smoothness` widens the transition either side of it so edges do
// not alias; `spill` pulls the key hue out of surviving pixels (green fringing
// on hair and shoulders). All normalized 0..1.
struct CompositorChromaKey {
  float keyR = 0.f;
  float keyG = 1.f;   // green screen by default
  float keyB = 0.f;
  float similarity = 0.4f;
  float smoothness = 0.1f;
  float spill = 0.2f;
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

// Canvas-pixel Tiles decoration; disabled for every ordinary scene layer.
struct TilesDecoration {
  bool enabled = false;
  bool glowPass = false;
  float borderWidth = 0.f;
  float radius = 0.f;
  std::string borderColor = "#000000";
  float glowSize = 0.f;
  float glowIntensity = 1.f;
  float glowSoftness = 0.f;
  std::string glowColor = "#FFFFFF";
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
  TilesDecoration tilesDecoration;
  float sourceCropLeftPercent = 0.f;
  float sourceCropRightPercent = 0.f;
  float sourceScale = 1.f;
  float sourceOffsetX = 0.f;
  float sourceOffsetY = 0.f;
  std::string mediaAssetId;
  std::string mediaAssetName;
  std::string mediaAssetKind;
  std::string mediaAssetPath;
  // Backgrounds are continuous show elements; stingers/clips remain one-shot.
  // Kept on the shared render layer so SuperSource and Tiles use the same
  // playback contract rather than inventing independent loop controls.
  bool mediaAssetLoop = false;
  bool hasColorGrade = false;
  CompositorColorGrade colorGrade;
  // Keying is per-LAYER, not per-participant: the same camera can be keyed in
  // one scene and not another, which a per-participant model cannot express.
  bool hasChromaKey = false;
  CompositorChromaKey chromaKey;
  // Set for overlay/lower-third/caption layers. Empty (default) for video
  // sources, which leaves overlay rendering on the prior solid-fill fallback.
  bool hasOverlayContent = false;
  CompositorOverlayContent overlay;
  // Optional hard viewport boundary for a remapped composite. Multiview PGM/PVW
  // layers use this so animated overlays cannot escape their monitor cell.
  bool hasClipRect = false;
  CompositorLayerRect clipRect;
  // Explicit solid-fill colour for a layer with no source frame at all (e.g.
  // the tiles wall background). Both the D3D11 compositor (resolveLayers) and
  // the CPU preview path (buildProgramFramePreview) honor this when set,
  // parsing it with the same compositor::parseHexColorRgba() used for
  // brand/border colors elsewhere. A layer with a real participant/media
  // source ignores this and uses its frame (or the bus-health slate —
  // warming/failed, #535 slice 4a) instead — this is ONLY for layers that are
  // deliberately sourceless.
  bool hasFillColor = false;
  std::string fillColor = "#808080";
  // bus health on air (#535 slice 4a): filled by MediaCore at plan build;
  // compositors resolve slate/black/frame from these, never from the id.
  std::string sourceHealth;
  std::string dropoutPolicy = "hold";
  std::string sourceDisplayName;
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
  uint64_t droppedVideoFrames = 0, droppedAudioPackets = 0;
  uint64_t queuedVideoFrames = 0, queuedAudioPackets = 0;
  uint64_t videoWorkUs = 0, audioWorkUs = 0, maximumWorkUs = 0;
  std::string sourceId;
  std::string displayName;
  std::string path;
  std::string kind = "iso";
  int64_t videoFrameCount = 0;
  int64_t audioSampleCount = 0;  // 0 until ISO-2 muxes per-source audio
  int64_t bytesWritten = 0;
  bool trackOpen = false;        // the writer opened + began writing its video track
  std::string encoderPath;       // "hardware" or "software" placement for this ISO
  std::string fallbackReason;    // e.g. hardware-capacity-exhausted
  std::string warning;           // per-source open/write failure (empty = healthy)
};

struct OutputSession {
  bool active = false;
  // Present only when the sink reports observed writer lifecycle.
  std::optional<contracts::OutputLifecycle> lifecycle;
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
  int64_t recordingProgramBytesWritten = 0;
  int64_t recordingDurationMs = 0;
  int64_t recordingVideoFrameCount = 0;
  // Scheduled Program slots absent between accepted real samples. Overlaps
  // encoder queue losses; do not sum the counters. Head/tail are unverified.
  int64_t recordingProgramMissingFrames = 0;
  bool recordingProgramContinuityObserved = false;
  int64_t recordingVideoPrerollFrameCount = 0;
  int64_t recordingVideoTailFrameCount = 0;
  int64_t recordingMuxVideoFrameCount = 0;
  int64_t recordingRequestedAt100ns = 0;
  int64_t recordingWriterReadyAt100ns = 0;
  int64_t recordingMuxEpoch100ns = 0;
  int64_t recordingStartupDroppedAudioPackets = 0;
  int64_t recordingAudioPrerollSampleCount = 0;
  int64_t recordingAudioTailSampleCount = 0;
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
  // Truth from the async writer queue. These are cumulative for the encoder
  // session and let the UI/support bundle surface back-pressure that was
  // previously visible only in stderr logs.
  int64_t encoderQueueDroppedVideoFrames = 0;
  int64_t encoderQueueDroppedAudioPackets = 0;
  // Video shed while the recording writer was still applying its SYNCHRONOUS
  // open (95-250ms on Windows). Kept out of encoderQueueDroppedVideoFrames for
  // the same reason recordingStartupDroppedAudioPackets is kept out of the
  // audio counter: it is a clipped head, not steady-state loss, and folding the
  // two together made every clean run report drops to a fail-closed judge.
  int64_t recordingStartupDroppedVideoFrames = 0;
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
  // Steady-clock capture boundary; zero retains legacy first-media epoch.
  int64_t captureEpoch100ns = 0;
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
  // Mux the program from ProgramFrame::programNv12 (the full-resolution GPU tap)
  // instead of the 320x180 `preview` thumbnail. Set by MediaCore only when the
  // compositor actually supplies that tap AND the requested recording size
  // matches it exactly — writing a differently-sized buffer into the writer is
  // precisely the defect this exists to fix. See ICompositor::suppliesProgramNv12.
  bool programNv12 = false;
};

// PR19 supervisor state for one destination. Declared here (rather than in the
// supervisor header) only because it rides on OutputSender; the supervisor owns
// every value in it.
struct OutputSupervisorState {
  // Monotonic per-destination run generation. Bumped on every restart and on
  // every operator re-arm, so a stale event from a retired child is rejectable.
  std::uint64_t generation = 0;
  // FRESH evidence at read time: accepted units advanced within the staleness
  // budget. Never a latch, never "we launched it".
  bool healthy = false;
  // The ladder ran out (or the failure was terminal). The destination stays
  // published as failed until an operator re-arms it.
  bool gaveUp = false;
  int consecutiveFailures = 0;
  int restarts = 0;
  std::int64_t nextAttemptInMs = 0;
  std::int64_t lastProgressAgeMs = -1;  // -1 = nothing has ever been accepted
  std::int64_t acceptedUnits = 0;
  // Evidence discarded because it belonged to a retired generation, and replies
  // discarded because they were malformed. Both are counted rather than hidden.
  std::int64_t staleEventsRejected = 0;
  std::int64_t malformedObservations = 0;
  std::string failureClass = "none";  // none | retryable | terminal
  std::string reason;
  // TRUE = this destination's media path runs inside corevideo-native.exe and a
  // wedge in it cannot be released (NDI today). Published so the residual risk
  // is visible in a bundle instead of being an implementation detail.
  bool inProcessRisk = false;
  bool interruptible = false;
};

// #597: the backpressure policy's view of THIS destination. Published
// UNCONDITIONALLY for a GPU-direct sender (the multiviewer-node rule: a node
// that vanishes in the case worth detecting is the mistake). A stream quietly
// running at 15 fps is the same class of defect as a silent codec downgrade.
// Absent means this destination is not on the GPU-direct path and has no
// bitstream queue to observe, which is NOT the same as "healthy" - never read
// an absent value as divisor 1 evidence.
//
// `divisor` (1 = every frame at the product's rate, 2 = half, 3 = a third,
// 4 = a quarter) is Lever A's input throttle. NO shedFrames HERE: the
// compositor sheds once for every destination (one encoder texture serves
// them all), so a per-sender count would claim this destination shed frames
// on its own. The real, global count is published once, at
// realtimeEvidence.encoderExport.shedFrames.
struct OutputBackpressureState {
  // THIS DESTINATION'S REQUEST, NOT THE RATE IT IS FED AT.
  //
  // FINAL-REVIEW FINDING 3. Lever A is per-ENCODER, not per destination: ONE
  // encoder texture feeds every GPU-direct sender, so MediaCore takes the MAX
  // divisor across the active ones and drives
  // ICompositor::setEncoderExportDivisor with that. The lever's limitation is
  // named in three places (MediaCore.h, the spec Outcome, CLAUDE.md) - but the
  // NODE's was not, and the node is what an operator readout binds to. With two
  // GPU-direct destinations the healthy sibling's node reported `divisor: 1`
  // while it was actually being fed at the maximum across senders, and a
  // destination added mid-show beside a throttled sibling published a
  // textbook-healthy reading at 15 fps.
  //
  // The snapshot therefore publishes BOTH: `divisor` (this value - what this
  // destination is asking for, which is what its own hysteresis and counters
  // are keyed on) and `appliedDivisor` (what the compositor is actually
  // exporting at, written by MediaCore where that fact exists - see
  // MediaCore::applyEncoderExportDivisor). Read `appliedDivisor` for the rate;
  // read `divisor` for this destination's own state. They differ exactly when a
  // sibling is worse off.
  int divisor = 1;
  // 0 = not throttled; StreamBackpressurePolicy::kMaxDivisor - 1 at the floor.
  // Always divisor - 1; published separately because a consumer should not
  // have to re-derive it.
  int level = 0;
  // Wall-clock age (ms) of the oldest chunk still queued for send, AS OF THE
  // OBSERVATION THAT DROVE THIS TICK'S DECISION - not necessarily the queue's
  // current state a moment later. On a tick where Lever B fires, this is
  // re-read AFTER the discard (see observeStreamBackpressure()), so it and
  // `queuedChunks` below describe the SAME instant rather than a pre-discard/
  // post-discard mismatch.
  std::int64_t bufferedMs = 0;
  std::int64_t queuedChunks = 0;
  // A chunk leaves the queue before the blocking FFmpeg pipe write begins.
  // These separate measurements expose that otherwise invisible interval.
  // inFlightWriteMs is zero between writes; maxWriteMs and slowWriteCount are
  // cumulative for this stream run. They are observations, not policy inputs.
  std::int64_t inFlightWriteMs = 0;
  std::int64_t maxWriteMs = 0;
  std::int64_t slowWriteCount = 0;
  // Times throttling was ENGAGED (1 -> 2). Steps within a throttle do not count.
  std::int64_t enteredCount = 0;
  // Cumulative chunks Lever B (the GOP-tail discard) has dropped from THIS
  // destination's queue. Per-stream-run: reset alongside the policy object
  // whenever this destination stops (see the `!wantsRtmp` stop path).
  // ONE discard EVENT (see discardEvents below) can drop MANY chunks - a
  // whole GOP tail at once - so discardedChunks >= discardEvents always, and
  // discardedChunks / discardEvents is the mean GOP tail length discarded.
  // They are deliberately two different counters, not a duplicate: discardEvents
  // answers "how many times did Lever B fire", discardedChunks answers "how
  // much video did it actually cost".
  std::int64_t discardedChunks = 0;
  // Cumulative GOP-tail discard events fired. See discardedChunks above for
  // how the two diverge.
  std::int64_t discardEvents = 0;
  // Why the divisor or discard state last changed:
  // "none" | "buffered-above-threshold" | "recovered" | "backlog-discard".
  // A static string literal from StreamBackpressurePolicy - never heap-owned,
  // so publishing it every tick (the 60 Hz output path, one GPU-direct sender)
  // costs no allocation.
  //
  // FINAL-REVIEW FINDING 12, said rather than changed: BOTH LEVERS WRITE THIS
  // ONE FIELD. On a tick where Lever B discards AND the divisor steps, the
  // policy's discard branch and its divisor ladder both assign `lastReason_`
  // in the same observe() call, and the ladder wins - so the published reason
  // reads "buffered-above-threshold" and the discard that happened on that same
  // tick is invisible HERE. Nothing is lost: `discardEvents` and
  // `discardedChunks` still count it, and the `[stream-backpressure] discard`
  // log line still names it. Do not read an absent "backlog-discard" as
  // evidence no discard occurred on that tick; read the counters.
  const char* lastReason = "none";
  // The bufferedMs observed on the tick that caused the last change.
  std::int64_t lastTransitionBufferedMs = 0;
  // Bumped every time the underlying StreamBackpressurePolicy is RECONSTRUCTED
  // (the `!wantsRtmp` stop path) - i.e. every time the per-run counters above
  // reset to zero. The node itself goes ABSENT between the stop and the next
  // GPU-direct tick, but that gap is invisible across a poll interval (the
  // shell polls at 250ms); `runId` gives a consumer that DID catch two
  // consecutive readings a way to tell "these counters are continuous, they
  // can only have grown" (same runId) from "a reset happened between these
  // two readings, do not diff them" (different runId) even without observing
  // the absence in between.
  std::int64_t runId = 0;
  // #597 fix round 3. Why `discardedChunks` and `discardEvents` can disagree,
  // carried NEXT TO THEM so a reader of the snapshot meets the explanation
  // where they meet the numbers.
  //
  // FINAL-REVIEW FINDING 11: this was a `std::string`, so a ~300-byte
  // allocation and copy rode every sync tick - and every OutputSenderSession
  // copy through AsyncOutputSender's snapshot mutex and into sessionState() -
  // two lines below the comment justifying `lastReason` as a `const char*`
  // precisely to avoid that. It is a compile-time constant sentence; it is now
  // typed as one.
  const char* discardCounterNote = "";
  // Wall-clock elapsedMs (the same clock every other OutputSender timestamp on
  // this destination uses - see startedAtMs/stoppedAtMs) at the moment this
  // state was last written by observeStreamBackpressure(). sync() has several
  // early-return paths above the Lever A/B observation (missing settings,
  // missing endpoint, no runtime, no frame yet, no program pixels yet); this
  // node is NOT refreshed on those ticks and snapshot() re-serves the same
  // struct - observedAtMs is what lets a consumer tell that a freeze happened,
  // the "a peek is not an observation" rule applied here.
  double observedAtMs = 0;
};

// TEST-ONLY (see IOutputSender::bitstreamQueueSnapshotForTest). A single
// struct so a test needs ONE call to read the queue back, not three.
struct BitstreamQueueSnapshotForTest {
  std::size_t depth = 0;
  bool hasKeyframe = false;
  std::int64_t bufferedMs = 0;
  // #597 Task 8b. True once the queue's overflow path has FAILED the sender
  // (BitstreamFailure::QueueOverflow). The sender-level symptom - status
  // "failed", lastResultCode "bitstream-queue-overflow" - only appears on a
  // sync() tick that actually reaches submitFrameToGpuEncoder(), i.e. with a
  // real hardware encoder and a launched FFmpeg, so a test that drives the
  // queue alone needs to read the failure where it is RECORDED.
  bool overflowFailed = false;
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
  // PR22: every destination carries a lifecycle and a terminal outcome. Before
  // this an RTMP/SRT/NDI stream that died mid-show had NOTHING an operator or a
  // support bundle could read as a state — only free-text adapter status
  // strings that no consumer agreed on. Recording had per-stream outcomes;
  // senders had none. Populated centrally by MediaCore from the pure
  // core::SenderLifecyclePolicy, so the three adapters keep exactly one status
  // machine each instead of gaining a second.
  std::optional<contracts::OutputLifecycle> lifecycle;
  // PR19: the output supervisor's view of this destination — its process/run
  // generation, whether it is healthy on FRESH evidence (accepted units
  // advancing, not a launch), where it sits on the restart ladder, and whether
  // its media path is actually isolated from ours. Published verbatim into
  // sessionState so a support bundle from a machine we cannot see says which
  // destination failed, how often, and why we stopped restarting it.
  // Populated by modules::SupervisedOutputSender; absent when a build wires an
  // output sender without a supervisor (unit tests, the synthetic sender).
  std::optional<OutputSupervisorState> supervisor;
  // #597 Lever A. Set by the sender on every sync while it is running
  // GPU-direct; absent on the raw CPU path (which already drops stale frames
  // and is deliberately untouched by this lever) and on a sender that has not
  // started. MediaCore reads it and drives ICompositor::setEncoderExportDivisor.
  std::optional<OutputBackpressureState> backpressure;
  // AsyncOutputSender's own worker, independent of FFmpeg pipe writes. A
  // destination can stop accepting sync/audio while the encoder's separate
  // thread continues to write video; pipe telemetry alone misses that pause.
  struct AsyncWorkerState {
    std::string operation = "idle";
    std::string stage;
    std::int64_t operationAgeMs = 0;
    std::int64_t queuedItems = 0;
    std::int64_t droppedSyncs = 0;
  };
  std::optional<AsyncWorkerState> asyncWorker;
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

class IZoomAudioConsumer {
 public:
  virtual ~IZoomAudioConsumer() = default;
  virtual void publish(AudioFrame frame) = 0;
};

class IZoomCaptureSource {
 public:
  virtual ~IZoomCaptureSource() = default;
  // Drain pictures pushed for this tick. The render tick calls this only when
  // no Zoom engine is configured, at the same site that used to pull
  // pollVideoFrames(). Live engine frames stay on the source bus.
  std::vector<VideoFrame> deliverVideo() {
    captureVideoTick();
    std::lock_guard<std::mutex> lock(zoomVideoMutex_);
    std::vector<VideoFrame> frames;
    frames.swap(videoQueue_);
    return frames;
  }
  // Drain packets pushed since the last call. The audio worker calls this
  // outside coreMutex, at the same site that used to pull pollAudioFrames().
  void deliverAudio(IZoomAudioConsumer& consumer) {
    captureAudioTick();
    std::vector<AudioFrame> frames;
    {
      std::lock_guard<std::mutex> lock(zoomAudioMutex_);
      frames.swap(audioQueue_);
    }
    for (auto& frame : frames) consumer.publish(std::move(frame));
  }

 protected:
  virtual void captureVideoTick() {}
  virtual void captureAudioTick() {}
  void postVideo(VideoFrame frame) {
    std::lock_guard<std::mutex> lock(zoomVideoMutex_);
    videoQueue_.push_back(std::move(frame));
  }
  void postAudio(AudioFrame frame) {
    std::lock_guard<std::mutex> lock(zoomAudioMutex_);
    audioQueue_.push_back(std::move(frame));
  }

 private:
  std::mutex zoomVideoMutex_;
  std::vector<VideoFrame> videoQueue_;
  std::mutex zoomAudioMutex_;
  std::vector<AudioFrame> audioQueue_;
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
  // Startup-only configuration. Unsupported compositors report zero active
  // frames, so consumers must not introduce an unmatched audio delay.
  virtual void configureProgramBuffer(int /*frames*/) {}
  // Allocate startup resources before the render clock starts. No frames or
  // delivery timestamps may be produced by this call. Resize stays on render.
  virtual void prepareProgramBuffer(int /*width*/, int /*height*/) {}
  virtual void setProgramProductionTiming(int64_t /*slot*/, int64_t /*anchorNs*/) {}
  [[nodiscard]] virtual int programBufferFrames() const { return 0; }
  virtual bool latestDeliveredProgramFrame(ProgramFrame& /*out*/) const { return false; }
  virtual bool takeDeliveredProgramFrame(ProgramFrame& /*out*/, int /*timeoutMs*/) { return false; }
  [[nodiscard]] virtual ProgramBufferDiagnostics programBufferDiagnostics() const { return {}; }
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
  // Ownership-preserving output fan-out. D3D11 returns its immutable tap buffer
  // directly so recorder, stream and virtual camera can share it. Older
  // compositors bridge through their existing copy-out implementation.
  virtual bool takeVcamNv12Shared(std::shared_ptr<const std::vector<uint8_t>>& outNv12,
                                  int& width, int& height) {
    std::vector<uint8_t> copy;
    if (!takeVcamNv12(copy, width, height)) return false;
    outNv12 = std::make_shared<const std::vector<uint8_t>>(std::move(copy));
    return true;
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

  // Does this compositor publish the full-resolution program as NV12
  // (ProgramFrame::programNv12)? The D3D11 adapter does — that tap is what feeds
  // the virtual camera and RTMP — while Metal publishes programFullBgra instead.
  // Recording uses it to mux real program pixels rather than the 320x180
  // preview; without it the whole show lands in a corner of a black frame.
  [[nodiscard]] virtual bool suppliesProgramNv12() const { return false; }

  // PUSH the program tap at the RENDER cadence instead of waiting to be polled.
  //
  // The virtual camera used to be published by the ~50Hz audio/output worker,
  // which polls takeVcamNv12 once per tick. That worker's 20ms period is an
  // AUDIO constant (960 samples at 48k), and gating video on it meant a 60fps
  // program reached every output at 50fps — measured 2026-08-07: render thread
  // 59.7fps, output worker 49.7Hz, virtual camera published 50.0fps. That is 10
  // discarded frames a second AND up to 20ms of quantisation on a path whose
  // whole budget is one 16.7ms frame.
  //
  // The sink is invoked on the tap thread the moment a new NV12 frame exists,
  // holding NO compositor lock. The callee must be cheap and must not block on
  // the render thread. Setting or clearing the sink waits for any in-flight
  // call, so clear it before the callee is destroyed.
  // Ownership is shared so the tap can hand the same immutable NV12 frame to
  // multiple outputs without a 3 MB memcpy on its cadence. In particular, the
  // virtual-camera publisher queues this buffer to its own latest-frame worker;
  // a slow Windows Frame Server consumer must never pace streaming/recording.
  using VcamFrameBuffer = std::shared_ptr<const std::vector<std::uint8_t>>;
  using VcamFrameSink = std::function<void(VcamFrameBuffer nv12, int width, int height)>;
  virtual void setVcamFrameSink(VcamFrameSink /*sink*/) {}
  // Does this compositor push frames to that sink? When it does, MediaCore must
  // NOT also publish from the output worker or every frame is published twice.
  [[nodiscard]] virtual bool publishesVcamFrames() const { return false; }

  // #597 Lever A: FEED THE ENCODER FEWER FRAMES, at the one place that actually
  // paces it. The hardware encoder's thread advances on the KEYED MUTEX of the
  // encoder shared texture - it acquires, encodes, releases, and can only
  // acquire again once the compositor's next export releases a new frame. Task 1
  // measured this directly: skipping only the sender's submit() while still
  // exporting every render left the stream byte-identical (ratio 0.998), while
  // halving the EXPORT rate halved egress (0.500). So the throttle lives here,
  // in the producer, not in the sender.
  //
  // At divisor d this compositor exports the encoder texture on RENDER FRAME
  // NUMBERS divisible by d and holds the rest, so the resulting encoder input
  // rate is programFps / d - NOT a fixed 60/30/20/15 ladder.
  //
  // FINAL-REVIEW FINDING 9: that ladder was stated as an invariant here, in
  // CLAUDE.md and - load-bearingly - in the spec's justification for
  // kMaxDivisor ("15 fps is the lowest frame rate worth putting on air"). It is
  // only true AT A 60 fps PROGRAM. `startProgramOutput` clamps `outputFps_` to
  // 1-120, so at a 30 fps program the same ladder is 30 / 15 / 10 / 7.5 fps and
  // the floor's whole justification is gone. Say the function, not one of its
  // values: divisor 1/2/3/4 feeds programFps / 1, / 2, / 3, / 4 - which IS
  // 60/30/20/15 at the 60 fps program this product targets, and is the number
  // to recompute for any other configured rate.
  //
  // The encoder's DECLARED frame rate never changes, so bits-per-frame - and
  // with it per-frame quality - is untouched.
  //
  // Control plane, not a per-frame call: MediaCore calls this only when the
  // value CHANGES. Defaulted to a no-op so the Metal and stub compositors are
  // unaffected; only the D3D11 adapter, which owns the encoder export,
  // implements it.
  virtual void setEncoderExportDivisor(int /*divisor*/) {}

  // #597 Task 6: the ONE effective divisor this compositor is applying, and
  // the frames it has actually held back because of it - published together
  // at realtimeEvidence.encoderExport, unconditionally, like the multiviewer
  // node. Defaulted to 1/0 (the healthy reading) so Metal and the stub
  // compositor are unaffected; only the D3D11 adapter, which owns the encoder
  // export, tracks a real shed count.
  //
  // encoderExportShedFrames() is CUMULATIVE FOR THE LIFE OF THE PROCESS - it is
  // never reset when a stream stops, unlike the per-sender counters beside it
  // in OutputBackpressureState (divisor/enteredCount/discardedChunks/discardEvents
  // are all per-run). A consumer computing "this show shed N frames" must diff
  // it itself across the run boundary; reading it directly includes every
  // previous run's sheds.
  [[nodiscard]] virtual int encoderExportDivisor() const { return 1; }
  [[nodiscard]] virtual std::int64_t encoderExportShedFrames() const { return 0; }
  // #597 Task 6 fix round 1, finding 9 (fix round 2, item 2: corrected claim):
  // was this compositor exporting the dedicated encoder texture on its last
  // render tick (the last tick's `renderPlan.fullProgramReadback`)? THIS IS
  // NOT "a stream is live" - `fullProgramReadback` is
  // `virtualCameraEnabled_ || outputActive || recording` (see MediaCore.cpp),
  // so `exporting` reads true with only the virtual camera on, or only a
  // recording running, and NO stream at all. It answers exactly one question:
  // "is SOMETHING consuming the encoder texture right now" - which is enough
  // to tell a genuinely fresh divisor of 1 (nothing consuming it, texture
  // export idle) from a stale non-1 divisor that could still be latched from
  // a stream that already ended (see `encoderExportDivisor()`'s doc and
  // MediaCore::applyEncoderExportDivisor's stop-path residual) - it does NOT
  // by itself prove a live STREAM is throttled; `divisor > 1` while `exporting`
  // is equally consistent with "the vcam or a recording is on and a throttled
  // stream ended minutes ago". Defaulted false so Metal/stub read as not
  // exporting, which is the honest answer for a compositor that never does.
  [[nodiscard]] virtual bool encoderExporting() const { return false; }
};

// One clip, for the one decoder MediaTransports owns per source. Play, pause,
// and loop are properties of that clip. The compositor plan is not a decode
// request: a plan layer never carries play state.
struct MediaDecodeRequest {
  std::string kind;  // "media-video" or "media-background"
  std::string sourceId;
  std::string assetId;
  std::string assetKind;
  std::string assetPath;
  bool playing = false;
  bool loop = false;
};

class IMediaDecoder {
 public:
  virtual ~IMediaDecoder() = default;
  virtual std::vector<VideoFrame> pollMediaFrames(const MediaDecodeRequest& request, int64_t timestampMs) = 0;
  // Exact monotonic presentation time for scheduled media. Legacy adapters
  // retain their millisecond contract through this additive default.
  virtual std::vector<VideoFrame> pollMediaFramesAt100ns(const MediaDecodeRequest& request, int64_t timestamp100ns) {
    return pollMediaFrames(request, timestamp100ns / 10000);
  }
  virtual std::vector<AudioFrame> pollMediaAudioFrames(const MediaDecodeRequest& request, int64_t timestampMs) {
    (void)request;
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

// The cheap per-item view of a recording writer (#529).
//
// `OutputSession` is the FULL status, and for the Media Foundation sink
// producing it means rebuilding every per-ISO status record (two mutexes and
// two string-bearing structs per ISO writer) and copying the whole struct by
// value. `AsyncEncoderSink`'s writer thread asked for that after EVERY queue
// item — ~540/s at eight ISOs plus Program at 60fps — when all it needed per
// item was "did the writer move, and did it fail".
//
// Splitting the two questions is what lets the full status be read rarely
// WITHOUT any sink ever answering a read with stale numbers: a submit-then-read
// caller (EncoderRecordingSession's tests, and anything that asks session()
// directly) still gets exact, current counts.
struct EncoderProgress {
  int64_t videoFramesWritten = 0;
  int64_t audioPacketsWritten = 0;
  int64_t droppedVideo = 0;
  int64_t droppedAudio = 0;
  std::string error;
  std::string warning;
};

class IEncoderSink {
 public:
  // Called once by AsyncEncoderSink before media submission. Each ISO file
  // may own an independent ordered writer; direct synchronous callers retain
  // their existing behavior.
  virtual void enableIndependentIsoWriters() {}
  virtual ~IEncoderSink() = default;
  // Cheap progress, called once per queue item by the async writer thread.
  // The DEFAULT derives it from session(), so every existing sink stays correct
  // with no edit — it simply does not get the saving. A sink whose session() is
  // expensive (the Media Foundation one) overrides this to read the counters it
  // already maintains, without rebuilding anything.
  [[nodiscard]] virtual EncoderProgress progress() const {
    const auto snapshot = session();
    return EncoderProgress{snapshot.recordingVideoFrameCount, snapshot.recordingAudioPacketCount,
                           snapshot.encoderQueueDroppedVideoFrames,
                           snapshot.encoderQueueDroppedAudioPackets, snapshot.recordingError,
                           snapshot.recordingWarning};
  }
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
  // Preserve the producer timestamp across asynchronous writers. Legacy sinks
  // retain their existing audio behavior through this default forwarding seam.
  virtual void submitAudioAt(const float* interleaved, int frameCount, int channels, int sampleRate,
                             int64_t timelineTimestamp100ns) {
    (void)timelineTimestamp100ns;
    submitAudio(interleaved, frameCount, channels, sampleRate);
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
  // Push program AUDIO alone, on the AUDIO cadence, decoupled from sync()'s video
  // cadence. Video and audio reach FFmpeg through SEPARATE inputs (a raw video
  // pipe and a PCM audio pipe), so they never needed to arrive together — but
  // carrying audio as a sync() argument tied both to one call, and that call was
  // the ~50Hz audio worker's. A 60fps program was therefore streamed at 50fps.
  // The video tick now calls sync() (video), the audio worker calls this.
  //
  // Senders that carry audio must treat "we have real audio" as STICKY state set
  // here, not as "this sync() call happened to carry PCM" — otherwise a video-only
  // sync looks like audio disappearing and restarts the encoder process.
  virtual void submitAudio(const std::vector<float>& /*pcm*/, int /*channels*/, int /*sampleRate*/) {}
  virtual OutputSenderSession fail(const std::string& destination, const std::string& message, double elapsedMs) = 0;
  virtual OutputSenderSession recover(const std::string& destination, double elapsedMs, const std::string& reason) = 0;
  // The destination's OWN SUPERVISOR restarting it automatically, as opposed to
  // recover(), which is the OPERATOR re-arming it. They were the same call until
  // #597 task 7 round 1, and a sender that holds its own restart floor cannot
  // treat them alike: a supervisor restart is precisely the restart the floor
  // exists to bound, while an operator is entitled to an immediate retry (the
  // house rule "an operator action always clears give-up"). The default forwards
  // to recover(), so every sender without a floor is unchanged - but the WRAPPER
  // LAW still applies: AsyncOutputSender / CompositeOutputSender /
  // SupervisedOutputSender must FORWARD it, or the distinction is silently
  // swallowed one layer up, which is the 1-arg connect() shape.
  virtual OutputSenderSession restartForSupervisor(const std::string& destination, double elapsedMs,
                                                   const std::string& reason) {
    return recover(destination, elapsedMs, reason);
  }
  virtual OutputSenderSession session() const = 0;
  // Optional lock-free stage of an in-flight sender call. AsyncOutputSender
  // reads this while its worker may be blocked; implementations must not take
  // a transport or state lock here.
  virtual const char* diagnosticStage() const { return ""; }
  // Non-blocking emergency cancellation used by the live async wrapper to
  // release a sender stuck in pipe/network I/O. Implementations should only
  // interrupt the transport here; normal state cleanup remains in sync().
  virtual void interrupt(const std::string&) {}
  // THE WRAPPER LAW APPLIES TO EVERY TEST-ONLY VIRTUAL BELOW, and none of them
  // is forwarded by AsyncOutputSender / SupervisedOutputSender /
  // CompositeOutputSender / the NDI sender. A test must therefore hold the
  // CONCRETE sender, never a wrapped one. That is survivable only because each
  // read seam's default is a ZERO/false value that makes a test's precondition
  // ASSERT fail loudly (e.g. ASSERT_EQ(before.depth, 4u)) rather than pass with
  // nothing under test - the silent-swallow shape that cost this codebase the
  // 1-arg connect() pink tiles and SRT's dropped pollAudioFrames.
  //
  // FINAL-REVIEW FINDING 10. This block used to end "If a THIRD seam is ever
  // wanted here, do not add it: move these behind a narrow
  // IBitstreamQueueTestAccess in its own header, reached by a
  // createRtmpOutputSenderForTest()." THERE ARE NOW FIVE. Tasks 5, 6 and 8b
  // each added one past that line without amending it, which is the same
  // failure mode as a disarmed assertion: an unamended rule nobody obeys stops
  // being a rule and becomes noise a reader learns to skip.
  //
  // THE RULE IS CORRECTED RATHER THAN OBEYED, and that choice is stated here
  // so the next reader can disagree with it knowingly. The extraction is the
  // right end state and is deliberately NOT done here: it moves five virtuals,
  // every call site in MediaCoreCommandTest.cpp and
  // OutputDestinationSupervisorTest.cpp, and the wrapper-law reasoning above -
  // a pure-refactor risk taken at merge time, on a branch whose whole subject
  // is an on-air regression, for no behavioural gain. Filed as
  // https://github.com/iamfatness/CoreVideoPro/issues/611.
  //
  // WHAT HOLDS UNTIL THEN: the guarantee above is PER-SEAM, not per-count.
  // Every one of the five defaults to a ZERO/false value that makes a test's
  // precondition ASSERT fail loudly rather than pass with nothing under test,
  // so five is no less safe than two. DO NOT ADD A SIXTH to this interface:
  // the next seam that is wanted is the trigger to do the extraction, and the
  // extraction is IBitstreamQueueTestAccess in its own header, reached by a
  // createRtmpOutputSenderForTest() that returns the concrete type.
  //
  // TEST-ONLY seam, structural guard only - the same guarantee
  // MediaCore::setStillImageDecoderForTest relies on: no env var, no command,
  // no config key and no wire field reaches it, and nothing outside
  // native/tests calls it. (A compile-time gate is impossible here:
  // corevideo-native-tests links the same corevideo_native library the product
  // does, so gating the seam out would delete it from the tests too.)
  //
  // Overrides the bitstream-queue measurement #597's Lever A observes, so the
  // backpressure decision can be driven without a hardware encoder and a real
  // congested network. A NEGATIVE bufferedMs clears the override and restores
  // the real measurement.
  virtual void setBackpressureObservationForTest(std::int64_t /*bufferedMs*/,
                                                 bool /*keyframeInQueue*/) {}
  // TEST-ONLY (same structural guard). Answers "would THIS program frame make
  // the sender flip its encode path?" - i.e. tear down FFmpeg AND the hardware
  // encoder and relaunch both. It evaluates the exact comparison
  // ensureFfmpegProcess makes (`resolveGpuEncodePath(frame) != activeUseGpuDirect_`)
  // and then ADOPTS the result the way a restart would, so a test can walk a
  // frame sequence the way the live sender walks it and count the restarts.
  //
  // This exists because #597's whole subject is encoder restarts, and the one
  // combination no test covered was a real sender looking at a frame the
  // compositor's Lever A had shed.
  virtual bool wouldRestartForEncodePathForTest(const ProgramFrame& /*frame*/) { return false; }

  // TEST-ONLY (same structural guard). #597 Lever B fix round 1 (review
  // finding 2): this is the ONLY seam the discard needed after the decision
  // logic moved out to the pure core::discardableGopTailLength() (see
  // StreamBackpressurePolicy.h) - boundary-condition coverage lives there,
  // with NO sender and NO seam at all. What is left to prove is that
  // observeStreamBackpressure()/sync() actually REACH the discard on a real
  // sender, which needs some way to put a real chunk in the real queue.
  // Pushes a chunk directly onto the bitstream queue, bypassing the real GPU
  // encoder, so that one call-site test can run without a hardware encoder
  // and a real congested network.
  virtual void enqueueBitstreamChunkForTest(std::size_t /*bytes*/, bool /*keyframe*/) {}
  // TEST-ONLY (same structural guard). #597 Task 8b. The seam above pushes
  // STRAIGHT onto the queue so a discard test can build a backlog; this one
  // offers a chunk through the REAL enqueueBitstream(), which is where the
  // 60-chunk / 2 MiB bound - and, since Task 8b, the last-resort GOP-tail
  // discard that replaced failing the sender - actually lives. Nothing else
  // can drive that path without a hardware encoder and a congested network.
  virtual void offerBitstreamChunkForTest(std::size_t /*bytes*/, bool /*keyframe*/) {}
  // TEST-ONLY (same structural guard). One read of the queue's true state -
  // depth, whether a keyframe is queued, and the real bitstreamBufferedMs()
  // measurement - unaffected by setBackpressureObservationForTest()'s
  // override, so a call-site test can confirm the queue actually shrank.
  virtual BitstreamQueueSnapshotForTest bitstreamQueueSnapshotForTest() const {
    return BitstreamQueueSnapshotForTest{};
  }
};

// Session control for a capture adapter (#535 lifecycle slice). ISource stays
// delivery-only. Opening, selecting, offsetting, and retiring a device session
// is this contract. Frame polling is not: that remains on ICaptureDevice until
// the bus is the only consumer.
class ICaptureDeviceLifecycle {
 public:
  virtual ~ICaptureDeviceLifecycle() = default;
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
  // Stop the device session and release its resources. Default no-op for
  // adapters without live sessions.
  virtual std::vector<CaptureDeviceInfo> disconnect(const std::string&) { return enumerate(); }
  virtual std::vector<CaptureDeviceInfo> configureSrtIngestSources(const std::vector<SrtIngestSourceConfig>&) { return enumerate(); }
  // Shell-announced shared-memory session. Default no-op: only the WinUI bridge
  // maps a buffer. Callers must not cast to that adapter.
  virtual void registerCaptureBuffer(const std::string&, const std::string&, int, int) {}
  virtual void unregisterCaptureBuffer(const std::string&) {}
};

class ICaptureVideoConsumer {
 public:
  virtual ~ICaptureVideoConsumer() = default;
  virtual void publish(VideoFrame frame) = 0;
  virtual void end(const std::string& participantId) = 0;
};

class ICaptureAudioConsumer {
 public:
  virtual ~ICaptureAudioConsumer() = default;
  virtual void publish(AudioFrame frame) = 0;
};

class ICaptureDevice : public ICaptureDeviceLifecycle {
 public:
  ~ICaptureDevice() override = default;
  // Publish what adapters have already pushed. The render tick does not pull
  // a frame vector. Video slots are re-published with this tick's timestamp so
  // a held picture stays on air and a frozen frameId can still age out.
  // Slots marked ended are removed. Audio packets are drained in order.
  void deliverVideo(ICaptureVideoConsumer& consumer, int64_t timestampMs) {
    captureVideoTick(timestampMs);
    std::vector<VideoFrame> frames;
    std::vector<std::string> ended;
    {
      std::lock_guard<std::mutex> lock(captureMailboxMutex_);
      for (auto it = videoSlots_.begin(); it != videoSlots_.end();) {
        if (it->second.ended) {
          ended.push_back(it->first);
          it = videoSlots_.erase(it);
        } else {
          frames.push_back(it->second.frame);
          ++it;
        }
      }
    }
    for (auto& frame : frames) {
      frame.timestampMs = timestampMs;
      consumer.publish(std::move(frame));
    }
    for (const auto& id : ended) consumer.end(id);
  }
  void deliverAudio(ICaptureAudioConsumer& consumer, int64_t timestampMs) {
    captureAudioTick(timestampMs);
    std::vector<AudioFrame> frames;
    {
      std::lock_guard<std::mutex> lock(captureMailboxMutex_);
      frames.swap(audioQueue_);
    }
    for (auto& frame : frames) consumer.publish(std::move(frame));
  }

 protected:
  // Adapters that learn about a frame only by looking (shared memory, a test
  // double) push during this hook. Adapters with their own arrival thread push
  // from that thread and leave the hook empty.
  virtual void captureVideoTick(int64_t) {}
  virtual void captureAudioTick(int64_t) {}

  void postVideo(VideoFrame frame) {
    const auto id = frame.participantId;
    std::lock_guard<std::mutex> lock(captureMailboxMutex_);
    videoSlots_[id] = VideoSlot{std::move(frame), false};
  }
  void postVideoEnd(const std::string& id) {
    std::lock_guard<std::mutex> lock(captureMailboxMutex_);
    auto& slot = videoSlots_[id];
    slot.ended = true;
    if (slot.frame.participantId.empty()) slot.frame.participantId = id;
  }
  // Replace the whole video set. Ids that disappeared are ended on the next deliver.
  void replaceVideo(std::vector<VideoFrame> frames) {
    std::unordered_set<std::string> live;
    std::lock_guard<std::mutex> lock(captureMailboxMutex_);
    for (auto& frame : frames) {
      auto id = frame.participantId;
      live.insert(id);
      videoSlots_[std::move(id)] = VideoSlot{std::move(frame), false};
    }
    for (auto& [id, slot] : videoSlots_) {
      if (!live.count(id)) slot.ended = true;
    }
  }
  void postAudio(AudioFrame frame) {
    std::lock_guard<std::mutex> lock(captureMailboxMutex_);
    audioQueue_.push_back(std::move(frame));
  }

 public:
  // Configured transport identities, including temporary PCM gaps. Must be a
  // cheap state snapshot, not device enumeration or network I/O.
  virtual std::vector<std::string> audioSourceIds() const { return {}; }

 private:
  struct VideoSlot {
    VideoFrame frame;
    bool ended = false;
  };
  std::mutex captureMailboxMutex_;
  std::map<std::string, VideoSlot> videoSlots_;
  std::vector<AudioFrame> audioQueue_;
};

struct ModuleSet {
  std::unique_ptr<IZoomCaptureSource> zoom;
  std::unique_ptr<ICompositor> compositor;
  // #535 slice 3b: the module set carries a DECODER FACTORY, not one
  // owner object. core::MediaTransports owns one decoder per media source
  // and calls this to open each. Empty on a platform with no media
  // decoder (and in the stub build), which is what MediaCore tests for.
  std::function<std::unique_ptr<IMediaDecoder>()> mediaDecoderFactory;
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
// One MF decoder per call; empty on a build with no Media Foundation.
std::function<std::unique_ptr<IMediaDecoder>()> createMediaFoundationMediaDecoderFactory();
std::unique_ptr<IAudioMonitorOutput> createStubAudioMonitorOutput();
// macOS CoreAudio twins; nullptr unless COREVIDEO_WITH_COREAUDIO.
std::unique_ptr<IAudioMonitorOutput> createCoreAudioMonitorOutput();
std::unique_ptr<IAudioCaptureSource> createCoreAudioCaptureSource();
// macOS capture twins (AVFoundation cameras / ScreenCaptureKit screens+windows);
// nullptr unless COREVIDEO_WITH_AVF_CAPTURE / COREVIDEO_WITH_SCK.
std::unique_ptr<ICaptureDevice> createAvfCaptureDevice();
std::unique_ptr<ICaptureDevice> createSckScreenCaptureDevice();
std::unique_ptr<IAudioMonitorOutput> createWasapiMonitorOutput();
std::unique_ptr<IAudioCaptureSource> createStubAudioCaptureSource();
std::unique_ptr<IAudioCaptureSource> createWasapiAudioCaptureSource();
std::unique_ptr<IEncoderSink> createStubRecordingEncoderSink();
// The macOS AVFoundation/VideoToolbox twin; nullptr unless COREVIDEO_WITH_AVF_ENCODER.
std::unique_ptr<IEncoderSink> createAVFoundationEncoderSink();
std::unique_ptr<IEncoderSink> createMediaFoundationEncoderSink();
std::unique_ptr<IOutputSender> createRtmpOutputSender();
std::unique_ptr<IOutputSender> createSrtOutputSender();
// SRT delivery over the shared FFmpeg sender (same pipeline as RTMP, MPEG-TS
// container, srt:// endpoint). Defined in RtmpOutputSenderAdapter.cpp.
std::unique_ptr<IOutputSender> createFfmpegSrtOutputSender();
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
