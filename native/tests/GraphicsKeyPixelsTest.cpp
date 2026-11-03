#include "modules/GraphicsKeyPixels.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using corevideo::modules::CompositorRenderPlanLayer;
using corevideo::modules::graphicsKeyFrameBytes;
using corevideo::modules::graphicsKeyLayerSelected;
using corevideo::modules::unpremultiplyBgra;
using corevideo::modules::unpremultiplyBgraRow;

namespace {

CompositorRenderPlanLayer layer(const std::string& kind, const std::string& instanceId, bool overlayContent) {
  CompositorRenderPlanLayer value;
  value.layerId = kind + ":" + (instanceId.empty() ? "none" : instanceId);
  value.kind = kind;
  value.graphicsInstanceId = instanceId;
  value.hasOverlayContent = overlayContent;
  return value;
}

}  // namespace

// The key carries exactly the native graphics instances the On Air strip shows
// on Program. Video layers and legacy overlays without an instance are
// Program-only: the whole point of the output is "our graphics over THEIR
// picture", so a Program pixel source must never be selected.
TEST(GraphicsKeyPixels, SelectsOnlyNativeGraphicsInstanceLayers) {
  EXPECT_TRUE(graphicsKeyLayerSelected(layer("overlay", "featured", true)));
  EXPECT_FALSE(graphicsKeyLayerSelected(layer("overlay", "", true)));            // legacy lower-third asset
  EXPECT_FALSE(graphicsKeyLayerSelected(layer("participant-video", "", false)));
  EXPECT_FALSE(graphicsKeyLayerSelected(layer("media-video", "", false)));
  EXPECT_FALSE(graphicsKeyLayerSelected(layer("overlay", "featured", false)));   // instance id without raster content
}

// Premultiplied -> straight: alpha 255 passes through exactly, alpha 0 becomes
// fully zero (no colour noise for receivers that ignore alpha), and a
// half-covered pixel's colour doubles back to what the graphic drew.
TEST(GraphicsKeyPixels, UnpremultiplyIsExactAtTheEndpointsAndRecoversMidAlpha) {
  const std::vector<uint8_t> src = {
      10, 20, 30, 255,   // opaque: unchanged
      7, 9, 11, 0,       // transparent: colour discarded
      64, 32, 128, 128,  // half alpha: colour doubles (rounded)
      200, 100, 50, 64,  // quarter alpha: x4, clamped at 255
  };
  std::vector<uint8_t> dst(src.size(), 0xCD);
  unpremultiplyBgraRow(src.data(), dst.data(), 4);
  const std::vector<uint8_t> expected = {
      10, 20, 30, 255,
      0, 0, 0, 0,
      128, 64, 255, 128,
      255, 255, 199, 64,
  };
  EXPECT_EQ(expected, dst);
}

// The staging surface a GPU hands back is row-padded; the published frame is
// tightly packed. Padding bytes must never reach the output and every row
// must be converted, so a receiver sees the key at the exact Program size.
TEST(GraphicsKeyPixels, WholeFrameHonoursSourcePitchAndPacksRows) {
  constexpr int kW = 3;
  constexpr int kH = 2;
  constexpr std::size_t kPitch = 20;  // 12 payload bytes + 8 padding per row
  std::vector<uint8_t> src(kPitch * kH, 0xEE);
  const auto put = [&](int x, int y, uint8_t b, uint8_t g, uint8_t r, uint8_t a) {
    uint8_t* p = src.data() + static_cast<std::size_t>(y) * kPitch + static_cast<std::size_t>(x) * 4u;
    p[0] = b; p[1] = g; p[2] = r; p[3] = a;
  };
  put(0, 0, 0, 0, 0, 0);
  put(1, 0, 255, 255, 255, 255);
  put(2, 0, 100, 50, 0, 200);
  put(0, 1, 1, 2, 3, 255);
  put(1, 1, 0, 0, 0, 0);
  put(2, 1, 60, 60, 60, 120);

  std::vector<uint8_t> dst(graphicsKeyFrameBytes(kW, kH), 0x00);
  ASSERT_EQ(dst.size(), 24u);
  unpremultiplyBgra(src.data(), kPitch, kW, kH, dst.data());

  const std::vector<uint8_t> expected = {
      0, 0, 0, 0,  255, 255, 255, 255,  128, 64, 0, 200,
      1, 2, 3, 255,  0, 0, 0, 0,  128, 128, 128, 120,
  };
  EXPECT_EQ(expected, dst);
  // No padding byte (0xEE) can appear anywhere in the packed output.
  for (const auto byte : dst) EXPECT_NE(byte, 0xEE);
}

