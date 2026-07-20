// Respawn backoff for the isolated VST3 host (VST round-2 spec A1).
//
// The scenario these pin: a crash-on-load plugin kills the host moments after
// every spawn. Without the policy the core hot-looped CreateProcess for the
// rest of the show; with it, retries ride the house ladder (5→10→20→40→60s),
// the 6th spawn never happens (give up → insert stays loudly auto-bypassed),
// and an operator action always recovers via reset().

#include "modules/PluginHostRespawnPolicy.h"

#include <gtest/gtest.h>

using corevideo::modules::PluginHostRespawnPolicy;

TEST(PluginHostRespawnPolicy, LadderMatchesHousePattern) {
  EXPECT_EQ(PluginHostRespawnPolicy::backoffMsForFailureCount(0), 5000);
  EXPECT_EQ(PluginHostRespawnPolicy::backoffMsForFailureCount(1), 10000);
  EXPECT_EQ(PluginHostRespawnPolicy::backoffMsForFailureCount(2), 20000);
  EXPECT_EQ(PluginHostRespawnPolicy::backoffMsForFailureCount(3), 40000);
  EXPECT_EQ(PluginHostRespawnPolicy::backoffMsForFailureCount(4), 60000);
  EXPECT_EQ(PluginHostRespawnPolicy::backoffMsForFailureCount(12), 60000);   // capped
  EXPECT_EQ(PluginHostRespawnPolicy::backoffMsForFailureCount(62), 60000);   // shift overflow guard
}

TEST(PluginHostRespawnPolicy, FirstStartIsImmediateAndSingleFlight) {
  PluginHostRespawnPolicy policy;
  EXPECT_TRUE(policy.requestStart(1000));
  // Launch in flight: concurrent requests are denied until the result lands.
  EXPECT_FALSE(policy.requestStart(1001));
  policy.onLaunchResult(1200, true);
  EXPECT_EQ(policy.consecutiveFailures(), 0);
  EXPECT_FALSE(policy.gaveUp());
}

TEST(PluginHostRespawnPolicy, CrashLoopClimbsLadderThenGivesUp) {
  PluginHostRespawnPolicy policy;
  int64_t now = 0;
  ASSERT_TRUE(policy.requestStart(now));
  policy.onLaunchResult(now, true);

  const int64_t expectedDelays[] = {5000, 10000, 20000, 40000, 60000};
  for (int retry = 0; retry < 5; ++retry) {
    now += 1000;  // host died ~1s after launch — a short (failed) run
    EXPECT_FALSE(policy.requestStart(now)) << "death classification denies, arms backoff";
    EXPECT_EQ(policy.consecutiveFailures(), retry + 1);
    EXPECT_EQ(policy.nextAttemptAtMs(), now + expectedDelays[retry]);
    // Denied all through the backoff window...
    EXPECT_FALSE(policy.requestStart(now + expectedDelays[retry] - 1));
    // ...allowed once it elapses.
    now += expectedDelays[retry];
    if (retry < 4) {
      ASSERT_TRUE(policy.requestStart(now));
      policy.onLaunchResult(now, true);
    }
  }
  // 5th retry: allowed after the 60s wait, spawns, dies — and that exhausts
  // the ladder.
  ASSERT_TRUE(policy.requestStart(now));
  policy.onLaunchResult(now, true);
  now += 1000;
  EXPECT_FALSE(policy.requestStart(now));
  EXPECT_TRUE(policy.gaveUp());
  // Given up: denied forever, no clock value re-enables it.
  EXPECT_FALSE(policy.requestStart(now + 10'000'000));
}

TEST(PluginHostRespawnPolicy, HealthyRunResetsTheLadder) {
  PluginHostRespawnPolicy policy;
  ASSERT_TRUE(policy.requestStart(0));
  policy.onLaunchResult(0, true);

  // Two quick deaths climb to failures=2.
  EXPECT_FALSE(policy.requestStart(1000));
  ASSERT_TRUE(policy.requestStart(1000 + 5000));
  policy.onLaunchResult(6000, true);
  EXPECT_FALSE(policy.requestStart(7000));
  EXPECT_EQ(policy.consecutiveFailures(), 2);

  // The next retry stays up past the healthy threshold (e.g. the plugin loads
  // fine now, or the host later idle-exits by design): the death both resets
  // the ladder AND respawns immediately — the 30s idle-exit cycle must never
  // pay a backoff.
  ASSERT_TRUE(policy.requestStart(7000 + 10000));
  policy.onLaunchResult(17000, true);
  const int64_t afterHealthy = 17000 + PluginHostRespawnPolicy::kHealthyRunMs + 1;
  EXPECT_TRUE(policy.requestStart(afterHealthy));
  EXPECT_EQ(policy.consecutiveFailures(), 0);
  policy.onLaunchResult(afterHealthy, true);
}

TEST(PluginHostRespawnPolicy, LaunchFailureCountsOnTheSameLadder) {
  PluginHostRespawnPolicy policy;
  ASSERT_TRUE(policy.requestStart(0));
  policy.onLaunchResult(0, false);  // CreateProcess failed
  EXPECT_EQ(policy.consecutiveFailures(), 1);
  EXPECT_FALSE(policy.requestStart(4999));
  EXPECT_TRUE(policy.requestStart(5000));
  policy.onLaunchResult(5000, false);
  EXPECT_EQ(policy.consecutiveFailures(), 2);
  EXPECT_EQ(policy.nextAttemptAtMs(), 5000 + 10000);
}

TEST(PluginHostRespawnPolicy, OperatorResetRecoversFromGiveUp) {
  PluginHostRespawnPolicy policy;
  // Drive straight to give-up via repeated launch failures: 5 failures arm
  // the full 5→60s ladder; the 6th trips give-up (same spawn budget as the
  // crash-loop path: one initial + five retries).
  int64_t now = 0;
  for (int attempt = 0; attempt < 6; ++attempt) {
    ASSERT_TRUE(policy.requestStart(now)) << "attempt " << attempt;
    policy.onLaunchResult(now, false);
    now = policy.nextAttemptAtMs();
  }
  EXPECT_TRUE(policy.gaveUp());
  EXPECT_FALSE(policy.requestStart(now + 1));

  // Operator re-selects the insert / clicks Open controls: fully recovers.
  policy.reset();
  EXPECT_FALSE(policy.gaveUp());
  EXPECT_EQ(policy.consecutiveFailures(), 0);
  EXPECT_TRUE(policy.requestStart(now + 2));
  policy.onLaunchResult(now + 2, true);
}
