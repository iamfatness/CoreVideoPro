// VcamHelperRespawnPolicy: when the core starts the next virtual-camera sink
// helper (plan 2026-11-16-mac-vcam-provider-arrival-fix, Task 3 design B).
#include "modules/VcamHelperRespawnPolicy.h"
#include "modules/VcamSinkHelper.h"

#include <gtest/gtest.h>

using namespace corevideo::modules;

TEST(VcamHelperRespawnPolicy, TheFirstAttemptIsImmediate) {
  VcamHelperRespawnPolicy policy;
  const auto d = policy.next(0);
  EXPECT_TRUE(d.spawnNow);
  EXPECT_FALSE(d.gaveUp);
}

// The owner's case and every first install: the extension is absent for
// minutes, then registers. The retry must still be running when it does.
TEST(VcamHelperRespawnPolicy, AnUnreachableCameraIsRetriedForeverAndNeverGivenUp) {
  VcamHelperRespawnPolicy policy;
  std::int64_t now = 0;
  for (int attempt = 1; attempt <= 2000; ++attempt) {
    ASSERT_TRUE(policy.next(now).spawnNow) << "attempt " << attempt;
    policy.onSpawned();
    policy.onOutcome(VcamHelperOutcome::CameraUnreachable, now);
    const auto wait = policy.next(now);
    ASSERT_FALSE(wait.gaveUp) << "attempt " << attempt;
    ASSERT_FALSE(wait.spawnNow);
    ASSERT_LE(wait.waitMs, kVcamHelperSlowRetryMs);
    now += wait.waitMs;
  }
  EXPECT_EQ(policy.attempts(), 2000u);
}

TEST(VcamHelperRespawnPolicy, UnreachableRetriesStartAtOneSecondAndSlowDownOnceSettled) {
  VcamHelperRespawnPolicy policy;
  std::int64_t now = 0;
  auto failOnce = [&] {
    policy.onSpawned();
    policy.onOutcome(VcamHelperOutcome::CameraUnreachable, now);
    const auto wait = policy.next(now).waitMs;
    now += wait;
    return wait;
  };
  for (int i = 1; i <= kVcamHelperFastRetries; ++i) EXPECT_EQ(failOnce(), 1000) << i;
  for (int i = kVcamHelperFastRetries + 1; i <= kVcamHelperSettledRetries; ++i) EXPECT_EQ(failOnce(), 2000) << i;
  EXPECT_EQ(failOnce(), 5000);
  EXPECT_EQ(failOnce(), 5000);
}

// An extension update: connected, lost for under a second, back. The first
// retries after a connection are the fast ones again.
TEST(VcamHelperRespawnPolicy, AConnectionRestoresTheFastRetry) {
  VcamHelperRespawnPolicy policy;
  std::int64_t now = 0;
  for (int i = 0; i < 40; ++i) {
    policy.onSpawned();
    policy.onOutcome(VcamHelperOutcome::CameraUnreachable, now);
    now += policy.next(now).waitMs;
  }
  policy.onSpawned();
  policy.onOutcome(VcamHelperOutcome::Connected, now);
  now += 60'000;
  policy.onOutcome(VcamHelperOutcome::CameraUnreachable, now);  // lost
  EXPECT_EQ(policy.next(now).waitMs, 1000);
}

TEST(VcamHelperRespawnPolicy, AFailingHelperClimbsTheHouseLadderAndGivesUpAfterFive) {
  VcamHelperRespawnPolicy policy;
  std::int64_t now = 0;
  const std::int64_t ladder[] = {5'000, 10'000, 20'000, 40'000};
  for (std::int64_t expected : ladder) {
    ASSERT_TRUE(policy.next(now).spawnNow);
    policy.onSpawned();
    policy.onOutcome(VcamHelperOutcome::HelperFailed, now);
    const auto wait = policy.next(now);
    ASSERT_FALSE(wait.gaveUp);
    EXPECT_EQ(wait.waitMs, expected);
    EXPECT_FALSE(policy.next(now + expected - 1).spawnNow);
    now += expected;
  }
  policy.onSpawned();
  policy.onOutcome(VcamHelperOutcome::HelperFailed, now);
  EXPECT_TRUE(policy.next(now).gaveUp);
  EXPECT_FALSE(policy.next(now + 3'600'000).spawnNow) << "give-up does not expire on its own";
}

