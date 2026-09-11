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

TEST(RouteSourcePolicy, ExactSourceRequiresFrameBoundIdentityAndNeverUsesLegacyFallbacks) {
  using namespace corevideo::core;
  ExactRouteSourceRef expected{"source", "instance", "epoch", "camera", 7};
  ExactRouteSourceIntent intent{expected};
  RouteSourcePolicyInput input{"capture-input", "clip", "clip.mp4", "device", "42", "43", &intent};
  auto missing = resolveRouteSource(input);
  EXPECT_EQ(missing.status, RouteSourceBinding::Status::Missing);
  EXPECT_TRUE(missing.sourceId.empty()); EXPECT_TRUE(missing.participantId.empty());
  for (int mutation = 0; mutation < 5; ++mutation) {
    auto actual = expected;
    switch (mutation) {
      case 0: actual.sourceId += "-other"; break;
      case 1: actual.instanceId += "-other"; break;
      case 2: actual.processEpoch += "-retired"; break;
      case 3: ++actual.generation; break;
      case 4: actual.kind = "share"; break;
    }
    input.frameIdentity = &actual;
    const auto result = resolveRouteSource(input);
    EXPECT_EQ(result.status, RouteSourceBinding::Status::Missing);
    EXPECT_TRUE(result.participantId.empty()); EXPECT_TRUE(result.sourceId.empty());
  }
  input.frameIdentity = &expected;
  EXPECT_EQ(resolveRouteSource(input).status, RouteSourceBinding::Status::ExactAvailable);
  EXPECT_EQ(resolveRouteSource(input).sourceId, "source");
  EXPECT_EQ(resolveRouteSource(input).kind, "participant-video");
  intent.reference.reset();
  EXPECT_EQ(resolveRouteSource(input).status, RouteSourceBinding::Status::Rejected);
  input.exactSource = nullptr;
  EXPECT_EQ(resolveRouteSource(input).sourceId, "media:clip");
}

TEST(RouteSourcePolicy, PresentInvalidExactReferenceCannotBecomeAnAbsentLegacyAssignment) {
  using namespace corevideo::core; using J = corevideo::rpc::Json;
  EXPECT_FALSE(parseExactRouteSource(J::Object{}));
  for (const auto& bad : std::vector<J>{J(), J(true), J("source"), J::Object{}}) {
    auto intent = parseExactRouteSource(J::Object{{"exactSourceRef", bad}});
    ASSERT_TRUE(intent); EXPECT_FALSE(intent->reference);
  }
  auto valid = J::parse(R"({"exactSourceRef":{"sourceId":"s","instanceId":"i","processEpoch":"e","generation":7,"kind":"camera"}})");
  ASSERT_TRUE(valid); ASSERT_TRUE(parseExactRouteSource(*valid)->reference);
  for (const auto& number : {"7.0", "7e0", "9007199254740990.5", "9007199254740992", "0", "-1"}) {
    const auto wire = std::string(R"({"exactSourceRef":{"sourceId":"s","instanceId":"i","processEpoch":"e","generation":)") + number + R"(,"kind":"camera"}})";
    auto parsed = J::parse(wire); ASSERT_TRUE(parsed);
    EXPECT_FALSE(parseExactRouteSource(*parsed)->reference);
  }
}
