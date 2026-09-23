#include "core/StreamBackpressurePolicy.h"

#include <gtest/gtest.h>

#include <vector>

using corevideo::core::discardableGopTailLength;
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

namespace {
auto identity() {
  return [](bool keyframe) { return keyframe; };
}
}  // namespace

// #597 Lever B, fix round 1 (review findings 2 + 5): the discard decision,
// extracted to core::discardableGopTailLength() so every boundary condition
// is provable with NO seam of any kind - not a sender, not a queue, not even
// a fake one. Just a std::vector<bool> of keyframe flags.

// The regression this whole suite exists to catch: initializing the "not
// found" sentinel to something a real index can also equal (e.g. 0) makes
// "no keyframe queued" and "keyframe at the head" the SAME value, and then a
// missing or wrong not-found guard can silently drop the ENTIRE queue - the
// stream-corruption failure the brief opens with. Because the real
// implementation uses `size()` as the sentinel (a value no real index can
// ever equal), this queue's non-empty, no-keyframe case can ONLY read 0 if
// the not-found guard is present and correct; regressing the guard away
// returns `size()` (5) here, which fails this assertion.
TEST(StreamBackpressurePolicy, DiscardableGopTailLength_NoKeyframeDropsNothing) {
  const std::vector<bool> allReferenceFrames{false, false, false, false, false};
  EXPECT_EQ(discardableGopTailLength(allReferenceFrames, identity()), 0u)
      << "no keyframe anywhere: nothing is safe to cut forward to";
}

TEST(StreamBackpressurePolicy, DiscardableGopTailLength_EmptyQueueDropsNothing) {
  const std::vector<bool> empty;
  EXPECT_EQ(discardableGopTailLength(empty, identity()), 0u);
}

// Distinct from the "not found" case above: here a keyframe genuinely exists
// at index 0, so there is nothing AHEAD of it, not "nothing found".
TEST(StreamBackpressurePolicy, DiscardableGopTailLength_KeyframeAlreadyAtHeadDropsNothing) {
  const std::vector<bool> keyframeFirst{true, false, false, false};
  EXPECT_EQ(discardableGopTailLength(keyframeFirst, identity()), 0u);
}

TEST(StreamBackpressurePolicy, DiscardableGopTailLength_AllKeyframesDropsNothing) {
  const std::vector<bool> allKeyframes{true, true, true};
  EXPECT_EQ(discardableGopTailLength(allKeyframes, identity()), 0u);
}

TEST(StreamBackpressurePolicy, DiscardableGopTailLength_DropsExactlyTheChunksAheadOfTheKeyframe) {
  // [P, P, K, P] - the two stale P-frames ahead of the keyframe are safe to
  // drop; the keyframe and the P-frame that depends on it are not.
  const std::vector<bool> referenceThenKeyframeThenReference{false, false, true, false};
  EXPECT_EQ(discardableGopTailLength(referenceThenKeyframeThenReference, identity()), 2u);
}

// Takes the FIRST keyframe, not the last - over-discarding past a nearer
// safe cut point would throw away video for nothing.
TEST(StreamBackpressurePolicy, DiscardableGopTailLength_StopsAtTheFirstKeyframeNotTheLast) {
  const std::vector<bool> twoKeyframes{false, true, false, true, false};
  EXPECT_EQ(discardableGopTailLength(twoKeyframes, identity()), 1u);
}

// The maximum-drop boundary. Every other case returns 0, 1 or 2, so nothing
// pinned the case where the keyframe is LAST and the whole rest of the queue is
// discardable - the shape that recovers the most latency and is therefore the
// one most worth getting wrong by one.
TEST(StreamBackpressurePolicy, DiscardableGopTailLength_DropsEveryChunkAheadOfATrailingKeyframe) {
  const std::vector<bool> chunks{false, false, false, true};
  EXPECT_EQ(corevideo::core::discardableGopTailLength(
                chunks, [](bool keyframe) { return keyframe; }),
            3u);
}
