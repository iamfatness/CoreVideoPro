#pragma once

#include "modules/Interfaces.h"
#include "rpc/Json.h"
#include "compositor/ColorGradeParams.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>

namespace corevideo::core {

// Control facts only. Pixels and device ownership belong to the isolated GPU
// monitor worker. A lost UI close cannot leave unbounded monitor demand alive.
class GradePreviewController {
 public:
  using Clock = std::chrono::steady_clock;
  static constexpr size_t Capacity = 3;
  static constexpr auto Lease = std::chrono::seconds(3);
  static constexpr auto Interval = std::chrono::milliseconds(100);

  bool configure(const rpc::Json& command, modules::CompositorColorGrade grade,
                 bool supported, Clock::time_point now = Clock::now()) {
    const auto id = command.getString("instanceId"), source = command.getString("sourceId");
    const auto number = command.getNumber("revision", -1);
    if (id.empty() || id.size() > 64 || source.empty() || source.size() > 256 ||
        !std::isfinite(number) || number < 0 || number > 2147483647 || std::floor(number) != number) return false;
    modules::GradePreviewDemand demand{id, source, static_cast<int64_t>(number), std::move(grade)};
    const bool enabled = !command.get("enabled") || command.get("enabled")->asBool();
    if (!enabled) { active_.erase(id); return true; }
    if (!supported) { emit({demand, 0, 0, 0, {}, "unavailable", "native-grade-preview-not-built"}); return true; }
    auto found = active_.find(id);
    if (found == active_.end()) {
      if (active_.size() >= Capacity) {
        emit({demand, 0, 0, 0, {}, "unavailable", "grade-preview-capacity"}); return true;
      }
      active_.emplace(id, Entry{std::move(demand), now});
    } else {
      if (demand.revision < found->second.demand.revision) return true;
      // Equal revision is a lease refresh, never permission to replace a draft.
      if (demand.revision == found->second.demand.revision && (source != found->second.demand.sourceId ||
          !modules::colorGradesEqual(demand.grade, found->second.demand.grade))) return false;
      if (demand.revision > found->second.demand.revision) found->second.demand = std::move(demand);
      found->second.renewed = now;
    }
    return true;
  }

  void tick(const std::vector<modules::VideoFrame>& frames, modules::ICompositor& compositor,
            int64_t sequence, Clock::time_point now = Clock::now()) {
    if (now - lastTick_ < Interval) return;
    lastTick_ = now;
    for (auto it = active_.begin(); it != active_.end();) {
      if (now - it->second.renewed >= Lease) it = active_.erase(it); else ++it;
    }
    if (active_.empty() && !submitted_) return;
    modules::MonitorRenderRequest request;
    request.sequence = sequence;
    for (const auto& [id, entry] : active_) request.gradePreviews.push_back(entry.demand);
    for (const auto& frame : frames) {
      if (std::any_of(request.gradePreviews.begin(), request.gradePreviews.end(),
          [&](const auto& demand) { return demand.sourceId == frame.participantId; })) request.frames.push_back(frame);
    }
    compositor.submitGradePreviews(std::move(request));
    submitted_ = !active_.empty();
    const auto result = compositor.latestGradePreviews();
    const auto health = compositor.gradePreviewDiagnostics();
    for (auto& item : active_) {
      const auto& id = item.first;
      auto& entry = item.second;
      const auto match = result ? std::find_if(result->gradePreviews.begin(), result->gradePreviews.end(),
          [&](const auto& surface) { return surface.demand.instanceId == id &&
              surface.demand.sourceId == entry.demand.sourceId && surface.demand.revision == entry.demand.revision; })
          : std::vector<modules::GradePreviewSurface>::const_iterator{};
      if (!result || match == result->gradePreviews.end()) {
        emit({entry.demand, 0, 0, 0, {}, health.failed ? "unavailable" : "preparing",
              health.failed ? health.failureReason : "awaiting-native-grade"});
        continue;
      }
      auto surface = *match;
      if (surface.status == "ready") {
        if (!entry.observed || entry.epoch != surface.sourceEpoch || entry.frame != surface.sourceFrameId) {
          entry.observed = true; entry.epoch = surface.sourceEpoch; entry.frame = surface.sourceFrameId;
          entry.advanced = now;
        } else if (now - entry.advanced >= std::chrono::seconds(1)) {
          surface.status = "stale"; surface.reason = "source-not-advancing";
        }
      }
      emit(surface);
    }
  }
  std::vector<rpc::Json> drain() { auto result = std::move(events_); events_.clear(); return result; }
  rpc::Json diagnostics(const modules::ICompositor& compositor) const {
    const auto worker = compositor.gradePreviewDiagnostics();
    return rpc::Json::Object{{"observationVersion", "grade-preview-v1"},
      {"supported", compositor.supportsGradePreview()}, {"activeEditors", double(active_.size())},
      {"editorCapacity", double(Capacity)}, {"leaseMs", 3000}, {"requestedIntervalMs", 100},
      {"submitted", double(worker.submitted)}, {"completed", double(worker.completed)},
      {"superseded", double(worker.superseded)}, {"failed", double(worker.failed)},
      {"pending", worker.pending}, {"pendingCapacity", 1}, {"lastWorkMs", worker.lastWorkMs},
      {"retainedInputs", double(worker.retainedInputs)}, {"retainedInputBytes", double(worker.retainedInputBytes)},
      {"failureReason", worker.failureReason}, {"displayPresentationVerified", false}};
  }
  size_t size() const { return active_.size(); }
 private:
  struct Entry {
    modules::GradePreviewDemand demand;
    Clock::time_point renewed, advanced{};
    uint64_t epoch = 0;
    int64_t frame = 0;
    bool observed = false;
  };
  void emit(const modules::GradePreviewSurface& surface) {
    if (events_.size() >= 16) events_.erase(events_.begin());
    const auto& t = surface.texture;
    events_.emplace_back(rpc::Json::Object{
      {"type", "grade-preview"}, {"instanceId", surface.demand.instanceId},
      {"sourceId", surface.demand.sourceId}, {"revision", double(surface.demand.revision)},
      {"sourceEpoch", double(surface.sourceEpoch)}, {"sourceFrameId", double(surface.sourceFrameId)},
      {"captureTimestamp100ns", double(surface.captureTimestamp100ns)},
      {"status", surface.status}, {"reason", surface.reason},
      {"texture", rpc::Json::Object{{"sharedHandleHex", t.sharedHandleHex}, {"width", t.width},
          {"height", t.height}, {"format", t.format}, {"frameNumber", double(t.frameNumber)}}}});
  }
  std::map<std::string, Entry> active_;
  std::vector<rpc::Json> events_;
  Clock::time_point lastTick_{};
  bool submitted_ = false;
};
}
