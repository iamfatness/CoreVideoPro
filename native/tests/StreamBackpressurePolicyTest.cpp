#include "core/StreamBackpressurePolicy.h"

#include <gtest/gtest.h>

using corevideo::core::StreamBackpressureObservation;
using corevideo::core::StreamBackpressurePolicy;
using corevideo::core::StreamBackpressureTransition;

namespace {
StreamBackpressureObservation at(std::int64_t bufferedMs, bool keyframe = true) {
  StreamBackpressureObservation o;
  o.bufferedMs = bufferedMs;
  o.keyframeInQueue = keyframe;
  return o;
}
// Feed `ticks` observations at `bufferedMs` and return the last decision.
corevideo::core::StreamBackpressureDecision feed(StreamBackpressurePolicy& p, std::int64_t bufferedMs,
                                                 int ticks, bool keyframe = true) {
  corevideo::core::StreamBackpressureDecision d;
  for (int i = 0; i < ticks; ++i) d = p.observe(at(bufferedMs, keyframe));
  return d;
}
}  // namespace

TEST(StreamBackpressurePolicy, StartsUnthrottled) {
  StreamBackpressurePolicy p;
  EXPECT_EQ(p.divisor(), 1);
  EXPECT_EQ(std::string(p.lastReason()), "none");
}

TEST(StreamBackpressurePolicy, AnUnknownMeasurementIsIgnored) {
  StreamBackpressurePolicy p;
  feed(p, -1, 5000);
  EXPECT_EQ(p.divisor(), 1);
}

// An unknown tick is NO EVIDENCE EITHER WAY: it must feed neither streak. The
// assertion above cannot prove that on its own -- it runs entirely at divisor 1,
// where a negative measurement also falls below the throttle threshold and takes
// the divisor_ == 1 short-circuit harmlessly. Deleting the `bufferedMs < 0` guard
// leaves it green. These two do fail, because without the guard a negative value
// both RESETS overStreak_ and ADVANCES healthyStreak_.
TEST(StreamBackpressurePolicy, AnUnknownMeasurementBreaksNeitherStreak) {
  // Entry: an unknown tick in the middle of a backlog run must not reset it.
  StreamBackpressurePolicy entering;
  feed(entering, 300, StreamBackpressurePolicy::kEnterAfterOverWaterTicks - 1);
  ASSERT_EQ(entering.divisor(), 1);
  feed(entering, -1, 1);
  const auto entered = feed(entering, 300, 1);
  EXPECT_EQ(entering.divisor(), 2);
  EXPECT_EQ(entered.transition, StreamBackpressureTransition::Enter);

  // Recovery: an unknown tick must not count toward the healthy run either.
  StreamBackpressurePolicy recovering;
  feed(recovering, 300, StreamBackpressurePolicy::kEnterAfterOverWaterTicks);
  ASSERT_EQ(recovering.divisor(), 2);
  feed(recovering, 10, StreamBackpressurePolicy::kRecoverAfterHealthyTicks - 1);
  feed(recovering, -1, 1);
  EXPECT_EQ(recovering.divisor(), 2);
  const auto exited = feed(recovering, 10, 1);
  EXPECT_EQ(recovering.divisor(), 1);
  EXPECT_EQ(exited.transition, StreamBackpressureTransition::Exit);
}

// A keyframe spikes the queue for a tick or two. Only sustained growth throttles.
TEST(StreamBackpressurePolicy, ASingleSpikeDoesNotThrottle) {
  StreamBackpressurePolicy p;
  feed(p, 900, StreamBackpressurePolicy::kEnterAfterOverWaterTicks - 1);
  EXPECT_EQ(p.divisor(), 1);
  feed(p, 0, 1);  // one healthy tick clears the streak
  feed(p, 900, StreamBackpressurePolicy::kEnterAfterOverWaterTicks - 1);
  EXPECT_EQ(p.divisor(), 1);
}

TEST(StreamBackpressurePolicy, SustainedBacklogStepsUpOneLevelAtATime) {
  StreamBackpressurePolicy p;
  const auto enter = feed(p, 300, StreamBackpressurePolicy::kEnterAfterOverWaterTicks);
  EXPECT_EQ(p.divisor(), 2);
  EXPECT_EQ(enter.transition, StreamBackpressureTransition::Enter);
  EXPECT_EQ(std::string(p.lastReason()), "buffered-above-threshold");
  const auto up = feed(p, 300, StreamBackpressurePolicy::kEnterAfterOverWaterTicks);
  EXPECT_EQ(p.divisor(), 3);
  EXPECT_EQ(up.transition, StreamBackpressureTransition::StepUp);
}

