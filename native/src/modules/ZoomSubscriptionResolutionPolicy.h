#pragma once

#include <string_view>

namespace corevideo::modules {

// WHAT RESOLUTION DOES A ZOOM VIDEO SUBSCRIPTION ASK FOR? (#478, 2026-09-11;
// controller ruling R4 in fix round 1)
//
// Resolution is part of `ZoomEngineRuntime::syncSpine`'s subscription key, and the
// engine REBUILDS a live renderer (destroy, then create at the new resolution) to
// change it — engine-video.cpp, `video_upgrade_subscription` /
// `video_resolution_rebuild`. So anything that moves the requested resolution under
// an unchanged source is a real engine-side re-subscribe.
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
// would leave a Program guest at the wrong resolution with nothing to say so.
//
// THE TIER (R4): the shell stamps a purpose that does not depend on who is talking
// (ZoomSourceSetPolicy.cs), and
//   * screen share                                        -> 1080P
//   * camera video, purpose program / preview (a FIXED route on a bus) -> 1080P, capped
//   * program-tiles, preview-tiles, multiview, iso         -> 720P
// so the resolution moves only on a Take or a cue — NEVER on who is talking. A
// follow-speaker route grants no purpose (fix round 2, N1): its speaker keeps their
// own tier, so a follow-speaker shot is 720P until an in-place resolution change is
// proven on a live renderer. There is NO ratchet: a guest who leaves
// Program/Preview goes back to 720P, and the engine now honours that downgrade
// (it used to keep a live renderer at its old, higher resolution forever, which
// over a show put every rotated guest at 1080P — the N x 1080P overload below).
// The core keeps the last decoded frame of a source across its re-subscribe
// (`latestDecodedFrames_` is only erased when a source is RETIRED), so the
// compositor holds the picture instead of dropping it; see the #478 CLAUDE.md
// paragraph for exactly how long each surface holds it.
//
// THE 1080P CAP. `kMaxConcurrentFullResolutionCameras` camera subscriptions may be
// at 1080P at once, granted in the shell's budget order (Program routes first,
// then Preview routes); beyond it a bus route gets 720P, and the runtime publishes
// how many were demoted (`zoomSubscriptionChurn.fullResolutionDemoted`). Evidence
// for the number: SIX concurrent 1080P raw subscriptions crashed the Zoom SDK
// subprocess (ntdll 0xc000000d) and overloaded the then-CPU I420 path — commit
// bd3caf29 (2026-06-28), which introduced per-purpose resolution for exactly that
// reason; every build since shipped with at most ONE camera at 1080P (the directed
// speaker) plus a screen share. 4 covers a two-up interview on BOTH buses, stays
// two below the measured failure point with a share on top, and is the number to
// raise only after a live engine-CPU soak proves more — never to lower silently.
//
// Only `participant-video` is tiered: the macOS shell sends kind "video" with
// purpose "program" for every assigned guest and must not be moved to N x 1080P.
struct ZoomSubscriptionResolutionPolicy {
  static constexpr int k720P = 1;
  static constexpr int k1080P = 2;
  static constexpr int kMaxConcurrentFullResolutionCameras = 4;

  [[nodiscard]] static bool wantsFullResolution(std::string_view kind, std::string_view purpose) {
    return kind == "participant-video" && (purpose == "program" || purpose == "preview");
  }

  // The resolution a request asks for BEFORE the concurrency cap.
  [[nodiscard]] static int requestedResolution(std::string_view kind, std::string_view purpose) {
    if (kind == "screen-share" || wantsFullResolution(kind, purpose)) {
      return k1080P;
    }
    return k720P;
  }

  // Applies the 1080P cap across ONE spine payload, in its order.
  class Budget {
   public:
    [[nodiscard]] int resolve(std::string_view kind, std::string_view purpose) {
      if (kind == "screen-share") {
        return k1080P;
      }
      if (!wantsFullResolution(kind, purpose)) {
        return k720P;
      }
      if (granted_ < kMaxConcurrentFullResolutionCameras) {
        ++granted_;
        return k1080P;
      }
      ++demoted_;
      return k720P;
    }
    [[nodiscard]] int granted() const { return granted_; }
    [[nodiscard]] int demoted() const { return demoted_; }

   private:
    int granted_ = 0;
    int demoted_ = 0;
  };
};

}  // namespace corevideo::modules
