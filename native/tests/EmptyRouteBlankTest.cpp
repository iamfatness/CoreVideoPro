#include "core/MediaCore.h"
#include "modules/Interfaces.h"
#include "rpc/Json.h"

#include <gtest/gtest.h>

namespace {

using corevideo::core::MediaCore;

const corevideo::modules::CompositorRenderPlanLayer* findLayer(
    const corevideo::modules::CompositorRenderPlan& plan, const std::string& layerId) {
  for (const auto& layer : plan.layers) {
    if (layer.layerId == layerId) {
      return &layer;
    }
  }
  return nullptr;
}

}  // namespace

// #480: a route with no source must render BLANK, never inherit videoFrames[index]
// (the positional fallback that showed a random guest after the shell wiped the
// layer).
TEST(EmptyRoute, ASourcelessFixedRouteRendersBlankNeverAPositionalGuest) {
  MediaCore core;
  (void)core.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json{corevideo::rpc::Json::Object{
          {"type", corevideo::rpc::Json{"load-scene-graph"}},
          {"sceneId", corevideo::rpc::Json{"s"}},
          {"routes", corevideo::rpc::Json{corevideo::rpc::Json::Array{
              corevideo::rpc::Json{corevideo::rpc::Json::Object{
                  {"routeId", corevideo::rpc::Json{"layer-0"}},
                  {"mode", corevideo::rpc::Json{"fixed"}},
                  {"rect", corevideo::rpc::Json{corevideo::rpc::Json::Object{
                      {"x", corevideo::rpc::Json{0.0}},
                      {"y", corevideo::rpc::Json{0.0}},
                      {"width", corevideo::rpc::Json{1.0}},
                      {"height", corevideo::rpc::Json{1.0}}}}}}}}}}}}});
  core.setTilesMemberFrameAgesForTest({});
  const auto plan = core.lastRenderPlanForTest();
  const auto* layer = findLayer(plan, "route:layer-0");
  ASSERT_NE(layer, nullptr);
  EXPECT_TRUE(layer->sourceId.empty());
  EXPECT_TRUE(layer->participantId.empty());
  EXPECT_TRUE(layer->hasFillColor);
  EXPECT_EQ(layer->fillColor, "#00000000");
}
