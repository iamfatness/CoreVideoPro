#if defined(_WIN32) && !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_D3D11
#include "modules/GpuVideoEncoder.h"
#include "modules/Interfaces.h"
#include "modules/MediaFoundationGpuVideoEncoder.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// THE CBR BITS-PER-FRAME GATE (stream backpressure, Task 1).
//
// The planned backpressure lever throttles the encoder's INPUT (it skips program
// frames) rather than dropping compressed output, because a compressed frame
// cannot be dropped on its own. That rests on ONE assumption: under CBR the
// hardware encoder allocates about `bitrate / declaredFrameRate` bits per frame,
// so feeding it half the frames emits about half the data per wall-clock second
// while per-frame quality holds. If instead its rate control chases the bitrate
// on a wall clock, it spends MORE bits per frame and egress does not fall - and
// throttling the input cannot relieve a backed-up stream at all.
//
// This probe measures it on the real hardware encoder. Two legs of identical
// wall-clock length over the same source, same 1920x1080 plan, same
// GpuVideoEncoderConfig (10000 kbps h264, DECLARED fps 60 in BOTH legs - that is
// the configuration the real throttle runs in; only the input cadence changes).
// Leg A produces a program frame every 16.7 ms (~60/s); leg B produces one every
// 33.3 ms (~30/s).
//
// WHAT PACES THE ENCODER, measured here 2026-09-23 and NOT what the task brief
// assumed: it is the COMPOSITOR's keyed mutex, not submit(). The encode loop
// reads `latestHandle_` on every NeedInput and calls AcquireSync(1, 34ms) on the
// shared texture; it can only acquire again after the producer's next render
// releases key 1, and the sample's PTS comes from the compositor's
// publishedFrameNumber, read under that mutex. A first cut of this probe skipped
// only the submit() call while still rendering at 60 Hz: both legs emitted 598
// chunks and identical bytes, because the encoder kept taking a fresh texture
// every render. So the input throttle must skip the PROGRAM FRAME (the render
// that publishes the encoder texture), which is what leg B does - and that is a
// real finding for the rest of this plan: dropping submit() calls alone throttles
// nothing.
//
// Self-skips loudly when no hardware encoder is present, like
// MediaFoundationGpuVideoEncoderTest, so CI stays green on machines without one.
namespace {

constexpr int kProbeWidth = 1920;
constexpr int kProbeHeight = 1080;
constexpr int kProbeFps = 60;
constexpr int kProbeBitrateKbps = 10000;  // the #597 incident's configuration

// The source must be genuinely expensive to code, or a CBR encoder is never
// rate-limited and the ratio measures nothing that resembles the incident
// (a 10 Mbps live show). A cycle of noise pictures, upscaled by the compositor
// to 1920x1080, keeps every frame full of high-frequency detail.
constexpr int kSourceWidth = 640;
constexpr int kSourceHeight = 360;
constexpr int kNoiseFrames = 16;

std::vector<std::shared_ptr<std::vector<uint8_t>>> makeNoiseBank(bool rateControlFixture = false) {
  std::vector<std::shared_ptr<std::vector<uint8_t>>> bank;
  bank.reserve(kNoiseFrames);
  uint32_t state = 0x13579bdfu;
  for (int i = 0; i < kNoiseFrames; ++i) {
    auto pixels = std::make_shared<std::vector<uint8_t>>(
        static_cast<size_t>(kSourceWidth) * kSourceHeight * 4, 0);
    for (size_t p = 0; p < pixels->size(); p += 4) {
      state ^= state << 13;
      state ^= state >> 17;
      state ^= state << 5;
      // Full-range independent noise saturates H.264 at QP 51 even at
      // 10 Mbps on the RTX 4090. Keep that stress input for the original
      // backpressure probe; rate-control conformance needs achievable content.
      const auto mask = rateControlFixture ? 63u : 255u;
      const auto offset = rateControlFixture ? 96u : 0u;
      (*pixels)[p + 0] = static_cast<uint8_t>(offset + (state & mask));
      (*pixels)[p + 1] = static_cast<uint8_t>(offset + ((state >> 8) & mask));
      (*pixels)[p + 2] = static_cast<uint8_t>(offset + ((state >> 16) & mask));
      (*pixels)[p + 3] = 0xFF;
    }
    bank.push_back(std::move(pixels));
  }
  return bank;
}

corevideo::modules::VideoFrame makeNoiseSourceFrame(
    int64_t frameNumber, const std::shared_ptr<std::vector<uint8_t>>& pixels) {
  corevideo::modules::VideoFrame frame;
  frame.participantId = "test";
  frame.width = frame.pixelWidth = kSourceWidth;
  frame.height = frame.pixelHeight = kSourceHeight;
  frame.naturalWidth = kSourceWidth;
  frame.naturalHeight = kSourceHeight;
  frame.pixelStride = kSourceWidth * 4;
  frame.timestampMs = frameNumber * 16;
  frame.frameId = frameNumber;
  frame.pixels = pixels;
  return frame;
}

struct LegResult {
  int64_t bytes = 0;
  int64_t chunks = 0;
  int64_t submits = 0;
  int64_t renders = 0;
  double measuredSeconds = 0.0;
  bool ran = false;
  bool unavailable = false;
};

int probeBitrateKbps() {
  // Diagnostic override only (the ratio gate always runs at the incident's
  // 10000 kbps); used to check whether the configured bitrate binds at all.
  if (const char* raw = std::getenv("COREVIDEO_RATE_PROBE_KBPS")) {
    const int parsed = std::atoi(raw);
    if (parsed > 0) return parsed;
  }
  return kProbeBitrateKbps;
}

// One leg. `halfInputRate` is the ONLY difference between the two: it produces a
// program frame (and submits it) every second 60 Hz slot instead of every slot.
LegResult runLeg(const char* label, bool halfInputRate, int bitrateKbps = -1,
                 int settleMs = 2500, int measureMs = 10000, bool rateControlFixture = false,
                 const char* codec = "h264", const char* rateControl = "cbr") {
  using clock = std::chrono::steady_clock;
  LegResult result;

  auto encoder = corevideo::modules::createMediaFoundationGpuVideoEncoder();
  if (!encoder) {
    std::fprintf(stderr, "[rate-probe] no GPU encoder implementation on this machine\n");
    return result;
  }
  auto compositor = corevideo::modules::createD3D11Compositor();
  if (!compositor) {
    std::fprintf(stderr, "[rate-probe] no D3D11 compositor on this machine\n");
    return result;
  }

  corevideo::modules::CompositorRenderPlan plan;
  plan.sceneId = "rate-probe";
  plan.renderPlanId = "rate-probe";
  plan.width = kProbeWidth;
  plan.height = kProbeHeight;
  plan.fps = kProbeFps;
  plan.skipCpuReadback = true;
  plan.fullProgramReadback = true;
  corevideo::modules::CompositorRenderPlanLayer layer;
  layer.layerId = "route:test";
  layer.kind = "participant-video";
  layer.participantId = "test";
  layer.sourceId = "zoom:test";
  layer.rect = {0.f, 0.f, 1.f, 1.f};
  layer.borderStyle = "none";
  plan.layers.push_back(layer);

  // Counted only inside the measurement window: a start-up burst or the flush at
  // stop() must never land in one leg's total.
  std::mutex sinkMutex;
  std::atomic<bool> counting{false};
  int64_t countedBytes = 0;
  int64_t countedChunks = 0;
  auto sink = [&](const corevideo::modules::GpuEncodedChunk& chunk) {
    std::lock_guard<std::mutex> lock(sinkMutex);
    if (!counting.load(std::memory_order_relaxed)) return;
    countedBytes += static_cast<int64_t>(chunk.size);
    ++countedChunks;
  };

  corevideo::modules::GpuVideoEncoderConfig config{
      kProbeWidth, kProbeHeight, kProbeFps,
      bitrateKbps > 0 ? bitrateKbps : probeBitrateKbps(), 2.0, rateControl, "high"};
  config.codec = codec;
  if (!encoder->start(config, sink)) {
    std::fprintf(stderr, "[rate-probe] encoder start unavailable on this machine (%s)\n",
                 encoder->lastFailure().c_str());
    result.unavailable = encoder->lastFailure() == std::string("no-hardware-mft-") + codec;
    ASSERT_TRUE(!rateControlFixture || result.unavailable) << "hardware rate-control test failed to start: "
                                    << encoder->lastFailure();
    return result;
  }

  const auto bank = makeNoiseBank(rateControlFixture);
  const auto kSettle = std::chrono::milliseconds(settleMs);
  const auto kMeasure = std::chrono::milliseconds(measureMs);
  const auto kTail = std::chrono::milliseconds(300);
  const auto slotInterval = std::chrono::nanoseconds(1000000000LL / kProbeFps);
  const auto legStart = clock::now();
  const auto legEnd = legStart + kSettle + kMeasure + kTail;
  clock::time_point countingStart{};
  clock::time_point countingEnd{};

  int64_t slot = 0;
  while (clock::now() < legEnd) {
    const auto elapsed = clock::now() - legStart;
    const bool inWindow = elapsed >= kSettle && elapsed < kSettle + kMeasure;
    if (inWindow && !counting.load(std::memory_order_relaxed)) {
      countingStart = clock::now();
      counting.store(true, std::memory_order_relaxed);
    } else if (!inWindow && counting.load(std::memory_order_relaxed)) {
      counting.store(false, std::memory_order_relaxed);
      countingEnd = clock::now();
    }

    const bool produceThisSlot = !halfInputRate || (slot % 2 == 0);
    if (!produceThisSlot) {
      ++slot;
      std::this_thread::sleep_until(legStart + slotInterval * slot);
      continue;
    }
    const auto frame = compositor->render(
        plan, {makeNoiseSourceFrame(slot + 1, bank[static_cast<size_t>(slot % kNoiseFrames)])});
    ++result.renders;
    if (!frame.encoderSharedTexture.sharedHandleHex.empty()) {
      corevideo::modules::GpuVideoEncoderFrame encodeFrame;
      encodeFrame.publishedFrameNumber = frame.encoderSharedTexture.publishedFrameNumber;
      encodeFrame.sharedHandleHex = frame.encoderSharedTexture.sharedHandleHex;
      encodeFrame.width = frame.encoderSharedTexture.width;
      encodeFrame.height = frame.encoderSharedTexture.height;
      encodeFrame.frameNumber = frame.frameNumber;
      if (!encoder->submit(encodeFrame)) {
        std::fprintf(stderr, "[rate-probe] %s: submit failed at slot %lld\n", label,
                     static_cast<long long>(slot));
        break;
      }
      if (inWindow) ++result.submits;
    }
    ++slot;
    std::this_thread::sleep_until(legStart + slotInterval * slot);
  }
  counting.store(false, std::memory_order_relaxed);
  if (countingEnd == clock::time_point{}) countingEnd = clock::now();

  encoder->stop();

  {
    std::lock_guard<std::mutex> lock(sinkMutex);
    result.bytes = countedBytes;
    result.chunks = countedChunks;
  }
  result.measuredSeconds =
      std::chrono::duration<double>(countingEnd - countingStart).count();
  result.ran = result.bytes > 0;
  std::fprintf(stderr,
               "[rate-probe] %s: bytes=%lld chunks=%lld submits(window)=%lld renders=%lld "
               "window=%.2fs -> %.0f kbps, %.1f submits/s\n",
               label, static_cast<long long>(result.bytes),
               static_cast<long long>(result.chunks), static_cast<long long>(result.submits),
               static_cast<long long>(result.renders), result.measuredSeconds,
               result.measuredSeconds > 0
                   ? static_cast<double>(result.bytes) * 8.0 / result.measuredSeconds / 1000.0
                   : 0.0,
               result.measuredSeconds > 0 ? result.submits / result.measuredSeconds : 0.0);
  return result;
}

}  // namespace

