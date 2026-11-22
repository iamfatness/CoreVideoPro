// Mac capture liveness (Sources Phase 2 plan, Task 7). Before this, the AVFoundation
// and ScreenCaptureKit adapters never wrote VideoFrame::captureLiveness, so every Mac
// capture row read "unreported" and dropoutPolicyApplies:false (SourceVideoIngress.cpp:124).
// Each case drives the REAL on-air decision (decideCaptureOnAir) with what the policy produced.
#include "core/CaptureLivenessPolicy.h"
#include "modules/MacCaptureLiveness.h"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

using corevideo::core::CaptureOnAirPhase;
using corevideo::core::decideCaptureOnAir;
using corevideo::modules::CaptureLivenessState;
using corevideo::modules::CaptureLossReason;
using corevideo::modules::MacCaptureLiveness;

namespace {
constexpr int64_t kMs = 1'000'000;
constexpr int64_t kT0 = 1'000'000 * kMs;  // an arbitrary steady-clock origin
}  // namespace

TEST(MacCaptureLiveness, NothingDeliveredIsUnreported) {
  MacCaptureLiveness camera(true);
  EXPECT_EQ(camera.value().state, CaptureLivenessState::Unreported)
      << "no delivery yet: the core keeps its legacy hold behaviour";
}

TEST(MacCaptureLiveness, ATrustedCameraStallsThenFailsOnSilence) {
  MacCaptureLiveness camera(true);
  camera.sample(kT0);
  EXPECT_EQ(decideCaptureOnAir(camera.value(), kT0 + 100 * kMs).phase, CaptureOnAirPhase::Live);
  EXPECT_EQ(decideCaptureOnAir(camera.value(), kT0 + 1'600 * kMs).phase, CaptureOnAirPhase::Stalled)
      << "1.6 s of silence from a camera is a stall (kOnAirStallNs = 1.5 s)";
  EXPECT_EQ(decideCaptureOnAir(camera.value(), kT0 + 5'100 * kMs).phase, CaptureOnAirPhase::Failed)
      << "5.1 s of silence fails (kCaptureSilentFailNs = 5 s)";
  camera.sample(kT0 + 6'000 * kMs);
  EXPECT_EQ(decideCaptureOnAir(camera.value(), kT0 + 6'010 * kMs).phase, CaptureOnAirPhase::Live)
      << "a new delivery is live again";
}

TEST(MacCaptureLiveness, AnUntrustedScreenIsNeverStalledBySilence) {
  // ScreenCaptureKit sends frames only when the screen changes: silence is not loss.
  MacCaptureLiveness screen(false);
  screen.sample(kT0);
  EXPECT_EQ(decideCaptureOnAir(screen.value(), kT0 + 60'000 * kMs).phase, CaptureOnAirPhase::Live)
      << "a static screen for a minute is still live";
  EXPECT_TRUE(decideCaptureOnAir(screen.value(), kT0).policyApplies) << "but it does report";
}

TEST(MacCaptureLiveness, AnExplicitLossHoldsThenFailsAndADeliveryRecovers) {
  for (const bool trusted : {true, false}) {
    MacCaptureLiveness source(trusted);
    source.sample(kT0);
    source.lost(CaptureLossReason::StreamError, kT0 + 10 * kMs);
    source.lost(CaptureLossReason::StreamError, kT0 + 900 * kMs);  // idempotent: keeps the first loss time
    EXPECT_EQ(source.value().reason, CaptureLossReason::StreamError);
    EXPECT_EQ(source.value().lostAtNs, kT0 + 10 * kMs) << "a repeated loss report does not restart the hold";
    EXPECT_EQ(decideCaptureOnAir(source.value(), kT0 + 1'000 * kMs).phase, CaptureOnAirPhase::LostHolding)
        << "trusted=" << trusted;
    EXPECT_EQ(decideCaptureOnAir(source.value(), kT0 + 2'100 * kMs).phase, CaptureOnAirPhase::Failed)
        << "the 2 s loss hold ends (kCaptureLostHoldNs), trusted=" << trusted;
    source.sample(kT0 + 3'000 * kMs);
    EXPECT_EQ(decideCaptureOnAir(source.value(), kT0 + 3'010 * kMs).phase, CaptureOnAirPhase::Live)
        << "a delivery after the loss recovers, trusted=" << trusted;
  }
}

#if defined(__APPLE__)
#include "modules/AvfCaptureSessionState.h"

namespace {
std::shared_ptr<std::vector<uint8_t>> i420(int width, int height) {
  return std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(width) * height * 3 / 2, 0x40);
}
}  // namespace

TEST(MacCaptureLiveness, AnAvfDeliveryStampsTrustedLivenessAndAHeldFrameKeepsItsSampleTime) {
  corevideo::modules::AvfSessionState state("capture:cam");
  state.publish(i420(64, 36), 64, 36, false, false);
  corevideo::modules::VideoFrame first;
  int64_t frameId = 0;
  ASSERT_TRUE(state.sampleLatest(first, frameId));
  EXPECT_EQ(first.captureLiveness.state, CaptureLivenessState::Live) << "a camera delivery reports live";
  EXPECT_TRUE(first.captureLiveness.cadenceTrusted) << "a camera delivers continuously";
  EXPECT_GT(first.captureLiveness.lastSampleNs, 0);
  // The held picture is re-sampled every render tick; its sample time is the DELIVERY,
  // never the re-emit (a stalled camera must age).
  corevideo::modules::VideoFrame held;
  ASSERT_TRUE(state.sampleLatest(held, frameId));
  EXPECT_EQ(held.captureLiveness.lastSampleNs, first.captureLiveness.lastSampleNs)
      << "re-sampling the held frame does not refresh liveness";
  state.markLost(CaptureLossReason::DeviceRemoved);
  ASSERT_TRUE(state.sampleLatest(held, frameId));
  EXPECT_EQ(held.captureLiveness.state, CaptureLivenessState::Lost) << "an unplugged camera reports the loss";
  EXPECT_EQ(held.captureLiveness.reason, CaptureLossReason::DeviceRemoved);
}
#endif  // __APPLE__
