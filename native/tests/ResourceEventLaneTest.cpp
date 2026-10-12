#include "rpc/ResourceEventLane.h"
#include <gtest/gtest.h>

using corevideo::rpc::ResourceEventLane;

TEST(ResourceEventLane, InitialRosterCannotDiscardProgramOrOtherSurfaceAnnouncements) {
  ResourceEventLane lane;
  EXPECT_TRUE(lane.push("program", "program-handle").admitted);
  for (int i=0; i<10; ++i) EXPECT_TRUE(lane.push("participant-" + std::to_string(i), "surface").admitted);
  EXPECT_TRUE(lane.push("preview", "preview-handle").admitted);
  EXPECT_TRUE(lane.push("multiview", "wall-handle").admitted);
  EXPECT_EQ(lane.size(), 13u); EXPECT_EQ(lane.refused(), 0u);
  EXPECT_EQ(lane.popFront().message, "program-handle");
}
TEST(ResourceEventLane, ReplacementRetiresOutsideCallerLockAndKeepsLatestAnnouncementOrder) {
  ResourceEventLane lane;
  EXPECT_TRUE(lane.push("program", "old").admitted);
  EXPECT_TRUE(lane.push("preview", "preview").admitted);
  auto result = lane.push("program", "new");
  EXPECT_TRUE(result.admitted); EXPECT_EQ(result.retired, "old");
  EXPECT_EQ(lane.size(), 2u); EXPECT_EQ(lane.coalesced(), 1u);
  EXPECT_EQ(lane.popFront().message, "preview");
  EXPECT_EQ(lane.popFront().message, "new"); EXPECT_EQ(lane.bytes(), 0u);
}
TEST(ResourceEventLane, PressureRefusesNewKeysWithoutEvictingExistingResourceIdentity) {
  ResourceEventLane lane;
  for (int i=0; i<ResourceEventLane::Capacity; ++i) EXPECT_TRUE(lane.push(std::to_string(i), "handle").admitted);
  EXPECT_FALSE(lane.push("overflow", "not-admitted").admitted);
  EXPECT_TRUE(lane.push("0", "replacement").admitted);
  EXPECT_EQ(lane.size(), ResourceEventLane::Capacity); EXPECT_EQ(lane.refused(), 1u);
  EXPECT_FALSE(lane.push("0", std::string(ResourceEventLane::ByteBudget + 1, 'x')).admitted);
  EXPECT_LE(lane.bytes(), ResourceEventLane::ByteBudget);
}
