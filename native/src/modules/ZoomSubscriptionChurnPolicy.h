#pragma once

namespace corevideo::modules {

// WHEN IS A ZOOM SUBSCRIPTION REALLY TORN DOWN?
//
// `ZoomEngineRuntime::syncSpine` keys the engine's raw-media subscriptions by
// (sourceUuid, resolution). Two things in the shipping spine make that key move
// underneath a source that the operator never touched:
//
//   * RESOLUTION CHURN. `ZoomMediaSpinePayloadBuilder` stamps each request with
//     a `purpose`, and syncSpine picks the resolution from it. Resolution is part
//     of the key, so raising it is a genuine engine-side renderer teardown and
//     rebuild, not bookkeeping. Until #478 (2026-09-11) the 1080P purpose was
//     `active-speaker`, so every speaker change re-subscribed; it is now a stable
//     tier (ZoomSubscriptionResolutionPolicy.h) that moves only on a cue/Take,
//     and a downgrade is never sent, so a source is raised at most once.
//   * CAP EVICTION. A source that falls out of the requested set — because the
//     candidate list reordered around `maxVideoSubscriptions`, e.g. when a
//     Tiles scene serialises an empty route list — is UNSUBSCRIBED by the
//     retire loop, and its frames simply stop arriving.
//
// Since #478 (2026-09-11) the shell subscribes ONLY sources (routes, Tiles
// members, wall slots, ISO guests — ZoomSourceSetPolicy.cs), so most drops are an
// operator UN-ROUTING a guest, not the budget. The shell names the ones the
// budget left out in `videoSubscriptionShortfall`; a retire is a cap eviction
// only when the participant is in that list, otherwise it is `unrouted`. Without
// that split every ordinary un-route would read as the cap defect.
//
// Either one makes a wall or its background go black and re-decode on a take,
// and on air it looks exactly like a render-plan rebuild. This classifies the
// transition so the two can be told apart from the snapshot instead of inferred.
//
// Pure decision, in the CaptureReaderStallPolicy / DeviceLossPolicy shape.
struct ZoomSubscriptionChurnPolicy {
  enum class Change {
    None,          // already subscribed at this key; nothing is sent
    Initial,       // first subscribe for this source — not churn
    Resolution,    // same source, different resolution: re-subscribed
    Resubscribe,   // subscribed again after having been dropped earlier
    CapEviction,   // dropped by the video budget while still in the meeting
    Unrouted,      // dropped because it stopped being a source (operator un-routed it)
    Departure,     // dropped because the participant left
  };

  struct SubscribeObservation {
    bool currentlySubscribed = false;  // present in the dedup map this tick
    int subscribedKey = -1;            // its key there (resolution, -1 for audio)
    int requestedKey = -1;             // the key this tick asks for
    bool everSubscribed = false;       // this source has a churn record already
  };

  [[nodiscard]] static Change classifySubscribe(const SubscribeObservation& observation) {
    if (observation.currentlySubscribed) {
      return observation.subscribedKey == observation.requestedKey ? Change::None
                                                                   : Change::Resolution;
    }
    return observation.everSubscribed ? Change::Resubscribe : Change::Initial;
  }

  // `overBudget`: the shell's videoSubscriptionShortfall names this participant.
  [[nodiscard]] static Change classifyRetire(bool participantStillInMeeting, bool overBudget) {
    if (!participantStillInMeeting) {
      return Change::Departure;
    }
    return overBudget ? Change::CapEviction : Change::Unrouted;
  }

  // An Initial subscribe is the subscription starting, not churning. Everything
  // else here is a real teardown or rebuild the operator can see.
  [[nodiscard]] static bool countsAsChurn(Change change) {
    return change == Change::Resolution || change == Change::Resubscribe ||
           change == Change::CapEviction || change == Change::Unrouted ||
           change == Change::Departure;
  }

  // A new engine-side renderer exists after these. The generation is what a
  // reader compares across a take to prove a source was re-subscribed.
  [[nodiscard]] static bool advancesGeneration(Change change) {
    return change != Change::None;
  }

  [[nodiscard]] static const char* reason(Change change) {
    switch (change) {
      case Change::None: return "none";
      case Change::Initial: return "initial";
      case Change::Resolution: return "resolution-change";
      case Change::Resubscribe: return "resubscribe";
      case Change::CapEviction: return "cap-eviction";
      case Change::Unrouted: return "unrouted";
      case Change::Departure: return "departure";
    }
    return "none";
  }
};

}  // namespace corevideo::modules