// The step-DOWN half, which nothing asserted: recovery gives back one level at a
// time, and only the last one reports Exit.
TEST(StreamBackpressurePolicy, RecoveryStepsDownOneLevelAtATime) {
  StreamBackpressurePolicy p;
  feed(p, 300, StreamBackpressurePolicy::kEnterAfterOverWaterTicks);
  feed(p, 300, StreamBackpressurePolicy::kEnterAfterOverWaterTicks);
  ASSERT_EQ(p.divisor(), 3);

  const auto down = feed(p, 10, StreamBackpressurePolicy::kRecoverAfterHealthyTicks);
  EXPECT_EQ(p.divisor(), 2);
  EXPECT_EQ(down.transition, StreamBackpressureTransition::StepDown);
  EXPECT_EQ(std::string(p.lastReason()), "recovered");

  const auto exit = feed(p, 10, StreamBackpressurePolicy::kRecoverAfterHealthyTicks);
  EXPECT_EQ(p.divisor(), 1);
  EXPECT_EQ(exit.transition, StreamBackpressureTransition::Exit);
}

TEST(StreamBackpressurePolicy, NeverExceedsTheFloor) {
  StreamBackpressurePolicy p;
  feed(p, 900, StreamBackpressurePolicy::kEnterAfterOverWaterTicks * 20);
  EXPECT_EQ(p.divisor(), StreamBackpressurePolicy::kMaxDivisor);
}

// The 100-250ms band is the anti-flap hysteresis. Throttling drains the queue,
// so recovering at the throttle threshold would oscillate.
TEST(StreamBackpressurePolicy, TheHysteresisBandHoldsWithoutRecovering) {
  StreamBackpressurePolicy p;
  feed(p, 300, StreamBackpressurePolicy::kEnterAfterOverWaterTicks);
  ASSERT_EQ(p.divisor(), 2);
  feed(p, 200, StreamBackpressurePolicy::kRecoverAfterHealthyTicks * 3);
  EXPECT_EQ(p.divisor(), 2) << "buffered inside the band must neither throttle nor recover";
}

TEST(StreamBackpressurePolicy, RecoversOnlyAfterASustainedHealthyRun) {
  StreamBackpressurePolicy p;
  feed(p, 300, StreamBackpressurePolicy::kEnterAfterOverWaterTicks);
  ASSERT_EQ(p.divisor(), 2);
  feed(p, 10, StreamBackpressurePolicy::kRecoverAfterHealthyTicks - 1);
  EXPECT_EQ(p.divisor(), 2);
  const auto exit = feed(p, 10, 1);
  EXPECT_EQ(p.divisor(), 1);
  EXPECT_EQ(exit.transition, StreamBackpressureTransition::Exit);
  EXPECT_EQ(std::string(p.lastReason()), "recovered");
}

// Lever B: only above the higher threshold, only once the throttle has engaged,
// only when a keyframe is queued to discard up to, and never every tick.
TEST(StreamBackpressurePolicy, DiscardNeedsBacklogThrottleKeyframeAndCooldown) {
  StreamBackpressurePolicy p;
  EXPECT_FALSE(feed(p, 900, 1).discardBacklog) << "not while still at divisor 1";
  feed(p, 300, StreamBackpressurePolicy::kEnterAfterOverWaterTicks);
  ASSERT_EQ(p.divisor(), 2);
  EXPECT_FALSE(p.observe(at(400)).discardBacklog) << "400ms is below the discard threshold";
  EXPECT_FALSE(p.observe(at(900, /*keyframe=*/false)).discardBacklog) << "no keyframe to discard up to";
  const auto fired = p.observe(at(900));
  EXPECT_TRUE(fired.discardBacklog);
  EXPECT_EQ(std::string(p.lastReason()), "backlog-discard");
  EXPECT_EQ(p.discardEvents(), 1);
  EXPECT_FALSE(p.observe(at(900)).discardBacklog) << "cooldown: never two ticks running";
  feed(p, 900, StreamBackpressurePolicy::kDiscardCooldownTicks);
  EXPECT_EQ(p.discardEvents(), 2);
}

TEST(StreamBackpressurePolicy, ShedFramesAndEnteredCountAreCounted) {
  StreamBackpressurePolicy p;
  feed(p, 300, StreamBackpressurePolicy::kEnterAfterOverWaterTicks);
  p.noteShedFrame();
  p.noteShedFrame();
  EXPECT_EQ(p.shedFrames(), 2);
  EXPECT_EQ(p.enteredCount(), 1);
}