TEST(StreamBackpressureRateProbe, HalvingTheInputRateRoughlyHalvesEgress) {
  const auto full = runLeg("leg-A 60/s", false);
  const auto half = runLeg("leg-B 30/s", true);
  if (!full.ran || !half.ran) {
    std::fprintf(stderr,
                 "[  SKIPPED ] StreamBackpressureRateProbe (no hardware encoder)\n");
    return;
  }

  // Normalise by the actual measured window so a scheduler hiccup in one leg
  // cannot masquerade as an encoder property.
  const double fullRate = static_cast<double>(full.bytes) / full.measuredSeconds;
  const double halfRate = static_cast<double>(half.bytes) / half.measuredSeconds;
  const double ratio = halfRate / fullRate;
  std::fprintf(stderr,
               "[rate-probe] full=%lld half=%lld ratio=%.3f (rate-normalised)\n",
               static_cast<long long>(full.bytes), static_cast<long long>(half.bytes), ratio);

  EXPECT_GT(ratio, 0.35) << "egress collapsed far below half: check submit pacing";
  EXPECT_LT(ratio, 0.75) << "egress did NOT fall with the input rate: the CBR "
                            "bits-per-frame assumption is FALSE and the input "
                            "throttle cannot work as designed - STOP and report";
}

// #601: Measure actual output for achievable, changing content at four rates.
// This does not prove a hard ceiling for arbitrary content: full-range noise
// saturates the encoder at QP 51. The original media-type-only configuration
// also passes this fixture; explicit codec properties are not a proven fix
// for saturation. Tolerance is +/-10% over six seconds after settling.
static void verifyConfiguredBitrate(const char* codec) {
  struct Case {
    const char* label;
    int kbps;
  };
  const Case cases[] = {{"cbr 2000", 2000}, {"cbr 4500", 4500},
                        {"cbr 6000", 6000}, {"cbr 10000", 10000}};
  std::vector<double> measured;
  for (const auto& c : cases) {
    const auto leg = runLeg(c.label, false, c.kbps, 1500, 6000, true, codec);
    if (leg.unavailable) {
      std::fprintf(stderr, "[  SKIPPED ] bitrate conformance: no hardware %s MFT\n", codec);
      return;
    }
    ASSERT_TRUE(leg.ran) << c.label << ": encoder emitted no bytes";
    EXPECT_GT(leg.measuredSeconds, 5.9) << "encoder stopped before the measurement window completed";
    // A bitrate pass bought by suppressing frames is not a rate-control pass.
    EXPECT_GT(leg.chunks / leg.measuredSeconds, kProbeFps * 0.97);
    const double kbps = static_cast<double>(leg.bytes) * 8.0 / leg.measuredSeconds / 1000.0;
    measured.push_back(kbps);
    EXPECT_GT(kbps, c.kbps * 0.90)
        << c.label << ": measured " << kbps << " kbps - encoder starved far below the "
        << "configured rate";
    EXPECT_LT(kbps, c.kbps * 1.10)
        << c.label << ": measured " << kbps << " kbps exceeds the configured rate tolerance";
  }
  // The defect's signature was that the rates were indistinguishable. Make that
  // impossible to pass: 10000 must actually cost about 5x what 2000 costs.
  ASSERT_EQ(measured.size(), 4u);
  EXPECT_GT(measured.back() / measured.front(), 3.0)
      << "10000 kbps and 2000 kbps produced nearly the same bytes - the setting is "
      << "not binding";
}

TEST(StreamBackpressureRateProbe, ConfiguredBitrateIsHonoured) { verifyConfiguredBitrate("h264"); }
TEST(StreamBackpressureRateProbe, HevcConfiguredBitrateIsHonoured) { verifyConfiguredBitrate("hevc"); }

TEST(StreamBackpressureRateProbe, VbrRespectsItsConfiguredPeakWithoutSheddingFrames) {
  for (const char* codec : {"h264", "hevc"}) {
    const auto leg = runLeg("vbr 6000 peak 9000", false, 6000, 1500, 6000, true, codec, "vbr");
    if (leg.unavailable) continue;
    ASSERT_TRUE(leg.ran);
    EXPECT_GT(leg.measuredSeconds, 5.9);
    EXPECT_GT(leg.chunks / leg.measuredSeconds, kProbeFps * 0.97);
    const double kbps = leg.bytes * 8.0 / leg.measuredSeconds / 1000;
    EXPECT_GT(kbps, 6000 * 0.70);
    EXPECT_LT(kbps, 9000 * 1.10);
  }
}
#endif
