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
  bool renew(const rpc::Json& command, Clock::time_point now = Clock::now()) {
    const auto found = active_.find(command.getString("instanceId"));
    if (found == active_.end() || found->second.demand.sourceId != command.getString("sourceId") ||
        command.getNumber("revision", -1) != found->second.demand.revision || now-found->second.renewed >= Lease) return false;
    found->second.renewed = now; return true;
  }

  bool configure(const rpc::Json& command, modules::CompositorColorGrade grade,
                 bool supported, Clock::time_point now = Clock::now()) {
    const auto id = command.getString("instanceId"), source = command.getString("sourceId");
    const auto number = command.getNumber("revision", -1);
    if (id.empty() || id.size() > 64 || source.empty() || source.size() > 256 ||
        !std::isfinite(number) || number < 0 || number > 2147483647 || std::floor(number) != number) return false;
    modules::GradePreviewDemand demand{id, source, static_cast<int64_t>(number), std::move(grade)};
    demand.scopesEnabled = command.get("scopesEnabled") && command.get("scopesEnabled")->asBool();
    demand.scopesOriginal = command.get("scopesOriginal") && command.get("scopesOriginal")->asBool();
    demand.compareOriginal = command.get("compareOriginal") && command.get("compareOriginal")->asBool();
    const double hm = command.getNumber("histogramMode", 1), wm = command.getNumber("waveformMode", 1), sv = command.getNumber("scopeView", 0);
    if (!std::isfinite(hm) || hm < 0 || hm > 1 || std::floor(hm) != hm ||
        !std::isfinite(wm) || wm < 0 || wm > 2 || std::floor(wm) != wm || !std::isfinite(sv) || sv<0 || sv>3 || std::floor(sv)!=sv) return false;
    demand.histogramMode = int(hm); demand.waveformMode = int(wm); demand.scopeView = int(sv);
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
          !modules::colorGradesEqual(demand.grade, found->second.demand.grade) ||
          demand.scopesEnabled != found->second.demand.scopesEnabled || demand.scopesOriginal != found->second.demand.scopesOriginal || demand.compareOriginal != found->second.demand.compareOriginal ||
          demand.histogramMode != found->second.demand.histogramMode || demand.waveformMode != found->second.demand.waveformMode || demand.scopeView != found->second.demand.scopeView)) return false;
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
      if (surface.scopes.texture.width > 0) {
        if (entry.scopeExport != surface.scopes.texture.frameNumber) {
          entry.scopeExport = surface.scopes.texture.frameNumber;
          entry.scopeCompletionObservedAtMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        }
        if (!entry.scopeObserved || entry.scopeEpoch != surface.scopes.sourceEpoch || entry.scopeFrame != surface.scopes.sourceFrameId) {
          entry.scopeObserved = true; entry.scopeEpoch = surface.scopes.sourceEpoch; entry.scopeFrame = surface.scopes.sourceFrameId; entry.scopeAdvanced = now;
        } else if (now-entry.scopeAdvanced >= std::chrono::seconds(1)) surface.scopes.status = "stale";
      }
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
    bool scopeObserved = false;
    uint64_t scopeEpoch = 0;
    int64_t scopeFrame = 0, scopeExport = -1, scopeCompletionObservedAtMs = 0;
    Clock::time_point scopeAdvanced{};
  };
  void emit(const modules::GradePreviewSurface& surface) {
    if (events_.size() >= 16) events_.erase(events_.begin());
    const auto& t = surface.texture;
    const auto& scope = surface.scopes; const auto& st = scope.texture;
    const auto found = active_.find(surface.demand.instanceId);
    const auto now = Clock::now();
    const auto age = found!=active_.end() && found->second.observed ? double(std::chrono::duration_cast<std::chrono::milliseconds>(now-found->second.advanced).count()) : 0;
    const auto scopeAge = found!=active_.end() && found->second.scopeObserved ? double(std::chrono::duration_cast<std::chrono::milliseconds>(now-found->second.scopeAdvanced).count()) : 0;
    events_.emplace_back(rpc::Json::Object{
      {"type", "grade-preview"}, {"instanceId", surface.demand.instanceId},
      {"sourceId", surface.demand.sourceId}, {"revision", double(surface.demand.revision)},
      {"sourceEpoch", double(surface.sourceEpoch)}, {"sourceFrameId", double(surface.sourceFrameId)},
      {"captureTimestamp100ns", double(surface.captureTimestamp100ns)},
      {"sourceAgeMs", std::max(0.0,age)},
      {"status", surface.status}, {"reason", surface.reason},
      {"scopes", rpc::Json::Object{{"status", scope.status},
        {"reason", scope.status=="unavailable"?"scope-render-or-export-unavailable":scope.status=="stale"?"source-not-advancing":scope.status=="held"?"source-unavailable":st.width==0?"awaiting-native-scopes":""}, {"sourceAgeMs", std::max(0.0,scopeAge)},
        {"completionObservedAtUnixMs", st.width>0 && found!=active_.end()?double(found->second.scopeCompletionObservedAtMs):0},
        {"view", surface.demand.scopeView}, {"revision", double(scope.revision)}, {"sourceEpoch", double(scope.sourceEpoch)}, {"sourceFrameId", double(scope.sourceFrameId)},
        {"captureTimestamp100ns", double(scope.captureTimestamp100ns)}, {"original", scope.original},
        {"sampleWidth", st.width>0?256:0}, {"sampleHeight", st.width>0?144:0}, {"sampleCount", st.width>0?36864:0}, {"colorSpace", "rec709-sdr-assumed"},
        {"units", "encoded 0-1; waveform 0-100%; Cb/Cr"},
        {"texture", rpc::Json::Object{{"sharedHandleHex", st.sharedHandleHex}, {"width", st.width}, {"height", st.height},
          {"format", st.format}, {"frameNumber", double(st.frameNumber)}}}}},
      {"texture", rpc::Json::Object{{"sharedHandleHex", t.sharedHandleHex}, {"width", t.width},
          {"height", t.height}, {"format", t.format}, {"frameNumber", double(t.frameNumber)}}}});
  }
  std::map<std::string, Entry> active_;
  std::vector<rpc::Json> events_;
  Clock::time_point lastTick_{};
  bool submitted_ = false;
};
}
