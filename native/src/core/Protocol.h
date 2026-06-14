#pragma once

#include <array>
#include <string_view>

namespace corevideo::core {

inline constexpr std::array<std::string_view, 14> kNativeMediaCoreCapabilities = {
    "zoom-raw-video",
    "zoom-raw-audio",
    "gpu-compositor",
    "scene-graph-rendering",
    "dynamic-overlays",
    "chroma-key",
    "smart-framing",
    "audio-mixer",
    "program-recording",
    "iso-recording",
    "rtmp-output",
    "ndi-output",
    "srt-output",
    "webrtc-output",
};

inline constexpr std::array<std::string_view, 11> kRequiredMvpCapabilities = {
    "zoom-raw-video",
    "zoom-raw-audio",
    "gpu-compositor",
    "scene-graph-rendering",
    "dynamic-overlays",
    "chroma-key",
    "smart-framing",
    "audio-mixer",
    "program-recording",
    "iso-recording",
    "rtmp-output",
};

inline constexpr std::array<std::string_view, 4> kNativeMediaCoreCommandTypes = {
    "load-scene-graph",
    "set-participant-transform",
    "set-overlay-asset",
    "start-program-output",
};

inline constexpr std::array<std::string_view, 10> kNativeBridgeCommandTypes = {
    "join",
    "leave",
    "snapshot",
    "set-output-profile",
    "start-recording",
    "stop-recording",
    "start-stream",
    "stop-stream",
    "get-output-health",
    "get-output-session",
};

}  // namespace corevideo::core
