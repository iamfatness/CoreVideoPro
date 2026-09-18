#include "modules/IsoStatusRefreshPolicy.h"

#ifdef COREVIDEO_USE_SYSTEM_GTEST
#include <gtest/gtest.h>
#else
#include "gtest/gtest.h"
#endif

using corevideo::modules::IsoStatusRefreshGate;

TEST(IsoStatusRefreshGate, TheFirstStatusIsAlwaysPublished) {
  // Until the first rebuild runs, the snapshot still carries the PREVIOUS
  // take's ISO rows. Waiting out an interval before publishing the new ones
  // would show an operator the wrong recording's health at the moment they are
  // most likely looking: the instant they hit record.
  IsoStatusRefreshGate gate;
  EXPECT_FALSE(gate.everRefreshed());
  EXPECT_TRUE(gate.allowMediaRefresh(0));
  EXPECT_TRUE(gate.allowMediaRefresh(5'000));
}

TEST(IsoStatusRefreshGate, MediaDrivenRebuildsAreRateLimitedToTheInterval) {
  IsoStatusRefreshGate gate(100);
  ASSERT_TRUE(gate.allowMediaRefresh(1'000));
  gate.markRefreshed(1'000);

  // The next ~6 frames at 60fps arrive inside the interval and must not rebuild.
  EXPECT_FALSE(gate.allowMediaRefresh(1'001));
  EXPECT_FALSE(gate.allowMediaRefresh(1'050));
  EXPECT_FALSE(gate.allowMediaRefresh(1'099));
  // The boundary itself is allowed, so the rate is "at least every interval"
  // rather than "strictly more than".
  EXPECT_TRUE(gate.allowMediaRefresh(1'100));
  EXPECT_TRUE(gate.allowMediaRefresh(1'400));
}

TEST(IsoStatusRefreshGate, AtEightIsosAndSixtyFpsTheRebuildRateCollapses) {
  // The whole point of #529, expressed as the number that changes: one second
  // of eight ISO sources plus Program at 60fps is 540 dispatched items. Every
  // one of them used to rebuild all eight ISO status records.
  IsoStatusRefreshGate gate(100);
  int rebuilds = 0;
  for (int item = 0; item < 540; ++item) {
    // 540 items spread evenly across 1000 ms.
    const int64_t nowMs = static_cast<int64_t>(item) * 1000 / 540;
    if (gate.allowMediaRefresh(nowMs)) {
      ++rebuilds;
      gate.markRefreshed(nowMs);
    }
  }
  // Ten a second (plus the mandatory first one), not 540.
  EXPECT_LE(rebuilds, 11);
  EXPECT_GE(rebuilds, 10);
}

TEST(IsoStatusRefreshGate, AStructuralRebuildResetsTheSameClock) {
  // Open and close rebuild unconditionally. They mark the gate so a media item
  // arriving immediately afterwards does not rebuild all over again — one
  // writer of the timestamp, whatever the reason for the rebuild.
  IsoStatusRefreshGate gate(100);
  gate.markRefreshed(2'000);            // e.g. the structural refresh in start()
  EXPECT_FALSE(gate.allowMediaRefresh(2'010));
  EXPECT_TRUE(gate.allowMediaRefresh(2'100));
}

TEST(IsoStatusRefreshGate, ABackwardsClockOpensTheGateRatherThanLatchingItShut) {
  // Fail toward refreshing: an extra rebuild costs a rebuild, a stuck gate
  // costs status that never updates again for the life of the recording.
  IsoStatusRefreshGate gate(100);
  gate.markRefreshed(10'000);
  EXPECT_FALSE(gate.allowMediaRefresh(10'050));
  EXPECT_TRUE(gate.allowMediaRefresh(9'000));
}

TEST(IsoStatusRefreshGate, AZeroIntervalRestoresTheUnthrottledBehaviour) {
  // Kept as a real option so a diagnostic build can compare against the old
  // rate without a code edit.
  IsoStatusRefreshGate gate(0);
  gate.markRefreshed(1'000);
  EXPECT_TRUE(gate.allowMediaRefresh(1'000));
  EXPECT_TRUE(gate.allowMediaRefresh(1'001));
  EXPECT_EQ(gate.intervalMs(), 0);
}

TEST(IsoStatusRefreshGate, ANegativeIntervalIsClampedNotTrusted) {
  IsoStatusRefreshGate gate(-50);
  EXPECT_EQ(gate.intervalMs(), 0);
}
