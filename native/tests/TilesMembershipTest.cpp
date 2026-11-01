#include "compositor/TilesMembership.h"
#include "compositor/TilesHoldEpisodeLog.h"

#include <gtest/gtest.h>

namespace {

using corevideo::compositor::admitTilesMembers;
using corevideo::compositor::admitTilesMembersWithHold;
using corevideo::compositor::classifyTilesMember;
using corevideo::compositor::kTilesHoldLogRearmMs;
using corevideo::compositor::kTilesHoldWindowMs;
using corevideo::compositor::TilesHoldEpisodeLog;
using corevideo::compositor::kTilesStaleFrameMs;
using corevideo::compositor::TilesMemberFrameAge;
using corevideo::compositor::TilesMemberState;

TEST(TilesMembership, FreshMembersAreAdmittedInOrder) {
  const std::vector<std::string> members{"zoom:1", "zoom:2", "capture:a"};
  const std::vector<TilesMemberFrameAge> ages{
      {"zoom:1", true, 16}, {"zoom:2", true, 33}, {"capture:a", true, 0}};
  EXPECT_EQ(admitTilesMembers(members, ages), members);
}

TEST(TilesMembership, AMemberThatNeverDeliveredAFrameIsNotDrawn) {
  const std::vector<TilesMemberFrameAge> ages{{"zoom:1", true, 16}, {"zoom:2", false, 0}};
  const auto admitted = admitTilesMembers({"zoom:1", "zoom:2"}, ages);
  ASSERT_EQ(admitted.size(), 1u);
  EXPECT_EQ(admitted[0], "zoom:1");
}

TEST(TilesMembership, AMemberWithNoAgeEntryAtAllIsNotDrawn) {
  const auto admitted = admitTilesMembers({"zoom:1", "zoom:missing"}, {{"zoom:1", true, 16}});
  ASSERT_EQ(admitted.size(), 1u);
  EXPECT_EQ(admitted[0], "zoom:1");
}

// The edges of the threshold, both pinned: an ordinary gap must not reflow the
// wall, and a dead feed must actually leave it.
TEST(TilesMembership, AnOrdinaryFrameGapKeepsTheMemberOnTheWall) {
  const std::vector<TilesMemberFrameAge> ages{{"zoom:1", true, kTilesStaleFrameMs - 1}};
  EXPECT_EQ(admitTilesMembers({"zoom:1"}, ages).size(), 1u);
}

// #1074 (live show 2026-10-31): a 1.5 to 3 s uplink stall used to empty the
// slot, reflow the wall, and pop the tile back. A stale feed is now HELD in
// its slot, and labelled, until the hold window runs out.
TEST(TilesMembership, AStaleFeedIsHeldInItsSlotNotDropped) {
  const std::vector<TilesMemberFrameAge> ages{{"zoom:1", true, kTilesStaleFrameMs + 1}};
  const auto admission = admitTilesMembersWithHold({"zoom:1"}, ages);
  EXPECT_EQ(admission.admitted, (std::vector<std::string>{"zoom:1"}));
  EXPECT_EQ(admission.held, (std::vector<std::string>{"zoom:1"}));
  EXPECT_EQ(admitTilesMembers({"zoom:1"}, ages).size(), 1u)
      << "the plain admission must agree with the labelled one";
}

TEST(TilesMembership, AFreshFeedIsNeverLabelledHeld) {
  const std::vector<TilesMemberFrameAge> ages{{"zoom:1", true, kTilesStaleFrameMs}};
  const auto admission = admitTilesMembersWithHold({"zoom:1"}, ages);
  EXPECT_EQ(admission.admitted.size(), 1u);
  EXPECT_TRUE(admission.held.empty());
}

// The hold is BOUNDED: a dead feed still leaves the wall, one tick past the
// window. Both edges pinned, inclusive at the window like the stale edge.
TEST(TilesMembership, AMemberExactlyAtTheHoldWindowIsStillHeld) {
  const std::vector<TilesMemberFrameAge> ages{{"zoom:1", true, kTilesHoldWindowMs}};
  const auto admission = admitTilesMembersWithHold({"zoom:1"}, ages);
  EXPECT_EQ(admission.admitted.size(), 1u);
  EXPECT_EQ(admission.held.size(), 1u);
}

TEST(TilesMembership, AStallLongerThanTheHoldWindowDropsTheMember) {
  const std::vector<TilesMemberFrameAge> ages{{"zoom:1", true, kTilesHoldWindowMs + 1}};
  const auto admission = admitTilesMembersWithHold({"zoom:1"}, ages);
  EXPECT_TRUE(admission.admitted.empty());
  EXPECT_TRUE(admission.held.empty());
  EXPECT_TRUE(admitTilesMembers({"zoom:1"}, ages).empty());
}

// Video-off, departure and un-route all take the frame OUT of the gather
// (hasFrame=false). Those drop at once; the hold window never applies, so a
// slot can never be held for a guest who is actually gone.
TEST(TilesMembership, AMemberWhoseFrameLeftTheGatherIsDroppedNotHeld) {
  const std::vector<TilesMemberFrameAge> ages{{"zoom:1", false, 0}, {"zoom:2", false, kTilesStaleFrameMs + 1}};
  const auto admission = admitTilesMembersWithHold({"zoom:1", "zoom:2"}, ages);
  EXPECT_TRUE(admission.admitted.empty());
  EXPECT_TRUE(admission.held.empty());
}

TEST(TilesMembership, ClassificationTableIsFreshHeldDroppedInThatOrder) {
  EXPECT_EQ(classifyTilesMember({"zoom:1", true, 0}), TilesMemberState::Fresh);
  EXPECT_EQ(classifyTilesMember({"zoom:1", true, kTilesStaleFrameMs}), TilesMemberState::Fresh);
  EXPECT_EQ(classifyTilesMember({"zoom:1", true, kTilesStaleFrameMs + 1}), TilesMemberState::Held);
  EXPECT_EQ(classifyTilesMember({"zoom:1", true, kTilesHoldWindowMs}), TilesMemberState::Held);
  EXPECT_EQ(classifyTilesMember({"zoom:1", true, kTilesHoldWindowMs + 1}), TilesMemberState::Dropped);
  EXPECT_EQ(classifyTilesMember({"zoom:1", false, 0}), TilesMemberState::Dropped);
  // The window is a parameter, so a caller can tighten it without a new rule.
  EXPECT_EQ(classifyTilesMember({"zoom:1", true, 2000}, 1500, 1999), TilesMemberState::Dropped);
}

// Held members keep their ORIGINAL slot order among the fresh ones: the wall
// must not reflow around a hold, that is the whole point of holding.
TEST(TilesMembership, AHeldMemberKeepsItsSlotOrderBetweenFreshOnes) {
  const std::vector<std::string> members{"zoom:1", "zoom:2", "zoom:3"};
  const std::vector<TilesMemberFrameAge> ages{
      {"zoom:1", true, 16}, {"zoom:2", true, kTilesStaleFrameMs + 500}, {"zoom:3", true, 16}};
  const auto admission = admitTilesMembersWithHold(members, ages);
  EXPECT_EQ(admission.admitted, members);
  EXPECT_EQ(admission.held, (std::vector<std::string>{"zoom:2"}));
}

// Lock the inclusive boundary: a member at EXACTLY the threshold is still live.
// The threshold exists to prevent reflow on ordinary frame gaps, so treating "exactly
// at the limit" as still-live is the conservative direction — it errs toward stability
// rather than toward dropping a tile.
TEST(TilesMembership, AMemberExactlyAtTheThresholdIsStillAdmitted) {
  const std::vector<TilesMemberFrameAge> ages{{"zoom:1", true, kTilesStaleFrameMs}};
  EXPECT_EQ(admitTilesMembers({"zoom:1"}, ages).size(), 1u);
}

// Re-admission after a DROP is immediate: one fresh frame puts the guest back,
// in their original slot order, and without a held label.
TEST(TilesMembership, AReturningFeedIsReadmittedInOriginalOrder) {
  const std::vector<std::string> members{"zoom:1", "zoom:2", "zoom:3"};
  const std::vector<TilesMemberFrameAge> stale{
      {"zoom:1", true, 16}, {"zoom:2", true, kTilesHoldWindowMs + 500}, {"zoom:3", true, 16}};
  ASSERT_EQ(admitTilesMembers(members, stale).size(), 2u);

  const std::vector<TilesMemberFrameAge> recovered{
      {"zoom:1", true, 16}, {"zoom:2", true, 0}, {"zoom:3", true, 16}};
  EXPECT_EQ(admitTilesMembers(members, recovered), members);
}

TEST(TilesMembership, DuplicateMembersAreDrawnOnce) {
  const std::vector<TilesMemberFrameAge> ages{{"zoom:1", true, 0}};
  const auto admitted = admitTilesMembers({"zoom:1", "zoom:1"}, ages);
  EXPECT_EQ(admitted.size(), 1u);
}

// #1074 review / #1071: the hold-and-drop clock is Zoom-only. A local member
// (capture:/media:) re-serves one frameId while healthy (static share, still,
// paused clip), so frame age says nothing about it: admitted on hasFrame
// alone, never labelled held, dropped only when its frame leaves the gather.
TEST(TilesMembership, ALocalMemberIsNeverHeldAndNeverAgesOut) {
  for (const char* id : {"capture:screen-1", "media:clip-7"}) {
    const std::vector<TilesMemberFrameAge> ages{{id, true, kTilesHoldWindowMs * 10}};
    const auto admission = admitTilesMembersWithHold({id}, ages);
    EXPECT_EQ(admission.admitted, (std::vector<std::string>{id})) << id;
    EXPECT_TRUE(admission.held.empty()) << id;
    EXPECT_EQ(classifyTilesMember(ages[0]), TilesMemberState::Fresh) << id;
    EXPECT_EQ(classifyTilesMember({id, false, 0}), TilesMemberState::Dropped) << id;
  }
}

TEST(TilesMembership, OnlyTheZoomPrefixAgesOut) {
  EXPECT_TRUE(corevideo::compositor::tilesMemberAgesOut("zoom:16778240"));
  EXPECT_FALSE(corevideo::compositor::tilesMemberAgesOut("capture:cam-1"));
  EXPECT_FALSE(corevideo::compositor::tilesMemberAgesOut("media:clip-7"));
  EXPECT_FALSE(corevideo::compositor::tilesMemberAgesOut("16778240"));
}

// ---- [tiles-hold] log latch: per episode, not per transition ------------

namespace {
struct LogCounts { int held = 0; int dropped = 0; int recovered = 0; };
void count(LogCounts& c, const char* label) {
  if (label == nullptr) return;
  const std::string s = label;
  if (s == "held") ++c.held; else if (s == "dropped") ++c.dropped; else if (s == "recovered") ++c.recovered;
}
}  // namespace

// The review's case: a degraded Zoom feed delivering one frame every 1.7 s
// flips Fresh (1.5 s) -> Held (0.2 s) on every frame. Ten frames, 17 ms ticks:
// one `held` line for the whole episode, nothing else.
TEST(TilesMembership, ADegradedFeedFlippingFreshAndHeldLogsHeldOnce) {
  TilesHoldEpisodeLog log;
  LogCounts c;
  int64_t now = 0;
  for (int frame = 0; frame < 10; ++frame) {
    for (int64_t age = 0; age <= 1700; age += 17, now += 17) {
      count(c, log.observe(classifyTilesMember({"zoom:1", true, age}), now));
    }
  }
  EXPECT_EQ(c.held, 1);
  EXPECT_EQ(c.dropped, 0);
  EXPECT_EQ(c.recovered, 0);
}

TEST(TilesMembership, HeldReArmsOnlyAfterContinuousFreshForTheReArmWindow) {
  TilesHoldEpisodeLog log;
  LogCounts c;
  count(c, log.observe(TilesMemberState::Held, 0));
  count(c, log.observe(TilesMemberState::Fresh, 100));
  count(c, log.observe(TilesMemberState::Fresh, 100 + kTilesHoldLogRearmMs - 1));
  count(c, log.observe(TilesMemberState::Held, 100 + kTilesHoldLogRearmMs));
  EXPECT_EQ(c.held, 1) << "fresh for less than the re-arm window must not re-log held";
  count(c, log.observe(TilesMemberState::Fresh, 10'000));
  count(c, log.observe(TilesMemberState::Fresh, 10'000 + kTilesHoldLogRearmMs));
  count(c, log.observe(TilesMemberState::Held, 20'000));
  EXPECT_EQ(c.held, 2) << "a new episode after a real recovery is logged";
  EXPECT_GT(kTilesHoldLogRearmMs, corevideo::compositor::kTilesStaleFrameMs)
      << "a re-arm at or below the stale threshold re-logs every frame of a degraded feed";
}

TEST(TilesMembership, DroppedIsAlwaysLoggedAndRecoveredOnlyAfterADrop) {
  TilesHoldEpisodeLog log;
  LogCounts c;
  count(c, log.observe(TilesMemberState::Held, 0));
  count(c, log.observe(TilesMemberState::Fresh, 17));  // a hold that ends is not a recovery
  EXPECT_EQ(c.recovered, 0);
  count(c, log.observe(TilesMemberState::Held, 34));
  count(c, log.observe(TilesMemberState::Dropped, 51));
  count(c, log.observe(TilesMemberState::Dropped, 68));  // once per drop
  EXPECT_TRUE(!log.heldAnnounced());
  count(c, log.observe(TilesMemberState::Fresh, 85));
  EXPECT_EQ(c.held, 1);
  EXPECT_EQ(c.dropped, 1);
  EXPECT_EQ(c.recovered, 1);
  count(c, log.observe(TilesMemberState::Dropped, 100));
  EXPECT_EQ(c.dropped, 2) << "a later drop is logged again";
}

// #1071: a LOCAL member (capture:/media:) does not age out on a frozen
// frameId — an idle screen share, a paused clip or a still image re-serves the
// same frame while healthy. Only a Zoom member's seat empties at the stale
// threshold. (tilesMemberAgesOut)
TEST(TilesMembership, AStaticLocalMemberStaysWhileItsFrameIsInTheGather) {
  const std::vector<std::string> members{"zoom:1", "capture:screen:1", "media:logo"};
  const std::vector<TilesMemberFrameAge> ages{
      {"zoom:1", true, kTilesHoldWindowMs + 1},  // past #1074's hold window: a Zoom member is dropped
      {"capture:screen:1", true, kTilesStaleFrameMs * 100},
      {"media:logo", true, kTilesStaleFrameMs * 100}};
  const std::vector<std::string> expected{"capture:screen:1", "media:logo"};
  EXPECT_EQ(admitTilesMembers(members, ages), expected);
}

// The fabrication rule is unchanged for a local member: no frame in this
// tick's gather (unplugged, un-routed) means no tile, at once.
TEST(TilesMembership, ALocalMemberWithNoFrameIsStillNotDrawn) {
  const std::vector<TilesMemberFrameAge> ages{{"capture:cam-1", false, 0}};
  EXPECT_TRUE(admitTilesMembers({"capture:cam-1"}, ages).empty());
  EXPECT_TRUE(admitTilesMembers({"media:logo"}, ages).empty()) << "no age entry at all";
}

}  // namespace
