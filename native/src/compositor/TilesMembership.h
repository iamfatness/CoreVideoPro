#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace corevideo::compositor {

// The shell decides who is ELIGIBLE for the wall; the core decides who is
// actually DRAWN. Only the core knows whether frames are arriving, and a wall
// that holds a slot for a dead feed shows a black square. The plugin accepts
// that trade; we do not have to.
constexpr int64_t kTilesStaleFrameMs = 1500;

struct TilesMemberFrameAge {
  std::string sourceId;
  bool hasFrame = false;
  int64_t lastFrameAgeMs = 0;
};

inline std::vector<std::string> admitTilesMembers(
    const std::vector<std::string>& members,
    const std::vector<TilesMemberFrameAge>& ages,
    int64_t staleAfterMs = kTilesStaleFrameMs) {
  std::vector<std::string> admitted;
  std::unordered_set<std::string> seen;
  admitted.reserve(members.size());
  for (const auto& member : members) {
    if (!seen.insert(member).second) {
      continue;
    }
    for (const auto& age : ages) {
      if (age.sourceId == member) {
        if (age.hasFrame && age.lastFrameAgeMs <= staleAfterMs) {
          admitted.push_back(member);
        }
        break;
      }
    }
  }
  return admitted;
}

}  // namespace corevideo::compositor
