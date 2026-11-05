#include <gtest/gtest.h>
#include "core/CaptureLivenessPolicy.h"
using namespace corevideo::core;
using corevideo::modules::CaptureLiveness;
using corevideo::modules::CaptureLivenessState;
using corevideo::modules::CaptureLossReason;
namespace {
constexpr int64_t kMs = 1'000'000;
CaptureLiveness live(int64_t lastSampleNs, bool trusted = true) {
  CaptureLiveness l; l.state = CaptureLivenessState::Live; l.cadenceTrusted = trusted; l.lastSampleNs = lastSampleNs; return l;
}
CaptureLiveness lost(int64_t lastSampleNs, int64_t lostAtNs, bool trusted = true) {
  auto l = live(lastSampleNs, trusted); l.state = CaptureLivenessState::Lost; l.lostAtNs = lostAtNs;
  l.reason = CaptureLossReason::DeviceRemoved; return l;
}
}  // namespace

TEST(CaptureLivenessPolicy, UnreportedIsTodaysBehaviourAndNeverAppliesPolicy) {
  const auto d = decideCaptureOnAir(CaptureLiveness{}, 100'000 * kMs);
  EXPECT_EQ(d.phase, CaptureOnAirPhase::Unreported);
  EXPECT_EQ(d.health, SourceHealth::Producing);
  EXPECT_FALSE(d.policyApplies);
}
TEST(CaptureLivenessPolicy, AdvancingSamplesStayLiveWhateverThePictureIs) {
  // A static slide still delivers samples: only sample time matters.
  for (int64_t t = 0; t < 600'000; t += 33) {
    const auto d = decideCaptureOnAir(live(t * kMs), t * kMs + 33 * kMs);
    ASSERT_EQ(d.phase, CaptureOnAirPhase::Live);
  }
}
TEST(CaptureLivenessPolicy, SilenceStallsAtExactlyTheStallBoundAndFailsAtTheSilentBound) {
  EXPECT_EQ(decideCaptureOnAir(live(0), kCaptureStallNs - 1).phase, CaptureOnAirPhase::Live);
  EXPECT_EQ(decideCaptureOnAir(live(0), kCaptureStallNs).phase, CaptureOnAirPhase::Stalled);
  EXPECT_EQ(decideCaptureOnAir(live(0), kCaptureStallNs).health, SourceHealth::Stalled);
  EXPECT_EQ(decideCaptureOnAir(live(0), kCaptureSilentFailNs - 1).phase, CaptureOnAirPhase::Stalled);
  EXPECT_EQ(decideCaptureOnAir(live(0), kCaptureSilentFailNs).phase, CaptureOnAirPhase::Failed);
  EXPECT_EQ(decideCaptureOnAir(live(0), kCaptureSilentFailNs).health, SourceHealth::Failed);
}
TEST(CaptureLivenessPolicy, UntrustedCadenceNeverStallsOrFailsOnSilence) {
  const auto d = decideCaptureOnAir(live(0, /*trusted=*/false), 3'600'000 * kMs);
  EXPECT_EQ(d.phase, CaptureOnAirPhase::Live);
  EXPECT_TRUE(d.policyApplies);
}
TEST(CaptureLivenessPolicy, ExplicitLossStallsImmediatelyThenFailsAfterTheHoldWindow) {
  const int64_t lostAt = 100 * kMs;
  EXPECT_EQ(decideCaptureOnAir(lost(90 * kMs, lostAt), lostAt).phase, CaptureOnAirPhase::LostHolding);
  EXPECT_EQ(decideCaptureOnAir(lost(90 * kMs, lostAt), lostAt).health, SourceHealth::Stalled);
  EXPECT_EQ(decideCaptureOnAir(lost(90 * kMs, lostAt), lostAt + kCaptureLostHoldNs - 1).phase,
            CaptureOnAirPhase::LostHolding);
  EXPECT_EQ(decideCaptureOnAir(lost(90 * kMs, lostAt), lostAt + kCaptureLostHoldNs).phase,
            CaptureOnAirPhase::Failed);
}
TEST(CaptureLivenessPolicy, ExplicitLossAfterLongSilenceNeverExceedsTheSilentBound) {
  // Last sample at 0, loss reported at 4.5 s: fail at 5.0 s, not 6.5 s.
  const auto l = lost(0, 4'500 * kMs);
  EXPECT_EQ(decideCaptureOnAir(l, kCaptureSilentFailNs - 1).phase, CaptureOnAirPhase::LostHolding);
  EXPECT_EQ(decideCaptureOnAir(l, kCaptureSilentFailNs).phase, CaptureOnAirPhase::Failed);
}
TEST(CaptureLivenessPolicy, ExplicitLossFailsEvenWithUntrustedCadence) {
  // lostAtNs == 0 is the "no loss" sentinel, so the loss time must be nonzero.
  constexpr int64_t lostAt = 1 * kMs;
  EXPECT_EQ(decideCaptureOnAir(lost(lostAt, lostAt, /*trusted=*/false), lostAt + kCaptureLostHoldNs).phase, CaptureOnAirPhase::Failed);
}
TEST(CaptureLivenessPolicy, FailInNsCountsDownToTheDeadlineWhileHolding) {
  constexpr int64_t lostAt = 1 * kMs;  // nonzero: 0 is the "no loss" sentinel
  const auto d = decideCaptureOnAir(lost(lostAt, lostAt), lostAt + 500 * kMs);
  EXPECT_EQ(d.failInNs, kCaptureLostHoldNs - 500 * kMs);
  EXPECT_EQ(decideCaptureOnAir(live(0), 0).failInNs, 0);
}
TEST(CaptureLivenessPolicy, RecoveryNeedsALiveSampleNewerThanTheLoss) {
  auto l = lost(0, 100 * kMs);
  l.state = CaptureLivenessState::Live;          // adapter says live again...
  l.lastSampleNs = 50 * kMs;                     // ...but on a sample older than the loss
  EXPECT_NE(decideCaptureOnAir(l, 200 * kMs).phase, CaptureOnAirPhase::Live);
  l.lastSampleNs = 150 * kMs;                    // a genuinely new sample
  EXPECT_EQ(decideCaptureOnAir(l, 200 * kMs).phase, CaptureOnAirPhase::Live);
}
