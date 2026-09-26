#pragma once

#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>

namespace corevideo::core {

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
};

}  // namespace corevideo::core
