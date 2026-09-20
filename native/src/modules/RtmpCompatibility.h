#pragma once

// RTMP codec/container compatibility matrix.
//
// Pure, deterministic resolution of a requested video codec to an
// RTMP-compatible configuration, independent of FFmpeg or any dev gate so it is
// unit-testable in the default stub build. Standard RTMP/FLV guarantees only
// H.264 video + AAC audio; H.265 and AV1 require enhanced-RTMP (E-RTMP). The
// matrix either carries H.265/AV1 over E-RTMP (with an advisory warning) when
// the caller opts in, or REFUSES the start with a named reason (2026-09-20: the
// silent downgrade delivered the wrong codec on the slow raw path; a stream
// starts with the codec the operator chose or it does not start).

#include <algorithm>
#include <cctype>
#include <string>

namespace corevideo::modules {

struct RtmpCompatibilityResult {
  std::string requestedVideoCodec;  // normalized request (h264/h265/av1)
  std::string videoCodec;           // codec actually sent (may be downgraded)
  std::string audioCodec = "aac";   // RTMP audio is AAC
  std::string container = "flv";    // RTMP container is FLV
  bool enhancedRtmp = false;        // true when h265/av1 is carried via E-RTMP
  bool fallbackApplied = false;     // true when the request was downgraded
  std::string warning;              // operator-facing note; empty when standard
  // 2026-09-20: a request the destination cannot carry is REFUSED at stream
  // start, never downgraded. `reason` is a stable code the sender publishes as
  // lastResultCode ("enhanced-rtmp-required"); `warning` is the operator sentence.
  bool refused = false;
  std::string reason;
};

// Normalize a free-form codec string to one of h264/h265/av1 (default h264).
inline std::string normalizeRtmpVideoCodec(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  if (value == "hevc") {
    return "h265";
  }
  if (value == "h264" || value == "h265" || value == "av1") {
    return value;
  }
  return "h264";
}

inline RtmpCompatibilityResult resolveRtmpCompatibility(const std::string& requestedVideoCodec, bool allowEnhancedRtmp) {
  RtmpCompatibilityResult result;
  const std::string requested = normalizeRtmpVideoCodec(requestedVideoCodec);
  result.requestedVideoCodec = requested;
  result.videoCodec = requested;

  if (requested == "h264") {
    return result;  // guaranteed-compatible baseline: H.264 + AAC over FLV
  }

  const std::string label = requested == "h265" ? "H.265" : "AV1";
  if (allowEnhancedRtmp) {
    result.enhancedRtmp = true;
    result.warning = label + " over RTMP uses enhanced-RTMP (E-RTMP); the ingest must support it.";
  } else {
    result.refused = true;
    result.reason = "enhanced-rtmp-required";
    result.warning = label + " over RTMP needs Enhanced RTMP; enable it in Stream settings or choose H.264.";
  }
  return result;
}

}  // namespace corevideo::modules
