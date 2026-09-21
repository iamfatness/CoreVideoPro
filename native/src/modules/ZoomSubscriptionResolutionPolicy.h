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
// THE TIER (R4, amended 2026-09-13, then 2026-09-20): the shell stamps a purpose
// that does not depend on who is talking (ZoomSourceSetPolicy.cs), and
//   * screen share                                        -> 1080P
//   * camera video, ANY purpose (program / preview / program-tiles /
//     preview-tiles / multiview / iso / active-speaker)   -> 1080P, capped
// so the resolution NEVER moves for an in-show source — not on who is talking,
// and since 2026-09-20 not on a Take or a cue either. OWNER RULING 2026-09-20:
// "All sources should be pulling at the highest available for zoom. We shouldn't
// only pull a 720 until they are in preview; that doesn't work once we do a cut."
// The live evidence behind it: with multiview at 720P and a Preview route at
// 1080P, every cue rebuilt that guest's ONE subscription at the other tier (churn
// 16-20 per guest in one morning, reason resolution-change), and Zoom's 720P and
// 1080P encodes of the same camera are NOT tone-identical (measured on the
// engine SHM: ~+2.5 mean Y and ~-1.5 mean U at 1080P, both tiers full range) —
// the operator saw it as "a color shift in preview when selecting a source".
// One tier for every in-show camera removes the flip, so the shift cannot recur.
// A follow-speaker route still grants no purpose (fix round 2, N1); its speaker
// is at the top tier like everyone else, so the old "follow-speaker shot is
// 720P" limitation is gone with it. There is still NO ratchet: a request that
// DOES drop (the cap's demotion, below) is honoured by the engine as a downgrade
// (it used to keep a live renderer at its old, higher resolution forever).
// The core keeps the last decoded frame of a source across its re-subscribe
// (`latestDecodedFrames_` is only erased when a source is RETIRED), so the
// compositor holds the picture instead of dropping it; see the #478 CLAUDE.md
// paragraph for exactly how long each surface holds it.
//
// THE 1080P CAP. `kMaxConcurrentFullResolutionCameras` camera subscriptions may be
// at 1080P at once, granted in the shell's budget order (Program routes first,
// then Preview routes); beyond it a camera gets 720P, and the runtime publishes
// how many were demoted (`zoomSubscriptionChurn.fullResolutionDemoted`). With
// every purpose at the top tier the cap is the ONLY thing that can still flip a
// guest: past 8 camera-on sources, a cue re-ranks the budget (Program routes,
// then Preview routes, then the rest) and the 9th camera trades places with the
// cued one — two re-subscribes per cue, published as fullResolutionDemoted. Say
// so rather than hide it; raising the cap needs a bigger-meeting soak. Evidence
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
  // 8, RAISED FROM 4 AND LIVE-SOAK-PROVEN (2026-09-13, owner "whole wall at
  // 1080p"; the reported bug was tiles dropping as the operator cycled preview).
  // The historic 4 came from a 2026-06 CPU-I420-era crash at 6 concurrent 1080p
  // (bd3caf29). That ceiling is a CPU-path artifact: on today's GPU pipeline an
  // 8-member Tiles wall ran all 8 at 1080p on Program for 30+ min with 60fps
  // delivery, zero underruns, no monitor shedding, and NO engine crash/respawn,
  // through repeated preview cues (totalChurn held flat; per-source churn = 1,
  // the one-time take). 8 is the number a soak PROVED and is the meeting's
  // camera count; raising it further needs a bigger-meeting soak — never raise
  // it on extrapolation (the rule this constant has always carried). Graceful
  // past 8: members are granted in payload order (AddTiles adds members before
  // the wall background), so a 9th 1080p source — a live wall background, a
  // >8-member wall, a non-wall bus route — is DEMOTED to 720P stably rather than
  // flipping a member. Screen share is 1080P and bypasses this camera counter.
  static constexpr int kMaxConcurrentFullResolutionCameras = 8;

  // EVERY camera purpose wants the top tier (2026-09-20). Resolution is part of
  // the engine's subscription key, so any purpose left at a lower tier flips a
  // live renderer the moment that guest is cued or taken — which is exactly the
  // tone shift and the tile drop the operator reported. Only the KIND gates it:
  // the macOS shell's kind "video" is deliberately not promoted (see above). The
  // purpose parameter stays so the cap's payload-order semantics and any future
  // per-purpose exception keep their seam; it is intentionally unused here.
  [[nodiscard]] static bool wantsFullResolution(std::string_view kind, std::string_view purpose) {
    (void)purpose;
    return kind == "participant-video";
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
