#include "core/StreamBackpressurePolicy.h"

#include <gtest/gtest.h>

#include <vector>

using corevideo::core::discardableGopTailLength;
using corevideo::core::discardableBacklogForArrival;
using corevideo::core::GopCutPoint;
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

// #597 Task 6 (amended after Task 4): noteShedFrame()/shedFrames() are gone -
// the compositor sheds once for every destination and holds no reference to
// any sender's policy, so nothing could ever call them. Only enteredCount()
// remains here.
TEST(StreamBackpressurePolicy, EnteredCountIsCounted) {
  StreamBackpressurePolicy p;
  feed(p, 300, StreamBackpressurePolicy::kEnterAfterOverWaterTicks);
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
// --- Task 8b fix round 1: the cut point is a PARAMETER, split by site. ------
//
// Lever B's ordinary discard keeps cutting to the NEAREST keyframe (every test
// above, which passes no cut point at all and therefore also pins the DEFAULT).
// The queue's OVERFLOW path cuts to the LAST, because its only alternative is
// failing the sender and rebuilding the encoder. Both rest on the same safety
// argument - every keyframe here is a self-contained IDR - so neither inspects
// a chunk for anything but `isKeyframe`.

TEST(StreamBackpressurePolicy, DiscardableGopTailLength_LastCutPointTakesTheFurthestKeyframe) {
  // [P, K, P, K, P] - Nearest frees 1, Last frees 3. Same queue, same safety.
  const std::vector<bool> twoKeyframes{false, true, false, true, false};
  EXPECT_EQ(discardableGopTailLength(twoKeyframes, identity(), GopCutPoint::Nearest), 1u);
  EXPECT_EQ(discardableGopTailLength(twoKeyframes, identity(), GopCutPoint::Last), 3u)
      << "the last resort must free the most room a safe cut can free";
}

// CHARACTERISATION TEST, NOT COVERAGE (fix round 2, item 4). A full queue whose
// ONLY keyframe is at the head frees nothing under EITHER cut point - the head
// keyframe is also the last one, so there is genuinely nothing ahead of it, and
// no mutation of the code this touches changes the answer. It is kept because
// it documents the boundary that proves Last is not a blanket "drop more", and
// because it is the shape fix round 1 exists for (under Nearest, with a LATER
// keyframe present, this case froze at 0 and failed the sender - that case is
// LastCutPointFreesRoomWhereNearestFreesNone, which does have a killer line).
// Do not count this one as coverage.
TEST(StreamBackpressurePolicy, DiscardableGopTailLength_LastCutPointStillDropsNothingForAHeadOnlyKeyframe) {
  const std::vector<bool> keyframeFirst{true, false, false, false};
  EXPECT_EQ(discardableGopTailLength(keyframeFirst, identity(), GopCutPoint::Last), 0u);
}

// ...and the shape that actually rescues the head-keyframe case at the cap: a
// LATER keyframe exists, so Last frees the whole run ahead of it where Nearest
// would have frozen at 0 and failed the sender.
TEST(StreamBackpressurePolicy, DiscardableGopTailLength_LastCutPointFreesRoomWhereNearestFreesNone) {
  const std::vector<bool> headKeyframeThenAnother{true, false, false, true, false};
  EXPECT_EQ(discardableGopTailLength(headKeyframeThenAnother, identity(), GopCutPoint::Nearest), 0u);
  EXPECT_EQ(discardableGopTailLength(headKeyframeThenAnother, identity(), GopCutPoint::Last), 3u);
}

// The not-found guard must survive the Last scan too: it keeps assigning
// `keyframeIndex` as it walks, so a range with NO keyframe must still leave the
// `size` sentinel intact and return 0 rather than dropping the whole queue.
TEST(StreamBackpressurePolicy, DiscardableGopTailLength_LastCutPointWithNoKeyframeStillDropsNothing) {
  const std::vector<bool> allReferenceFrames{false, false, false, false, false};
  EXPECT_EQ(discardableGopTailLength(allReferenceFrames, identity(), GopCutPoint::Last), 0u);
}

// An all-keyframe queue is the maximum-drop boundary under Last: every chunk
// but the final keyframe is ahead of the cut point, so `size - 1` is dropped.
// That is correct - each is independently decodable, and the stream resumes at
// the newest picture - and it is the value most worth pinning, because it is
// the one place an off-by-one would drop the chunk we are cutting TO.
TEST(StreamBackpressurePolicy, DiscardableGopTailLength_LastCutPointOnAnAllKeyframeQueueKeepsOnlyTheNewest) {
  const std::vector<bool> allKeyframes{true, true, true};
  EXPECT_EQ(discardableGopTailLength(allKeyframes, identity(), GopCutPoint::Nearest), 0u);
  EXPECT_EQ(discardableGopTailLength(allKeyframes, identity(), GopCutPoint::Last), 2u)
      << "never size (that would drop the keyframe being cut to), never 0";
}

// --- The arriving chunk is a cut point too (fix round 1, second half). -----
// Measured on the burst gate: 60 queued P-frames, no keyframe anywhere, and
// the chunk being refused was itself keyframe-sized. The queue could not be
// cut because its own cut point was at the door.
TEST(StreamBackpressurePolicy, DiscardableBacklogForArrival_AKeyframeArrivalMakesTheWholeBacklogDiscardable) {
  const std::vector<bool> allReferenceFrames{false, false, false, false, false};
  EXPECT_EQ(discardableBacklogForArrival(allReferenceFrames, identity(), /*arrivalIsKeyframe=*/false,
                                         GopCutPoint::Last),
            0u)
      << "no keyframe queued and none arriving: there is genuinely nothing safe to drop";
  EXPECT_EQ(discardableBacklogForArrival(allReferenceFrames, identity(), /*arrivalIsKeyframe=*/true,
                                         GopCutPoint::Last),
            5u)
      << "the decoder resumes at the arriving IDR, so the whole backlog is discardable";
}

// A non-keyframe arrival changes nothing anywhere: this delegates to
// discardableGopTailLength unchanged, including its cut point.
TEST(StreamBackpressurePolicy, DiscardableBacklogForArrival_ANonKeyframeArrivalDelegatesUnchanged) {
  const std::vector<bool> twoKeyframes{false, true, false, true, false};
  EXPECT_EQ(discardableBacklogForArrival(twoKeyframes, identity(), false, GopCutPoint::Nearest), 1u);
  EXPECT_EQ(discardableBacklogForArrival(twoKeyframes, identity(), false, GopCutPoint::Last), 3u);
  EXPECT_EQ(discardableBacklogForArrival(twoKeyframes, identity(), true, GopCutPoint::Last), 5u);
}

// CHARACTERISATION TEST, NOT COVERAGE (fix round 2, item 4): an empty queue has
// nothing to drop under every cut point and every arrival, so no mutation of
// the code it touches can fail it. Kept as documentation of the degenerate
// input; do not count it as coverage.
TEST(StreamBackpressurePolicy, DiscardableBacklogForArrival_AKeyframeArrivalOnAnEmptyQueueDropsNothing) {
  const std::vector<bool> empty;
  EXPECT_EQ(discardableBacklogForArrival(empty, identity(), true, GopCutPoint::Last), 0u);
}

TEST(StreamBackpressurePolicy, DiscardableGopTailLength_DropsEveryChunkAheadOfATrailingKeyframe) {
  const std::vector<bool> chunks{false, false, false, true};
  EXPECT_EQ(corevideo::core::discardableGopTailLength(
                chunks, [](bool keyframe) { return keyframe; }),
            3u);
}
