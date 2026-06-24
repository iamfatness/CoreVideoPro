#pragma once

#include <array>
#include <string_view>

namespace corevideo::core {

inline constexpr std::array<std::string_view, 16> kNativeMediaCoreCapabilities = {
    "zoom-raw-video",
    "zoom-raw-audio",
    "gpu-compositor",
    "scene-graph-rendering",
    "dynamic-overlays",
    "chroma-key",
    "smart-framing",
    "audio-mixer",
    "local-audio-capture",
    "audio-monitor-output",
    "program-recording",
    "iso-recording",
    "rtmp-output",
    "ndi-output",
    "srt-output",
    "webrtc-output",
};

inline constexpr std::array<std::string_view, 13> kRequiredMvpCapabilities = {
    "zoom-raw-video",
    "zoom-raw-audio",
    "gpu-compositor",
    "scene-graph-rendering",
    "dynamic-overlays",
    "chroma-key",
    "smart-framing",
    "audio-mixer",
    "local-audio-capture",
    "audio-monitor-output",
    "program-recording",
    "iso-recording",
    "rtmp-output",
};

inline constexpr std::array<std::string_view, 22> kNativeMediaCoreCommandTypes = {
    "load-scene-graph",
    "set-participant-transform",
    "set-overlay-asset",
    "start-program-output",
    "prepare-encoder-session",
    "start-encoder-session",
    "stop-encoder-session",
    "fail-output-sender",
    "recover-output-sender",
    "set-recording-targets",
    "start-recording-session",
    "stop-recording-session",
    "fail-recording-session",
    "recover-recording-session",
    "sync-participant-audio-mix",
    "sync-audio-routing-matrix",
    "sync-capture-audio-sources",
    "push-caption-cue",
    "set-caption-enabled",
    "set-brand-kit",
    "set-media-playback",
    "recommend-auto-production",
};

inline constexpr std::array<std::string_view, 14> kNativeBridgeCommandTypes = {
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
    "list-capture-devices",
    "select-capture-input",
    "set-capture-audio-sync-offset",
    "connect-capture-device",
};

inline constexpr std::array<std::string_view, 4> kCoreRequestTypes = {
    "zoom-join",
    "zoom-leave",
    "zoom-snapshot",
    "zoom-media-spine-sync",
};

inline constexpr std::array<std::string_view, 2> kZoomMediaSpineSyncTypeNames = {
    "ZoomMediaSpineSyncPayload",
    "ZoomMediaSpineNativeSnapshot",
};

inline constexpr std::array<std::string_view, 3> kCoreEventTypes = {
    "zoom-video-frame",
    "program-frame-preview",
    "program-shared-texture",
};

}  // namespace corevideo::core
