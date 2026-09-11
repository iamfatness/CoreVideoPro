#pragma once

#include <string_view>

namespace corevideo::modules {

// WHAT RESOLUTION DOES A ZOOM VIDEO SUBSCRIPTION ASK FOR? (#478, 2026-09-11)
//
// Resolution is part of `ZoomEngineRuntime::syncSpine`'s subscription key, and the
// engine REBUILDS a live renderer (destroy, then create at the new resolution) to
// raise it — engine-video.cpp `video_upgrade_subscription`. So anything that moves
// the requested resolution under an unchanged source is a real frame gap on air.
//
// It used to be `purpose == "active-speaker" ? 1080P : 720P`, so every change of
// speaker re-subscribed two guests. On a 12-person show that fired several times a
// minute and read as Tiles/multiview "flashing" (#478 live evidence: totalChurn
// 53 -> 59 in 20 s, lastResolutionChanges non-zero).
//
// WHY NOT CHANGE THE RESOLUTION IN PLACE? The SDK declares
// `IZoomSDKRenderer::setRawDataResolution` (h/rawdata/rawdata_renderer_interface.h:50)
// with no documentation of what it does on a renderer that is already subscribed,
// and this engine has only ever called it BEFORE `subscribe()` (engine-video.cpp:70).
// An in-place change cannot be proven without a live meeting, and a silent no-op
// would leave a Program guest at the wrong resolution with nothing to say so. So the
// resolution is instead decided by a STABLE tier that moves only on a Take or a cue:
//
//   * screen share                                  -> 1080P
//   * camera video with purpose program / preview   -> 1080P  (a FIXED bus route)
//   * every other purpose (program-tiles, preview-tiles, multiview, iso,
//     active-speaker — a follow-speaker route, whose person changes with talk)
//                                                   -> 720P
//
// The shell (ZoomVideoSubscriptionPolicy.cs) guarantees that a participant's purpose
// does not depend on who is talking. Only `participant-video` is tiered: the macOS
// shell sends kind "video" with purpose "program" for every assigned guest and must
// not be moved to N x 1080P by this change.
//
// A DOWNGRADE IS NOT SENT. The engine treats a request at or below a live renderer's
// resolution as `video_subscribe_noop_existing` (engine-video.cpp, EngineVideo::
// subscribe) — the renderer stays where it is. So the core keeps the higher key too:
// otherwise the churn ledger records a "resolution-change" teardown that never
// happened, and the next re-raise is a real one the engine would have skipped.
// Net effect: a guest's renderer is raised at most once per subscription (the first
// time they are cued to a bus), and never on a speaker change.
struct ZoomSubscriptionResolutionPolicy {
  static constexpr int k720P = 1;
  static constexpr int k1080P = 2;

  [[nodiscard]] static int requestedResolution(std::string_view kind, std::string_view purpose) {
    if (kind == "screen-share") {
      return k1080P;
    }
    if (kind == "participant-video" && (purpose == "program" || purpose == "preview")) {
      return k1080P;
    }
    return k720P;
  }

  // The key a subscription should hold this tick, given what is already subscribed.
  [[nodiscard]] static int effectiveKey(bool currentlySubscribed, int subscribedKey, int requestedKey) {
    if (currentlySubscribed && subscribedKey > requestedKey) {
      return subscribedKey;
    }
    return requestedKey;
  }
};

}  // namespace corevideo::modules
