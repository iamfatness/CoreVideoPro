#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

// GPU-DIRECT HARDWARE ENCODE SEAM (#521 slice 1).
//
// The compositor already produces the program as a GPU keyed-mutex texture
// (ProgramFrame::sharedTexture on Windows, an IOSurface on macOS). The streaming
// path historically ignored it: it read the frame back to the CPU and piped
// ~186 MB/s of raw NV12 to an external ffmpeg, which capped 1080p60 at ~0.76x of
// realtime with the hardware encoder sitting IDLE. This seam feeds the hardware
// encoder from the GPU texture directly (vMix/Vectar parity), emitting an H.264,
// HEVC or AV1 elementary-stream bitstream (codec on the config); ffmpeg is
// demoted to a pure muxer/transport.
//
// The interface deliberately carries NO D3D11 / Media Foundation / Metal types,
// so the Windows implementation (MediaFoundationGpuVideoEncoder) and the macOS
// one (VideoToolbox, slice 4) both sit behind it without their consumers — the
// output senders — ever seeing a platform type.

namespace corevideo::modules {

// Encoder settings, resolved by the caller from the output settings.
struct GpuVideoEncoderConfig {
  int width = 0;
  int height = 0;
  int fps = 60;
  int bitrateKbps = 6000;
  double keyframeIntervalSeconds = 2.0;
  std::string rateControl = "cbr";   // "cbr" | "vbr"
  std::string h264Profile = "high";  // "high" | "main" | "baseline" | "auto"
  // 2026-09-20: the codec the hardware MFT is bound for. Normalized: "h264",
  // "hevc" or "av1" ("h265" is the operator-settings spelling; the sender maps
  // it). The Windows implementation enumerates the MFT for this subtype and, for
  // HEVC, disables B-frames — the FLV muxer refuses reordered raw HEVC.
  std::string codec = "h264";
};

// The GPU frame to encode — an OPAQUE platform handle, never a D3D11/Metal type.
// Mirrors ProgramFrameSharedTexture: exactly one of the two identifiers is set.
struct GpuVideoEncoderFrame {
  std::string sharedHandleHex;  // Windows: keyed-mutex DXGI shared HANDLE (hex).
  uint32_t iosurfaceId = 0;     // macOS: IOSurface global id. 0 on Windows.
  int width = 0;
  int height = 0;
  int64_t frameNumber = 0;
};

// One encoded output unit handed to the sink. `data` is owned by the encoder and
// valid only for the duration of the callback — the sink must copy what it keeps.
struct GpuEncodedChunk {
  const uint8_t* data = nullptr;
  size_t size = 0;
  bool keyframe = false;
  int64_t frameNumber = 0;
};

using GpuEncodedChunkSink = std::function<void(const GpuEncodedChunk&)>;

// A hardware video encoder fed a GPU texture. Implementations run their own
// device/thread and must never touch coreMutex or the render thread.
class GpuVideoEncoder {
 public:
  virtual ~GpuVideoEncoder() = default;

  // Initialise the hardware encoder for `config` and route encoded output to
  // `sink`. Returns false if no hardware encoder is available or init fails —
  // the caller then uses the CPU-pipe fallback. Never throws.
  [[nodiscard]] virtual bool start(const GpuVideoEncoderConfig& config, GpuEncodedChunkSink sink) = 0;

  // Submit one GPU frame for encoding. Returns false when the encoder is
  // unhealthy (e.g. device loss) so the caller can let its supervisor restart.
  [[nodiscard]] virtual bool submit(const GpuVideoEncoderFrame& frame) = 0;

  // Stop encoding, join the encoder thread, release all GPU/encoder resources.
  virtual void stop() = 0;

  // False once the encoder has hit an unrecoverable fault (device loss, sustained
  // encode failure). The sender's OutputDestinationSupervisor watches this.
  [[nodiscard]] virtual bool healthy() const = 0;

