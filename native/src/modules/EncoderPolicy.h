#pragma once

#include <string>
#include <vector>

namespace corevideo::modules {

// WHICH ENCODERS THIS PRODUCT SHIPS (owner decision 2026-08-06).
//
// Supported hardware tiers: NVIDIA NVENC on Windows, Apple VideoToolbox on
// macOS. Intel Quick Sync and AMD AMF are not supported tiers — not optimised,
// not tested. HEVC/H.265 ENCODE is not shipped at all.
//
// The reasoning is licensing as much as engineering, and it is worth keeping
// written down because each exclusion looks like an oversight otherwise:
//
//   * This FFmpeg is LGPL (no --enable-gpl). libx264/libx265 would force the
//     whole binary to GPL, which a paid product cannot casually take on.
//   * HEVC carries the worst patent position of the mainstream codecs — several
//     competing pools, one of which asserts CONTENT-DISTRIBUTION royalties, not
//     just encoder royalties. Dropping HEVC encode removes that exposure, and it
//     buys little here: RTMP endpoints do not accept it without Enhanced RTMP,
//     NDI uses its own codec, the virtual camera is uncompressed, and long-GOP
//     HEVC is harder to edit than H.264 for ISO stems.
//   * libopenh264's royalty shield covers only CISCO'S OWN prebuilt binary. A
//     self-compiled one carries the patent cost without the benefit.
//   * Real-time 1080p60 software AV1 is not viable, so libsvtav1 as a "fallback"
//     would silently hand the CPU the work the GPU was chosen for.
//   * Hardware/OS encoders carry the vendor's patent licensing, which is the
//     practical mitigation for a paid product distributing an encoder.
//
// H.264 stays the delivery default because it is what endpoints actually accept.
// AV1 is the forward path: royalty-free by design, and its hardware support only
// grows.

// Every encoder this product is willing to run. Anything absent is absent on
// purpose (see above) — do not add to this list without revisiting the licence
// and patent position.
inline bool isSupportedEncoder(const std::string& encoder) {
  return encoder == "h264_nvenc" || encoder == "av1_nvenc" || encoder == "h264_videotoolbox" ||
         encoder == "h264_mf";
}

// Is there hardware for this codec on this platform?
//   AV1 encode needs NVENC on Ada (RTX 40-series) or newer, and does NOT exist on
//   Apple Silicon at all — the M3/M4 media engine decodes AV1 but cannot encode
//   it. Answering honestly is what lets the sender say "you asked for AV1 and are
//   getting H.264" instead of silently shipping the wrong codec.
inline bool codecHasSupportedHardwareEncoder(const std::string& normalizedCodec) {
  if (normalizedCodec == "h265") {
    return false;  // HEVC encode is not shipped on any platform
  }
#if defined(__APPLE__)
  return normalizedCodec == "h264";
#else
  return normalizedCodec == "h264" || normalizedCodec == "av1";
#endif
}

// The encoder to prefer, before availability probing.
inline std::string preferredEncoderFor(const std::string& normalizedCodec,
                                       const std::string& normalizedMode) {
  if (normalizedMode == "nvenc") {
    return normalizedCodec == "av1" ? "av1_nvenc" : "h264_nvenc";
  }
  if (normalizedMode == "videotoolbox") {
    return "h264_videotoolbox";  // no AV1 encoder exists on Apple Silicon
  }
#if defined(__APPLE__)
  return "h264_videotoolbox";
#else
  return normalizedCodec == "av1" ? "av1_nvenc" : "h264_nvenc";
#endif
}

// Ordered candidates to probe for availability.
//
// h264_mf (Windows Media Foundation) is the ONLY fallback, and it is an OS API
// rather than a supported GPU tier: without it a machine with no NVIDIA card
// cannot record or stream AT ALL, which for a paid product is a refund on first
// launch rather than a degraded experience. There is deliberately NO software
// codec fallback, and none for AV1 — an Ada-class GPU or nothing.
inline std::vector<std::string> encoderCandidatesFor(const std::string& normalizedCodec,
                                                     const std::string& normalizedMode) {
  // An explicit hardware choice is operator intent: return it alone so FFmpeg
  // reports the precise device/driver failure instead of quietly substituting.
  if (normalizedMode == "nvenc" || normalizedMode == "videotoolbox") {
    return {preferredEncoderFor(normalizedCodec, normalizedMode)};
  }
#if defined(__APPLE__)
  return {"h264_videotoolbox"};
#else
  if (normalizedCodec == "av1") {
    return {"av1_nvenc"};
  }
  return {"h264_nvenc", "h264_mf"};
#endif
}

}  // namespace corevideo::modules
