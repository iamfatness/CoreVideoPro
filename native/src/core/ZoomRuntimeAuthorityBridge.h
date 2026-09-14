#pragma once
#include "core/ZoomSourceAuthorityAdapter.h"
#include "modules/ZoomEngineRuntime.h"

namespace corevideo::core {
// Pure conversion. Caller owns intake ordering and invalidates shadow continuity
// on any rejection; never feed a partial observation to the adapter.
class ZoomRuntimeAuthorityBridge final {
 public:
  static constexpr size_t kMaxSources = 8192;
  enum class Status { Converted, Invalid, Capacity, UnsupportedDurableIdentity };
  struct Result {
    Status status{Status::Invalid};
    std::optional<ZoomSourceAuthorityAdapter::Observation> observation;
  };
  static Result convert(const modules::ZoomEngineRuntime::AuthorityObservation&,
                        size_t maxSources = kMaxSources);
};
} // namespace corevideo::core
