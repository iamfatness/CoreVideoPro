#include "compositor/TilesMembership.h"

#include <gtest/gtest.h>

namespace {

using corevideo::compositor::admitTilesMembers;
using corevideo::compositor::kTilesStaleFrameMs;
using corevideo::compositor::TilesMemberFrameAge;

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

TEST(TilesMembership, AStaleFeedLeavesTheWall) {
  const std::vector<TilesMemberFrameAge> ages{{"zoom:1", true, kTilesStaleFrameMs + 1}};
  EXPECT_TRUE(admitTilesMembers({"zoom:1"}, ages).empty());
}

// Lock the inclusive boundary: a member at EXACTLY the threshold is still live.
// The threshold exists to prevent reflow on ordinary frame gaps, so treating "exactly
// at the limit" as still-live is the conservative direction — it errs toward stability
// rather than toward dropping a tile.
TEST(TilesMembership, AMemberExactlyAtTheThresholdIsStillAdmitted) {
  const std::vector<TilesMemberFrameAge> ages{{"zoom:1", true, kTilesStaleFrameMs}};
  EXPECT_EQ(admitTilesMembers({"zoom:1"}, ages).size(), 1u);
}

// Re-admission is immediate: one fresh frame puts the guest back, in their
// original slot order.
TEST(TilesMembership, AReturningFeedIsReadmittedInOriginalOrder) {
  const std::vector<std::string> members{"zoom:1", "zoom:2", "zoom:3"};
  const std::vector<TilesMemberFrameAge> stale{
      {"zoom:1", true, 16}, {"zoom:2", true, kTilesStaleFrameMs + 500}, {"zoom:3", true, 16}};
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

}  // namespace
