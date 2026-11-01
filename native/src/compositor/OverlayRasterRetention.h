#pragma once

// Keeps the last ready overlay raster per layer while a replacement is being
// prepared (#1073 review). The overlay raster cache is keyed by content AND
// band pixel size, so a wall reflow that resizes every tile's name band sends
// every name back to "preparing" at once; the raster queue is bounded (one
// serial worker, 32 queued, 64 resident), so on a big wall the new textures
// land over several frames. Without this, each of those frames would show
// either nothing or the legacy bare accent bar in place of the text.
//
// Pure bookkeeping, templated on the view handle type so it is testable
// without D3D. The lease is whatever keeps the texture alive (the raster
// cache's shared result); the caller still registers it with its GPU read
// fence every frame it draws from it.

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace corevideo::compositor {

template <typename View>
class OverlayRasterRetention {
 public:
  struct Entry {
    std::shared_ptr<const void> lease;
    View* view = nullptr;
    int width = 0, height = 0;
    std::uint64_t lastUsedFrame = 0;
  };

  // `fresh` is this frame's raster for `layerId` (nullptr while preparing).
  // Returns the view to draw: the fresh one (now retained), else the retained
  // previous raster (sized for the OLD band; the caller scales it through the
  // viewport), else nullptr. Nothing else is ever substituted.
  View* resolve(const std::string& layerId, View* fresh, std::shared_ptr<const void> lease,
                int width, int height, std::uint64_t frame) {
    auto& entry = entries_[layerId];
    entry.lastUsedFrame = frame;
    if (fresh) {
      entry.lease = std::move(lease);
      entry.view = fresh;
      entry.width = width;
      entry.height = height;
      return fresh;
    }
    return entry.view;
  }

  const Entry* find(const std::string& layerId) const {
    const auto it = entries_.find(layerId);
    return it == entries_.end() ? nullptr : &it->second;
  }

  // Drops entries no layer has drawn for `maxIdleFrames` frames, and, as a
  // hard bound, everything but the most recent frame's entries once the map
  // outgrows `maxEntries`. Call once per rendered frame.
  void prune(std::uint64_t frame, std::uint64_t maxIdleFrames = 120, std::size_t maxEntries = 512) {
    // Decided ONCE, before erasing: re-reading size() inside the loop would
    // stop the overflow sweep as soon as it dipped under the bound.
    const bool overflowing = entries_.size() > maxEntries;
    for (auto it = entries_.begin(); it != entries_.end();) {
      const bool idle = frame > it->second.lastUsedFrame + maxIdleFrames;
      const bool overflow = overflowing && it->second.lastUsedFrame != frame;
      it = (idle || overflow) ? entries_.erase(it) : std::next(it);
    }
  }

  void clear() { entries_.clear(); }
  std::size_t size() const { return entries_.size(); }

 private:
  std::unordered_map<std::string, Entry> entries_;
};

}  // namespace corevideo::compositor
