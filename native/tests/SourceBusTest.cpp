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

// --- Task 2: SourceBus membership queries ---

TEST(SourceBus, ContainsAndSourceIdsReflectMembership) {
  SourceBus bus;
  EXPECT_FALSE(bus.contains("zoom:1"));
  bus.add(std::make_shared<ZoomParticipantSource>("zoom:1", 1280, 720));
  bus.add(std::make_shared<ZoomParticipantSource>("zoom:2", 1280, 720));
  EXPECT_TRUE(bus.contains("zoom:1"));
  EXPECT_TRUE(bus.contains("zoom:2"));
  auto ids = bus.sourceIds();
  ASSERT_EQ(ids.size(), 2u);
  EXPECT_EQ(ids[0], "zoom:1");   // std::map order
  EXPECT_EQ(ids[1], "zoom:2");
  bus.remove("zoom:1");
  EXPECT_FALSE(bus.contains("zoom:1"));
}

// --- Task 3: SourceBus::sourceFor accessor ---

TEST(SourceBus, SourceForReturnsTheStoredSourceOrNullptr) {
  SourceBus bus;
  EXPECT_EQ(bus.sourceFor("zoom:1"), nullptr);
  auto src = std::make_shared<ZoomParticipantSource>("zoom:1", 1280, 720);
  bus.add(src);
  EXPECT_EQ(bus.sourceFor("zoom:1"), src.get());
  bus.remove("zoom:1");
  EXPECT_EQ(bus.sourceFor("zoom:1"), nullptr);
}

// --- Task 3: syncZoomParticipantSources (roster -> bus) ---
#include "core/ZoomBusRoster.h"

static corevideo::modules::VideoFrame zoomFrame(const std::string& id, int64_t frameId) {
  corevideo::modules::VideoFrame f;
  f.participantId = id;
  f.i420 = std::make_shared<const std::vector<uint8_t>>(1280 * 720 * 3 / 2, 0x10);
  f.i420Width = 1280; f.i420Height = 720; f.frameId = frameId;
  return f;
}

TEST(ZoomBusRoster, AddsFeedsAndRemovesPerParticipant) {
  corevideo::core::SourceBus bus;
  // Also register a non-zoom source that must never be touched.
  bus.add(std::make_shared<corevideo::core::TestPatternSource>("test:pattern"));

  // Production keys Zoom sources by the RAW participant id (no "zoom:"
  // prefix) — matching the engine roster/continuity keying downstream in
  // MediaCore. Use raw-shaped ids here so this test actually proves removal
  // fires for the real keying, not a prefix that production never uses.
  corevideo::core::syncZoomParticipantSources(bus, {zoomFrame("16791552", 5), zoomFrame("22334455", 5)},
                                             {"16791552", "22334455"});
  EXPECT_TRUE(bus.contains("16791552"));
  EXPECT_TRUE(bus.contains("22334455"));
  EXPECT_TRUE(bus.contains("test:pattern"));

  // Ingest produces one frame per participant, keyed correctly.
  auto r = bus.ingest(0, 1000);
  int zoomFrames = 0;
  for (const auto& v : r.video) if (v.participantId == "16791552" || v.participantId == "22334455") ++zoomFrames;
  EXPECT_EQ(zoomFrames, 2);

  // 22334455 departs (gone from the engine roster AND no frame); 16791552
  // stays. test:pattern untouched (kind-based removal must never touch a
  // non-zoom source).
  corevideo::core::syncZoomParticipantSources(bus, {zoomFrame("16791552", 6)}, {"16791552"});
  EXPECT_TRUE(bus.contains("16791552"));
  EXPECT_FALSE(bus.contains("22334455"));
  EXPECT_TRUE(bus.contains("test:pattern"));
}

// A/B on 2026-09-19 (fake engine, program recorded to MP4): when the engine
// retires a participant's video subscription while that participant is still
// routed (budget eviction, spine churn around a Take), the pre-bus
// RealZoomCaptureSource store kept painting the participant's LAST frame until
// the engine roster dropped them; slice 1 removed the bus source on the first
// tick without a decoded frame, so program cut to the fallback slate for the
// whole gap (luma 188 -> 150 for ~500 ms). That is the "flashing sources" the
// owner saw live. Parity rule: hold the last frame while the engine still
// lists the participant; remove only when the roster has let them go.
TEST(ZoomBusRoster, HoldsLastFrameAcrossASubscriptionGapWhileTheEngineStillListsTheParticipant) {
  corevideo::core::SourceBus bus;
  corevideo::core::syncZoomParticipantSources(bus, {zoomFrame("101", 5), zoomFrame("103", 7)}, {"101", "103"});
  ASSERT_TRUE(bus.contains("103"));

  // Gap: no decoded frame for 103 this tick, but the engine roster still lists it.
  corevideo::core::syncZoomParticipantSources(bus, {zoomFrame("101", 6)}, {"101", "103"});
  EXPECT_TRUE(bus.contains("103")) << "source must survive a subscription gap";
  auto r = bus.ingest(0, 1000);
  bool heldLastFrame = false;
  for (const auto& v : r.video) {
    if (v.participantId == "103" && v.frameId == 7 && v.hasI420()) heldLastFrame = true;
  }
  EXPECT_TRUE(heldLastFrame) << "the held source must keep serving its last real frame";

  // The engine roster let 103 go (participant left / slot reassigned): now removed.
  corevideo::core::syncZoomParticipantSources(bus, {zoomFrame("101", 8)}, {"101"});
  EXPECT_FALSE(bus.contains("103"));
  EXPECT_TRUE(bus.contains("101"));
}

