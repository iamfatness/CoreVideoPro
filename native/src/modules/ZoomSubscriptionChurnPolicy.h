#pragma once

namespace corevideo::modules {

// WHEN IS A ZOOM SUBSCRIPTION REALLY TORN DOWN?
//
// `ZoomEngineRuntime::syncSpine` keys the engine's raw-media subscriptions by
// (sourceUuid, resolution). Two things in the shipping spine make that key move
// underneath a source that the operator never touched:
//
//   * RESOLUTION CHURN. `ZoomMediaSpinePayloadBuilder` stamps each request with
//     a `purpose`, and syncSpine picks 1080P for `active-speaker`/screen-share
//     and 720P for everything else. Resolution is part of the key, so a source
//     flipping in or out of active-speaker is genuinely re-subscribed: an
//     engine-side renderer teardown and rebuild, not bookkeeping.
//   * CAP EVICTION. A source that falls out of the requested set — because the
//     candidate list reordered around `maxVideoSubscriptions`, e.g. when a
//     Tiles scene serialises an empty route list — is UNSUBSCRIBED by the
//     retire loop, and its frames simply stop arriving.
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
    CapEviction,   // dropped from the requested set while still in the meeting
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

  [[nodiscard]] static Change classifyRetire(bool participantStillInMeeting) {
    return participantStillInMeeting ? Change::CapEviction : Change::Departure;
  }

  // An Initial subscribe is the subscription starting, not churning. Everything
  // else here is a real teardown or rebuild the operator can see.
  [[nodiscard]] static bool countsAsChurn(Change change) {
    return change == Change::Resolution || change == Change::Resubscribe ||
           change == Change::CapEviction || change == Change::Departure;
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
      case Change::Departure: return "departure";
    }
    return "none";
  }
};

}  // namespace corevideo::modules
