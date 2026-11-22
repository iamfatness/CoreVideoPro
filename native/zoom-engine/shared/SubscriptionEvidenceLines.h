#pragma once
// The five Zoom subscription-evidence lines the macOS engine writes (Sources Phase 2 plan,
// Task 8; comms spec C2). Moved out of main-macos.mm verbatim so a native test can feed each
// one through the core's parser (parseZoomEngineEvent) and ZoomSubscriptionSdkEvidence.
// The Windows engine writes the same stages and field names inline (engine-video.cpp:69-115).
//
// Header-only and SDK-free: callers cast their SDK error enum to int. On macOS that number is
// a ZoomSDKError, on Windows an SDKError; the snapshot's evidence node says which ("platform").
#include <cstdint>
#include <string>

namespace corevideo::engine::evidence {

namespace detail {
inline std::string head(const char* stage, const std::string& sourceUuid, std::uint32_t participantId) {
  return std::string(R"({"cmd":"debug","stage":")") + stage + R"(","source_uuid":")" + sourceUuid +
         R"(","participant_id":)" + std::to_string(participantId);
}
}  // namespace detail

inline std::string createRendererFailed(const std::string& sourceUuid, std::uint32_t participantId,
                                        int code, int resolution) {
  return detail::head("create_renderer_failed", sourceUuid, participantId) + R"(,"code":)" +
         std::to_string(code) + R"(,"resolution":)" + std::to_string(resolution) + "}";
}

inline std::string setResolution(const std::string& sourceUuid, std::uint32_t participantId,
                                 int code, int resolution) {
  return detail::head("set_resolution", sourceUuid, participantId) + R"(,"code":)" +
         std::to_string(code) + R"(,"resolution":)" + std::to_string(resolution) + "}";
}

inline std::string videoSubscribe(const std::string& sourceUuid, std::uint32_t participantId,
                                  int code, int resolution) {
  return detail::head("video_subscribe", sourceUuid, participantId) + R"(,"code":)" +
         std::to_string(code) + R"(,"resolution":)" + std::to_string(resolution) + "}";
}

inline std::string resolutionDowngraded(const std::string& sourceUuid, std::uint32_t participantId,
                                        int requested, int actual) {
  return detail::head("video_resolution_downgraded", sourceUuid, participantId) + R"(,"requested":)" +
         std::to_string(requested) + R"(,"actual":)" + std::to_string(actual) + "}";
}

inline std::string subscribeFailedAll(const std::string& sourceUuid, std::uint32_t participantId,
                                      int requested) {
  return detail::head("video_subscribe_failed_all", sourceUuid, participantId) + R"(,"requested":)" +
         std::to_string(requested) + "}";
}

}  // namespace corevideo::engine::evidence
