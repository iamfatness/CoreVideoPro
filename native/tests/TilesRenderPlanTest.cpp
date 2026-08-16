#include "core/MediaCore.h"
#include "modules/Interfaces.h"
#include "rpc/Json.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace {

using corevideo::core::MediaCore;

int countLayersOfKind(const corevideo::modules::CompositorRenderPlan& plan,
                      const std::string& kind) {
  return static_cast<int>(std::count_if(
      plan.layers.begin(), plan.layers.end(),
      [&](const auto& layer) { return layer.kind == kind; }));
}

const corevideo::modules::CompositorRenderPlanLayer* findLayer(
    const corevideo::modules::CompositorRenderPlan& plan, const std::string& layerId) {
  for (const auto& layer : plan.layers) {
    if (layer.layerId == layerId) {
      return &layer;
    }
  }
  return nullptr;
}

// Build the command with the Json::Object/Json::Array literal pattern used
// throughout MediaCoreCommandTest.cpp. Json has no .set(), and Json::parse
// returns std::optional<Json> — see the API facts in Global Constraints.
void loadWall(MediaCore& core, const std::vector<std::string>& members) {
  corevideo::rpc::Json::Array memberJson;
  for (const auto& member : members) {
    memberJson.push_back(corevideo::rpc::Json{member});
  }
  (void)core.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json{corevideo::rpc::Json::Object{
          {"type", corevideo::rpc::Json{"load-scene-graph"}},
          {"sceneId", corevideo::rpc::Json{"s"}},
          {"routes", corevideo::rpc::Json{corevideo::rpc::Json::Array{}}},
          {"tiles", corevideo::rpc::Json{corevideo::rpc::Json::Object{
              {"layerId", corevideo::rpc::Json{"tiles:s"}},
              {"members", corevideo::rpc::Json{memberJson}},
              {"style", corevideo::rpc::Json{corevideo::rpc::Json::Object{
                  {"backgroundColor", corevideo::rpc::Json{"#101418"}}}}}}}}}}});
}

// C1 fix vehicle: a capture device that is ALWAYS connected and delivers a
// real-pixel frame keyed "capture:frozen-1" every poll, with a controllable
// frameId. Used by the I4 test to reproduce the frozen-but-subscribed-guest
// defect against the REAL production tick (not the {}-frames test seam),
// exactly the way a stalled UVC/SDI capture card would: pollVideoFrames still
// returns a frame every tick (the device never disappears), but the payload
// never changes.
class FrozenFrameIdCaptureDevice final : public corevideo::modules::ICaptureDevice {
 public:
  std::vector<corevideo::modules::CaptureDeviceInfo> enumerate() const override { return {}; }
  std::vector<corevideo::modules::CaptureDeviceInfo> selectInput(const std::string&,
                                                                  const std::string&) override {
    return {};
  }
  std::vector<corevideo::modules::CaptureDeviceInfo> setAudioSyncOffset(const std::string&, int) override {
    return {};
  }
  std::vector<corevideo::modules::CaptureDeviceInfo> connect(const std::string&) override { return {}; }

  std::vector<corevideo::modules::VideoFrame> pollVideoFrames(int64_t timestampMs) override {
    corevideo::modules::VideoFrame frame;
    frame.participantId = "capture:frozen-1";
    frame.width = frame.naturalWidth = frame.pixelWidth = 64;
    frame.height = frame.naturalHeight = frame.pixelHeight = 64;
    frame.pixelStride = 64 * 4;
    // Re-stamped with the CURRENT tick's clock every poll — exactly what
    // every real producer in this codebase does (ZoomEngineRuntime.cpp, the
    // capture adapters), even when re-serving a held/frozen frame. This is
    // the part of the defect that made the OLD (timestampMs-based) age
    // computation always read ~0.
    frame.timestampMs = timestampMs;
    // NEVER advances — the frozen-guest scenario I4 exists to catch.
    frame.frameId = 1;
    frame.pixels = pixels_;
    return {frame};
  }

 private:
  std::shared_ptr<const std::vector<uint8_t>> pixels_ =
      std::make_shared<std::vector<uint8_t>>(64 * 64 * 4, 128);
};

}  // namespace

