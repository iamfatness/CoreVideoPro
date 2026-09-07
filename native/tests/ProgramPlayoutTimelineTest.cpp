#include "core/ProgramPlayoutTimeline.h"
#include <gtest/gtest.h>
using corevideo::core::ProgramPlayoutTimeline;

TEST(ProgramPlayoutTimeline, ThreeFramePrefillWaitsExactFiftyMilliseconds) {
  ProgramPlayoutTimeline time(3, 1000);
  EXPECT_EQ(time.nextDeadlineNs(), 50'001'000);
  EXPECT_FALSE(time.takeDue(50'000'999).has_value());
  const auto due = time.takeDue(50'001'000);
  ASSERT_TRUE(due.has_value());
  EXPECT_EQ(due->slot, 0);
  EXPECT_EQ(due->deadlineNs, 50'001'000);
  EXPECT_EQ(due->skippedSlots, 0);
  EXPECT_FALSE(time.takeDue(50'001'000).has_value());
}
TEST(ProgramPlayoutTimeline, TwoFrameDelayUsesRationalNanosecondBoundaries) {
  ProgramPlayoutTimeline time(2, 0);
  EXPECT_EQ(time.deadlineNs(0), 33'333'334);
  EXPECT_EQ(time.productionSlot(16'666'666), 0);
  EXPECT_EQ(time.productionSlot(16'666'667), 1);
  EXPECT_FALSE(time.takeDue(33'333'333).has_value());
  ASSERT_TRUE(time.takeDue(33'333'334).has_value());
  EXPECT_EQ(time.nextDeadlineNs(), 50'000'000);
}
TEST(ProgramPlayoutTimeline, StallDiscardsExpiredSlotsWithoutMovingOriginalAnchor) {
  ProgramPlayoutTimeline time(3, 0);
  const auto due = time.takeDue(100'000'000);
  ASSERT_TRUE(due.has_value());
  EXPECT_EQ(due->slot, 3);
  EXPECT_EQ(due->deadlineNs, 100'000'000);
  EXPECT_EQ(due->skippedSlots, 3);
  EXPECT_TRUE(time.isExpired(time.productionSlot(0)));
  EXPECT_TRUE(time.isExpired(time.productionSlot(33'333'334)));
  EXPECT_EQ(time.nextDeadlineNs(), 116'666'667);
  EXPECT_EQ(time.anchorNs(), 0);
  EXPECT_EQ(time.skippedSlots(), 3);
  const auto recovery = time.takeDue(116'666'667);
  ASSERT_TRUE(recovery.has_value());
  EXPECT_EQ(recovery->slot, 4);
  EXPECT_EQ(recovery->skippedSlots, 0);
}
TEST(ProgramPlayoutTimeline, MissingPacketCannotBeRetriedAfterItsDeliveryDecision) {
  ProgramPlayoutTimeline time(3, 0);
  ASSERT_TRUE(time.takeDue(50'000'000).has_value());
  EXPECT_TRUE(time.isExpired(0));
  EXPECT_FALSE(time.takeDue(51'000'000).has_value());
  EXPECT_EQ(time.nextSlot(), 1);
}
TEST(ProgramPlayoutTimeline, SixtySecondProductionKeepsExactRationalDelay) {
  ProgramPlayoutTimeline time(3, 0);
  EXPECT_EQ(time.productionSlot(60'000'000'000LL), 3600);
  EXPECT_EQ(time.deadlineNs(3600), 60'050'000'000LL);
  EXPECT_EQ(time.deadlineNs(3597), 60'000'000'000LL);
}
TEST(ProgramPlayoutTimeline, PreAnchorPacketsAreRejectedAndDefaultDepthIsThree) {
  ProgramPlayoutTimeline time(0, 1'000'000);
  EXPECT_EQ(time.bufferFrames(), 3);
  EXPECT_EQ(time.productionSlot(999'999), -1);
  EXPECT_TRUE(time.isExpired(-1));
}
TEST(ProgramPlayoutTimeline, ReplacementStartsAtGlobalSlotWithoutHistoricalLoss) {
  for (const int depth : {2, 3}) {
    ProgramPlayoutTimeline time(depth, 1000, 3600);
    const auto deadline = 60'000'001'000LL + (depth == 2 ? 33'333'334LL : 50'000'000LL);
    EXPECT_EQ(time.anchorNs(), 1000);
    EXPECT_EQ(time.nextSlot(), 3600);
    EXPECT_EQ(time.nextDeadlineNs(), deadline);
    EXPECT_TRUE(time.isExpired(3599));
    EXPECT_FALSE(time.isExpired(3600));
    EXPECT_FALSE(time.takeDue(deadline - 1).has_value());
    const auto due = time.takeDue(deadline);
    ASSERT_TRUE(due.has_value());
    EXPECT_EQ(due->slot, 3600);
    EXPECT_EQ(due->skippedSlots, 0);
    EXPECT_EQ(time.skippedSlots(), 0);
  }
}
TEST(ProgramPlayoutTimeline, ReplacementCountsOnlySlotsLostAfterInitialization) {
  ProgramPlayoutTimeline time(3, 1000, 3600);
  const auto due = time.takeDue(time.deadlineNs(3603));
  ASSERT_TRUE(due.has_value());
  EXPECT_EQ(due->slot, 3603);
  EXPECT_EQ(due->skippedSlots, 3);
  EXPECT_EQ(time.skippedSlots(), 3);
  EXPECT_EQ(time.nextSlot(), 3604);
  EXPECT_EQ(time.nextDeadlineNs(), time.deadlineNs(3604));
}
TEST(ProgramPlayoutTimeline, NegativeInitialSlotClampsToZero) {
  ProgramPlayoutTimeline time(3, 1000, -5);
  EXPECT_EQ(time.nextSlot(), 0);
  EXPECT_EQ(time.nextDeadlineNs(), 50'001'000);
  EXPECT_EQ(time.skippedSlots(), 0);
}
