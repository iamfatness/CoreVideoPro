#include "core/Director.h"
#include "core/MediaCore.h"
#include "rpc/Json.h"

#include <gtest/gtest.h>

namespace {

using corevideo::core::DirectorRecommendation;
using corevideo::core::DirectorSignals;
using corevideo::core::recommendScene;

// Mirrors the representative cases in src/engine/localDirectorProvider.test.ts so
// the C++ kernel and the TypeScript reference stay behaviourally identical.

TEST(Director, LeadsWithSharedSurfaceOnActiveScreenShare) {
  DirectorSignals signals;
  signals.liveCount = 3;
  signals.screenShareActive = true;
  signals.screenShareHasSharer = true;
  signals.screenShareSharerName = "Ada";

  const auto recommendation = recommendScene(signals);
  EXPECT_EQ(recommendation.ruleId, "screen-share-priority");
  EXPECT_EQ(recommendation.recommendedSceneId, "speaker-slides");
}

TEST(Director, UsesHostFocusIntroForSingleLiveFeed) {
  DirectorSignals signals;
  signals.liveCount = 1;
  signals.activeContributorCount = 1;
  signals.meanAudioLevel = 0.3;

  const auto recommendation = recommendScene(signals);
  EXPECT_EQ(recommendation.recommendedSceneId, "intro");
  EXPECT_EQ(recommendation.ruleId, "single-speaker");
}

TEST(Director, KeepsTwoLiveContributorsTwoUp) {
  DirectorSignals signals;
  signals.liveCount = 2;
  signals.activeContributorCount = 2;
  signals.meanAudioLevel = 0.3;
  signals.hasDominantSpeaker = true;

  const auto recommendation = recommendScene(signals);
  EXPECT_EQ(recommendation.recommendedSceneId, "interview");
  EXPECT_EQ(recommendation.ruleId, "focused-interview");
}

TEST(Director, KeepsContestedMultiSpeakerRoomAsPanel) {
  DirectorSignals signals;
  signals.liveCount = 4;
  signals.activeContributorCount = 3;
  signals.meanAudioLevel = 0.5;
  signals.crossTalk = true;

  const auto recommendation = recommendScene(signals);
  EXPECT_EQ(recommendation.recommendedSceneId, "panel");
  EXPECT_EQ(recommendation.ruleId, "panel-discussion");
}

TEST(Director, FocusesLoneDominantSpeakerCarryingLargeRoom) {
  // The signal-driven divergence: a single dominant speaker in a large room
  // reads cleaner as host-focus than a sparse grid.
  DirectorSignals signals;
  signals.liveCount = 5;
  signals.activeContributorCount = 1;
  signals.meanAudioLevel = 0.4;
  signals.hasDominantSpeaker = true;

  const auto recommendation = recommendScene(signals);
  EXPECT_EQ(recommendation.recommendedSceneId, "intro");
  EXPECT_EQ(recommendation.ruleId, "single-speaker");
}

TEST(Director, LowersConfidenceWhenFeedsAreDegraded) {
  DirectorSignals healthy;
  healthy.liveCount = 4;
  healthy.activeContributorCount = 3;
  healthy.meanAudioLevel = 0.5;

  DirectorSignals degraded = healthy;
  degraded.degradedCount = 3;

  EXPECT_TRUE(recommendScene(degraded).confidence < recommendScene(healthy).confidence);
}

TEST(Director, AlwaysProducesAClampedZeroToHundredProposal) {
  DirectorSignals signals;
  signals.liveCount = 3;
  signals.degradedCount = 5;
  signals.activeContributorCount = 3;
  signals.meanAudioLevel = 0.9;

  const auto recommendation = recommendScene(signals);
  EXPECT_GE(recommendation.confidence, 0);
  EXPECT_TRUE(recommendation.confidence <= 100);
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
}

}  // namespace
