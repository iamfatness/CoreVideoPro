#pragma once
#include "modules/Interfaces.h"
#include "rpc/Json.h"
#include <cmath>
#include <algorithm>
#include <array>
namespace corevideo::core {
// Small authoritative control state. Identity fences prevent a reused Zoom id
// from inheriting a grade, and revisions reject edits from another old editor.
class SourceGradeState {
 public:
  struct EventBatch {
    std::vector<rpc::Json> events;
    std::array<int64_t, 16> revisions{};
    std::vector<rpc::Json> assemble() && {
      for (size_t i = 0; i < events.size(); ++i) {
        auto object = events[i].asObject();
        object["appliedRevision"] = double(revisions[i]);
        events[i] = rpc::Json(std::move(object));
      }
      return std::move(events);
    }
  };
  EventBatch captureAnnotations(std::vector<rpc::Json> events) const {
    EventBatch batch;
    batch.events = std::move(events);
    // GradePreviewController has a hard sixteen-event capture bound.
    for (size_t i = 0; i < batch.events.size(); ++i)
      batch.revisions.at(i) = revision(batch.events[i].getString("sourceId"));
    return batch;
  }
  void observe(const std::vector<modules::VideoFrame>& frames) {
    epochs_.clear();
    for(const auto& frame:frames) if(epochs_.size()<256) epochs_[frame.participantId]=frame.sourceEpoch;
  }
  rpc::Json apply(const rpc::Json& command, modules::CompositorColorGrade grade) {
    const auto id=command.getString("sourceId"); const auto epoch=command.getNumber("sourceEpoch",-1);
    const auto expected=command.getNumber("expectedRevision",-1);
    if(id.empty() || id.size()>256 || !std::isfinite(epoch) || epoch<0 || epoch>9007199254740991.0 || std::floor(epoch)!=epoch ||
       !std::isfinite(expected) || expected<0 || expected>2147483647 || std::floor(expected)!=expected)
      return rpc::Json::Object{{"accepted",false},{"reason","invalid-source-grade"}};
    const auto live=epochs_.find(id);
    if(live==epochs_.end() || live->second!=uint64_t(epoch)) return rpc::Json::Object{{"accepted",false},{"reason","source-identity-changed"}};
    const auto current=grades_.find(id); const int64_t revision=current==grades_.end()?0:current->second.revision;
    if(expected!=revision) return rpc::Json::Object{{"accepted",false},{"reason","grade-revision-conflict"},{"revision",double(revision)}};
    if(current==grades_.end() && grades_.size()>=256) return rpc::Json::Object{{"accepted",false},{"reason","grade-capacity"}};
    grades_[id]={uint64_t(epoch),revision+1,std::move(grade)};
    return rpc::Json::Object{{"accepted",true},{"revision",double(revision+1)},{"sourceEpoch",epoch}};
  }
  void applyTo(modules::CompositorRenderPlan& plan,const std::vector<modules::VideoFrame>& frames) const {
    for(const auto& frame:frames) {const auto entry=grades_.find(frame.participantId); if(entry!=grades_.end())
      plan.sourceGrades[frame.participantId]=entry->second.epoch==frame.sourceEpoch ? entry->second.grade : modules::CompositorColorGrade{};}
    for(auto& layer:plan.layers) {
      const auto& id=layer.participantId.empty()?layer.sourceId:layer.participantId;
      const auto entry=grades_.find(id); if(entry==grades_.end()) continue;
      const auto frame=std::find_if(frames.begin(),frames.end(),[&](const auto& f){return f.participantId==id;});
      layer.hasColorGrade=true;
      layer.colorGrade=frame!=frames.end() && frame->sourceEpoch==entry->second.epoch ? entry->second.grade : modules::CompositorColorGrade{};
    }
  }
  // G0 (grading plan Task 1): what the next apply needs, answered from the LAST observe().
  // `present` is map membership, never `sourceEpoch != 0`: Mac capture frames carry epoch 0.
  struct Query {
    bool present = false;      // the last observe() saw a frame keyed sourceId
    uint64_t sourceEpoch = 0;  // that frame's epoch; 0 when !present
    int64_t revision = 0;      // the accepted grade's revision; 0 when none
  };
  [[nodiscard]] Query query(const std::string& sourceId) const {
    Query q;
    const auto live = epochs_.find(sourceId);
    if (live != epochs_.end()) { q.present = true; q.sourceEpoch = live->second; }
    q.revision = revision(sourceId);
    return q;
  }
  struct Status {
    std::string sourceId;
    int64_t revision = 0;
    uint64_t appliedEpoch = 0;
    uint64_t liveEpoch = 0;     // 0 when !livePresent
    bool livePresent = false;   // the last observe() saw a frame for sourceId
    bool active = false;        // livePresent && liveEpoch == appliedEpoch: what applyTo paints
  };
  // One entry per ACCEPTED grade (never for an observed but ungraded source), in
  // sourceId order (the std::map's order). At most 256 entries (grade-capacity).
  [[nodiscard]] std::vector<Status> statuses() const {
    std::vector<Status> out;
    out.reserve(grades_.size());
    for (const auto& [id, applied] : grades_) {
      Status s;
      s.sourceId = id;
      s.revision = applied.revision;
      s.appliedEpoch = applied.epoch;
      const auto live = epochs_.find(id);
      s.livePresent = live != epochs_.end();
      s.liveEpoch = s.livePresent ? live->second : 0;
      s.active = s.livePresent && s.liveEpoch == s.appliedEpoch;
      out.push_back(std::move(s));
    }
    return out;
  }
  int64_t revision(const std::string& sourceId) const {const auto e=grades_.find(sourceId);return e==grades_.end()?0:e->second.revision;}
  std::vector<rpc::Json> annotate(std::vector<rpc::Json> events) const {
    return captureAnnotations(std::move(events)).assemble();
  }
 private:
  struct Applied {uint64_t epoch;int64_t revision;modules::CompositorColorGrade grade;};
  std::map<std::string,uint64_t> epochs_;
  std::map<std::string,Applied> grades_;
};
}
