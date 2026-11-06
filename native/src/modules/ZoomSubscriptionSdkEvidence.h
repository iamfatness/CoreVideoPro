#pragma once

#include "modules/ZoomEngineClient.h"
#include "rpc/Json.h"
#include <deque>
#include <utility>

namespace corevideo::modules {

// Low-rate subscription responses only; never raw SDK payloads, auth, names, or frames.
// Observation does not alter subscription policy or label a request as delivered geometry.
class ZoomSubscriptionSdkEvidence {
 public:
  static constexpr std::size_t kCapacity = 64;

  void observe(const ZoomEngineEvent& event, std::uint64_t monotonicMs) {
    if (event.kind != ZoomEngineEventKind::Debug ||
        (event.stage != "set_resolution" && event.stage != "video_subscribe" &&
         event.stage != "video_resolution_downgraded" &&
         event.stage != "video_subscribe_failed_all" && event.stage != "create_renderer_failed")) return;
    rpc::Json::Object row{
        {"sequence", static_cast<double>(++sequence_)},
        {"observedMonotonicMs", static_cast<double>(monotonicMs)},
        {"stage", event.stage},
        {"participantId", std::to_string(event.participantId)},
        {"sourceUuid", event.sourceUuid.substr(0, 256)},
    };
    const auto add = [&](const char* key, std::optional<int> value) {
      if (value) row.emplace(key, *value);
    };
    add("sdkCode", event.sdkCode);
    add("resolution", event.resolution);
    add("requestedResolution", event.requestedResolution);
    add("actualResolution", event.actualResolution);
    if (events_.size() == kCapacity) { events_.pop_front(); ++dropped_; }
    events_.emplace_back(std::move(row));
  }

  void reset() { events_.clear(); sequence_ = 0; dropped_ = 0; }

  [[nodiscard]] rpc::Json json() const {
    return rpc::Json::Object{
        {"capacity", static_cast<double>(kCapacity)},
        {"dropped", static_cast<double>(dropped_)},
        {"events", rpc::Json::Array(events_.begin(), events_.end())},
    };
  }

 private:
  std::deque<rpc::Json> events_;
  std::uint64_t sequence_ = 0;
  std::uint64_t dropped_ = 0;
};
}  // namespace corevideo::modules
