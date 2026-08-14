#pragma once

#include "modules/ZoomEngineClient.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace corevideo::modules {

// Stabilizes Zoom's raw active-speaker events before they are allowed to drive
// Preview/Program routing. New candidates must remain eligible for sensitivityMs
// and the incumbent is guaranteed holdMs on air. The incumbent deliberately
// survives mute/video-off and short roster gaps: those are common during a real
// meeting and should not cut Program to black.
class ZoomActiveSpeakerDirector {
 public:
  void configure(std::uint32_t sensitivityMs, std::uint32_t holdMs,
                 bool requireVideo = true,
                 std::vector<std::uint32_t> excludedParticipantIds = {}) {
    sensitivityMs_ = sensitivityMs;
    holdMs_ = holdMs;
    requireVideo_ = requireVideo;
    excludedParticipantIds.erase(
        std::remove(excludedParticipantIds.begin(), excludedParticipantIds.end(), 0),
        excludedParticipantIds.end());
    std::sort(excludedParticipantIds.begin(), excludedParticipantIds.end());
    excludedParticipantIds.erase(
        std::unique(excludedParticipantIds.begin(), excludedParticipantIds.end()),
        excludedParticipantIds.end());
    excludedParticipantIds_ = std::move(excludedParticipantIds);
  }

  bool updateRoster(const std::vector<ZoomEngineParticipant>& roster,
                    std::uint32_t rawSpeakerId, std::uint64_t nowMs) {
    roster_ = roster;
    rawSpeakerId_ = rawSpeakerId;
    enforceIncumbent(nowMs);

    const auto candidate = chooseCandidate(rawSpeakerId);
    if (directedSpeakerId_ == 0) {
      return fillVacancy(candidate, nowMs);
    }
    if (candidate == 0 || candidate == directedSpeakerId_) {
      candidateSpeakerId_ = 0;
      candidateSinceMs_ = 0;
      return false;
    }
    if (candidate != candidateSpeakerId_) {
      candidateSpeakerId_ = candidate;
      candidateSinceMs_ = nowMs;
    }
    return tick(nowMs);
  }

  bool tick(std::uint64_t nowMs) {
    enforceIncumbent(nowMs);
    if (directedSpeakerId_ == 0) {
      return fillVacancy(chooseCandidate(rawSpeakerId_), nowMs);
    }
    if (!participantAllowed(candidateSpeakerId_) ||
        !participantFrameFresh(candidateSpeakerId_)) {
      return false;
    }
    const auto candidateAge = nowMs >= candidateSinceMs_ ? nowMs - candidateSinceMs_ : 0;
    const auto heldFor = nowMs >= lastSwitchMs_ ? nowMs - lastSwitchMs_ : 0;
    if (candidateAge < sensitivityMs_ || heldFor < holdMs_) {
      return false;
    }
    return promote(candidateSpeakerId_, nowMs);
  }

  void reset() {
    roster_.clear();
    freshFrameParticipantIds_.clear();
    rawSpeakerId_ = 0;
    directedSpeakerId_ = 0;
    directedMissingSinceMs_ = 0;
    candidateSpeakerId_ = 0;
    candidateSinceMs_ = 0;
    lastSwitchMs_ = 0;
  }

  [[nodiscard]] std::uint32_t directedSpeakerId() const { return directedSpeakerId_; }
  [[nodiscard]] std::uint32_t candidateSpeakerId() const { return candidateSpeakerId_; }

  void setFrameFresh(std::uint32_t id, bool fresh) {
    if (id == 0) {
      return;
    }
    const auto found = std::lower_bound(freshFrameParticipantIds_.begin(),
                                        freshFrameParticipantIds_.end(), id);
    if (fresh) {
      if (found == freshFrameParticipantIds_.end() || *found != id) {
        freshFrameParticipantIds_.insert(found, id);
      }
    } else if (found != freshFrameParticipantIds_.end() && *found == id) {
      freshFrameParticipantIds_.erase(found);
    }
  }

