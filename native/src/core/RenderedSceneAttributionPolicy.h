#pragma once

namespace corevideo::core {

// WHAT PROGRAM ACTUALLY RENDERED — and when the snapshot is allowed to say it.
//
// The buffered Program path attributes the snapshot from the frame the program
// buffer last DELIVERED, not from the frame the render tick just produced
// (MediaCore::renderTick, the `programBufferFrames() > 0` branch). The buffer
// republishes that same delivered frame on every peek, so "a frame came back"
// is NOT evidence that Program advanced — and the old code took it as such,
// with no branch at all for "the buffer handed back nothing". Either way the
// rendered-scene attribution simply stopped being written.
//
// Live show 2026-09-09: `programFrame.sceneId` sat on the pre-take scene for
// 15+ seconds while Program was demonstrably compositing the new one, and never
// caught up. That is this hole. An operator reads that field to know what is on
// air; it must follow Program, or admit it does not know — never keep asserting
// a scene nothing has confirmed.
//
// Pure decision, in the CaptureReaderStallPolicy / OutputLifecyclePolicy shape:
// no clock, no state, no allocation.
struct RenderedSceneAttributionPolicy {
  enum class Action {
    Follow,    // a new delivery arrived with plan evidence: restamp from it
    Hold,      // brief delivery jitter: keep the current attribution
    Forget,    // nothing has confirmed this attribution: publish nothing
  };

  struct Decision {
    Action action = Action::Forget;
    // Snapshot-visible wording. "live" = attributed to a delivery this tick.
    const char* state = "unknown";
  };

  // Delivery runs at the Program cadence, so a handful of ticks without one is
  // ordinary jitter (an export contended by the monitor wall, a slot expiring
  // under a render overrun). Past this the field would be asserting a scene no
  // delivered frame has proved. 12 ticks is 200ms at 60fps — under a video
  // frame of operator-visible lag, far under the 15s observed on air.
  static constexpr int kHoldTicks = 12;

  [[nodiscard]] static Decision evaluate(bool deliveryAdvanced,
                                         bool hasRenderPlanEvidence,
                                         int ticksSinceDeliveryAdvanced) {
    if (deliveryAdvanced) {
      // A delivered frame with no plan evidence cannot attribute anything. It
      // must not inherit the previous frame's scene (the same lie, one frame
      // later), so the attribution is dropped rather than held.
      return hasRenderPlanEvidence ? Decision{Action::Follow, "live"}
                                   : Decision{Action::Forget, "unknown"};
    }
    if (ticksSinceDeliveryAdvanced <= kHoldTicks) {
      return {Action::Hold, "holding"};
    }
    return {Action::Forget, "unknown"};
  }
};

}  // namespace corevideo::core
