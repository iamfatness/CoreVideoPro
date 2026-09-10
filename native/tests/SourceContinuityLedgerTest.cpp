// native/tests/SourceContinuityLedgerTest.cpp
#include "core/SourceContinuityLedger.h"
#include <gtest/gtest.h>

using corevideo::core::SourceContinuityLedger;

TEST(SourceContinuityLedger, AFirstSightingIsGenerationOne) {
  SourceContinuityLedger ledger;
  ledger.observe("media:bg", 1, 0);
  const auto c = ledger.lookup("media:bg");
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(c->generation, 1u);
  EXPECT_EQ(c->lastFrameId, 1);
}

TEST(SourceContinuityLedger, AdvancingFrameIdsKeepTheGeneration) {
  SourceContinuityLedger ledger;
  for (std::int64_t tick = 0; tick < 100; ++tick) ledger.observe("media:bg", tick + 1, tick);
  EXPECT_EQ(ledger.lookup("media:bg")->generation, 1u);
  EXPECT_EQ(ledger.lookup("media:bg")->lastFrameId, 100);
}

TEST(SourceContinuityLedger, AFrameIdRegressionIsARestart) {
  SourceContinuityLedger ledger;
  ledger.observe("media:clip", 40, 0);
  ledger.observe("media:clip", 41, 1);
  ledger.observe("media:clip", 1, 2);  // decoder reopened
  EXPECT_EQ(ledger.lookup("media:clip")->generation, 2u);
}

TEST(SourceContinuityLedger, AHeldFrameIsNotARestart) {
  SourceContinuityLedger ledger;
  ledger.observe("capture:cam", 7, 0);
  ledger.observe("capture:cam", 7, 1);
  ledger.observe("capture:cam", 7, 2);
  EXPECT_EQ(ledger.lookup("capture:cam")->generation, 1u);
}

TEST(SourceContinuityLedger, ReappearingAfterALongAbsenceIsARestart) {
  SourceContinuityLedger ledger;
  ledger.observe("media:bg", 5, 0);
  ledger.observe("media:bg", 6, SourceContinuityLedger::absentTicksBeforeRestart + 2);
  EXPECT_EQ(ledger.lookup("media:bg")->generation, 2u);
}

TEST(SourceContinuityLedger, ABriefGapIsNotARestart) {
  SourceContinuityLedger ledger;
  ledger.observe("media:bg", 5, 0);
  ledger.observe("media:bg", 6, 3);
  EXPECT_EQ(ledger.lookup("media:bg")->generation, 1u);
}

TEST(SourceContinuityLedger, SnapshotOnlyReturnsKnownSources) {
  SourceContinuityLedger ledger;
  ledger.observe("a", 1, 0);
  const auto snap = ledger.snapshot({"a", "b"});
  EXPECT_EQ(snap.size(), 1u);
  EXPECT_EQ(snap.count("a"), 1u);
}
