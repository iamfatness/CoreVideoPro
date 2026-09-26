#pragma once

#include <algorithm>
#include <cmath>
#include <string>
#include <map>
#include <vector>

#include "core/SpeakerFloor.h"

namespace corevideo::core {

// Pure, deterministic on-device director kernel.
//
// The C++ decision twin of TypeScript selectLocalProposal. SpeakerFloor owns
// the turn ledger; this kernel scores distinct guest identities, recommends a
// scene, and returns the exact people and sources for its slots.
//
// Like the renderer engine it is decision-pure: no I/O, no model, no network. The
// host (MediaCore) derives these signals from the core's current state and asks
// the kernel for a recommendation; the kernel never mutates anything.

// Coarse, transport-neutral context alongside the floor snapshot. All plain data.
struct DirectorSignals {
  // feedHealth.liveCount — live (non video-off) participant feeds.
  int liveCount = 0;
  // feedHealth.degradedCount — feeds that are recovering or low-resolution.
  int degradedCount = 0;
  // engagement.activeContributorCount — unmuted-or-talking live participants.
  int activeContributorCount = 0;
  // engagement.meanAudioLevel — mean audio level across live feeds (0..1).
  double meanAudioLevel = 0.0;
  // speakerTurns.dominantSpeakerId !== undefined — exactly one active speaker.
  bool hasDominantSpeaker = false;
  // speakerTurns.crossTalk — 2+ feeds active simultaneously.
  bool crossTalk = false;
  // screenShare.active — an active screen share is present.
  bool screenShareActive = false;
  // screenShare.sharerId present — a known sharer participant id.
  bool screenShareHasSharer = false;
  // screenShare.sharerName, when known, for the rationale text.
  std::string screenShareSharerName;
};

// The deterministic scene recommendation, mirroring DirectorProposal.
struct DirectorRecommendation {
  std::string ruleId;
  std::string recommendedSceneId;
  int confidence = 0;
  std::string rationale;
  struct SlotBinding {
    int slotIndex = 0;
    std::string personId;
    std::string sourceId;
  };
  std::vector<SlotBinding> slotBindings;
};

inline int clampDirectorConfidence(double value) {
  const double rounded = std::round(value);
  return static_cast<int>(std::max(0.0, std::min(100.0, rounded)));
}

// The live recommendation consumes an epoch-scoped floor snapshot. Camera count
// alone cannot earn a box.
inline DirectorRecommendation recommendScene(const DirectorSignals& signals,
                                              const std::vector<FloorPerson>& floor,
                                              std::uint64_t nowMs) {
  std::vector<FloorPerson> guests;
  std::vector<FloorPerson> hosts;
  std::map<std::string, FloorPerson> uniquePeople;
  for (const auto& person : floor) {
    if (!person.hasVideo || person.id.empty()) continue;
    auto [found, inserted] = uniquePeople.emplace(person.id, person);
    if (!inserted) {
      found->second.isHost = found->second.isHost || person.isHost;
      found->second.talkingNow = found->second.talkingNow || person.talkingNow;
      found->second.score = (std::max)(found->second.score, person.score);
      found->second.lastSpokeAtMs = (std::max)(found->second.lastSpokeAtMs, person.lastSpokeAtMs);
      if (person.sourceId.starts_with("zoom:") && !found->second.sourceId.starts_with("zoom:"))
        found->second.sourceId = person.sourceId;
    }
  }
  for (const auto& [_, person] : uniquePeople) {
    (person.isHost ? hosts : guests).push_back(person);
  }
  const auto byFloor = [](const FloorPerson& a, const FloorPerson& b) {
    if (a.score != b.score) return a.score > b.score;
    if (a.lastSpokeAtMs != b.lastSpokeAtMs) return a.lastSpokeAtMs > b.lastSpokeAtMs;
    return a.id < b.id;
  };
  std::sort(guests.begin(), guests.end(), byFloor);
  std::sort(hosts.begin(), hosts.end(), byFloor);
  auto bind = [](DirectorRecommendation& result, const FloorPerson& person) {
    result.slotBindings.push_back({static_cast<int>(result.slotBindings.size()), person.id, person.sourceId});
  };
  const double degradePenalty = std::min(8.0, signals.degradedCount * 2.0);
  if (signals.screenShareActive) {
    DirectorRecommendation result{"screen-share-priority", "speaker-slides",
        clampDirectorConfidence(96.0 + (signals.screenShareHasSharer ? 1.0 : -2.0) - degradePenalty),
        "Share surface leads; guest talker is the picture-in-picture."};
    if (!guests.empty()) bind(result, guests.front());
    else if (!hosts.empty()) bind(result, hosts.front());
    return result;
  }
  if (guests.empty()) {
    DirectorRecommendation result{"single-speaker", "intro", hosts.empty() ? 0 : 90,
        hosts.empty() ? "No guest video or host camera is available." : "Only host is on camera."};
    if (!hosts.empty()) bind(result, hosts.front());
    return result;
  }
  const auto recent = [nowMs](const FloorPerson& person) {
    return person.talkingNow || (person.lastSpokeAtMs > 0 &&
        nowMs >= person.lastSpokeAtMs && nowMs - person.lastSpokeAtMs <= 12'000);
  };
  const auto scoredCount = std::count_if(guests.begin(), guests.end(),
      [](const FloorPerson& person) { return person.score > 0.0; });
  if (scoredCount == 0) {
    return {"single-speaker", "intro", 0, "No guest has earned a talker slot yet."};
  }
  if ((guests.size() >= 5 && scoredCount >= 1) || (guests.size() >= 3 && scoredCount >= 2)) {
    DirectorRecommendation result{"panel-discussion", "panel",
        clampDirectorConfidence(90.0 + (scoredCount >= 3 ? 3.0 : 0.0) - degradePenalty),
        "Guest floor supports a gallery."};
    for (size_t index = 0; index < (std::min)(guests.size(), size_t{6}); ++index) bind(result, guests[index]);
    return result;
  }
  if (guests.size() >= 2 && guests[0].score > 0.0 && recent(guests[1])) {
    DirectorRecommendation result{"focused-interview", "interview",
        clampDirectorConfidence(92.0 - (signals.crossTalk ? 4.0 : 0.0) - degradePenalty),
        "Two distinct guests have the floor within 12 seconds."};
    bind(result, guests[0]);
    bind(result, guests[1]);
    return result;
  }
  DirectorRecommendation result{"single-speaker", "intro",
      clampDirectorConfidence(90.0 + signals.meanAudioLevel * 6.0 - degradePenalty),
      "One guest has earned the current talker slot."};
  bind(result, guests.front());
  return result;
}

}  // namespace corevideo::core
