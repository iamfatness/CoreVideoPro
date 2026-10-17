#pragma once

#include "contracts/Lifecycle.h"
#include "modules/ZoomEngineClient.h"
#include "modules/Interfaces.h"
#include <map>
#include <optional>
#include <algorithm>

namespace corevideo::graphics {
// A binding projection of the authoritative roster, not another participant
// lifecycle owner. Pixel sourceEpoch is deliberately absent: it also changes
// on resolution upgrades within one participant incarnation.
class GraphicsSourceCatalog {
 public:
  explicit GraphicsSourceCatalog(std::string coreEpoch) : coreEpoch_(std::move(coreEpoch)) {}
  bool observeZoom(const std::vector<modules::ZoomEngineParticipant>& roster, uint64_t sessionEpoch,
                   const std::vector<modules::VideoFrame>& frames) {
    bool changed = false;
    if (sessionEpoch_ != sessionEpoch) {
      sessionEpoch_ = sessionEpoch; entries_.clear(); changed = true;
      processEpoch_ = coreEpoch_ + ":zoom:" + std::to_string(sessionEpoch);
    }
    std::string status = sessionEpoch ? "available" : "unavailable";
    if (!sessionEpoch) {
      if (!entries_.empty()) { entries_.clear(); changed = true; }
      if (status_ != status) { status_ = status; changed = true; }
      return publish(changed);
    }
    ++sweep_;
    for (const auto& participant : roster) {
      if (!participant.id) continue;
      if (participant.sourceGeneration == 0 || participant.sourceGeneration > 9007199254740991ULL || participant.displayName.size() > 4096) {
        status = "unqualified-source-identity";
        continue;
      }
      // Graphics binds configured video sources, not every attendee. A roster
      // alone does not prove that a source with this incarnation has reached
      // the media spine. Metadata-only frames can qualify identity; actual
      // composition additionally requires pixels from the same generation.
      const auto rawId = std::to_string(participant.id);
      if (std::none_of(frames.begin(), frames.end(), [&](const auto& frame) {
        return frame.participantId == rawId && frame.sourceGeneration == participant.sourceGeneration;
      })) continue;
      const auto id = "zoom:" + std::to_string(participant.id);
      auto found = entries_.find(id);
      if (found != entries_.end() && found->second.seen == sweep_) {
        entries_.clear(); status_ = "ambiguous-roster"; return publish(true);
      }
      if (found == entries_.end()) {
        if (entries_.size() >= 256) { status = "source-catalog-capacity"; continue; }
        found = entries_.emplace(id, Entry{}).first; changed = true;
      }
      auto& entry = found->second;
      if (entry.identity.generation > static_cast<int64_t>(participant.sourceGeneration)) {
        entry.seen = sweep_; status = "stale-source-generation"; continue;
      }
      if (entry.identity.generation != static_cast<int64_t>(participant.sourceGeneration)) {
        entry.identity = {id, "zoom-" + std::to_string(participant.id) + "-generation-" + std::to_string(participant.sourceGeneration),
                          processEpoch_, static_cast<int64_t>(participant.sourceGeneration)};
        changed = true;
      }
      // Metadata remains metadata. Never infer subscription, frame delivery,
      // persistent-person identity or readiness from these roster properties.
      if (entry.displayName != participant.displayName || entry.videoOn != participant.hasVideo) {
        entry.displayName = participant.displayName; entry.videoOn = participant.hasVideo; changed = true;
      }
      entry.seen = sweep_;
    }
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (it->second.seen != sweep_) { it = entries_.erase(it); changed = true; }
      else ++it;
    }
    if (status_ != status) { status_ = status; changed = true; }
    return publish(changed);
  }

  static bool same(const contracts::SourceInstanceIdentity& a, const contracts::SourceInstanceIdentity& b) {
    return a.sourceId == b.sourceId && a.instanceId == b.instanceId && a.processEpoch == b.processEpoch && a.generation == b.generation;
  }
  bool contains(const contracts::SourceInstanceIdentity& identity) const {
    const auto found = entries_.find(identity.sourceId);
    return found != entries_.end() && same(found->second.identity, identity);
  }
  static std::optional<contracts::SourceInstanceIdentity> read(const rpc::Json& value) {
    if (!contracts::validateSourceInstanceIdentity(value)) return {};
    for (const auto& [key, item] : value.asObject()) {
      if (key != "sourceId" && key != "instanceId" && key != "processEpoch" && key != "generation") return {};
      if (item.isString() && item.asString().size() > 512) return {};
    }
    return contracts::SourceInstanceIdentity{value.getString("sourceId"), value.getString("instanceId"),
        value.getString("processEpoch"), static_cast<int64_t>(value.getNumber("generation"))};
  }
  const rpc::Json& rows() const { return rows_; }
  const std::string& status() const { return status_; }
 private:
  bool publish(bool changed) {
    if (!changed) return false;
    rpc::Json::Array rows;
    for (const auto& [_, entry] : entries_)
      rows.emplace_back(rpc::Json::Object{{"identity", contracts::toJson(entry.identity)},
          {"displayName", entry.displayName}, {"videoOn", entry.videoOn}});
    rows_ = std::move(rows);
    return true;
  }
  struct Entry {
    contracts::SourceInstanceIdentity identity;
    std::string displayName;
    bool videoOn = false;
    uint64_t seen = 0;
  };
  std::string coreEpoch_, processEpoch_, status_ = "unavailable";
  uint64_t sessionEpoch_ = 0, sweep_ = 0;
  std::map<std::string, Entry> entries_;
  rpc::Json rows_ = rpc::Json::Array{};
};
} // namespace corevideo::graphics
