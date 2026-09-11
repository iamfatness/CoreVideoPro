#include "core/RouteSourcePolicy.h"
#include <gtest/gtest.h>

using corevideo::core::resolveRouteSource;

TEST(RouteSourcePolicy, FixedGuestSurvivesMissingFrameAndRosterReorder) {
  const auto binding = resolveRouteSource({"fixed", {}, {}, {}, "guest-7", "other-guest"});
  EXPECT_EQ(binding.participantId, "guest-7");
  EXPECT_EQ(binding.sourceId, "zoom:guest-7");
}

TEST(RouteSourcePolicy, CaptureInputUsesExactNamespacedFrameIdentity) {
  const auto binding = resolveRouteSource({"capture-input", {}, {}, "camera-2", "guest-7", "other-guest"});
  EXPECT_EQ(binding.participantId, "capture:camera-2");
  EXPECT_EQ(binding.sourceId, "capture:camera-2");
}

TEST(RouteSourcePolicy, ValidMediaTakesPrecedenceOverCaptureAndParticipant) {
  const auto binding = resolveRouteSource({"capture-input", "clip-1", "clip.mp4", "camera-2", "guest-7", {}});
  EXPECT_EQ(binding.kind, "media-video");
  EXPECT_EQ(binding.sourceId, "media:clip-1");
  EXPECT_TRUE(binding.participantId.empty());
}

TEST(RouteSourcePolicy, IncompleteMediaKeepsExistingParticipantBinding) {
  const auto binding = resolveRouteSource({"fixed", "clip-1", {}, {}, "guest-7", {}});
  EXPECT_EQ(binding.kind, "participant-video");
  EXPECT_EQ(binding.sourceId, "zoom:guest-7");
}

TEST(RouteSourcePolicy, ScreenShareRetainsFrameKind) {
  const auto binding = resolveRouteSource({"screen-share", {}, {}, {}, "guest-7", {}});
  EXPECT_EQ(binding.kind, "screen-share");
  EXPECT_EQ(binding.sourceId, "zoom:guest-7");
}

TEST(RouteSourcePolicy, LegacyUnassignedRouteRetainsPositionalFallback) {
  for (const auto* mode : {"fixed", "none"}) {
    const auto binding = resolveRouteSource({mode, {}, {}, {}, {}, "guest-7"});
    EXPECT_EQ(binding.sourceId, "zoom:guest-7") << mode;
  }
}

// #478 R2: a follow-speaker route shows the DIRECTED speaker, never the frame that
// happens to sit at its route index (the positional fallback, ordered by uuid).
TEST(RouteSourcePolicy, AFollowSpeakerRouteBindsTheDirectedSpeakerNeverAPositionalSource) {
  const auto directed = resolveRouteSource({"active-speaker", {}, {}, {}, {}, "lowest-pid", "speaker"});
  EXPECT_EQ(directed.participantId, "speaker");
  EXPECT_EQ(directed.sourceId, "zoom:speaker");

  // Nobody directed yet: bind NOTHING rather than a random source.
  const auto nobody = resolveRouteSource({"active-speaker", {}, {}, {}, {}, "lowest-pid", {}});
  EXPECT_TRUE(nobody.participantId.empty());
  EXPECT_TRUE(nobody.sourceId.empty());

  // A follow route that names a guest explicitly still shows that guest.
  const auto named = resolveRouteSource({"active-speaker", {}, {}, {}, "guest-7", "lowest-pid", "speaker"});
  EXPECT_EQ(named.participantId, "guest-7");
}

TEST(RouteSourcePolicy, MissingGuestWithoutAssignmentOrFallbackRemainsUnbound) {
  const auto binding = resolveRouteSource({"fixed", {}, {}, {}, {}, {}});
  EXPECT_TRUE(binding.sourceId.empty());
  EXPECT_TRUE(binding.participantId.empty());
}
