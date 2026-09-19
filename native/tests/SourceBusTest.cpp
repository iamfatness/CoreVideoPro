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

// --- Task 2: SourceBus aggregator ---
#include "core/SourceBus.h"

using corevideo::core::SourceBus;
using corevideo::core::SourceHealth;

TEST(SourceBus, IngestMergesFramesAndCountsNewFrameIds) {
  SourceBus bus;
  bus.add(std::make_shared<TestPatternSource>("test:pattern"));
  ASSERT_FALSE(bus.empty());

  const auto r1 = bus.ingest(/*programTime100ns=*/10'000'000, /*nowNs=*/1'000);
  ASSERT_EQ(r1.video.size(), 1u);
  EXPECT_EQ(r1.video.front().participantId, "test:pattern");

  const auto r2 = bus.ingest(20'000'000, /*nowNs=*/2'000);
  ASSERT_EQ(r2.video.size(), 1u);

  const auto snap = bus.snapshot(/*nowNs=*/2'000);
  ASSERT_EQ(snap.size(), 1u);
  EXPECT_EQ(snap.front().counters.framesIngested, 2u);  // two NEW frameIds
  EXPECT_EQ(snap.front().health, SourceHealth::Producing);
}

TEST(SourceBus, AStaleSourceDecaysToStalled) {
  SourceBus bus;
  bus.add(std::make_shared<TestPatternSource>("test:pattern"));
  bus.ingest(10'000'000, /*nowNs=*/1'000);
  // No further ingest; read far in the future (> 200ms).
  const auto snap = bus.snapshot(/*nowNs=*/1'000 + 300'000'000);
  ASSERT_EQ(snap.size(), 1u);
  EXPECT_EQ(snap.front().health, SourceHealth::Stalled);
}

TEST(SourceBus, RemoveDropsTheSource) {
  SourceBus bus;
  bus.add(std::make_shared<TestPatternSource>("test:pattern"));
  bus.remove("test:pattern");
  EXPECT_TRUE(bus.empty());
  EXPECT_TRUE(bus.ingest(10'000'000, 1'000).video.empty());
}

// --- Task 2 review follow-up: coverage gaps ---
namespace {
class FixedFrameIdSource final : public corevideo::core::ISource {
 public:
  explicit FixedFrameIdSource(std::string sourceId = "test:fixed")
      : sourceId_(std::move(sourceId)) {
    descriptor_.sourceId = sourceId_;
    descriptor_.kind = "test";
    descriptor_.hasVideo = true;
  }

  const corevideo::core::SourceDescriptor& descriptor() const override {
    return descriptor_;
  }

  corevideo::core::SourceTick poll(int64_t /*programTime100ns*/) override {
    corevideo::modules::VideoFrame frame;
    frame.participantId = sourceId_;
    frame.frameId = 7;  // fixed: never advances
    corevideo::core::SourceTick tick;
    tick.video.push_back(std::move(frame));
    tick.health = corevideo::core::SourceHealth::Producing;
    return tick;
  }

  corevideo::core::SourceIngestCounters counters() const override {
    return counters_;
  }

 private:
  std::string sourceId_;
  corevideo::core::SourceDescriptor descriptor_;
  corevideo::core::SourceIngestCounters counters_;
};
}  // namespace

TEST(SourceBus, ARepeatedFrameIdDoesNotCountAsIngested) {
  SourceBus bus;
  bus.add(std::make_shared<FixedFrameIdSource>("test:fixed"));

  bus.ingest(10'000'000, /*nowNs=*/1'000);
  bus.ingest(20'000'000, /*nowNs=*/2'000);

  const auto snap = bus.snapshot(/*nowNs=*/2'000);
  ASSERT_EQ(snap.size(), 1u);
  EXPECT_EQ(snap.front().counters.framesIngested, 1u);
}

TEST(SourceBus, AnAddedButNeverIngestedSourceIsWarming) {
  SourceBus bus;
  bus.add(std::make_shared<TestPatternSource>("test:pattern"));

  const auto snap = bus.snapshot(/*nowNs=*/12345);
  ASSERT_EQ(snap.size(), 1u);
  EXPECT_EQ(snap.front().health, SourceHealth::Warming);
}