TEST(TilesRenderPlan, EachAdmittedMemberBecomesOneTileLayer) {
  MediaCore core; loadWall(core, {"zoom:1", "zoom:2", "zoom:3"});
  core.setTilesMemberFrameAgesForTest({{"zoom:1", true, 0}, {"zoom:2", true, 0}, {"zoom:3", true, 0}});

  const auto plan = core.lastRenderPlanForTest();
  EXPECT_EQ(countLayersOfKind(plan, "participant-video"), 3);
  ASSERT_NE(findLayer(plan, "tile:zoom:2"), nullptr);
  EXPECT_EQ(findLayer(plan, "tile:zoom:2")->sourceId, "zoom:2");

  // Review fix: assert the EXACT layer id set, not just a count — a count
  // alone cannot distinguish "3 tiles + nothing else" from "3 tiles + 2
  // leaked legacy-fallback layers" (the interleave C2 fixed elsewhere), so
  // the wall's interaction with the rest of the plan is actually under test.
  std::vector<std::string> layerIds;
  for (const auto& layer : plan.layers) {
    layerIds.push_back(layer.layerId);
  }
  std::sort(layerIds.begin(), layerIds.end());
  const std::vector<std::string> expected = {"tile:zoom:1", "tile:zoom:2", "tile:zoom:3", "tiles-bg:tiles:s"};
  EXPECT_EQ(layerIds, expected);
}

TEST(TilesRenderPlan, TheWallDrawsABackgroundBeneathEveryTile) {
  MediaCore core; loadWall(core, {"zoom:1"});
  core.setTilesMemberFrameAgesForTest({{"zoom:1", true, 0}});

  const auto plan = core.lastRenderPlanForTest();
  const auto* background = findLayer(plan, "tiles-bg:tiles:s");
  ASSERT_NE(background, nullptr);
  EXPECT_EQ(background->kind, "tiles-background");
  // Review fix (I5): borderColor is NEVER read as a fill by either compositor
  // path — it feeds ONLY computeBorderFraming, and with borderStyle="none"/
  // thickness 0 nothing draws; a layer with no matching frame instead renders
  // the compositor's default mid-grey placeholder. Asserting against
  // borderColor was a tautology that could not fail even with the wrong
  // pixels landing on PROGRAM. hasFillColor/fillColor are the fields both the
  // D3D11 resolveLayers() branch and the CPU buildProgramFramePreview()
  // branch actually read for a deliberately sourceless solid layer.
  EXPECT_TRUE(background->hasFillColor);
  EXPECT_EQ(background->fillColor, "#101418");
  for (const auto& layer : plan.layers) {
    if (layer.kind == "participant-video") {
      EXPECT_LT(background->order, layer.order);
    }
  }
}

// Fill, never letterbox — a wall of mixed cameras stays even because tiles crop
// their sides rather than growing bars.
TEST(TilesRenderPlan, EveryTileFillsRatherThanFits) {
  MediaCore core; loadWall(core, {"zoom:1", "zoom:2"});
  core.setTilesMemberFrameAgesForTest({{"zoom:1", true, 0}, {"zoom:2", true, 0}});

  for (const auto& layer : core.lastRenderPlanForTest().layers) {
    if (layer.kind == "participant-video") {
      EXPECT_EQ(layer.fitMode, "fill");
    }
  }
}

// T1 ships no styling: a border here would composite chrome into PROGRAM, the
// virtual camera, and every recording. T2 adds it deliberately.
TEST(TilesRenderPlan, TilesCarryNoBorderBeforeStylingShips) {
  MediaCore core; loadWall(core, {"zoom:1"});
  core.setTilesMemberFrameAgesForTest({{"zoom:1", true, 0}});

  for (const auto& layer : core.lastRenderPlanForTest().layers) {
    if (layer.kind == "participant-video") {
      EXPECT_EQ(layer.borderStyle, "none");
      EXPECT_NEAR(layer.borderThickness, 0.f, 1e-6f);
    }
  }
}

TEST(TilesRenderPlan, AStaleMemberIsNotDrawnAndTheWallReflows) {
  MediaCore core; loadWall(core, {"zoom:1", "zoom:2"});
  core.setTilesMemberFrameAgesForTest({{"zoom:1", true, 0}, {"zoom:2", true, 0}});
  const auto twoUp = core.lastRenderPlanForTest();
  const float pairedWidth = findLayer(twoUp, "tile:zoom:1")->rect.width;

  core.setTilesMemberFrameAgesForTest(
      {{"zoom:1", true, 0}, {"zoom:2", true, corevideo::compositor::kTilesStaleFrameMs + 1}});
  const auto soloPlan = core.lastRenderPlanForTest();

  EXPECT_EQ(countLayersOfKind(soloPlan, "participant-video"), 1);
  EXPECT_GT(findLayer(soloPlan, "tile:zoom:1")->rect.width, pairedWidth);
}

