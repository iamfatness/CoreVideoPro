#pragma once

#include "modules/Interfaces.h"
#include "rpc/Json.h"
#include "graphics/GraphicsTimeline.h"
#include "graphics/GraphicsSourceCatalog.h"
#include "graphics/GraphicsTemplateRegistry.h"
#include "graphics/GraphicsPlacement.h"
#include "graphics/GraphicsClock.h"
#include "graphics/GraphicsAutomation.h"
#include <functional>
#include <map>
#include <deque>
#include <memory>
#include <string>

namespace corevideo::graphics {
class GraphicsTemplate;

// Serialized by the core control owner. Render plans contain values/immutable
// revisions only; raster workers never mutate this state. Compatibility looks
// are the first consumer; unsupported template families are refused explicitly.
class GraphicsInstances {
 public:
  enum class ScopePass { All, Scene, Global };
  explicit GraphicsInstances(std::string epoch = {}, std::function<double()> monotonicMs = {});
  bool apply(const rpc::Json& command);
  void append(modules::CompositorRenderPlan& plan, bool preview,
              const std::vector<modules::VideoFrame>& frames = {}, ScopePass scope = ScopePass::All) const;
  void observe(const modules::ProgramFrame& frame);
  void observeSources(const std::vector<modules::ZoomEngineParticipant>& roster, uint64_t sessionEpoch,
                          const std::vector<modules::VideoFrame>& frames = {},
                          const std::function<std::string_view(const std::string&)>& captureLabel = {});
  const rpc::Json& snapshot() const { return snapshot_; }
  bool hasCue() const;
  // Take: every cued instance whose cue is ready and opted in (rideTake) goes
  // to Program from its cue and leaves Preview. Rehearsal cues stay put.
  void promoteCuedForTake();
  // Private monitor input, not an instance mutation, cue or Program revision.
  std::optional<modules::CompositorOverlayContent> authoringPreview(const rpc::Json& draft);

 private:
  struct Revision {
    uint64_t number = 1;
    std::string name, title, sourceId;
    compositor::LowerThirdAppearance appearance;
    GraphicsTransition entrance, exit;
    std::optional<contracts::SourceInstanceIdentity> binding;
    std::array<uint8_t, 32> contentHash{};
    std::shared_ptr<const GraphicsTemplate> definition;
    std::shared_ptr<const GraphicsTemplateInput> templateInput;
    rpc::Json templateFields,imageReference;
    std::shared_ptr<const GraphicsAsset> image;
    std::optional<GraphicsPlacement> placement;
    bool fieldsReady = false;
  };
  struct Instance {
    std::string id, templateId, scope, sceneId, label;
    int rackOrder=0;
    std::shared_ptr<const Revision> draft, cue, pendingCue, program, clockProgramPending;
    std::shared_ptr<GraphicsClockController> clock;
    uint64_t clockConfigurationRevision=1;
    rpc::Json clockRequestedConfiguration;
    std::shared_ptr<const rpc::Json> clockSample;
    std::shared_ptr<const GraphicsPreparedResource> draftResource, cueResource, programResource;
    std::string cuePreparation = "unprepared", cueWarning;
    bool cued = false, showPending = false, updatePending = false;
    bool rideTake = true;
    bool awaitingEntrance = false, currentlyComposed = false;
    uint64_t readyRevision = 0;
    uint64_t cueRequestedRevision = 0;
    int64_t firstDrawSubmittedFrame = 0;
    std::string preparation = "unprepared", warning;
    GraphicsTimeline timeline;
    std::string visibility = "hidden";
  };
  static bool validId(const std::string& value);
  std::vector<const Instance*> orderedInstances() const;
  bool revisionCapacity(const std::string& instanceId,const Revision&) const;
  bool nodeCapacity(const std::string& instanceId,const Revision&) const;
  bool readRevision(const rpc::Json& request, Revision& revision);
  static void hashRevision(const std::string& templateId, Revision& revision);
  modules::CompositorRenderPlanLayer layer(const Instance&, const Revision&, int width, int height) const;
  bool finish(const std::string& requestId, bool accepted, const std::string& reason);
  void publish();
  void show(Instance& instance, bool fromCue = false);
  static void clearCue(Instance& instance);
  bool updateTemplates();
  bool advanceAnimations();
  bool updateClocks();
  bool updateAuthoring();
  bool applyAutomation(const modules::ProgramFrame&);
  GraphicsAutomation automation_;
  bool automationApplying_=false;
  uint64_t automationSequence_=0;
  std::function<double()> monotonicMs_;
  std::map<std::string, Instance> instances_;
  struct Operation { std::string clientId, requestId, fingerprint; rpc::Json result; double admittedMs=0; };
  std::deque<Operation> operations_;
  std::string epoch_, pendingClient_, pendingFingerprint_, pendingInstance_;
  std::set<std::string> managedFamilies_;
  std::optional<GraphicsSourceCatalog> sources_;
  uint64_t revisionClock_ = 0;
  rpc::Json lastResult_;
  GraphicsTemplateRegistry templates_;
  std::shared_ptr<GraphicsTemplateDraft> authoringCompiler_;
  std::shared_ptr<const void> authoringPublication_;
  std::string authoringKey_;
  rpc::Json authoringInput_,authoringState_;
  std::shared_ptr<const GraphicsTemplateRegistry::Publication> templatesPublication_;
  rpc::Json snapshot_ = rpc::Json::Object{{"schemaVersion", 1}, {"instances", rpc::Json::Array{}}};
};
}  // namespace corevideo::graphics