namespace {
using corevideo::modules::GraphicsKeyFrame;
using corevideo::modules::GraphicsKeyRetryLatch;
using corevideo::modules::GraphicsKeyRing;
using corevideo::modules::GraphicsKeySelection;
using corevideo::modules::ProgramFrame;

GraphicsKeyFrame keyFrame(int64_t frameNumber, int64_t publishedMs, uint8_t fill = 0x55) {
  GraphicsKeyFrame frame;
  frame.bgra = std::make_shared<const std::vector<uint8_t>>(graphicsKeyFrameBytes(4, 2), fill);
  frame.width = 4;
  frame.height = 2;
  frame.frameNumber = frameNumber;
  frame.publishedMs = publishedMs;
  return frame;
}
}  // namespace

// Review item 3: with a Program buffer the output worker sends a DELAYED
// Program frame. The key it gets must be the one rendered with that frame, not
// the newest, or graphics lead Program by the buffer depth.
TEST(GraphicsKeyPixels, RingSelectsTheKeyRenderedWithTheDelayedProgramFrame) {
  GraphicsKeyRing ring;
  ring.arm(1000);
  for (int64_t n = 10; n <= 15; ++n) ring.publish(keyFrame(n, 1000 + n));
  GraphicsKeySelection selection;
  ASSERT_TRUE(ring.select(12, 1020, selection));  // buffer depth 3 behind the newest (15)
  EXPECT_FALSE(selection.stale);
  EXPECT_TRUE(selection.exact);
  EXPECT_EQ(12, selection.frame.frameNumber);
  // Readback one frame behind an unbuffered Program: the newest at-or-before.
  ASSERT_TRUE(ring.select(16, 1020, selection));
  EXPECT_EQ(15, selection.frame.frameNumber);
  EXPECT_FALSE(selection.exact);
  // Capacity: the oldest entries roll off; never more than kCapacity held.
  ring.publish(keyFrame(16, 1016));
  EXPECT_EQ(GraphicsKeyRing::kCapacity, ring.size());
  ASSERT_TRUE(ring.select(5, 1020, selection));  // older than everything: closest, not exact
  EXPECT_EQ(11, selection.frame.frameNumber);
  EXPECT_FALSE(selection.exact);
}

// Review item 1: a key that is never produced (rig cannot build) or stops
// being produced (Map failures, render stall) goes STALE after 250 ms instead
// of the last graphic being re-sent forever.
TEST(GraphicsKeyPixels, RingWarmsThenGoesStaleWhenNoKeyArrivesOrKeysStop) {
  GraphicsKeyRing ring;
  GraphicsKeySelection selection;
  EXPECT_FALSE(ring.select(1, 0, selection));  // not armed: no key requested
  ring.arm(1000);
  EXPECT_FALSE(ring.select(1, 1249, selection));  // warming
  ASSERT_TRUE(ring.select(1, 1250, selection));   // never produced: stale
  EXPECT_TRUE(selection.stale);
  EXPECT_FALSE(selection.frame.bgra);

  ring.publish(keyFrame(7, 2000));
  ASSERT_TRUE(ring.select(7, 2249, selection));
  EXPECT_FALSE(selection.stale);
  EXPECT_TRUE(selection.frame.bgra != nullptr);
  ASSERT_TRUE(ring.select(9, 2250, selection));   // readbacks stopped 250 ms ago
  EXPECT_TRUE(selection.stale);
  EXPECT_FALSE(selection.frame.bgra);
}

