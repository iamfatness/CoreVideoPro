#include "core/MediaCore.h"
#include "core/SpeakerFloor.h"
#include "rpc/Json.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>
#include <cmath>

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

TEST(SpeakerFloorLedger, DebouncesTurnsScoresGuestsAndClearsOnEpoch) {
  SpeakerFloor floor;
  auto guest = [](bool talking) {
    return std::vector<corevideo::core::FloorObservation>{{"zoom:guest-7", {}, false, true, talking}};
  };
  floor.observeFrame(1, 1'000, guest(true));
  floor.observeFrame(1, 1'399, guest(true));
  EXPECT_FALSE(floor.snapshot(1'399)[0].talkingNow);
  floor.observeFrame(1, 1'400, guest(true));
  EXPECT_TRUE(floor.snapshot(1'400)[0].talkingNow);
  EXPECT_EQ(floor.snapshot(1'400)[0].turnStartedAtMs, 1'000u);
  floor.observeFrame(1, 2'000, guest(false));
  floor.observeFrame(1, 2'599, guest(false));
  EXPECT_TRUE(floor.snapshot(2'599)[0].talkingNow);
  floor.observeFrame(1, 2'600, guest(false));
  const auto done = floor.snapshot(2'600);
  EXPECT_FALSE(done[0].talkingNow);
  EXPECT_EQ(done[0].lastTurnMs, 1'000u);
  EXPECT_EQ(done[0].lastSpokeAtMs, 2'000u);
  EXPECT_EQ(done[0].windowTalkMs, 1'000u);
  EXPECT_NEAR(done[0].score, 2.0 * std::exp(-0.6 / 8.0) + 0.125 + 0.025, 0.000001);
  floor.observeFrame(2, 3'000, guest(false));
  EXPECT_EQ(floor.snapshot(3'000)[0].score, 0.0);
}

TEST(SpeakerFloorLedger, AliasesCollapseAndZoomFrameWinsPreferredSource) {
  SpeakerFloor floor;
  floor.observeFrame(1, 1'000, {{"capture:camera-2", "guest-7", false, true, true},
                                {"zoom:guest-7", {}, false, true, true}});
  const auto people = floor.snapshot(1'000);
  ASSERT_EQ(people.size(), 1u);
  EXPECT_EQ(people[0].id, "guest-7");
  EXPECT_EQ(people[0].sourceId, "zoom:guest-7");
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
