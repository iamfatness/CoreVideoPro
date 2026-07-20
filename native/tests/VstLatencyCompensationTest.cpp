// VST round-2 A2/A3: the compensating delay line, the recording PTS latency
// latch, and the host-transport v3 block layout — all pure/logic so they run
// without a plugin, a host process, or Media Foundation.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "host-transport.h"
#include "modules/AudioDsp.h"
#include "modules/RecordingPtsClock.h"

namespace {

using corevideo::modules::CompensatingDelayState;
using corevideo::modules::applyCompensatingDelay;

std::vector<float> interleavedRamp(size_t frames, float startL, float startR) {
  std::vector<float> pcm(frames * 2);
  for (size_t frame = 0; frame < frames; ++frame) {
    pcm[frame * 2] = startL + static_cast<float>(frame);
    pcm[frame * 2 + 1] = startR + static_cast<float>(frame);
  }
  return pcm;
}

}  // namespace

// ---------------------------------------------------------------------------
// A3 compensating delay
// ---------------------------------------------------------------------------

// A zero target with an idle state is a bit-identical pass-through (fast path).
TEST(CompensatingDelay, ZeroDelayIsBitIdentical) {
  CompensatingDelayState state;
  auto pcm = interleavedRamp(64, 0.0f, 100.0f);
  const auto original = pcm;
  applyCompensatingDelay(pcm.data(), 64, state, 0, 48000.0);
  EXPECT_EQ(pcm, original);
}

// A fixed delay of N frames delays the signal by exactly N frames: after the
// declick fade window the output equals the input shifted by the delay, and the
// leading frames read from the zero-initialized ring (silence).
TEST(CompensatingDelay, DelaysByExactlyTheRequestedFrames) {
  CompensatingDelayState state;
  const size_t delay = 240;  // 5ms @48k
  // First block: 480 frames of a ramp. The declick fade is only ~5ms and only
  // fires on a delay CHANGE (0 was the initial applied delay, so the very first
  // apply sets delay=240 with a fade from the previous 0-delay tap). To test
  // the steady delayed relationship cleanly, drive two blocks and inspect the
  // second (past the fade).
  auto first = interleavedRamp(480, 0.0f, 1000.0f);
  applyCompensatingDelay(first.data(), 480, state, delay, 48000.0);

  auto second = interleavedRamp(480, 480.0f, 1480.0f);  // continues the ramp
  const auto secondInput = second;
  applyCompensatingDelay(second.data(), 480, state, delay, 48000.0);

  // The second block's output at frame f is the input from `delay` frames
  // earlier — i.e. the tail of the FIRST block for the first `delay` frames,
  // then the head of the second block.
  for (size_t frame = delay; frame < 480; ++frame) {
    EXPECT_NEAR(second[frame * 2], secondInput[(frame - delay) * 2], 1e-3f)
        << "left frame " << frame;
    EXPECT_NEAR(second[frame * 2 + 1], secondInput[(frame - delay) * 2 + 1], 1e-3f)
        << "right frame " << frame;
  }
}

// A delay CHANGE crossfades (declick) rather than hard-splicing. Driven by a
// continuous unit-slope ramp: the delayed output normally steps by ~1/sample;
// a HARD splice from delay 200 to 480 would jump by ~280 (the tap-position
// difference) in a single sample. The 5ms crossfade spreads that jump out, so
// every per-sample step stays small.
TEST(CompensatingDelay, DelayChangeIsDeclicked) {
  CompensatingDelayState state;
  // Warm at a NONZERO delay with a long ramp so the ring is allocated and both
  // taps (200 and 480 back) land in filled signal, not silence.
  const size_t warmFrames = 2000;
  auto warm = interleavedRamp(warmFrames, 0.0f, 0.0f);
  applyCompensatingDelay(warm.data(), warmFrames, state, 200, 48000.0);

  // Continue the SAME ramp and switch the delay to 480.
  auto block = interleavedRamp(960, static_cast<float>(warmFrames), static_cast<float>(warmFrames));
  applyCompensatingDelay(block.data(), 960, state, 480, 48000.0);
  float maxStep = 0.0f;
  for (size_t frame = 1; frame < 960; ++frame) {
    maxStep = std::max(maxStep, std::abs(block[frame * 2] - block[(frame - 1) * 2]));
  }
  // Smooth ramp steps are ~1/sample; the declick keeps the transition well
  // under the ~280 a hard splice would produce.
  EXPECT_LT(maxStep, 10.0f) << "declick ramp should keep steps small, saw " << maxStep;
}

