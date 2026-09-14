#pragma once
#include "core/ShowPlanGenerator.h"

namespace corevideo::core {
// Construction admits both bus leases atomically from the store. Evidence is
// historical projection, never proof of application, rendering or delivery.
class SceneVersionShadowEvidence final {
 public:
  const std::shared_ptr<const ShowPlans> plans;
  static std::shared_ptr<const SceneVersionShadowEvidence> capture(
      const SceneVersionStore&, const ShowStateSnapshot&, const SourceRegistry::Snapshot&,
      ShowPlanGenerationContext, std::optional<SceneVersionRef> program,
      std::optional<SceneVersionRef> preview);
 private:
  explicit SceneVersionShadowEvidence(std::shared_ptr<const ShowPlans> value) : plans(std::move(value)) {}
};
}
