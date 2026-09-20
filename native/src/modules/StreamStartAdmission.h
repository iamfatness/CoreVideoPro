#pragma once

// STREAM START ADMISSION (2026-09-20): refuse, never downgrade.
//
// On 2026-09-20 an H.265 request was silently turned into H.264 on the raw-pipe
// fallback, which delivers ~0.87x of real time at 1080p60; YouTube reported it
// was not receiving enough data and the operator had no idea why. Rule: a
// stream starts with the codec the operator chose, on a path that can carry it,
// or it does not start — and the refusal names why, in a stable code the shell
// renders. Pure and header-only (the CaptureReaderStallPolicy shape) so the
// decision is unit-tested without FFmpeg, a GPU or a destination.
//
// Order of checks (the most operator-actionable first):
//   1. compatibility refusal   -> "enhanced-rtmp-required" (turn the checkbox on)
//   2. no hardware encoder     -> "no-hardware-encoder"    (this machine cannot)
//      (HEVC/AV1 on ANY CPU-fallback reason land here too: the raw path cannot
//       hold 1080p60 until sub-project 2, and it has no HEVC/AV1 encoder we ship)
//   3. GPU encoder start failed-> "gpu-encoder-start-failed" (HRESULT quoted)
// H.264 on the CPU fallback is ADMITTED: every machine that streams today keeps
// streaming exactly as it does.

#include <string>

namespace corevideo::modules {

struct StreamStartAdmissionInputs {
  std::string requestedCodec;          // RtmpCompatibility spelling: "h264" | "h265" | "av1"
  bool compatibilityRefused = false;   // RtmpCompatibilityResult::refused
  std::string compatibilityReason;     // RtmpCompatibilityResult::reason
  bool codecHasHardwareEncoder = true; // codecHasSupportedHardwareEncoder AND the probe (when not pending)
  bool gpuPathChosen = true;           // chooseStreamEncodePath(...) == GpuDirect
  const char* gpuPathReason = "gpu-direct";
  bool gpuEncoderStartFailed = false;  // GpuVideoEncoder::start() returned false
  std::string gpuEncoderFailureDetail;
};

struct StreamStartAdmission {
  bool refused = false;
  std::string resultCode;  // one of the three codes above, or empty when admitted
  std::string message;     // operator sentence, empty when admitted
};

inline const char* operatorCodecLabel(const std::string& codec) {
  if (codec == "h265" || codec == "hevc") return "H.265";
  if (codec == "av1") return "AV1";
  return "H.264";
}

[[nodiscard]] inline StreamStartAdmission admitStreamStart(const StreamStartAdmissionInputs& in) {
  StreamStartAdmission out;
  const std::string label = operatorCodecLabel(in.requestedCodec);
  const bool isH264 = in.requestedCodec == "h264";
  if (in.compatibilityRefused) {
    out.refused = true;
    out.resultCode = in.compatibilityReason.empty() ? "enhanced-rtmp-required" : in.compatibilityReason;
    out.message = label + " over RTMP needs Enhanced RTMP; enable it in Stream settings or choose H.264.";
    return out;
  }
  if (!in.codecHasHardwareEncoder || (!isH264 && !in.gpuPathChosen)) {
    out.refused = true;
    out.resultCode = "no-hardware-encoder";
    out.message = label + " needs a hardware encoder on the GPU-direct path and this machine cannot provide one (" +
                  std::string(in.gpuPathReason ? in.gpuPathReason : "unknown") +
                  "). Choose H.264 or stream from a machine with an NVIDIA " +
                  (in.requestedCodec == "av1" ? "RTX 40-series or newer" : "GPU") + ".";
    return out;
  }
  if (in.gpuEncoderStartFailed) {
    out.refused = true;
    out.resultCode = "gpu-encoder-start-failed";
    out.message = "The " + label + " hardware encoder failed to start (" + in.gpuEncoderFailureDetail +
                  "). The stream was not started.";
    return out;
  }
  return out;
}

}  // namespace corevideo::modules
