#include "core/Director.h"
#include "core/MediaCore.h"
#include "rpc/Json.h"

#include <gtest/gtest.h>

namespace {

using corevideo::core::DirectorSignals;
using corevideo::core::recommendScene;

// Mirrors the representative cases in src/engine/localDirectorProvider.test.ts so
// the C++ kernel and the TypeScript reference stay behaviourally identical.

TEST(DirectorFloor, OneGuestNeverPadsInterviewWithHostOrSelf) {
  DirectorSignals signals;
  signals.liveCount = 2;
  std::vector<corevideo::core::FloorPerson> floor{
      {.id="guest-7", .sourceId="zoom:guest-7", .hasVideo=true, .lastSpokeAtMs=2'000, .score=3.0},
      {.id="host", .sourceId="zoom:host", .isHost=true, .hasVideo=true, .lastSpokeAtMs=2'000, .score=9.0}};
  const auto recommendation = recommendScene(signals, floor, 2'000);
  EXPECT_EQ(recommendation.recommendedSceneId, "intro");
  ASSERT_EQ(recommendation.slotBindings.size(), 1u);
  EXPECT_EQ(recommendation.slotBindings[0].personId, "guest-7");
}

TEST(DirectorFloor, TwoRecentlySpeakingDistinctGuestsEarnInterview) {
  DirectorSignals signals;
  std::vector<corevideo::core::FloorPerson> floor{
      {.id="a", .sourceId="zoom:a", .hasVideo=true, .talkingNow=true, .lastSpokeAtMs=10'000, .score=5.0},
      {.id="b", .sourceId="zoom:b", .hasVideo=true, .lastSpokeAtMs=5'000, .score=1.0},
      {.id="host", .sourceId="zoom:host", .isHost=true, .hasVideo=true, .score=8.0}};
  const auto result = recommendScene(signals, floor, 10'000);
  EXPECT_EQ(result.recommendedSceneId, "interview");
  ASSERT_EQ(result.slotBindings.size(), 2u);
  EXPECT_EQ(result.slotBindings[0].personId, "a");
  EXPECT_EQ(result.slotBindings[1].personId, "b");
}

TEST(DirectorFloor, StaleSecondGuestReturnsSoloAndShareStillWins) {
  DirectorSignals signals;
  std::vector<corevideo::core::FloorPerson> floor{
      {.id="a", .sourceId="zoom:a", .hasVideo=true, .talkingNow=true, .lastSpokeAtMs=20'000, .score=5.0},
      {.id="b", .sourceId="zoom:b", .hasVideo=true, .lastSpokeAtMs=5'000, .score=0.5}};
  EXPECT_EQ(recommendScene(signals, floor, 20'000).recommendedSceneId, "intro");
  signals.screenShareActive = true;
  const auto share = recommendScene(signals, floor, 20'000);
  EXPECT_EQ(share.recommendedSceneId, "speaker-slides");
  ASSERT_EQ(share.slotBindings.size(), 1u);
  EXPECT_EQ(share.slotBindings[0].personId, "a");
}

TEST(DirectorFloor, QuietCamerasDoNotEarnTalkerSlots) {
  DirectorSignals signals;
  std::vector<corevideo::core::FloorPerson> floor{
      {.id="a", .sourceId="zoom:a", .hasVideo=true},
      {.id="b", .sourceId="zoom:b", .hasVideo=true}};
  const auto result = recommendScene(signals, floor, 10'000);
  EXPECT_EQ(result.recommendedSceneId, "intro");
  EXPECT_EQ(result.confidence, 0);
  EXPECT_TRUE(result.slotBindings.empty());
}

TEST(DirectorFloor, DuplicatePersonSnapshotsNeverProduceDuplicateInterviewSeats) {
  DirectorSignals signals;
  std::vector<corevideo::core::FloorPerson> floor{
      {.id="a", .sourceId="capture:a", .hasVideo=true, .lastSpokeAtMs=10'000, .score=5.0},
      {.id="a", .sourceId="zoom:a", .hasVideo=true, .lastSpokeAtMs=10'000, .score=5.0},
      {.id="host", .sourceId="zoom:host", .isHost=true, .hasVideo=true, .lastSpokeAtMs=10'000, .score=8.0}};
  const auto result = recommendScene(signals, floor, 10'000);
  EXPECT_EQ(result.recommendedSceneId, "intro");
  ASSERT_EQ(result.slotBindings.size(), 1u);
  EXPECT_EQ(result.slotBindings[0].personId, "a");
  EXPECT_EQ(result.slotBindings[0].sourceId, "zoom:a");
}

TEST(DirectorFloor, GalleryDropsQuietCamerasAfterTwentySecondsButKeepsTwoFaces) {
  DirectorSignals signals;
  std::vector<corevideo::core::FloorPerson> floor{
      {.id="a", .sourceId="zoom:a", .hasVideo=true, .talkingNow=true, .lastSpokeAtMs=30'000, .score=5.0},
      {.id="b", .sourceId="zoom:b", .hasVideo=true, .lastSpokeAtMs=25'000, .score=2.0},
      {.id="c", .sourceId="zoom:c", .hasVideo=true, .lastSpokeAtMs=15'000, .score=1.0},
      {.id="d", .sourceId="zoom:d", .hasVideo=true, .lastSpokeAtMs=2'000, .score=0.5},
      {.id="e", .sourceId="zoom:e", .hasVideo=true, .score=0.0}};
  const auto gallery = recommendScene(signals, floor, 30'000);
  EXPECT_EQ(gallery.recommendedSceneId, "panel");
  ASSERT_EQ(gallery.slotBindings.size(), 3u);
  EXPECT_EQ(gallery.slotBindings[0].personId, "a");
  EXPECT_EQ(gallery.slotBindings[1].personId, "b");
  EXPECT_EQ(gallery.slotBindings[2].personId, "c");
  floor[1].lastSpokeAtMs = 1'000;
  const auto minimum = recommendScene(signals, floor, 30'000);
  ASSERT_EQ(minimum.slotBindings.size(), 2u);
  EXPECT_EQ(minimum.slotBindings[0].personId, "a");
}

// End-to-end through MediaCore: the recommend-auto-production command surfaces a
// recommendation in the snapshot, derived from the core's current participants.
TEST(Director, MediaCoreSurfacesRecommendationInSnapshot) {
  corevideo::core::MediaCore core;
  (void)core.joinZoom(corevideo::rpc::Json::Object{{"displayName", "Host"}});

  const corevideo::rpc::Json command = corevideo::rpc::Json::Object{{"type", "recommend-auto-production"}};
  const auto snapshot = core.applyCommand(command);

  const auto* autoProduction = snapshot.get("autoProduction");
  ASSERT_NE(autoProduction, nullptr);
  EXPECT_FALSE(autoProduction->getString("ruleId").empty());
  EXPECT_FALSE(autoProduction->getString("recommendedSceneId").empty());
  const double confidence = autoProduction->getNumber("confidence", -1);
  EXPECT_TRUE(confidence >= 0);
  EXPECT_TRUE(confidence <= 100);
  ASSERT_NE(autoProduction->get("slotBindings"), nullptr);
  EXPECT_TRUE(autoProduction->get("slotBindings")->isArray());
}

}  // namespace
