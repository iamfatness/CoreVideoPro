#include "core/FollowSpeakerHold.h"
#include "core/RouteSourcePolicy.h"
#include <gtest/gtest.h>

#include <set>
#include <string>

using corevideo::core::resolveRouteSource;

TEST(RouteSourcePolicy, FixedGuestSurvivesMissingFrameAndRosterReorder) {
  const auto binding = resolveRouteSource({"fixed", {}, {}, {}, "guest-7"});
  EXPECT_EQ(binding.participantId, "guest-7");
  EXPECT_EQ(binding.sourceId, "zoom:guest-7");
}

TEST(RouteSourcePolicy, CaptureInputUsesExactNamespacedFrameIdentity) {
  const auto binding = resolveRouteSource({"capture-input", {}, {}, "camera-2", "guest-7"});
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

TEST(RouteSourcePolicy, AnUnassignedRouteBindsNothingNeverAPositionalGuest) {
  for (const auto* mode : {"fixed", "none", "capture-input", "screen-share"}) {
    const auto binding = resolveRouteSource({mode, {}, {}, {}, {}});
    EXPECT_TRUE(binding.sourceId.empty()) << mode;
    EXPECT_TRUE(binding.participantId.empty()) << mode;
  }
}

// #478 R2: a follow-speaker route shows the DIRECTED speaker, never the frame that
// happens to sit at its route index (the positional fallback, ordered by uuid).
TEST(RouteSourcePolicy, AFollowSpeakerRouteBindsTheDirectedSpeakerNeverAPositionalSource) {
  // NAMED FIELDS, NOT POSITIONAL (419 refresh, 2026-09-12). This branch inserted
  // positionalFallbackParticipantId AHEAD of directedSpeakerParticipantId, so the
  // original positional form bound "speaker" into the wrong member and this test
  // failed with an empty binding. Naming the field is what makes the test immune
  // to the next field added to RouteSourcePolicyInput.
  const auto follow = [](std::string_view participantId, std::string_view directed) {
    corevideo::core::RouteSourcePolicyInput input{};
    input.mode = "active-speaker";
    input.participantId = participantId;
    input.directedSpeakerParticipantId = directed;
    return input;
  };

  const auto directed = resolveRouteSource(follow({}, "speaker"));
  EXPECT_EQ(directed.participantId, "speaker");
  EXPECT_EQ(directed.sourceId, "zoom:speaker");

  // Nobody directed yet: bind NOTHING rather than a random source.
  const auto nobody = resolveRouteSource(follow({}, {}));
  EXPECT_TRUE(nobody.participantId.empty());
  EXPECT_TRUE(nobody.sourceId.empty());

  // A follow route that names a guest explicitly still shows that guest.
  const auto named = resolveRouteSource(follow("guest-7", "speaker"));
  EXPECT_EQ(named.participantId, "guest-7");
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

// #478 N2: a follow route binds only a speaker with a frame THIS tick, falls back to
// the most recent previously directed speaker who has one, and forgets everything
// when the meeting session changes.
namespace {
struct Frames {
  std::set<std::string> ids;
  bool operator()(const std::string& id) const { return ids.count(id) > 0; }
};
}  // namespace

TEST(FollowSpeakerHold, BindsTheDirectedSpeakerOnlyWhileTheyHaveAFrame) {
  corevideo::core::FollowSpeakerHold hold;
  hold.observe(1, "alice");
  EXPECT_EQ(hold.pick(Frames{{"alice", "bob"}}), "alice");
  hold.observe(1, "bob");
  EXPECT_EQ(hold.pick(Frames{{"alice", "bob"}}), "bob");
}

TEST(FollowSpeakerHold, ASpeakerDroppedFromTheSourcesMidTalkFallsBackToThePreviousSpeaker) {
  corevideo::core::FollowSpeakerHold hold;
  hold.observe(1, "alice");
  hold.observe(1, "bob");
  // An off-air change drops bob from the sources: the director releases him (nobody is
  // directed) and his video is retired in the same tick, so he has no frame.
  hold.observe(1, "");
  EXPECT_EQ(hold.pick(Frames{{"alice"}}), "alice");
  // Nobody remembered has a frame: bind NOBODY (the layer renders empty).
  EXPECT_EQ(hold.pick(Frames{{"carol"}}), "");
}

TEST(FollowSpeakerHold, ASpeakerWhoLeavesIsNeverBoundWhileTheDirectorStillHoldsThem) {
  corevideo::core::FollowSpeakerHold hold;
  hold.observe(1, "alice");
  hold.observe(1, "bob");
  // bob leaves: the director keeps him for its 60 s grace, but his frames are gone.
  hold.observe(1, "bob");
  EXPECT_EQ(hold.pick(Frames{{"alice"}}), "alice");
}

TEST(FollowSpeakerHold, TheNextMeetingForgetsEveryoneEvenAReusedIdWithAFrame) {
  corevideo::core::FollowSpeakerHold hold;
  hold.observe(1, "16778240");
  EXPECT_EQ(hold.pick(Frames{{"16778240"}}), "16778240");
  // Leave, then the next meeting (the epoch moves). Zoom reused 16778240 for a DIFFERENT
  // person who is a source here and has a frame: never bound until actually directed.
  hold.observe(2, "");
  EXPECT_EQ(hold.pick(Frames{{"16778240"}}), "");
  EXPECT_TRUE(hold.remembered().empty());
  hold.observe(2, "16778240");
  EXPECT_EQ(hold.pick(Frames{{"16778240"}}), "16778240");
}