TEST(VcamHelperRespawnPolicy, AnOperatorRearmClearsGiveUp) {
  VcamHelperRespawnPolicy policy;
  for (int i = 0; i < kVcamHelperMaxConsecutiveFailures; ++i) policy.onOutcome(VcamHelperOutcome::HelperFailed, 0);
  ASSERT_TRUE(policy.gaveUp());
  policy.rearm();
  EXPECT_FALSE(policy.gaveUp());
  EXPECT_TRUE(policy.next(0).spawnNow);
}

// A helper that connects and dies at once has proven nothing; one that ran for
// the healthy window has earned the budget back.
TEST(VcamHelperRespawnPolicy, OnlyAHealthyRunEarnsTheHelperFailureBudgetBack) {
  VcamHelperRespawnPolicy shortRuns;
  std::int64_t now = 0;
  for (int i = 0; i < kVcamHelperMaxConsecutiveFailures; ++i) {
    shortRuns.onOutcome(VcamHelperOutcome::Connected, now);
    now += 500;
    shortRuns.onOutcome(VcamHelperOutcome::HelperFailed, now);
  }
  EXPECT_TRUE(shortRuns.gaveUp());

  VcamHelperRespawnPolicy longRuns;
  now = 0;
  for (int i = 0; i < 20; ++i) {
    longRuns.onOutcome(VcamHelperOutcome::Connected, now);
    now += kVcamHelperHealthyRunMs;
    longRuns.onOutcome(VcamHelperOutcome::HelperFailed, now);
    ASSERT_FALSE(longRuns.gaveUp()) << i;
    ASSERT_EQ(longRuns.consecutiveHelperFailures(), 1) << i;
    now += longRuns.next(now).waitMs;
  }
}

TEST(VcamHelperRespawnPolicy, AnUnreachableCameraDoesNotSpendTheHelperFailureBudget) {
  VcamHelperRespawnPolicy policy;
  for (int i = 0; i < 100; ++i) policy.onOutcome(VcamHelperOutcome::CameraUnreachable, i * 1000);
  EXPECT_EQ(policy.consecutiveHelperFailures(), 0);
  EXPECT_FALSE(policy.gaveUp());
}

TEST(VcamHelperRespawnPolicy, WarningsAreClassifiedByTheirCameraPrefix) {
  EXPECT_EQ(classifyVcamHelperWarning("CAMERA_EXTENSION_NOT_FOUND: x"), VcamHelperOutcome::CameraUnreachable);
  EXPECT_EQ(classifyVcamHelperWarning("CAMERA_PRODUCER_REFUSED: x"), VcamHelperOutcome::CameraUnreachable);
  EXPECT_EQ(classifyVcamHelperWarning("CAMERA_SINK_FAILED: x"), VcamHelperOutcome::CameraUnreachable);
  EXPECT_EQ(classifyVcamHelperWarning("CAMERA_EXTENSION_LOST: x"), VcamHelperOutcome::CameraUnreachable);
  // Unknown is a helper failure: it must reach give-up, not retry silently forever.
  EXPECT_EQ(classifyVcamHelperWarning(""), VcamHelperOutcome::HelperFailed);
  EXPECT_EQ(classifyVcamHelperWarning("something else"), VcamHelperOutcome::HelperFailed);
}

TEST(VcamHelperRespawnPolicy, OnlyNotFoundMeansTheNewbornProcessDidNotSeeTheDevice) {
  EXPECT_FALSE(vcamConnectResultSawDevice("CAMERA_EXTENSION_NOT_FOUND: CoreVideo Pro Camera is not installed"));
  EXPECT_TRUE(vcamConnectResultSawDevice("CAMERA_PRODUCER_REFUSED: refused"));
  EXPECT_TRUE(vcamConnectResultSawDevice("CAMERA_SINK_FAILED: the camera has no sink stream."));
}

TEST(VcamSinkHelperParent, OnlyAChildOfTheSameExecutableMayRun) {
  const std::string core = "/Library/Application Support/CoreVideo Pro/corevideo-native";
  EXPECT_TRUE(vcamHelperParentAllowed(core, core));
  EXPECT_FALSE(vcamHelperParentAllowed(core, "/bin/zsh"));
  EXPECT_FALSE(vcamHelperParentAllowed(core, ""));
  EXPECT_FALSE(vcamHelperParentAllowed("", ""));
}