// The output worker stamps a TRANSPARENT key for a stale selection, and the
// sender reports key-stale (a warning) while still sending it, so the
// downstream keyer clears. A fresh key is live/ok; no pixels is "starting".
TEST(GraphicsKeyPixels, StaleKeyIsSentTransparentAndReportedKeyStale) {
  ProgramFrame frame;
  frame.width = 8;
  frame.height = 4;
  std::shared_ptr<const std::vector<uint8_t>> cache;

  GraphicsKeySelection stale;
  stale.stale = true;
  corevideo::modules::stampGraphicsKey(frame, stale, cache);
  ASSERT_TRUE(frame.graphicsKeyBgraShared != nullptr);
  EXPECT_EQ(8, frame.graphicsKeyWidth);
  EXPECT_EQ(4, frame.graphicsKeyHeight);
  EXPECT_TRUE(frame.graphicsKeyStale);
  EXPECT_EQ(graphicsKeyFrameBytes(8, 4), frame.graphicsKeyBgraShared->size());
  for (const auto byte : *frame.graphicsKeyBgraShared) EXPECT_EQ(0, byte);
  auto verdict = corevideo::modules::graphicsKeySendVerdict(frame);
  EXPECT_TRUE(verdict.send);
  EXPECT_EQ("warning", verdict.status);
  EXPECT_EQ("key-stale", verdict.resultCode);
  EXPECT_FALSE(verdict.warning.empty());
  const auto* firstTransparent = frame.graphicsKeyBgraShared.get();
  corevideo::modules::stampGraphicsKey(frame, stale, cache);
  EXPECT_EQ(firstTransparent, frame.graphicsKeyBgraShared.get());  // reused, no per-tick allocation

  GraphicsKeySelection fresh;
  fresh.frame = keyFrame(3, 10, 0x80);
  corevideo::modules::stampGraphicsKey(frame, fresh, cache);
  EXPECT_FALSE(frame.graphicsKeyStale);
  EXPECT_EQ(fresh.frame.bgra.get(), frame.graphicsKeyBgraShared.get());
  EXPECT_EQ(3, frame.graphicsKeyFrameNumber);
  verdict = corevideo::modules::graphicsKeySendVerdict(frame);
  EXPECT_TRUE(verdict.send);
  EXPECT_EQ("live", verdict.status);
  EXPECT_EQ("ok", verdict.resultCode);

  ProgramFrame empty;
  verdict = corevideo::modules::graphicsKeySendVerdict(empty);
  EXPECT_FALSE(verdict.send);
  EXPECT_EQ("key-pixels-missing", verdict.resultCode);
}

// Review item 2: a failing rig is retried for the same size only every 5 s,
// a new size is tried at once, and each failing size is logged once.
TEST(GraphicsKeyPixels, RetryLatchBacksOffPerSizeAndLogsEachFailingSizeOnce) {
  GraphicsKeyRetryLatch latch;
  EXPECT_TRUE(latch.shouldAttempt(1920, 1080, 0));
  EXPECT_TRUE(latch.noteFailure(1920, 1080, 0));
  EXPECT_FALSE(latch.shouldAttempt(1920, 1080, 16));
  EXPECT_FALSE(latch.shouldAttempt(1920, 1080, 4999));
  EXPECT_TRUE(latch.shouldAttempt(3840, 2160, 16));  // a new size is not held back
  EXPECT_TRUE(latch.shouldAttempt(1920, 1080, 5000));
  EXPECT_FALSE(latch.noteFailure(1920, 1080, 5000));  // same size again: not re-logged
  EXPECT_TRUE(latch.noteFailure(3840, 2160, 5001));   // a different failing size is logged
  latch.noteSuccess();
  EXPECT_TRUE(latch.shouldAttempt(3840, 2160, 5002));
  EXPECT_TRUE(latch.noteFailure(3840, 2160, 5003));   // a failure after recovery is logged again
}