  // The implementation's own last failure detail (e.g. "set-bframes-off",
  // "no-codec-api"), for the operator sentence when start() refuses a stream.
  // Default-implemented so no other implementation has to change; empty means
  // "no detail recorded".
  [[nodiscard]] virtual std::string lastFailure() const { return {}; }
};

// ---------------------------------------------------------------------------
// Pure path-selection policy: GPU-direct vs the CPU-pipe fallback. Kept pure
// and header-only (the CaptureReaderStallPolicy / DeviceLossPolicy shape) so it
// is unit-tested without a GPU. The decision is made ONCE at stream start.
// ---------------------------------------------------------------------------

struct GpuEncodePathInputs {
  bool hardwareEncoderAvailable = false;  // a hardware encoder MFT exists for the profile
  bool sessionAvailable = false;          // EncoderCapacityProbe has a free session
  bool forcedOffByEnv = false;            // COREVIDEO_GPU_ENCODE=0
  bool platformSupported = false;         // a GpuVideoEncoder impl exists on this platform
};

enum class GpuEncodePath {
  GpuDirect,
  CpuFallback,
};

struct GpuEncodePathPolicy {
  [[nodiscard]] static GpuEncodePath choose(const GpuEncodePathInputs& in) {
    if (in.platformSupported && !in.forcedOffByEnv && in.hardwareEncoderAvailable &&
        in.sessionAvailable) {
      return GpuEncodePath::GpuDirect;
    }
    return GpuEncodePath::CpuFallback;
  }

  // Names the FIRST blocker (platform -> env -> hardware -> session), so a log
  // line / support bundle says exactly why a machine is on the CPU fallback.
  [[nodiscard]] static const char* reason(const GpuEncodePathInputs& in) {
    if (!in.platformSupported) return "platform-unsupported";
    if (in.forcedOffByEnv) return "forced-off-by-env";
    if (!in.hardwareEncoderAvailable) return "no-hardware-encoder";
    if (!in.sessionAvailable) return "no-free-encoder-session";
    return "gpu-direct";
  }
};

// The stream sender's start-time decision. It folds two sender-specific gates
// into the base policy, applied ONLY when the base policy already allows
// GPU-direct so the most specific base blocker (no hardware, no session, env
// off) keeps precedence in the reason. `codec` is the codec ACTUALLY SENT
// (resolved through RtmpCompatibility) and `codecHasGpuEncoder` is what the
// capacity probe says about that codec on this machine — since 2026-09-20 every
// shipped codec (h264/hevc/av1) can take this path, so a codec with no hardware
// encoder reports the same "no-hardware-encoder" as no hardware at all (the old
// "codec-not-h264" is retired). The compositor must also be exporting the
// dedicated encoder texture on the frame that starts the process (otherwise the
// encoder has nothing to open and the bitstream-mode muxer would stall).
[[nodiscard]] inline GpuEncodePath chooseStreamEncodePath(const GpuEncodePathInputs& base,
                                                          std::string_view codec,
                                                          bool codecHasGpuEncoder,
                                                          bool frameHasEncoderTexture,
                                                          const char** reason) {
  (void)codec;  // carried for logging/diagnostics by callers; the decision is the two bools
  if (GpuEncodePathPolicy::choose(base) == GpuEncodePath::CpuFallback) {
    if (reason) *reason = GpuEncodePathPolicy::reason(base);
    return GpuEncodePath::CpuFallback;
  }
  if (!codecHasGpuEncoder) {
    if (reason) *reason = "no-hardware-encoder";
    return GpuEncodePath::CpuFallback;
  }
  if (!frameHasEncoderTexture) {
    if (reason) *reason = "no-encoder-texture";
    return GpuEncodePath::CpuFallback;
  }
  if (reason) *reason = "gpu-direct";
  return GpuEncodePath::GpuDirect;
}

}  // namespace corevideo::modules
