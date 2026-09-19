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

// --- Task 3: MediaCore ingests the bus and composites it into program (F1 gate) ---
#include "core/MediaCore.h"
#include "modules/Interfaces.h"
#include "modules/ProgramFramePreview.h"

TEST(SourceBusMediaCore, ATestPatternBusSourceCompositesIntoProgram) {
  corevideo::core::MediaCore core(corevideo::modules::createStubModules());
  core.addSourceForTest(std::make_shared<corevideo::core::TestPatternSource>("test:pattern"));

  // Route the bus source full-frame to program.
  (void)core.applyCommands(corevideo::rpc::Json::Array{corevideo::rpc::Json::Object{
      {"type", "load-scene-graph"}, {"sceneId", "bus"},
      {"routes", corevideo::rpc::Json::Array{
          corevideo::rpc::Json::Object{{"routeId", "tp"}, {"mode", "fixed"}, {"participantId", "test:pattern"}}}}}});

  // Drive a few render ticks to settle.
  for (int i = 0; i < 3; ++i) {
    core.renderDisplayTick();
  }

  // Metadata: the bus source shows up in the published program video sources.
  // Keep the returned state alive in a local — sessionState() returns by
  // value, and a pointer taken from a temporary dangles past this statement.
  const auto state = core.sessionState();
  const auto* videoSources = state.get("programFrame")->get("videoSources");
  ASSERT_NE(videoSources, nullptr);
  bool foundTestPattern = false;
  for (const auto& src : videoSources->asArray()) {
    const auto* sourceId = src.get("sourceId");
    const auto* participantId = src.get("participantId");
    if ((sourceId && sourceId->asString() == "test:pattern") ||
        (participantId && participantId->asString() == "test:pattern")) {
      foundTestPattern = true;
      break;
    }
  }
  EXPECT_TRUE(foundTestPattern);

  // Pixels: the F1 CPU-compositor gate — center samples the SMPTE green bar.
  const auto previews = core.drainProgramFramePreviewEvents();
  ASSERT_FALSE(previews.empty());
  const auto* preview = previews.back().get("preview");
  if (preview == nullptr) {
    preview = previews.back().get("programFramePreview");
  }
  ASSERT_NE(preview, nullptr);
  const int w = static_cast<int>(preview->get("width")->asNumber());
  const int h = static_cast<int>(preview->get("height")->asNumber());
  const auto px = corevideo::modules::base64Decode(preview->getString("bgraBase64"));
  const size_t c = ((static_cast<size_t>(h / 2)) * static_cast<size_t>(w) + static_cast<size_t>(w / 2)) * 4;
  ASSERT_LE(c + 3, px.size());
  EXPECT_GT(px[c + 1], 200);  // G high  (green SMPTE center bar)
  EXPECT_LT(px[c + 2], 80);   // R low
  EXPECT_LT(px[c + 0], 80);   // B low
  const uint32_t centerPixel = (static_cast<uint32_t>(px[c + 3]) << 24) |
                                (static_cast<uint32_t>(px[c + 2]) << 16) |
                                (static_cast<uint32_t>(px[c + 1]) << 8) |
                                static_cast<uint32_t>(px[c + 0]);
  EXPECT_NE(centerPixel, corevideo::compositor::colorFromParticipantId("test:pattern"));
}

// --- Task 4: sessionState() sources[] node ---

TEST(SourceBusSnapshot, SourcesNodeCarriesPerSourceCounters) {
  corevideo::core::MediaCore core(corevideo::modules::createStubModules());
  core.addSourceForTest(std::make_shared<corevideo::core::TestPatternSource>("test:pattern"));

  for (int i = 0; i < 3; ++i) {
    core.renderDisplayTick();
  }

  const auto state = core.sessionState();
  const auto* sources = state.get("sources");
  ASSERT_NE(sources, nullptr);
  const auto& arr = sources->asArray();
  ASSERT_EQ(arr.size(), 1u);
  EXPECT_EQ(arr.front().getString("sourceId"), "test:pattern");
  EXPECT_GE(arr.front().get("framesIngested")->asNumber(), 1.0);
  EXPECT_EQ(arr.front().get("droppedFrames")->asNumber(), 0.0);
}

TEST(SourceBusSnapshot, AFreshMediaCoreWithNoSourceStillEmitsAnEmptySourcesArray) {
  corevideo::core::MediaCore core(corevideo::modules::createStubModules());

  const auto state = core.sessionState();
  const auto* sources = state.get("sources");
  ASSERT_NE(sources, nullptr);
  EXPECT_TRUE(sources->asArray().empty());
}

// --- Task 1: ZoomParticipantSource ---

#include "core/ZoomParticipantSource.h"
using corevideo::core::ZoomParticipantSource;

TEST(ZoomParticipantSource, PollReturnsTheSetFrameKeyedByParticipant) {
  ZoomParticipantSource src("zoom:42", 1280, 720);
  EXPECT_EQ(src.descriptor().sourceId, "zoom:42");
  EXPECT_EQ(src.descriptor().kind, "zoom");
  EXPECT_TRUE(src.descriptor().hasVideo);
  // No frame yet -> Warming, no video.
  auto warmup = src.poll(0);
  EXPECT_TRUE(warmup.video.empty());
  EXPECT_EQ(warmup.health, corevideo::core::SourceHealth::Warming);

  corevideo::modules::VideoFrame f;
  f.participantId = "zoom:42";
  f.i420 = std::make_shared<const std::vector<uint8_t>>(1280 * 720 * 3 / 2, 0x10);
  f.i420Width = 1280; f.i420Height = 720; f.frameId = 7;
  src.setLatest(f);
  auto tick = src.poll(0);
  ASSERT_EQ(tick.video.size(), 1u);
  EXPECT_EQ(tick.video.front().participantId, "zoom:42");
  EXPECT_EQ(tick.video.front().frameId, 7);
  EXPECT_TRUE(tick.video.front().hasI420());
  EXPECT_EQ(tick.health, corevideo::core::SourceHealth::Producing);
}
