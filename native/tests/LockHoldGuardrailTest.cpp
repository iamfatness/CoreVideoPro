#include "core/LockHoldGuardrail.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

using corevideo::core::LockHoldGuardrail;
using corevideo::core::ScopedLockHoldTimer;

// Phase 2 increment 6: the coreMutex hold-duration guardrail. Semantics under
// test: per-site telemetry counters, the sub-ms default budget, sanctioned
// long-hold budgets, and the rate-capped warning (never a hard failure by
// default — strict mode is opt-in via COREVIDEO_LOCK_GUARDRAIL_STRICT).

TEST(LockHoldGuardrail, UnderBudgetHoldCountsButNeverWarns) {
  LockHoldGuardrail::resetForTest();

  EXPECT_FALSE(LockHoldGuardrail::recordHold("test.under", 500, LockHoldGuardrail::kDefaultBudgetUs));
  EXPECT_FALSE(LockHoldGuardrail::recordHold("test.under", 999, LockHoldGuardrail::kDefaultBudgetUs));

  const auto stats = LockHoldGuardrail::statsForSite("test.under");
  EXPECT_EQ(stats.holds, 2u);
  EXPECT_EQ(stats.overBudget, 0u);
  EXPECT_EQ(stats.warningsLogged, 0u);
  EXPECT_EQ(stats.worstHeldUs, 999);
}

TEST(LockHoldGuardrail, OverBudgetHoldCountsAndRateCapsWarnings) {
  LockHoldGuardrail::resetForTest();

  // A burst of over-budget holds inside one second must log at most a couple of
  // warnings (first occurrence + at most one per second), never one per hold.
  for (int i = 0; i < 200; ++i) {
    EXPECT_TRUE(LockHoldGuardrail::recordHold("test.over", 5'000, LockHoldGuardrail::kDefaultBudgetUs));
  }

  const auto stats = LockHoldGuardrail::statsForSite("test.over");
  EXPECT_EQ(stats.holds, 200u);
  EXPECT_EQ(stats.overBudget, 200u);
  EXPECT_GE(stats.warningsLogged, 1u);
  EXPECT_LE(stats.warningsLogged, 3u);  // rate cap: not one warning per hold
  EXPECT_EQ(stats.worstHeldUs, 5'000);
}

TEST(LockHoldGuardrail, SanctionedSiteBudgetsAllowLongerHolds) {
  LockHoldGuardrail::resetForTest();

  // 5ms is over the sub-ms default but sanctioned for the command-handle site.
  EXPECT_TRUE(LockHoldGuardrail::recordHold("test.cmd", 5'000, LockHoldGuardrail::kDefaultBudgetUs));
  EXPECT_FALSE(
      LockHoldGuardrail::recordHold("test.cmd", 5'000, LockHoldGuardrail::kCommandHandleBudgetUs));
  EXPECT_FALSE(LockHoldGuardrail::recordHold("test.render", 5'000, LockHoldGuardrail::kRenderTickBudgetUs));
  EXPECT_TRUE(LockHoldGuardrail::recordHold("test.render", 9'000, LockHoldGuardrail::kRenderTickBudgetUs));
}

TEST(LockHoldGuardrail, SitesAreTrackedIndependently) {
  LockHoldGuardrail::resetForTest();

  (void)LockHoldGuardrail::recordHold("test.a", 10, LockHoldGuardrail::kDefaultBudgetUs);
  (void)LockHoldGuardrail::recordHold("test.b", 2'000, LockHoldGuardrail::kDefaultBudgetUs);

  EXPECT_EQ(LockHoldGuardrail::statsForSite("test.a").overBudget, 0u);
  EXPECT_EQ(LockHoldGuardrail::statsForSite("test.b").overBudget, 1u);
  EXPECT_EQ(LockHoldGuardrail::statsForSite("test.unknown").holds, 0u);
}

TEST(LockHoldGuardrail, ScopedTimerMeasuresRealHoldDuration) {
  LockHoldGuardrail::resetForTest();

  {
    ScopedLockHoldTimer timer("test.scoped", LockHoldGuardrail::kDefaultBudgetUs);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  const auto stats = LockHoldGuardrail::statsForSite("test.scoped");
  EXPECT_EQ(stats.holds, 1u);
  EXPECT_EQ(stats.overBudget, 1u);  // 5ms sleep > 1ms budget
  EXPECT_GE(stats.worstHeldUs, 4'000);
}
