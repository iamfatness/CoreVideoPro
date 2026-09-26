#include "core/MediaCore.h"
#include "core/SpeakerFloor.h"
#include "rpc/Json.h"

#include <gtest/gtest.h>

#include <string>

namespace {
using corevideo::core::MediaCore;
using corevideo::core::SpeakerFloor;
using corevideo::rpc::Json;

Json route(const char* id, const char* mode, const char* participantId,
           const char* captureId = "", const char* personId = "") {
  return Json::Object{{"routeId", id}, {"mode", mode}, {"participantId", participantId},
                      {"captureDeviceId", captureId}, {"personId", personId}};
}

corevideo::modules::CompositorRenderPlan planFor(const Json::Array& routes) {
  MediaCore core;
  (void)core.applyCommands(Json::Array{Json::Object{{"type", "load-scene-graph"},
                                                     {"sceneId", "two-up"}, {"routes", routes}}});
  core.setTilesMemberFrameAgesForTest({});
  return core.lastRenderPlanForTest();
}

const corevideo::modules::CompositorRenderPlanLayer* findLayer(
    const corevideo::modules::CompositorRenderPlan& plan, const std::string& id) {
  for (const auto& layer : plan.layers) if (layer.layerId == id) return &layer;
  return nullptr;
}
}  // namespace

TEST(SpeakerFloor, ZoomAliasesCollapseToOnePersonAndHostFlagPropagates) {
  SpeakerFloor floor;
  floor.observeSource("zoom:guest-7", {}, false);
  floor.observeSource("participant:guest-7", {}, true);
  EXPECT_EQ(floor.personFor("zoom:guest-7"), "guest-7");
  EXPECT_EQ(floor.personFor("participant:guest-7"), "guest-7");
  EXPECT_TRUE(floor.isHost("guest-7"));
  EXPECT_TRUE(floor.claimPersonSlot("zoom:guest-7"));
  EXPECT_FALSE(floor.claimPersonSlot("participant:guest-7"));
}

TEST(SpeakerFloor, CaptureAliasUsesTheSamePersonAsZoom) {
  SpeakerFloor floor;
  floor.observeSource("capture:host-camera", "host-user", true);
  floor.observeSource("zoom:host-user", {}, false);
  EXPECT_EQ(floor.personFor("capture:host-camera"), "host-user");
  EXPECT_TRUE(floor.isHost("host-user"));
  EXPECT_TRUE(floor.claimPersonSlot("capture:host-camera"));
  EXPECT_FALSE(floor.claimPersonSlot("zoom:host-user"));
}

TEST(SpeakerFloorRenderPlan, TwoUpRefusesDuplicateZoomPersonOnProgram) {
  const auto plan = planFor({route("left", "fixed", "guest-7"),
                             route("right", "active-speaker", "guest-7")});
  const auto* left = findLayer(plan, "route:left");
  const auto* right = findLayer(plan, "route:right");
  ASSERT_NE(left, nullptr);
  ASSERT_NE(right, nullptr);
  EXPECT_EQ(left->sourceId, "zoom:guest-7");
  EXPECT_TRUE(right->sourceId.empty());
  EXPECT_TRUE(right->participantId.empty());
  EXPECT_TRUE(right->hasFillColor);
  EXPECT_EQ(right->fillColor, "#00000000");
  EXPECT_EQ(right->opacity, 0.f);
  EXPECT_FALSE(plan.warnings.empty());
}

TEST(SpeakerFloorRenderPlan, ExplicitCaptureAliasCannotDuplicateZoomGuest) {
  const auto plan = planFor({route("left", "capture-input", "", "camera-2", "guest-7"),
                             route("right", "fixed", "guest-7")});
  const auto* left = findLayer(plan, "route:left");
  const auto* right = findLayer(plan, "route:right");
  ASSERT_NE(left, nullptr);
  ASSERT_NE(right, nullptr);
  EXPECT_EQ(left->sourceId, "capture:camera-2");
  EXPECT_TRUE(right->sourceId.empty());
}

TEST(SpeakerFloorRenderPlan, LaterCaptureAliasAppliesBeforeFirstSlotIsClaimed) {
  const auto plan = planFor({route("camera-first", "capture-input", "", "camera-2"),
                             route("zoom-second", "fixed", "guest-7"),
                             route("camera-alias", "capture-input", "", "camera-2", "guest-7")});
  ASSERT_NE(findLayer(plan, "route:camera-first"), nullptr);
  ASSERT_NE(findLayer(plan, "route:zoom-second"), nullptr);
  EXPECT_EQ(findLayer(plan, "route:camera-first")->sourceId, "capture:camera-2");
  EXPECT_TRUE(findLayer(plan, "route:zoom-second")->sourceId.empty());
  EXPECT_TRUE(findLayer(plan, "route:camera-alias")->sourceId.empty());
}

TEST(SpeakerFloorRenderPlan, DistinctGuestsKeepBothSlotsAndShareIsNotAPersonSlot) {
  const auto plan = planFor({route("left", "fixed", "guest-7"),
                             route("share", "screen-share", "guest-7"),
                             route("right", "fixed", "guest-8")});
  ASSERT_NE(findLayer(plan, "route:left"), nullptr);
  ASSERT_NE(findLayer(plan, "route:share"), nullptr);
  ASSERT_NE(findLayer(plan, "route:right"), nullptr);
  EXPECT_EQ(findLayer(plan, "route:left")->sourceId, "zoom:guest-7");
  EXPECT_EQ(findLayer(plan, "route:share")->sourceId, "zoom:guest-7");
  EXPECT_EQ(findLayer(plan, "route:right")->sourceId, "zoom:guest-8");
}
