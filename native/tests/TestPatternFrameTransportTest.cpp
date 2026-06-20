#include "compositor/CompositorLayout.h"
#include "modules/Interfaces.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace {

using corevideo::modules::CompositorRenderPlan;
using corevideo::modules::CompositorRenderPlanLayer;
using corevideo::modules::FramePool;
using corevideo::modules::VideoFrame;

uint32_t previewPixelRgba(const corevideo::modules::ProgramFramePreviewPixels& preview, int x, int y) {
  const int cx = std::max(0, std::min(x, preview.width - 1));
  const int cy = std::max(0, std::min(y, preview.height - 1));
  const size_t offset = static_cast<size_t>((cy * preview.width + cx) * 4);
  return (static_cast<uint32_t>(preview.bgra[offset + 3]) << 24) |
         (static_cast<uint32_t>(preview.bgra[offset + 2]) << 16) |
         (static_cast<uint32_t>(preview.bgra[offset + 1]) << 8) |
         static_cast<uint32_t>(preview.bgra[offset + 0]);
}

}  // namespace

// --- The F1 frame pool: bounded, ref-counted, with dropped-frame back-pressure.

TEST(FramePool, RecyclesBuffersAndCountsDropsWhenExhausted) {
  auto pool = FramePool::create(2, 16);
  ASSERT_NE(pool, nullptr);

  auto a = pool->acquire(16);
  auto b = pool->acquire(16);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(pool->outstanding(), 2u);

  // Third acquire with both buffers still held is back-pressure: dropped frame.
  auto c = pool->acquire(16);
  EXPECT_EQ(c, nullptr);
  EXPECT_EQ(pool->droppedFrames(), 1);

  // Releasing one buffer lets the next acquire succeed from the free list.
  a.reset();
  EXPECT_EQ(pool->outstanding(), 1u);
  auto d = pool->acquire(16);
  EXPECT_NE(d, nullptr);
  EXPECT_EQ(pool->droppedFrames(), 1);
}

TEST(FramePool, StaysAliveWhileAcquiredBuffersOutliveTheOwner) {
  std::shared_ptr<std::vector<uint8_t>> buffer;
  {
    auto pool = FramePool::create(1, 8);
    buffer = pool->acquire(8);
    ASSERT_NE(buffer, nullptr);
  }
  // The pool's owning shared_ptr is gone, but the acquired buffer kept it alive
  // through its deleter; writing to the buffer must not crash.
  ASSERT_GE(buffer->size(), 8u);
  (*buffer)[0] = 42;
  EXPECT_EQ((*buffer)[0], 42);
}

// --- The deterministic test-pattern source publishes real BGRA pixels.

TEST(TestPatternCaptureDevice, PollPublishesPopulatedBgraFramesAndCounters) {
  auto device = corevideo::modules::createTestPatternCaptureDevice("test-pattern-1", 320, 180, 30);
  ASSERT_NE(device, nullptr);

  // Before any poll, the source reports no signal and no ingested frames.
  auto before = device->enumerate();
  ASSERT_EQ(before.size(), 1u);
  EXPECT_FALSE(before.front().signalPresent);
  EXPECT_EQ(before.front().framesIngested, 0);

  const auto frames = device->pollVideoFrames(100);
  ASSERT_EQ(frames.size(), 1u);
  const auto& frame = frames.front();
  EXPECT_TRUE(frame.hasPixels());
  EXPECT_EQ(frame.pixelWidth, 320);
  EXPECT_EQ(frame.pixelHeight, 180);
  EXPECT_EQ(frame.pixelStride, 320 * 4);
  EXPECT_EQ(frame.timestampMs, 100);
  EXPECT_EQ(frame.participantId, "capture:test-pattern-1");
  ASSERT_NE(frame.pixels, nullptr);
  EXPECT_EQ(frame.pixels->size(), static_cast<size_t>(320 * 180 * 4));

  // After producing a frame, the snapshot reports a real signal + counter.
  auto after = device->enumerate();
  EXPECT_TRUE(after.front().signalPresent);
  EXPECT_EQ(after.front().framesIngested, 1);
}

// --- End-to-end: test-pattern pixels flow through the CPU compositor into a
//     non-empty, correctly-sized ProgramFrame preview (the F1 gate).

TEST(F1FrameTransport, TestPatternPixelsReachProgramFrameThroughCpuCompositor) {
  auto modules = corevideo::modules::createStubModules();
  ASSERT_NE(modules.compositor, nullptr);
  ASSERT_NE(modules.captureDevice, nullptr);

  const auto frames = modules.captureDevice->pollVideoFrames(33);
  // The composite stub device includes the deterministic test pattern, so at
  // least one polled frame carries real pixels.
  const VideoFrame* pixelFrame = nullptr;
  for (const auto& frame : frames) {
    if (frame.hasPixels()) {
      pixelFrame = &frame;
      break;
    }
  }
  ASSERT_NE(pixelFrame, nullptr) << "Test-pattern source must publish populated pixels.";

  CompositorRenderPlan plan;
  plan.renderPlanId = "f1-test-pattern";
  plan.width = 1280;
  plan.height = 720;
  CompositorRenderPlanLayer layer;
  layer.layerId = "route:test-pattern";
  layer.kind = "participant-video";
  layer.participantId = pixelFrame->participantId;
  layer.sourceId = "capture:test-pattern-1";
  layer.order = 0;
  layer.rect = {0.f, 0.f, 1.f, 1.f};
  plan.layers.push_back(layer);

  const auto programFrame = modules.compositor->render(plan, {*pixelFrame});

  // Non-empty, correctly-sized program-frame preview pixels.
  EXPECT_EQ(programFrame.renderPlanId, "f1-test-pattern");
  EXPECT_GT(programFrame.preview.width, 0);
  EXPECT_GT(programFrame.preview.height, 0);
  EXPECT_EQ(
      programFrame.preview.bgra.size(),
      static_cast<size_t>(programFrame.preview.width * programFrame.preview.height * 4));
  EXPECT_NE(programFrame.programPixelSignature, 0u);

  // The composed pixels must be the real test-pattern bars, NOT the synthetic
  // participant slate color. Sampling several columns must reveal more than one
  // distinct bar color (color bars are not a flat fill).
  const int midY = programFrame.preview.height / 2;
  const uint32_t slate = corevideo::compositor::colorFromParticipantId(pixelFrame->participantId);
  bool sawNonSlate = false;
  uint32_t firstColor = previewPixelRgba(programFrame.preview, 0, midY);
  bool sawSecondColor = false;
  for (int x = 0; x < programFrame.preview.width; ++x) {
    const uint32_t color = previewPixelRgba(programFrame.preview, x, midY);
    if (color != slate) {
      sawNonSlate = true;
    }
    if (color != firstColor) {
      sawSecondColor = true;
    }
  }
  EXPECT_TRUE(sawNonSlate) << "Program frame must carry real test-pattern pixels, not the slate.";
  EXPECT_TRUE(sawSecondColor) << "Test pattern must render multiple distinct bar colors.";
}
