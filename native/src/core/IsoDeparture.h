#pragma once
#include <string>
#include <unordered_set>
#include <vector>

namespace corevideo::core {

// #1030: which selected ISO sources are Zoom participants that have LEFT the
// meeting. `selected` holds canonical ISO source ids ("zoom:<sdk id>",
// "capture:..."); `rosterIds` holds the current meeting roster's SDK ids as
// decimal strings. Only Zoom sources can depart. An empty roster is "unknown"
// (not joined yet, or the engine is between processes), never "everyone
// left", so it marks nothing. A participant who rejoins under the same id is
// simply present again on the next tick.
inline std::unordered_set<std::string> departedIsoSources(
    const std::vector<std::string>& selected, const std::unordered_set<std::string>& rosterIds) {
  std::unordered_set<std::string> departed;
  if (rosterIds.empty()) return departed;
  static const std::string kZoom = "zoom:";
  for (const auto& id : selected) {
    if (id.compare(0, kZoom.size(), kZoom) != 0) continue;
    if (!rosterIds.count(id.substr(kZoom.size()))) departed.insert(id);
  }
  return departed;
}

}  // namespace corevideo::core
