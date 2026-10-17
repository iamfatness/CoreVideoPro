#pragma once

#include "modules/Interfaces.h"
#include "rpc/Json.h"
#include "graphics/GraphicsTimeline.h"
#include <functional>
#include <map>
#include <deque>
#include <memory>
#include <string>

namespace corevideo::graphics {

// Serialized by the core control owner. Render plans contain values/immutable
// revisions only; raster workers never mutate this state. Compatibility looks
// are the first consumer; unsupported template families are refused explicitly.
class GraphicsInstances {
 public:
  explicit GraphicsInstances(std::string epoch = {}, std::function<double()> monotonicMs = {});
  bool apply(const rpc::Json& command);
  void append(modules::CompositorRenderPlan& plan, bool preview) const;
  void observe(const modules::ProgramFrame& frame);
  const rpc::Json& snapshot() const { return snapshot_; }
  bool hasCue() const;

 private:
  struct Revision {
    uint64_t number = 1;
    std::string name, title, sourceId;
    compositor::LowerThirdAppearance appearance;
    GraphicsTransition entrance, exit;
  };
  struct Instance {
    std::string id, templateId, scope, sceneId;
    std::shared_ptr<const Revision> draft, program;
    bool cued = false, showPending = false, updatePending = false;
    uint64_t readyRevision = 0;
    int64_t firstDrawSubmittedFrame = 0;
    std::string preparation = "unprepared", warning;
    GraphicsTimeline timeline;
    std::string visibility = "hidden";
  };
  static bool validId(const std::string& value);
  static bool readRevision(const rpc::Json& request, Revision& revision);
  modules::CompositorRenderPlanLayer layer(const Instance&, const Revision&) const;
  bool finish(const std::string& requestId, bool accepted, const std::string& reason);
  void publish();
  void show(Instance& instance);
  bool advanceAnimations();
  std::function<double()> monotonicMs_;
  std::map<std::string, Instance> instances_;
  struct Operation { std::string clientId, requestId, fingerprint; rpc::Json result; };
  std::deque<Operation> operations_;
  std::string epoch_, pendingClient_, pendingFingerprint_;
  uint64_t revisionClock_ = 0;
  rpc::Json lastResult_;
  rpc::Json snapshot_ = rpc::Json::Object{{"schemaVersion", 1}, {"instances", rpc::Json::Array{}}};
};
}  // namespace corevideo::graphics
