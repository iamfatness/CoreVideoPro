#pragma once

#include "modules/Interfaces.h"
#include "rpc/Json.h"
#include "graphics/GraphicsTimeline.h"
#include "graphics/GraphicsSourceCatalog.h"
#include "graphics/GraphicsTemplateRegistry.h"
#include "graphics/GraphicsPlacement.h"
#include "graphics/GraphicsClock.h"
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
  void observeZoomSources(const std::vector<modules::ZoomEngineParticipant>& roster, uint64_t sessionEpoch,
                          const std::vector<modules::VideoFrame>& frames = {});
  const rpc::Json& snapshot() const { return snapshot_; }
  bool hasCue() const;
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
    rpc::Json templateFields;
    std::optional<GraphicsPlacement> placement;
    bool fieldsReady = false;
  };
  struct Instance {
    std::string id, templateId, scope, sceneId;
    std::shared_ptr<const Revision> draft, cue, pendingCue, program, clockProgramPending;
    std::shared_ptr<GraphicsClockController> clock;
    uint64_t clockConfigurationRevision=1;
    rpc::Json clockRequestedConfiguration;
    std::shared_ptr<const rpc::Json> clockSample;
    std::shared_ptr<const GraphicsPreparedResource> draftResource, cueResource, programResource;
    std::string cuePreparation = "unprepared", cueWarning;
    bool cued = false, showPending = false, updatePending = false;
    bool awaitingEntrance = false, currentlyComposed = false;
    uint64_t readyRevision = 0;
    uint64_t cueRequestedRevision = 0;
    int64_t firstDrawSubmittedFrame = 0;
    std::string preparation = "unprepared", warning;
    GraphicsTimeline timeline;
    std::string visibility = "hidden";
  };
  static bool validId(const std::string& value);
  static bool readRevision(const rpc::Json& request, Revision& revision);
  static void hashRevision(const std::string& templateId, Revision& revision);
  modules::CompositorRenderPlanLayer layer(const Instance&, const Revision&, int width, int height) const;
  bool finish(const std::string& requestId, bool accepted, const std::string& reason);
  void publish();
  void show(Instance& instance, bool fromCue = false);
  bool updateTemplates();
  bool advanceAnimations();
  bool updateClocks();
  std::function<double()> monotonicMs_;
  std::map<std::string, Instance> instances_;
  struct Operation { std::string clientId, requestId, fingerprint; rpc::Json result; };
  std::deque<Operation> operations_;
  std::string epoch_, pendingClient_, pendingFingerprint_, pendingInstance_;
  std::optional<GraphicsSourceCatalog> sources_;
  uint64_t revisionClock_ = 0;
  rpc::Json lastResult_;
  GraphicsTemplateRegistry templates_;
  std::shared_ptr<const GraphicsTemplateRegistry::Publication> templatesPublication_;
  rpc::Json snapshot_ = rpc::Json::Object{{"schemaVersion", 1}, {"instances", rpc::Json::Array{}}};
};
}  // namespace corevideo::graphics