// Review item 6: the all-0 / all-255 row fast paths must give exactly what the
// per-pixel rule gives (colour under alpha 0 discarded, alpha 255 copied).
TEST(GraphicsKeyPixels, RowFastPathsMatchThePerPixelRule) {
  constexpr int kW = 5;
  std::vector<uint8_t> transparentRow(kW * 4), opaqueRow(kW * 4), mixedRow(kW * 4);
  for (int x = 0; x < kW; ++x) {
    const uint8_t v = static_cast<uint8_t>(17 * (x + 1));
    uint8_t* t = &transparentRow[x * 4]; t[0] = v; t[1] = v; t[2] = v; t[3] = 0;
    uint8_t* o = &opaqueRow[x * 4]; o[0] = v; o[1] = static_cast<uint8_t>(v + 1); o[2] = static_cast<uint8_t>(v + 2); o[3] = 255;
    uint8_t* m = &mixedRow[x * 4]; m[0] = v / 2; m[1] = v / 2; m[2] = v / 2; m[3] = x == 2 ? 128 : 255;
  }
  std::vector<uint8_t> out(kW * 4, 0xCD);
  unpremultiplyBgraRow(transparentRow.data(), out.data(), kW);
  EXPECT_EQ(std::vector<uint8_t>(kW * 4, 0), out);
  unpremultiplyBgraRow(opaqueRow.data(), out.data(), kW);
  EXPECT_EQ(opaqueRow, out);
  unpremultiplyBgraRow(mixedRow.data(), out.data(), kW);
  for (int x = 0; x < kW; ++x) {
    if (x == 2) {
      EXPECT_EQ(128, out[x * 4 + 3]);
      EXPECT_EQ(static_cast<int>((mixedRow[x * 4] * 255u + 64u) / 128u), static_cast<int>(out[x * 4]));
    } else {
      EXPECT_EQ(mixedRow[x * 4], out[x * 4]);
      EXPECT_EQ(255, out[x * 4 + 3]);
    }
  }
}

// Cost evidence for the doc (not a gate): CPU time to unpremultiply one key
// frame at 1080p and 4K, for a typical lower third (most rows transparent)
// and the worst case (every row mixed). Printed, never asserted.
TEST(GraphicsKeyPixels, UnpremultiplyCostIsMeasuredForTypicalAndWorstFrames) {
  const int sizes[2][2] = {{1920, 1080}, {3840, 2160}};
  for (const auto& size : sizes) {
    const int w = size[0], h = size[1];
    std::vector<uint8_t> typical(graphicsKeyFrameBytes(w, h), 0), worst(graphicsKeyFrameBytes(w, h));
    // Typical: a lower-third band over the bottom fifth: anti-aliased edge rows
    // mixed, interior rows opaque, everything above transparent.
    const int bandTop = h * 78 / 100, bandBottom = h * 96 / 100;
    for (int y = bandTop; y < bandBottom; ++y) {
      const bool edge = y < bandTop + 3 || y >= bandBottom - 3;
      for (int x = 0; x < w; ++x) {
        uint8_t* p = &typical[(static_cast<std::size_t>(y) * w + x) * 4];
        const uint8_t a = edge ? static_cast<uint8_t>(64 + (x % 128)) : 255;
        p[0] = p[1] = p[2] = static_cast<uint8_t>(a / 2); p[3] = a;
      }
    }
    for (std::size_t i = 0; i < worst.size(); i += 4) {
      const uint8_t a = static_cast<uint8_t>(1 + (i / 4) % 254);
      worst[i] = worst[i + 1] = worst[i + 2] = static_cast<uint8_t>(a / 2); worst[i + 3] = a;
    }
    std::vector<uint8_t> out(graphicsKeyFrameBytes(w, h));
    const auto time = [&](const std::vector<uint8_t>& src) {
      constexpr int kRuns = 10;
      const auto start = std::chrono::steady_clock::now();
      for (int run = 0; run < kRuns; ++run) unpremultiplyBgra(src.data(), static_cast<std::size_t>(w) * 4u, w, h, out.data());
      return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / kRuns;
    };
    const double typicalMs = time(typical);
    const double worstMs = time(worst);
    std::printf("[graphics-key-cost] %dx%d typical_lower_third_ms=%.2f worst_all_mixed_ms=%.2f\n", w, h, typicalMs, worstMs);
    EXPECT_EQ(out.size(), graphicsKeyFrameBytes(w, h));
  }
}
