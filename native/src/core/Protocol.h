#pragma once

#include <array>
#include <string_view>

namespace corevideo::core {

// Exactly the capability names MediaCore::profile() reports in capabilityStates.
// ContractParity compares this with a constructed core in both directions, so a
// name the core never reports cannot sit here looking supported.
inline constexpr std::array<std::string_view, 22> kNativeMediaCoreCapabilities = {
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
    "hls-output",
    "srt-ingest",
    "rtmp-ingest",
    "ndi-ingest",
    "decklink-capture",
    "aja-capture",
    "uvc-capture",
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

// Contract view of the live MediaCore::applyCommandMutation branches. The
// dispatcher itself decides admission; ContractParity checks this view and the
// production builder against every branch in both directions.
inline constexpr std::array<std::string_view, 48> kNativeMediaCoreCommandTypes = {
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
    "set-audio-monitor-control",
    "scan-vst-plugins",
    "open-vst-editor",
    "set-vst-param",
    "set-vst-state",
    "sync-audio-routing-matrix",
    "set-audio-route-control",
    "sync-capture-audio-sources",
    "push-caption-cue",
    "set-caption-enabled",
    "set-brand-kit",
    "set-media-playback",
    "set-media-transport",
    "set-multiview-layout",
    "configure-multiviewer",
    "configure-srt-ingest-sources",
    "configure-rtmp-ingest-sources",
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

// Every request type JsonRpcServer dispatches on. ContractParity compares this
// with the dispatcher in both directions and checks that each request the C#
// and Swift shells send is in it.
inline constexpr std::array<std::string_view, 33> kCoreRequestTypes = {
    "handshake",
    "ping",
    "snapshot",
    "media-core-sync",
    "native-media-core-sync",
    "load-scene-graph",
    "set-participant-transform",
    "set-overlay-asset",
    "start-program-output",
    "zoom-join",
    "zoom-leave",
    "zoom-cancel",
    "zoom-stop-capture",
    "zoom-snapshot",
    "zoom-media-spine-sync",
    "set-zoom-guest-av-sync-offset",
    "get-output-health",
    "get-output-session",
    "list-capture-devices",
    "select-capture-input",
    "set-capture-audio-sync-offset",
    "connect-capture-device",
    "disconnect-capture-device",
    "register-capture-shm",
    "unregister-capture-shm",
    "browser-add",
    "browser-remove",
    "browser-reload",
    "scan-vst-plugins",
    "open-vst-editor",
    "get-vst-state",
    "set-vst-param",
    "set-vst-state",
};

// Unsolicited events the core writes to the shell; each must have a parser in
// the C# shell (ContractParity).
inline constexpr std::array<std::string_view, 4> kCoreEventTypes = {
    "zoom-video-frame",
    "zoom-source-format",
    "program-frame-preview",
    "program-shared-texture",
};

}  // namespace corevideo::core
