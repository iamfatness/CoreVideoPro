#pragma once

#include <functional>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace corevideo::core {

struct FloorObservation {
  std::string sourceId;
  std::string personId;
  bool isHost = false;
  bool hasVideo = false;
  bool talking = false;
};

struct FloorPerson {
  std::string id;
  std::string sourceId;
  bool isHost = false;
  bool hasVideo = false;
  bool talkingNow = false;
  std::uint64_t turnStartedAtMs = 0;
  std::uint64_t lastSpokeAtMs = 0;
  std::uint64_t lastTurnMs = 0;
  std::uint64_t windowTalkMs = 0;
  double score = 0.0;
};

// Slice A identity and one-person-per-plan policy. The talk ledger is added in
// Slice C; this type already owns the person identity that ledger must use.
class SpeakerFloor {
 public:
  void observeSource(std::string_view sourceId, std::string_view explicitPersonId = {},
                     bool isHost = false) {
    if (sourceId.empty()) return;
    const std::string key(sourceId);
    const std::string person = isZoomSource(sourceId) || explicitPersonId.empty()
        ? defaultPersonId(sourceId) : std::string(explicitPersonId);
    if (person.empty()) return;
    // One physical source cannot claim two people just because two scene
    // layers supplied conflicting hints. The first binding wins this plan.
    const auto [it, inserted] = sourcePeople_.emplace(key, person);
    (void)inserted;
    personHosts_[it->second] = personHosts_[it->second] || isHost;
  }

  [[nodiscard]] std::string personFor(std::string_view sourceId) const {
    if (sourceId.empty()) return {};
    if (const auto found = sourcePeople_.find(sourceId); found != sourcePeople_.end())
      return found->second;
    return defaultPersonId(sourceId);
  }

  [[nodiscard]] bool isHost(std::string_view personId) const {
    const auto found = personHosts_.find(personId);
    return found != personHosts_.end() && found->second;
  }

  // Called only for visible person-video layers. Screen share, media,
  // backgrounds and overlays are surfaces, not extra person seats.
  bool claimPersonSlot(std::string_view sourceId) {
    const auto person = personFor(sourceId);
    return !person.empty() && boundPeople_.insert(person).second;
  }