// ---------------------------------------------------------------------------
// A3 recording PTS latency latch
// ---------------------------------------------------------------------------

// With no plugin latency the audio PTS is unchanged (regression guard for the
// existing A/V-sync contract).
TEST(RecordingPtsLatency, ZeroLatencyDoesNotShiftAudioPts) {
  corevideo::modules::RecordingPtsClock clock;
  clock.setAudioContentLatency(0);
  ASSERT_TRUE(clock.videoPts(0, 1).has_value());
  EXPECT_EQ(clock.audioPts(100 * 10'000LL, 960, 48000), 100 * 10'000LL);
}

// A reported plugin latency shifts the audio timeline EARLIER by exactly that
// many samples (the delayed content's timestamps reflect the delay), clamped so
// the first PTS never goes below the shared epoch.
TEST(RecordingPtsLatency, LatencyShiftsAudioEarlierAndClampsToEpoch) {
  corevideo::modules::RecordingPtsClock clock;
  // Epoch at t=0; first audio buffer at 100ms with a 480-sample (10ms) plugin
  // latency → the audio content is 10ms early relative to its arrival stamp.
  clock.setAudioContentLatency(480);
  ASSERT_TRUE(clock.videoPts(0, 1).has_value());
  const std::int64_t tenMs = 10 * 10'000LL;
  const std::int64_t hundredMs = 100 * 10'000LL;
  EXPECT_EQ(clock.audioPts(hundredMs, 960, 48000), hundredMs - tenMs);
  EXPECT_EQ(clock.audioContentLatencySamples(), 480);

  // Clamp: a latency larger than the base offset cannot push PTS negative.
  corevideo::modules::RecordingPtsClock clamped;
  clamped.setAudioContentLatency(48000);  // 1s of latency
  ASSERT_TRUE(clamped.videoPts(0, 1).has_value());
  EXPECT_EQ(clamped.audioPts(5 * 10'000LL, 960, 48000), 0);  // clamped to epoch, not negative
}

// The latency is LATCHED at the first buffer — a mid-session change does not
// jump the gapless sample clock.
TEST(RecordingPtsLatency, LatencyLatchesAtFirstBuffer) {
  corevideo::modules::RecordingPtsClock clock;
  clock.setAudioContentLatency(480);
  ASSERT_TRUE(clock.videoPts(0, 1).has_value());
  const std::int64_t first = clock.audioPts(100 * 10'000LL, 960, 48000);
  // Change the latency AFTER the first buffer — must be ignored.
  clock.setAudioContentLatency(4800);
  const std::int64_t second = clock.audioPts(137 * 10'000LL, 960, 48000);
  // Second is exactly 20ms (960 samples) after the first, no re-latch jump.
  EXPECT_EQ(second, first + 20 * 10'000LL);
  EXPECT_EQ(clock.audioContentLatencySamples(), 480);
}

// ---------------------------------------------------------------------------
// host-transport v3 block layout
// ---------------------------------------------------------------------------

TEST(HostTransportV3, MagicBumpedAndParamStateFieldsPresent) {
  using namespace corevideo::pluginhost;
  // CVP3 — a stale host/core pair fails loudly on the magic mismatch.
  EXPECT_EQ(kHostBlockMagic, 0x43565033u);

  // Heap-allocated: the block carries a 1 MiB state area (it lives in SHM in
  // production), too large for the default test stack.
  auto block = std::make_unique<HostAudioBlock>();
  EXPECT_EQ(block->magic, kHostBlockMagic);
  // Param surface defaults.
  EXPECT_EQ(block->paramPublishedCount, 0);
  EXPECT_EQ(block->paramTotalCount, 0);
  EXPECT_EQ(block->latencySamples, 0u);
  // The published cap keeps the block bounded even for hundred-param shells.
  EXPECT_EQ(static_cast<int32_t>(std::size(block->params)), kHostParamPublishMax);
  // The state area is generous but finite (loud-fail past it).
  EXPECT_EQ(static_cast<int32_t>(std::size(block->stateData)), kHostStateMaxBytes);
}

// The published-param entry copies bounded strings (ASCII-folded) safely even
// when the source overruns the field.
TEST(HostTransportV3, BlockStringCopyIsBounded) {
  using namespace corevideo::pluginhost;
  HostParamEntry entry;
  std::string longTitle(200, 'x');
  copyBlockString(entry.title, longTitle.c_str());
  EXPECT_EQ(std::strlen(entry.title), static_cast<size_t>(kHostParamTitleMax - 1));
}
