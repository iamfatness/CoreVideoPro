#include "core/AnchoredFrameDeadlineTracker.h"
#include <gtest/gtest.h>

using corevideo::core::AnchoredFrameDeadlineTracker;

TEST(AnchoredFrameDeadlineTracker, NanosecondBoundaryAndDuplicateCompletion) {
  AnchoredFrameDeadlineTracker tracker;
  EXPECT_EQ(tracker.nextDeadlineOffsetNs(), 16'666'667);
  EXPECT_TRUE(tracker.recordCompletion(16'666'667));
  EXPECT_FALSE(tracker.recordCompletion(20'000'000));
  EXPECT_EQ(tracker.completedSlots(), 1);
  EXPECT_EQ(tracker.deadlineMisses(), 0);
  tracker.advance(16'666'667);
  EXPECT_EQ(tracker.nextDeadlineOffsetNs(), 33'333'334);
  EXPECT_TRUE(tracker.recordCompletion(33'333'335));
  EXPECT_EQ(tracker.deadlineMisses(), 1);
  EXPECT_EQ(tracker.maximumCompletionLatenessNs(), 1);
}

TEST(AnchoredFrameDeadlineTracker, ThirtyMillisecondCompletionIsAMissWithoutSkippingDebt) {
  AnchoredFrameDeadlineTracker tracker;
  tracker.recordCompletion(30'000'000);
  EXPECT_EQ(tracker.completedSlots(), 1);
  EXPECT_EQ(tracker.deadlineMisses(), 1);
  EXPECT_EQ(tracker.lastCompletionLatenessNs(), 13'333'333);
  EXPECT_EQ(tracker.advance(30'000'000), 0);
  EXPECT_EQ(tracker.nextDeadlineOffsetNs(), 33'333'334);
}

TEST(AnchoredFrameDeadlineTracker, HundredMillisecondStallCountsEveryDiscardedSlot) {
  AnchoredFrameDeadlineTracker tracker;
  tracker.recordCompletion(100'000'000);
  EXPECT_EQ(tracker.advance(100'000'000), 5);
  EXPECT_EQ(tracker.skippedSlots(), 5);
  EXPECT_EQ(tracker.completedSlots(), 1);
  EXPECT_EQ(tracker.deadlineMisses(), 1);
  EXPECT_EQ(tracker.slotIndex(), 6);
  EXPECT_EQ(tracker.nextDeadlineOffsetNs(), 116'666'667);
}

TEST(AnchoredFrameDeadlineTracker, RationalSixtySecondCadenceHasNoAccumulatedDrift) {
  AnchoredFrameDeadlineTracker tracker;
  for (int slot = 0; slot < 3600; ++slot) {
    const auto deadline = tracker.nextDeadlineOffsetNs();
    if (slot == 3599) EXPECT_EQ(deadline, 60'000'000'000LL);
    tracker.recordCompletion(deadline);
    tracker.advance(deadline);
  }
  EXPECT_EQ(tracker.completedSlots(), 3600);
  EXPECT_EQ(tracker.deadlineMisses(), 0);
  EXPECT_EQ(tracker.skippedSlots(), 0);
}

TEST(AnchoredFrameDeadlineTracker, UnfinishedSlotIsExplicitlySkippedAndPartialCountersRemain) {
  AnchoredFrameDeadlineTracker tracker;
  EXPECT_EQ(tracker.advance(16'666'667), 1);
  EXPECT_EQ(tracker.skippedSlots(), 1);
  tracker.recordCompletion(35'000'000);
  EXPECT_EQ(tracker.completedSlots(), 1);
  EXPECT_EQ(tracker.deadlineMisses(), 1);
}