  void observeFrame(std::uint64_t epoch, std::uint64_t nowMs,
                    const std::vector<FloorObservation>& observations) {
    if (epoch != epoch_) {
      epoch_ = epoch;
      ledger_.clear();
    }
    // Source links may be reassigned by the operator during one meeting.
    // Keep turn history by person, but resolve today's source aliases afresh.
    sourcePeople_.clear();
    personHosts_.clear();
    boundPeople_.clear();
    for (auto& [_, state] : ledger_) {
      state.hasVideo = false;
      state.talking = false;
      state.sourceId.clear();
      state.sourceHasVideo = false;
    }
    for (const auto& observation : observations) {
      observeSource(observation.sourceId, observation.personId, observation.isHost);
      const auto person = personFor(observation.sourceId);
      if (person.empty()) continue;
      auto& state = ledger_[person];
      state.hasVideo = state.hasVideo || observation.hasVideo;
      state.talking = state.talking || observation.talking;
      // A Zoom tile is preferred to a linked capture of the same person.
      if (state.sourceId.empty() ||
          (observation.hasVideo && !state.sourceHasVideo) ||
          (observation.hasVideo == state.sourceHasVideo && isZoomSource(observation.sourceId) &&
           !isZoomSource(state.sourceId))) {
        state.sourceId = observation.sourceId;
        state.sourceHasVideo = observation.hasVideo;
      }
    }
    for (auto& [_, state] : ledger_) {
      if (state.talking) {
        state.pendingEndAtMs.reset();
        if (!state.pendingStartAtMs) state.pendingStartAtMs = nowMs;
        if (!state.talkingNow && nowMs >= *state.pendingStartAtMs + 400) {
          state.talkingNow = true;
          state.turnStartedAtMs = *state.pendingStartAtMs;
        }
      } else {
        state.pendingStartAtMs.reset();
        if (state.talkingNow) {
          if (!state.pendingEndAtMs) state.pendingEndAtMs = nowMs;
          if (nowMs >= *state.pendingEndAtMs + 600) {
            const auto end = *state.pendingEndAtMs;
            state.lastSpokeAtMs = end;
            state.lastTurnMs = end >= state.turnStartedAtMs ? end - state.turnStartedAtMs : 0;
            state.turns.emplace_back(state.turnStartedAtMs, end);
            state.talkingNow = false;
            state.turnStartedAtMs = 0;
            state.pendingEndAtMs.reset();
          }
        }
      }
      while (!state.turns.empty() && state.turns.front().second + 45'000 < nowMs)
        state.turns.pop_front();
    }
  }

  [[nodiscard]] std::vector<FloorPerson> snapshot(std::uint64_t nowMs) const {
    std::vector<FloorPerson> people;
    people.reserve(ledger_.size());
    const auto windowStart = nowMs > 45'000 ? nowMs - 45'000 : 0;
    for (const auto& [id, state] : ledger_) {
      FloorPerson person;
      person.id = id;
      person.sourceId = state.sourceId;
      person.isHost = isHost(id);
      person.hasVideo = state.hasVideo;
      person.talkingNow = state.talkingNow;
      person.turnStartedAtMs = state.turnStartedAtMs;
      person.lastSpokeAtMs = state.talkingNow ? nowMs : state.lastSpokeAtMs;
      person.lastTurnMs = state.lastTurnMs;
      for (const auto& [start, end] : state.turns)
        if (end > windowStart) person.windowTalkMs += end - (std::max)(start, windowStart);
      if (state.talkingNow && nowMs > state.turnStartedAtMs)
        person.windowTalkMs += nowMs - (std::max)(state.turnStartedAtMs, windowStart);
      if (person.lastSpokeAtMs != 0) {
        const double ageSec = nowMs >= person.lastSpokeAtMs
            ? static_cast<double>(nowMs - person.lastSpokeAtMs) / 1000.0 : 0.0;
        person.score = 3.0 * (person.talkingNow ? 1.0 : 0.0) +
            2.0 * std::exp(-ageSec / 8.0) +
            (std::min)(static_cast<double>(person.lastTurnMs) / 8000.0, 1.0) +
            0.5 * (std::min)(static_cast<double>(person.windowTalkMs) / 20000.0, 1.0);
      }
      people.push_back(std::move(person));
    }
    return people;
  }

 private:
  static bool isZoomSource(std::string_view sourceId) {
    return sourceId.starts_with("zoom:") || sourceId.starts_with("participant:");
  }

  static std::string defaultPersonId(std::string_view sourceId) {
    if (sourceId.starts_with("zoom:")) return std::string(sourceId.substr(5));
    if (sourceId.starts_with("participant:")) return std::string(sourceId.substr(12));
    return std::string(sourceId);
  }

  std::map<std::string, std::string, std::less<>> sourcePeople_;
  std::map<std::string, bool, std::less<>> personHosts_;
  std::set<std::string, std::less<>> boundPeople_;
  struct TurnState {
    std::string sourceId;
    bool sourceHasVideo = false;
    bool hasVideo = false;
    bool talking = false;
    bool talkingNow = false;
    std::optional<std::uint64_t> pendingStartAtMs;
    std::optional<std::uint64_t> pendingEndAtMs;
    std::uint64_t turnStartedAtMs = 0;
    std::uint64_t lastSpokeAtMs = 0;
    std::uint64_t lastTurnMs = 0;
    std::deque<std::pair<std::uint64_t, std::uint64_t>> turns;
  };
  std::uint64_t epoch_ = 0;
  std::map<std::string, TurnState, std::less<>> ledger_;
};

}  // namespace corevideo::core
