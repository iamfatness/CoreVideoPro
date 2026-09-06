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
  for (const auto* mode : {"fixed", "active-speaker", "none"}) {
    const auto binding = resolveRouteSource({mode, {}, {}, {}, {}, "guest-7"});
    EXPECT_EQ(binding.sourceId, "zoom:guest-7") << mode;
  }
}

TEST(RouteSourcePolicy, MissingGuestWithoutAssignmentOrFallbackRemainsUnbound) {
  const auto binding = resolveRouteSource({"fixed", {}, {}, {}, {}, {}});
  EXPECT_TRUE(binding.sourceId.empty());
  EXPECT_TRUE(binding.participantId.empty());
}
