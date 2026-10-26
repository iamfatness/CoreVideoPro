#include "core/LockHoldGuardrail.h"
#include "core/RenderTickPhases.h"
#include "modules/EngineMutexGuardrail.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
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

TEST(LockHoldGuardrail, OnlyRenderWorkRetainsTheLongerSanctionedBudget) {
  LockHoldGuardrail::resetForTest();

  // Projection/serialization moved off-lock; command capture no longer gets
  // the former 50ms exemption. Render work retains its explicit GPU budget.
  EXPECT_TRUE(LockHoldGuardrail::recordHold("test.cmd", 5'000, LockHoldGuardrail::kDefaultBudgetUs));
  EXPECT_TRUE(
      LockHoldGuardrail::recordHold("test.cmd", 5'000, LockHoldGuardrail::kCommandHandleBudgetUs));
  EXPECT_TRUE(LockHoldGuardrail::recordHold("test.pump", 2'000, LockHoldGuardrail::kFramePumpBudgetUs));
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

// #1056: an over-budget hold that carries a detail (the render tick's phase
// breakdown) logs WHERE the time went, on every over-budget hold up to one
// per second, and never formats the detail for an in-budget hold.
namespace {
class CountingDetail final : public corevideo::core::LockHoldDetail {
 public:
  std::string describe(long long heldUs) const override {
    ++calls;
    return "phases_us=[program=" + std::to_string(heldUs) + "]";
  }
  mutable int calls = 0;
};
}  // namespace

TEST(LockHoldGuardrail, OverBudgetHoldWithDetailRecordsItsAttribution) {
  LockHoldGuardrail::resetForTest();
  CountingDetail detail;

  EXPECT_FALSE(LockHoldGuardrail::recordHold("test.detail", 7'000, LockHoldGuardrail::kRenderTickBudgetUs, &detail));
  EXPECT_EQ(detail.calls, 0) << "an in-budget hold must not format its detail";
  EXPECT_EQ(LockHoldGuardrail::statsForSite("test.detail").attributionsLogged, 0u);

  EXPECT_TRUE(LockHoldGuardrail::recordHold("test.detail", 15'127, LockHoldGuardrail::kRenderTickBudgetUs, &detail));
  const auto stats = LockHoldGuardrail::statsForSite("test.detail");
  EXPECT_EQ(stats.attributionsLogged, 1u);
  EXPECT_NE(stats.lastAttribution.find("program=15127"), std::string::npos) << stats.lastAttribution;
  EXPECT_NE(stats.lastAttribution.find("held_us=15127"), std::string::npos) << stats.lastAttribution;
}

TEST(LockHoldGuardrail, AttributionIsRateCappedPerSecondNotPerMinute) {
  LockHoldGuardrail::resetForTest();
  CountingDetail detail;
  for (int i = 0; i < 200; ++i)
    (void)LockHoldGuardrail::recordHold("test.burst", 9'000, LockHoldGuardrail::kRenderTickBudgetUs, &detail);
  const auto stats = LockHoldGuardrail::statsForSite("test.burst");
  EXPECT_GE(stats.attributionsLogged, 1u);
  EXPECT_LE(stats.attributionsLogged, 2u) << "a burst inside one second logs one attribution, not one per hold";
  EXPECT_LE(detail.calls, 3) << "suppressed holds must not pay for formatting";
}

TEST(RenderTickPhases, DescribesEveryPhaseItsEngineWaitAndTheUnattributedRest) {
  using corevideo::core::RenderTickPhases;
  using Phase = RenderTickPhases::Phase;
  RenderTickPhases phases;
  const auto t0 = std::chrono::steady_clock::time_point{} + std::chrono::seconds(10);
  phases.begin(t0);
  phases.setSlot(172717);
  phases.setOutsideHold(/*preReadNs=*/400'000, /*lockWaitNs=*/2'000'000);
  phases.mark(Phase::R1Poll, t0 + std::chrono::microseconds(100));
  phases.mark(Phase::R2Decoded, t0 + std::chrono::microseconds(3'100));
  phases.addEngineWait(Phase::R2Decoded, 2'500'000);
  phases.mark(Phase::Program, t0 + std::chrono::microseconds(12'100));
  EXPECT_TRUE(phases.visited(Phase::R2Decoded));
  EXPECT_FALSE(phases.visited(Phase::R5CpuArrival));
  EXPECT_EQ(phases.attributedNs(), 12'100'000);
  EXPECT_EQ(phases.frameGroupEngineWaitNs(), 2'500'000);

  const auto text = phases.describe(15'127);
  EXPECT_NE(text.find("slot=172717"), std::string::npos) << text;
  EXPECT_NE(text.find("r1-poll=100/wait=0"), std::string::npos) << text;
  EXPECT_NE(text.find("r2-decoded=3000/wait=2500"), std::string::npos) << text;
  EXPECT_NE(text.find("program=9000"), std::string::npos) << text;
  EXPECT_NE(text.find("other_us=3027"), std::string::npos) << text;
  EXPECT_NE(text.find("frame_group_engine_wait_us=2500"), std::string::npos) << text;
  EXPECT_NE(text.find("pre-read=400 lock-wait=2000"), std::string::npos) << text;
  EXPECT_EQ(text.find("setup="), std::string::npos) << "an empty phase is omitted: " << text;

  // begin() starts a fresh tick.
  phases.begin(t0);
  EXPECT_EQ(phases.attributedNs(), 0);
  EXPECT_FALSE(phases.visited(Phase::R2Decoded));
}

TEST(RenderTickPhases, MarksOnlyReachTheInstanceInstalledOnThisThread) {
  using corevideo::core::RenderTickPhases;
  RenderTickPhases phases;
  phases.begin();
  RenderTickPhases::markCurrent(RenderTickPhases::Phase::Program);  // nothing installed
  EXPECT_FALSE(phases.visited(RenderTickPhases::Phase::Program));
  {
    RenderTickPhases::Scope scope(phases);
    std::thread other([] { RenderTickPhases::markCurrent(RenderTickPhases::Phase::Emit); });
    other.join();
    RenderTickPhases::markCurrent(RenderTickPhases::Phase::Program);
  }
  EXPECT_TRUE(phases.visited(RenderTickPhases::Phase::Program));
  EXPECT_FALSE(phases.visited(RenderTickPhases::Phase::Emit)) << "another thread must not mark this tick";
  EXPECT_EQ(RenderTickPhases::current(), nullptr);
}

// #1056: the engine mutex reports how long THIS thread waited for it, so a
// frame-group call's time splits into wait (contention) and body.
TEST(EngineMutexGuardrail, ContendedLockAccumulatesThisThreadsWait) {
  corevideo::modules::EngineMutex mutex;
  std::atomic<bool> held{false};
  std::thread owner([&] {
    std::lock_guard<corevideo::modules::EngineMutex> lock(mutex);
    held = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  });
  while (!held.load()) std::this_thread::yield();
  const auto before = corevideo::modules::EngineMutexGuardrail::threadWaitNs();
  { std::lock_guard<corevideo::modules::EngineMutex> lock(mutex); }
  const auto waited = corevideo::modules::EngineMutexGuardrail::threadWaitNs() - before;
  owner.join();
  EXPECT_GE(waited, 10'000'000) << "waited " << waited << " ns behind a 20 ms holder";

  const auto uncontended = corevideo::modules::EngineMutexGuardrail::threadWaitNs();
  { std::lock_guard<corevideo::modules::EngineMutex> lock(mutex); }
  EXPECT_EQ(corevideo::modules::EngineMutexGuardrail::threadWaitNs(), uncontended)
      << "an uncontended acquisition waits for nothing";
}
