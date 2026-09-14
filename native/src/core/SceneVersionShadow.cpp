#include "core/SceneVersionShadow.h"

namespace corevideo::core {
std::shared_ptr<const SceneVersionShadowEvidence> SceneVersionShadowEvidence::capture(
    const SceneVersionStore& store, const ShowStateSnapshot& show,
    const SourceRegistry::Snapshot& registry, ShowPlanGenerationContext context,
    std::optional<SceneVersionRef> program, std::optional<SceneVersionRef> preview) {
  const auto [pgm, pvw] = store.resolveBuses(program, preview);
  // No partial evidence: preparation cannot succeed using only one valid bus.
  if (pgm.status != SceneVersionStore::Status::Unchanged || pvw.status != SceneVersionStore::Status::Unchanged ||
      (program && program->authorityEpoch != show.authorityEpoch) ||
      (preview && preview->authorityEpoch != show.authorityEpoch)) return {};
  auto plans = generateShowPlans(show, registry, context,
      VersionedSceneBindings{{program, pgm.lease}, {preview, pvw.lease}});
  return std::shared_ptr<const SceneVersionShadowEvidence>(new SceneVersionShadowEvidence(std::move(plans)));
}
}
