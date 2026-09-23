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

// A keyframe spikes the queue for a tick or two. Only sustained growth throttles.
TEST(StreamBackpressurePolicy, ASingleSpikeDoesNotThrottle) {
  StreamBackpressurePolicy p;
  feed(p, 900, StreamBackpressurePolicy::kEnterAfterOverWaterTicks - 1);
  EXPECT_EQ(p.divisor(), 1);
  feed(p, 0, 1);  // one healthy tick clears the streak
  feed(p, 900, StreamBackpressurePolicy::kEnterAfterOverWaterTicks - 1);
  EXPECT_EQ(p.divisor(), 1);
}

TEST(StreamBackpressurePolicy, SustainedBacklogStepsDownOneLevelAtATime) {
  StreamBackpressurePolicy p;
  const auto enter = feed(p, 300, StreamBackpressurePolicy::kEnterAfterOverWaterTicks);
  EXPECT_EQ(p.divisor(), 2);
  EXPECT_EQ(enter.transition, StreamBackpressureTransition::Enter);
  EXPECT_EQ(std::string(p.lastReason()), "buffered-above-threshold");
  const auto up = feed(p, 300, StreamBackpressurePolicy::kEnterAfterOverWaterTicks);
  EXPECT_EQ(p.divisor(), 3);
  EXPECT_EQ(up.transition, StreamBackpressureTransition::StepUp);
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
