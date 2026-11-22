// Sources Phase 2 plan, Task 6 (D4): the PVW tally on the Mac Sources rows comes from what
// the core rendered on Preview, not from the shell's own Preview routes. The core published
// only programFrame.videoSources; previewFrame.videoSources is its twin, published from the
// Preview plan the core actually built for a render, and emptied when no Preview scene is up.
#include "core/MediaCore.h"
#include "modules/Interfaces.h"
#include "rpc/Json.h"

#include <gtest/gtest.h>

#include <set>
#include <string>

namespace {
using corevideo::rpc::Json;

Json::Object route(const std::string& id, const std::string& participant) {
  return Json::Object{{"routeId", id}, {"mode", "fixed"}, {"participantId", participant},
                      {"rect", Json::Object{{"x", 0.0}, {"y", 0.0}, {"width", 1.0}, {"height", 1.0}}}};
}

std::set<std::string> previewSourceIds(corevideo::core::MediaCore& core) {
  std::set<std::string> ids;
  const auto state = core.sessionState();
  const auto* frame = state.get("previewFrame");
  if (!frame) return {"<no previewFrame node>"};
  for (const auto& source : frame->get("videoSources")->asArray()) ids.insert(source.getString("sourceId"));
  return ids;
}
}  // namespace

TEST(RenderedPreviewSources, PublishedFromTheBuiltPreviewPlanAndClearedWithIt) {
  corevideo::core::MediaCore core(corevideo::modules::createStubModules());
  EXPECT_EQ(previewSourceIds(core), std::set<std::string>{}) << "the node exists before anything happens";
  (void)core.applyCommands(Json::Array{
      Json::Object{{"type", "load-scene-graph"}, {"sceneId", "pgm"}, {"routes", Json::Array{route("p0", "101")}}},
      Json::Object{{"type", "set-preview-scene"}, {"sceneId", "pvw"}, {"routes", Json::Array{route("v0", "102")}}},
  });
  (void)core.applyCommands(Json::Array{});  // a render tick builds the Preview plan
  EXPECT_EQ(previewSourceIds(core), std::set<std::string>{"zoom:102"})
      << "the guest on the rendered Preview, never the Program guest";
  (void)core.applyCommands(Json::Array{
      Json::Object{{"type", "set-preview-scene"}, {"sceneId", "empty"}, {"routes", Json::Array{}}}});
  (void)core.applyCommands(Json::Array{});
  EXPECT_EQ(previewSourceIds(core), std::set<std::string>{}) << "no Preview scene: nobody is on Preview";
}
