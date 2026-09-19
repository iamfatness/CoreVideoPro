// native/tests/SourceBusTest.cpp
#include "core/TestPatternSource.h"
#include "compositor/CompositorLayout.h"

#include <gtest/gtest.h>

using corevideo::core::TestPatternSource;

TEST(SourceContract, TestPatternSourceProducesSmpteBars) {
  TestPatternSource src("test:pattern", 640, 360);
  EXPECT_EQ(src.descriptor().sourceId, "test:pattern");
  EXPECT_EQ(src.descriptor().width, 640);
  EXPECT_TRUE(src.descriptor().hasVideo);

  const auto tick = src.poll(/*programTime100ns=*/10'000'000);
  ASSERT_EQ(tick.video.size(), 1u);
  const auto& frame = tick.video.front();
  EXPECT_EQ(frame.participantId, "test:pattern");
  EXPECT_TRUE(frame.hasPixels());
  EXPECT_EQ(frame.pixelWidth, 640);
  EXPECT_EQ(frame.pixelHeight, 360);
  EXPECT_EQ(frame.pixelStride, 640 * 4);
  EXPECT_GE(frame.frameId, 1);

  // Center bar is green (BGRA) — real pattern, not a slate.
  const auto& px = *frame.pixels;
  const size_t row = static_cast<size_t>(360 / 2) * static_cast<size_t>(640 * 4);
  const size_t center = row + static_cast<size_t>(640 / 2) * 4;
  EXPECT_EQ(px[center + 0], 0);    // B
  EXPECT_EQ(px[center + 1], 255);  // G
  EXPECT_EQ(px[center + 2], 0);    // R

  // frameId advances each poll; counters follow.
  const auto tick2 = src.poll(20'000'000);
  EXPECT_GT(tick2.video.front().frameId, frame.frameId);
  EXPECT_EQ(src.counters().framesIngested, 2u);
}
