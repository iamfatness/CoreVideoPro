#pragma once

#include <array>
#include <string_view>

namespace corevideo::core {

inline constexpr std::array<std::string_view, 21> kNativeMediaCoreCapabilities = {
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
    "tiles-layer",
    "rtmp-output",
    "ndi-output",
    "srt-output",
    "srt-ingest",
    "decklink-capture",
    "aja-capture",
    "uvc-capture",
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

// The live dispatcher in MediaCore::applyCommandMutation handles exactly these
// names. A production builder that emits anything else is a protocol failure.
inline constexpr std::array<std::string_view, 45> kNativeMediaCoreCommandTypes = {
    "begin-take-transition",
    "load-scene-graph",
    "set-preview-scene",
    "set-participant-transform",
    "set-overlay-asset",
    "set-color-grade",
    "set-source-policy",
    "set-output-profile",
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
    "sync-virtual-camera",
    "sync-audio-monitor",
    "scan-vst-plugins",
    "open-vst-editor",
    "set-vst-param",
    "set-vst-state",
    "sync-audio-routing-matrix",
    "sync-capture-audio-sources",
    "push-caption-cue",
    "set-caption-enabled",
    "set-brand-kit",
    "set-media-playback",
    "set-media-transport",
    "set-multiview-layout",
    "configure-multiviewer",
    "configure-srt-ingest-sources",
    "browser-add",
    "browser-remove",
    "browser-reload",
    "simulate-breakout-room-change",
    "recommend-auto-production",
    "set-verbose-diagnostics",
    "set-zoom-source-roster",
    "set-active-speaker",
    "set-screen-share-source",
};

inline bool isNativeMediaCoreCommand(std::string_view type) {
  for (const auto name : kNativeMediaCoreCommandTypes) {
    if (name == type) return true;
  }
  return false;
}

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

inline constexpr std::array<std::string_view, 5> kCoreRequestTypes = {
    "zoom-join",
    "zoom-leave",
    "zoom-stop-capture",
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
