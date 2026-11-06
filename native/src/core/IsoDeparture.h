#pragma once
#include <string>
#include <optional>
#include <unordered_set>
#include <vector>

namespace corevideo::core {

// #1030: which selected ISO sources are Zoom participants that have LEFT the
// meeting. `selected` holds canonical ISO source ids ("zoom:<sdk id>",
// "capture:..."); `rosterIds` holds the current meeting roster's SDK ids as
// decimal strings. The caller supplies an AUTHORITATIVE roster, including an
// explicitly empty one. Unavailable knowledge is handled by the tracker below.
inline std::unordered_set<std::string> departedIsoSources(
    const std::vector<std::string>& selected, const std::unordered_set<std::string>& rosterIds) {
  std::unordered_set<std::string> departed;
  static const std::string kZoom = "zoom:";
  for (const auto& id : selected) {
    if (id.compare(0, kZoom.size(), kZoom) != 0) continue;
    if (!rosterIds.count(id.substr(kZoom.size()))) departed.insert(id);
  }
  return departed;
}

class IsoDepartureTracker {
 public:
  std::unordered_set<std::string> observe(const std::vector<std::string>& selected,
      const std::optional<std::unordered_set<std::string>>& roster) {
    if (roster) departed_ = departedIsoSources(selected, *roster);
    else {
      const std::unordered_set<std::string> selectedIds(selected.begin(), selected.end());
      for (auto it = departed_.begin(); it != departed_.end();) {
        if (!selectedIds.count(*it)) it = departed_.erase(it);
        else ++it;
      }
    }
    return departed_;
  }
  void clear() { departed_.clear(); }
 private:
  std::unordered_set<std::string> departed_;
};

}  // namespace corevideo::core