// Mirrors the roster-merge gate in MediaCore (`if (!engineFrames.empty())`):
// with an EMPTY engine roster the old path drew every stored frame and removed
// nothing, so an empty roster must not evict anything either.
TEST(ZoomBusRoster, EmptyEngineRosterRemovesNothing) {
  corevideo::core::SourceBus bus;
  corevideo::core::syncZoomParticipantSources(bus, {zoomFrame("101", 5)}, {"101"});
  corevideo::core::syncZoomParticipantSources(bus, {}, {});
  EXPECT_TRUE(bus.contains("101"));
}

// --- Task 1: CaptureDeviceSource ---
#include "core/CaptureDeviceSource.h"

static corevideo::modules::VideoFrame bgraFrame(const std::string& id, int w, int h, int64_t frameId) {
  corevideo::modules::VideoFrame f;
  f.participantId = id;
  f.width = f.pixelWidth = f.naturalWidth = w;
  f.height = f.pixelHeight = f.naturalHeight = h;
  f.pixelStride = w * 4;
  f.pixels = std::make_shared<const std::vector<uint8_t>>(static_cast<size_t>(w) * h * 4, 0x7f);
  f.frameId = frameId;
  return f;
}

TEST(CaptureDeviceSource, PollServesTheAdaptersLatestBgraFrameKeyedByDevice) {
  corevideo::core::CaptureDeviceSource src("capture:decklink-1", 640, 360);
  EXPECT_EQ(src.descriptor().sourceId, "capture:decklink-1");
  EXPECT_EQ(src.descriptor().kind, "capture");
  EXPECT_EQ(src.descriptor().pixelFormat, "bgra");
  EXPECT_TRUE(src.descriptor().hasVideo);

  auto warm = src.poll(0);
  EXPECT_TRUE(warm.video.empty());
  EXPECT_EQ(warm.health, corevideo::core::SourceHealth::Warming);

  src.setLatest(bgraFrame("capture:decklink-1", 640, 360, 7));
  auto tick = src.poll(0);
  ASSERT_EQ(tick.video.size(), 1u);
  EXPECT_EQ(tick.video[0].participantId, "capture:decklink-1");
  EXPECT_TRUE(tick.video[0].hasPixels());
  EXPECT_EQ(tick.video[0].frameId, 7);
  EXPECT_EQ(tick.health, corevideo::core::SourceHealth::Producing);
  EXPECT_EQ(src.counters().lastFrameId, 7);
  EXPECT_EQ(src.counters().framesIngested, 1u);
}

// --- Task 2: pure syncCaptureSources helper ---
#include "core/CaptureBusRoster.h"

// Capture adapters hold their own last frame and re-emit it every tick while the
// device is connected (WinUiCaptureDeviceAdapter::pollVideoFrames); a device that
// is absent from a tick has disconnected or never delivered, and today the
// compositor draws no frame for it. So, UNLIKE the Zoom rule (ZoomBusRoster.h,
// #554), the bus does NOT hold a capture frame: absent from the tick == removed.
TEST(CaptureBusRoster, MirrorsTheAdaptersTickAddsFeedsAndRemovesOnAbsence) {
  corevideo::core::SourceBus bus;
  bus.add(std::make_shared<corevideo::core::TestPatternSource>("test:pattern"));
  bus.add(std::make_shared<corevideo::core::ZoomParticipantSource>("16778240", 1280, 720));

  corevideo::core::syncCaptureSources(bus, {bgraFrame("capture:decklink-1", 640, 360, 1),
                                            bgraFrame("capture:browser:1", 1920, 1080, 1)});
  EXPECT_TRUE(bus.contains("capture:decklink-1"));
  EXPECT_TRUE(bus.contains("capture:browser:1"));
  EXPECT_EQ(bus.sourceFor("capture:decklink-1")->descriptor().kind, "capture");
  EXPECT_EQ(bus.sourceFor("capture:decklink-1")->descriptor().width, 640);

  auto r = bus.ingest(0, 1000);
  int captureFrames = 0;
  for (const auto& v : r.video) {
    if (v.participantId.rfind("capture:", 0) == 0) { ++captureFrames; EXPECT_TRUE(v.hasPixels()); }
  }
  EXPECT_EQ(captureFrames, 2);

  // decklink-1 disconnected (adapter emits nothing for it): removed this tick.
  // The Zoom and test sources are never touched by the capture sync.
  corevideo::core::syncCaptureSources(bus, {bgraFrame("capture:browser:1", 1920, 1080, 2)});
  EXPECT_FALSE(bus.contains("capture:decklink-1"));
  EXPECT_TRUE(bus.contains("capture:browser:1"));
  EXPECT_TRUE(bus.contains("16778240"));
  EXPECT_TRUE(bus.contains("test:pattern"));

  // A device that reconnects comes back as a fresh source.
  corevideo::core::syncCaptureSources(bus, {bgraFrame("capture:decklink-1", 1920, 1080, 1)});
  EXPECT_TRUE(bus.contains("capture:decklink-1"));
  EXPECT_EQ(bus.sourceFor("capture:decklink-1")->descriptor().width, 1920);
}

TEST(CaptureBusRoster, EmptyTickRemovesEveryCaptureSourceAndNothingElse) {
  corevideo::core::SourceBus bus;
  bus.add(std::make_shared<corevideo::core::ZoomParticipantSource>("16778240", 1280, 720));
  corevideo::core::syncCaptureSources(bus, {bgraFrame("capture:screen:3", 1024, 600, 1)});
  corevideo::core::syncCaptureSources(bus, {});
  EXPECT_FALSE(bus.contains("capture:screen:3"));
  EXPECT_TRUE(bus.contains("16778240"));
}
