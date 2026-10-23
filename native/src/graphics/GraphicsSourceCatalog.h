#pragma once

#include "contracts/Lifecycle.h"
#include "modules/ZoomEngineClient.h"
#include "modules/Interfaces.h"
#include <map>
#include <optional>
#include <algorithm>
#include <functional>
#include <string_view>

namespace corevideo::graphics {
// A binding projection of authoritative Zoom and source-bus incarnations, not another participant
// lifecycle owner. Pixel sourceEpoch is deliberately absent: it also changes
// on resolution upgrades within one participant incarnation.
class GraphicsSourceCatalog {
 public:
  explicit GraphicsSourceCatalog(std::string coreEpoch) : coreEpoch_(std::move(coreEpoch)) {}
  bool observeSources(const std::vector<modules::ZoomEngineParticipant>& roster, uint64_t sessionEpoch,
                   const std::vector<modules::VideoFrame>& frames,
                   const std::function<std::string_view(const std::string&)>& captureLabel = {}) {
    bool changed = false;
    if (sessionEpoch_ != sessionEpoch) {
      sessionEpoch_ = sessionEpoch; eraseZoom(); changed = true;
      processEpoch_ = coreEpoch_ + ":zoom:" + std::to_string(sessionEpoch);
    }
    std::string status = sessionEpoch ? "available" : "unavailable";
    ++sweep_;
    for (const auto& participant : roster) {
      if (!sessionEpoch) break;
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
        eraseZoom(); status_ = "ambiguous-roster"; return publish(true);
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
    // CaptureDeviceSource stamps its authoritative video lifetime on frame refs.
    // Neither a device label nor a pixel-format epoch can qualify a binding.
    for (const auto& frame : frames) {
      if (frame.participantId.rfind("capture:", 0) != 0 || frame.participantId.size() > 512 ||
          !frame.hasContent() || frame.sourceGeneration == 0 || frame.sourceGeneration > 9007199254740991ULL) continue;
      const auto& id = frame.participantId;
      auto found = entries_.find(id);
      if (found == entries_.end()) {
        if (entries_.size() >= 256) { status = "source-catalog-capacity"; continue; }
        found = entries_.emplace(id, Entry{}).first; changed = true;
      }
      auto& entry = found->second;
      if (entry.seen == sweep_ && entry.identity.generation != int64_t(frame.sourceGeneration)) {
        // Conflicting representations must not select an arbitrary subject.
        entry.identity.generation = 0; status = "ambiguous-source-generation"; changed = true; continue;
      }
      if (entry.identity.generation != int64_t(frame.sourceGeneration)) {
        entry.identity = {id, id + "-generation-" + std::to_string(frame.sourceGeneration),
                          coreEpoch_ + ":capture", int64_t(frame.sourceGeneration)};
        changed = true;
      }
      // Native accepted source policy owns the stable device/route label; an
      // obsolete incarnation-specific person override is never reused here.
      auto name = captureLabel ? captureLabel(id) : std::string_view(id);
      if (name.empty() || name.size() > 4096) name = id;
      if (entry.displayName != name || !entry.videoOn) { entry.displayName = name; entry.videoOn = true; changed = true; }
      entry.seen = sweep_; status = "available";
    }
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (it->second.seen != sweep_) { it = entries_.erase(it); changed = true; }
      else ++it;
    }
    if (status_ != status) { status_ = status; changed = true; }
    return publish(changed);
  }

  static std::string frameId(const std::string& sourceId) {
    return sourceId.rfind("zoom:", 0) == 0 ? sourceId.substr(5) : sourceId;
  }
  static std::string sourceId(const std::string& frameId) {
    return frameId.rfind("capture:", 0) == 0 ? frameId : "zoom:" + frameId;
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
  void eraseZoom() {
    for (auto it = entries_.begin(); it != entries_.end();)
      if (it->first.rfind("zoom:", 0) == 0) it = entries_.erase(it); else ++it;
  }
  bool publish(bool changed) {
    if (!changed) return false;
    rpc::Json::Array rows;
    for (const auto& [_, entry] : entries_)
      if (entry.identity.generation > 0) rows.emplace_back(rpc::Json::Object{{"identity", contracts::toJson(entry.identity)},
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