TEST(TilesRenderPlan, NoTilesLayerLeavesTheOrdinaryRoutePlanUntouched) {
  MediaCore core;
  loadWall(core, {});
  const auto plan = core.lastRenderPlanForTest();
  EXPECT_EQ(countLayersOfKind(plan, "tiles-background"), 0);
}

// C1: a capture-class member must keep its FULL scheme-qualified id as
// participantId. The compositor matches layers to frames by EXACT
// participantId (frameForParticipant), and every capture/browser producer
// stamps VideoFrame::participantId with the full "capture:<id>"
// (WinUiCaptureDeviceAdapter.cpp, BrowserSourceHostAdapter.cpp) — mirroring
// how the ordinary route path already builds it
// (`layer.participantId = "capture:" + route.captureDeviceId;`). Stripping
// everything before the first colon (as if every member used the zoom:
// scheme) turned "capture:dev-1" into "dev-1", matching no frame and drawing
// a permanent solid placeholder — silently, because warnUnmatchedCaptureLayer
// only fires for keys starting "capture:"/"media:".
TEST(TilesRenderPlan, ACaptureMemberKeepsItsFullSchemeQualifiedParticipantId) {
  MediaCore core; loadWall(core, {"capture:dev-1"});
  core.setTilesMemberFrameAgesForTest({{"capture:dev-1", true, 0}});

  const auto* tile = findLayer(core.lastRenderPlanForTest(), "tile:capture:dev-1");
  ASSERT_NE(tile, nullptr);
  EXPECT_EQ(tile->participantId, "capture:dev-1");
  EXPECT_EQ(tile->sourceId, "capture:dev-1");
}

// C2 regression: `routes:[] + tiles` is EXACTLY the shape loadWall() sends
// (and what a later task is specced to produce). Before the fix, the
// pre-existing "no routes -> show whatever frames arrived" fallback branch in
// buildRenderPlanForScene fired REGARDLESS of a configured wall, interleaving
// one full-canvas layer per decoded frame with the wall's own
// tiles-bg/tile:* layers after sortCompositorRenderPlan — full-canvas cells
// compositing OVER individual wall tiles. This drives a REAL render tick (not
// the {}-frames test-seam rebuild other tests above use) so the default stub
// Zoom source's two synthetic placeholder frames actually flow through
// videoFrames, reproducing the exact shape that used to leak through.
TEST(TilesRenderPlan, AConfiguredWallSuppressesTheLegacyFullCanvasFallback) {
  MediaCore core;
  loadWall(core, {"zoom:1"});
  // One more ordinary tick (no commands), same real-frame-gather path as the
  // one already run inside loadWall()'s applyCommands.
  (void)core.applyCommands(corevideo::rpc::Json::Array{});

  const auto plan = core.lastRenderPlanForTest();
  for (const auto& layer : plan.layers) {
    EXPECT_NE(layer.layerId, "zoom:synthetic-speaker-1");
    EXPECT_NE(layer.layerId, "zoom:synthetic-speaker-2");
  }
}

// I4: a frozen-but-subscribed guest (frameId never advances, even though the
// producer keeps re-stamping timestampMs with the current tick's clock —
// exactly how every real frame producer in this codebase behaves) must
// eventually leave the wall, the same as any other stale feed. Drives real
// render ticks (no test-seam age injection) against a fake capture device
// with a controllable, frozen frameId.
TEST(TilesRenderPlan, AFrozenFrameIdAgesTheMemberOutEvenAsTimestampKeepsAdvancing) {
  auto modules = corevideo::modules::createStubModules();
  modules.captureDevice = std::make_unique<FrozenFrameIdCaptureDevice>();
  MediaCore core(std::move(modules));
  loadWall(core, {"capture:frozen-1"});

  ASSERT_NE(findLayer(core.lastRenderPlanForTest(), "tile:capture:frozen-1"), nullptr)
      << "the member should be admitted on its first real frame";

  // Drive enough ordinary ticks to cross kTilesStaleFrameMs at the default
  // output rate (60fps ~= 17ms/tick, so ~88 ticks; 400 gives ample margin
  // without the test becoming a real-time sleep loop).
  bool leftTheWall = false;
  for (int tick = 0; tick < 400; ++tick) {
    (void)core.applyCommands(corevideo::rpc::Json::Array{});
    if (findLayer(core.lastRenderPlanForTest(), "tile:capture:frozen-1") == nullptr) {
      leftTheWall = true;
      break;
    }
  }
  EXPECT_TRUE(leftTheWall);
}
