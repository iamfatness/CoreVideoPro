#include "modules/EncoderSessionReadPolicy.h"

#include <gtest/gtest.h>

using corevideo::modules::EncoderSessionReadGate;

TEST(EncoderSessionReadGate, StructuralItemsAlwaysRead) {
  // Start latches the baseline frame count, Stop's barrier decides whether the
  // take may claim completion. Neither may be deferred, and neither happens at
  // media rate, so there is nothing to save by throttling them.
  EncoderSessionReadGate gate;
  gate.markRead(1'000);
  EXPECT_FALSE(gate.shouldRead(/*structural=*/false, /*queueDrained=*/false, /*failed=*/false, 1'010));
  EXPECT_TRUE(gate.shouldRead(/*structural=*/true, /*queueDrained=*/false, /*failed=*/false, 1'010));
}

TEST(EncoderSessionReadGate, AFailureAlwaysReads) {
  EncoderSessionReadGate gate;
  gate.markRead(1'000);
  EXPECT_TRUE(gate.shouldRead(false, false, /*failed=*/true, 1'010));
}

TEST(EncoderSessionReadGate, ADrainedBurstIsTheSettledStateAndAlwaysReads) {
  // The primary mechanism. A drained queue is the only moment a settled reading
  // exists, and drainForTest waits for exactly that state.
  EncoderSessionReadGate gate;
  gate.markRead(1'000);
  EXPECT_TRUE(gate.shouldRead(false, /*queueDrained=*/true, false, 1'001));
}

TEST(EncoderSessionReadGate, MidBurstItemsDoNotRead) {
  EncoderSessionReadGate gate;
  gate.markRead(1'000);
  for (int64_t nowMs = 1'001; nowMs < 1'100; nowMs += 10) {
    EXPECT_FALSE(gate.shouldRead(false, false, false, nowMs)) << "at " << nowMs;
  }
}

TEST(EncoderSessionReadGate, ASustainedBacklogStillRefreshesOnTheBackstop) {
  // A queue that never drains must not leave the published snapshot frozen for
  // the life of the recording — which is exactly the overload case an operator
  // most needs to see.
  EncoderSessionReadGate gate;
  gate.markRead(1'000);
  EXPECT_FALSE(gate.shouldRead(false, false, false, 1'099));
  EXPECT_TRUE(gate.shouldRead(false, false, false, 1'100));
}

TEST(EncoderSessionReadGate, TheBurstRuleCollapsesTheReadRateAtEightIsos) {
  // The number that motivated the change: MediaCore::renderIsoVideoTick drains
  // everything pending, so one 60Hz tick enqueues ~9 items (eight ISO plus
  // Program) as a burst. Reading once per drained burst instead of once per
  // item is ~9x fewer reads, and the saving grows with the ISO count.
  EncoderSessionReadGate gate;
  int reads = 0;
  int64_t nowMs = 0;
  constexpr int kTicks = 60;      // one second
  constexpr int kBurst = 9;       // eight ISO sources + Program
  for (int tick = 0; tick < kTicks; ++tick) {
    for (int itemIndex = 0; itemIndex < kBurst; ++itemIndex) {
      const bool drained = itemIndex == kBurst - 1;
      if (gate.shouldRead(false, drained, false, nowMs)) {
        ++reads;
        gate.markRead(nowMs);
      }
    }
    nowMs += 1000 / kTicks;
  }
  // 61: one per drained burst, plus the very first item, which must establish
  // a snapshot before any burst has completed. Against 540 reads (one per item)
  // for the same second of media.
  EXPECT_EQ(reads, kTicks + 1);
}

TEST(EncoderSessionReadGate, TheFirstItemEstablishesASnapshot) {
  // Nothing has been published yet, so there is no stale value to preserve.
  EncoderSessionReadGate gate;
  EXPECT_FALSE(gate.everRead());
  EXPECT_TRUE(gate.shouldRead(false, false, false, 5'000));
}

TEST(EncoderSessionReadGate, ABackwardsClockOpensTheGateRatherThanLatchingItShut) {
  EncoderSessionReadGate gate;
  gate.markRead(10'000);
  EXPECT_FALSE(gate.shouldRead(false, false, false, 10'050));
  EXPECT_TRUE(gate.shouldRead(false, false, false, 9'000));
}

TEST(EncoderSessionReadGate, AZeroIntervalRestoresTheReadEveryItemBehaviour) {
  EncoderSessionReadGate gate(0);
  gate.markRead(1'000);
  EXPECT_TRUE(gate.shouldRead(false, false, false, 1'000));
  EXPECT_EQ(gate.staleAfterMs(), 0);
}

TEST(EncoderSessionReadGate, ANegativeIntervalIsClampedNotTrusted) {
  EncoderSessionReadGate gate(-50);
  EXPECT_EQ(gate.staleAfterMs(), 0);
}