 private:
  static constexpr std::uint64_t kAbsenceGraceMs = 60'000;

  [[nodiscard]] bool excluded(std::uint32_t id) const {
    return std::binary_search(excludedParticipantIds_.begin(),
                              excludedParticipantIds_.end(), id);
  }

  [[nodiscard]] bool inRoster(std::uint32_t id) const {
    return std::any_of(roster_.begin(), roster_.end(),
                       [id](const auto& participant) { return participant.id == id; });
  }

  [[nodiscard]] bool participantAllowed(std::uint32_t id) const {
    if (id == 0 || excluded(id)) {
      return false;
    }
    const auto found = std::find_if(roster_.begin(), roster_.end(),
                                    [id](const auto& participant) { return participant.id == id; });
    return found != roster_.end() && !found->isMuted && (!requireVideo_ || found->hasVideo);
  }

  [[nodiscard]] bool participantFrameFresh(std::uint32_t id) const {
    return std::binary_search(freshFrameParticipantIds_.begin(),
                              freshFrameParticipantIds_.end(), id);
  }

  [[nodiscard]] std::uint32_t chooseCandidate(std::uint32_t rawSpeakerId) const {
    if (participantAllowed(rawSpeakerId)) {
      return rawSpeakerId;
    }
    const auto talking = std::find_if(roster_.begin(), roster_.end(), [this](const auto& participant) {
      return participant.isTalking && participantAllowed(participant.id);
    });
    return talking == roster_.end() ? 0 : talking->id;
  }

  void enforceIncumbent(std::uint64_t nowMs) {
    if (directedSpeakerId_ == 0) {
      directedMissingSinceMs_ = 0;
      return;
    }
    if (excluded(directedSpeakerId_)) {
      directedSpeakerId_ = 0;
      directedMissingSinceMs_ = 0;
      return;
    }
    if (inRoster(directedSpeakerId_)) {
      directedMissingSinceMs_ = 0;
      return;
    }
    if (directedMissingSinceMs_ == 0) {
      directedMissingSinceMs_ = nowMs;
    } else if (nowMs >= directedMissingSinceMs_ &&
               nowMs - directedMissingSinceMs_ > kAbsenceGraceMs) {
      directedSpeakerId_ = 0;
      directedMissingSinceMs_ = 0;
    }
  }

  bool promote(std::uint32_t id, std::uint64_t nowMs) {
    if (id == 0 || id == directedSpeakerId_) {
      return false;
    }
    directedSpeakerId_ = id;
    directedMissingSinceMs_ = 0;
    candidateSpeakerId_ = 0;
    candidateSinceMs_ = 0;
    lastSwitchMs_ = nowMs;
    return true;
  }

  bool fillVacancy(std::uint32_t candidate, std::uint64_t nowMs) {
    if (candidate != candidateSpeakerId_) {
      candidateSpeakerId_ = candidate;
      candidateSinceMs_ = candidate == 0 ? 0 : nowMs;
    }
    // Cold-start/finally-expired vacancy has no on-air speaker to protect.
    return promote(candidate, nowMs);
  }

  std::vector<ZoomEngineParticipant> roster_;
  std::vector<std::uint32_t> excludedParticipantIds_;
  std::vector<std::uint32_t> freshFrameParticipantIds_;
  std::uint32_t rawSpeakerId_ = 0;
  std::uint32_t directedSpeakerId_ = 0;
  std::uint32_t candidateSpeakerId_ = 0;
  std::uint64_t directedMissingSinceMs_ = 0;
  std::uint64_t candidateSinceMs_ = 0;
  std::uint64_t lastSwitchMs_ = 0;
  std::uint32_t sensitivityMs_ = 500;
  std::uint32_t holdMs_ = 2000;
  bool requireVideo_ = true;
};

}  // namespace corevideo::modules
