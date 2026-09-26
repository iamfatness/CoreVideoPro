#include "core/MediaCore.h"
#include "modules/Interfaces.h"
#include "rpc/Json.h"
#include "compositor/TilesPinnedLayout.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <chrono>
#include <map>
#include <thread>
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
void loadWall(MediaCore& core, const std::vector<std::string>& members, bool manual = false) {
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
                  {"fillMode", corevideo::rpc::Json{manual ? "manual" : "auto"}},
                  {"backgroundColor", corevideo::rpc::Json{"#101418"}}}}}}}}}}});
}

// Same shape as loadWall(), but the scene ALSO carries a media background — the
// SuperSource backdrop an operator sets on the scene.
void loadWallWithSceneBackground(MediaCore& core, const std::vector<std::string>& members) {
  corevideo::rpc::Json::Array memberJson;
  for (const auto& member : members) {
    memberJson.push_back(corevideo::rpc::Json{member});
  }
  (void)core.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json{corevideo::rpc::Json::Object{
          {"type", corevideo::rpc::Json{"load-scene-graph"}},
          {"sceneId", corevideo::rpc::Json{"s"}},
          {"routes", corevideo::rpc::Json{corevideo::rpc::Json::Array{}}},
          {"background", corevideo::rpc::Json{corevideo::rpc::Json::Object{
              {"mediaAssetId", corevideo::rpc::Json{"bg-1"}},
              {"mediaAssetName", corevideo::rpc::Json{"Backdrop"}},
              {"mediaAssetKind", corevideo::rpc::Json{"image"}},
              {"mediaAssetPath", corevideo::rpc::Json{"C:/backdrops/stage.png"}}}}},
          {"tiles", corevideo::rpc::Json{corevideo::rpc::Json::Object{
              {"layerId", corevideo::rpc::Json{"tiles:s"}},
              {"members", corevideo::rpc::Json{memberJson}},
              {"style", corevideo::rpc::Json{corevideo::rpc::Json::Object{
                  {"backgroundColor", corevideo::rpc::Json{"#101418"}}}}}}}}}}});
}

// Same shape as loadWall(), but the scene ALSO carries ordinary routes. Used
// to characterise the routes+wall interleave (see the test below); the shell
// no longer produces this shape (BuildProductionSyncContext serializes an
// empty route list for a gallery scene, by construction), but the core wire
// accepts it from anything, so the behaviour is pinned rather than latent.
void loadWallWithRoutes(MediaCore& core,
                        const std::vector<std::string>& members,
                        const std::vector<int>& routeZIndexes) {
  corevideo::rpc::Json::Array memberJson;
  for (const auto& member : members) {
    memberJson.push_back(corevideo::rpc::Json{member});
  }
  corevideo::rpc::Json::Array routeJson;
  for (size_t i = 0; i < routeZIndexes.size(); ++i) {
    routeJson.push_back(corevideo::rpc::Json{corevideo::rpc::Json::Object{
        {"routeId", corevideo::rpc::Json{"r" + std::to_string(i)}},
        {"mode", corevideo::rpc::Json{"fixed"}},
        {"participantId", corevideo::rpc::Json{"route-p" + std::to_string(i)}},
        {"zIndex", corevideo::rpc::Json{static_cast<double>(routeZIndexes[i])}},
        // hasRect is what makes the core honour zIndex as the layer order
        // (buildRenderPlanForScene: `if (route.hasRect) layer.order = route.zIndex`).
        {"rect", corevideo::rpc::Json{corevideo::rpc::Json::Object{
            {"x", corevideo::rpc::Json{0.0}},
            {"y", corevideo::rpc::Json{0.0}},
            {"width", corevideo::rpc::Json{0.5}},
            {"height", corevideo::rpc::Json{0.5}}}}}}});
  }
  (void)core.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json{corevideo::rpc::Json::Object{
          {"type", corevideo::rpc::Json{"load-scene-graph"}},
          {"sceneId", corevideo::rpc::Json{"s"}},
          {"routes", corevideo::rpc::Json{routeJson}},
          {"tiles", corevideo::rpc::Json{corevideo::rpc::Json::Object{
              {"layerId", corevideo::rpc::Json{"tiles:s"}},
              {"members", corevideo::rpc::Json{memberJson}},
              {"style", corevideo::rpc::Json{corevideo::rpc::Json::Object{
                  {"backgroundColor", corevideo::rpc::Json{"#101418"}}}}}}}}}}});
}

// Captures what the core actually handed the compositor on the last render
// tick — BOTH the render plan and how many video frames the gather produced.
//
// `MediaCore::lastRenderPlan_` (lastRenderPlanForTest) is deliberately cached
// ONLY while a PROGRAM wall is configured, so it cannot answer "what does a
// no-wall scene's plan look like" or "did the frame gather actually produce
// frames" at all. This stub answers both from the real production tick,
// without adding a test-only seam to MediaCore.
class RecordingCompositor final : public corevideo::modules::ICompositor {
 public:
  std::string rendererName() const override { return "recording-test"; }

  corevideo::modules::ProgramFrame render(
      const corevideo::modules::CompositorRenderPlan& renderPlan,
      const std::vector<corevideo::modules::VideoFrame>& frames) override {
    lastPlan = renderPlan;
    lastFrameCount = frames.size();
    ++renderCount;
    corevideo::modules::ProgramFrame frame;
    frame.width = renderPlan.width;
    frame.height = renderPlan.height;
    frame.layerCount = static_cast<int>(renderPlan.layers.size());
    frame.frameNumber = renderCount;
    frame.renderPlanId = renderPlan.renderPlanId;
    frame.renderer = "recording-test";
    frame.health = "live";
    return frame;
  }

  // The PREVIEW bus's own composite (the "third composite"), which the core
  // runs only when hasPreviewScene() says the preview scene has a layer.
  // Recording it here proves a wall-only preview scene actually COMPOSITES,
  // rather than trusting the `previewScene.composite` telemetry that reports
  // the same predicate.
  corevideo::modules::ProgramFrameSharedTexture renderPreview(
      const corevideo::modules::CompositorRenderPlan& renderPlan,
      const std::vector<corevideo::modules::VideoFrame>&) override {
    lastPreviewPlan = renderPlan;
    ++previewRenderCount;
    return {};
  }

  corevideo::modules::CompositorRenderPlan lastPlan;
  corevideo::modules::CompositorRenderPlan lastPreviewPlan;
  size_t lastFrameCount = 0;
  int64_t renderCount = 0;
  int64_t previewRenderCount = 0;
};

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

  void captureVideoTick(int64_t timestampMs) override {
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
    replaceVideo({std::move(frame)});
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

TEST(TilesRenderPlan, ManualSlotsRetainGeometryAcrossMissingAndReturningSources) {
  MediaCore core;
  loadWall(core, {"zoom:1", "", "zoom:3"}, true);
  ASSERT_EQ(core.tilesLayerForTest().members.size(), 3u);
  EXPECT_TRUE(core.tilesLayerForTest().members[1].empty());
  core.setTilesMemberFrameAgesForTest({{"zoom:1", true, 0}, {"zoom:3", true, 0}});
  const auto before = core.lastRenderPlanForTest();
  ASSERT_NE(findLayer(before, "tile:zoom:3"), nullptr);
  const auto original = findLayer(before, "tile:zoom:3")->rect;
  core.setTilesMemberFrameAgesForTest({{"zoom:1", false, 0}, {"zoom:3", true, 0}, {"zoom:unassigned", true, 0}});
  const auto missing = core.lastRenderPlanForTest();
  EXPECT_EQ(countLayersOfKind(missing, "participant-video"), 1);
  ASSERT_NE(findLayer(missing, "tile:zoom:3"), nullptr);
  EXPECT_EQ(findLayer(missing, "tile:zoom:3")->rect.x, original.x);
  EXPECT_EQ(findLayer(missing, "tile:zoom:3")->rect.width, original.width);
  EXPECT_EQ(findLayer(missing, "tile:zoom:3")->rect.y, original.y);
  core.setTilesMemberFrameAgesForTest({{"zoom:1", true, 0}, {"zoom:3", true, 0}});
  const auto returned = core.lastRenderPlanForTest();
  EXPECT_EQ(countLayersOfKind(returned, "participant-video"), 2);
  EXPECT_EQ(findLayer(returned, "tile:zoom:3")->rect.x, original.x);
}

TEST(TilesRenderPlan, PinnedHostAndMultiplePinsLeaveNonOverlappingGridSpace) {
  using corevideo::compositor::tilesLargestFreeRect;
  const auto besideHost = tilesLargestFreeRect({{0, 0, .4f, 1}});
  EXPECT_EQ(besideHost.x, .4f);
  EXPECT_EQ(besideHost.width, .6f);
  const auto betweenPins = tilesLargestFreeRect({{0, 0, .25f, 1}, {.75f, 0, .25f, 1}});
  EXPECT_EQ(betweenPins.x, .25f);
  EXPECT_EQ(betweenPins.width, .5f);
  EXPECT_EQ(betweenPins.height, 1.f);
  const auto full = tilesLargestFreeRect({{0, 0, 1, 1}});
  EXPECT_EQ(full.width * full.height, 0.f);
}

TEST(TilesRenderPlan, OverridesAndLiveBackgroundReachRenderedPlanWithoutReplacingMissingSources) {
  MediaCore core;
  const auto command = corevideo::rpc::Json::parse(R"({"type":"load-scene-graph","sceneId":"pinned","routes":[],
    "tiles":{"layerId":"tiles:pinned","members":["zoom:1","zoom:2"],
      "style":{"backgroundSourceId":"zoom:background","backgroundColor":"#123456","animateLayout":true,"animationDurationMs":800,
        "borderShape":"rounded","borderThickness":8,"borderColor":"#FF0000","cornerRadius":24,
        "glowSize":16,"glowColor":"#00FF00","glowIntensity":50,"glowSoftness":75},
      "overrides":{"zoom:1":{"rect":{"x":0,"y":0,"w":0.4,"h":1},"cropLeftPercent":10,"cropRightPercent":20,"z":3}}}})");
  ASSERT_TRUE(command.has_value());
  core.applyCommands(corevideo::rpc::Json::Array{*command});
  core.setTilesMemberFrameAgesForTest({{"zoom:1", true, 0}, {"zoom:2", true, 0}, {"zoom:background", true, 0}});
  const auto plan = core.lastRenderPlanForTest();
  const auto* host = findLayer(plan, "tile:zoom:1");
  const auto* guest = findLayer(plan, "tile:zoom:2");
  const auto* background = findLayer(plan, "tiles-source-bg:tiles:pinned");
  ASSERT_NE(host, nullptr); ASSERT_NE(guest, nullptr); ASSERT_NE(background, nullptr);
  EXPECT_EQ(host->rect.width, .4f);
  EXPECT_TRUE(guest->rect.x >= host->rect.x + host->rect.width);
  EXPECT_EQ(host->sourceCropLeftPercent, 10.f);
  EXPECT_EQ(host->sourceCropRightPercent, 20.f);
  ASSERT_TRUE(host->tilesDecoration.enabled);
  EXPECT_EQ(host->tilesDecoration.borderWidth, 8.f);
  EXPECT_EQ(host->tilesDecoration.radius, 24.f);
  EXPECT_EQ(host->tilesDecoration.borderColor, "#FF0000");
  EXPECT_EQ(host->tilesDecoration.glowSize, 16.f);
  EXPECT_EQ(host->tilesDecoration.glowColor, "#00FF00");
  EXPECT_EQ(host->tilesDecoration.glowIntensity, .5f);
  EXPECT_EQ(host->tilesDecoration.glowSoftness, .75f);
  const auto* halo = findLayer(plan, "tiles-glow:zoom:1");
  ASSERT_NE(halo, nullptr);
  EXPECT_TRUE(halo->tilesDecoration.glowPass);
  EXPECT_TRUE(halo->order < host->order && halo->order < guest->order);
  EXPECT_EQ(halo->rect.width, host->rect.width);
  EXPECT_EQ(halo->tilesDecoration.radius, host->tilesDecoration.radius);
  EXPECT_TRUE(background->order < host->order);
  EXPECT_EQ(background->participantId, "background");
  EXPECT_TRUE(core.tilesLayerForTest().style.animateLayout);
  EXPECT_EQ(core.tilesLayerForTest().style.animationDurationMs, 800);
  core.setTilesMemberFrameAgesForTest({{"zoom:1", true, 0}, {"zoom:2", true, 0}});
  EXPECT_EQ(findLayer(core.lastRenderPlanForTest(), "tiles-source-bg:tiles:pinned"), nullptr);
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
TEST(TilesRenderPlan, TilesDoNotDoubleApplyGenericSceneBorders) {
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

// Whole-branch review fix: this test used to call `loadWall(core, {})` and read
// lastRenderPlanForTest(). An empty-members tiles node fails MediaCore's
// `tilesLayer_.present && !tilesLayer_.members.empty()` assignment gate, so
// lastRenderPlan_ was never written and the test inspected a DEFAULT-CONSTRUCTED
// plan — zero tiles-background layers is true of an empty struct, so it could
// not fail for any reason. It now drives a real no-wall scene WITH routes
// through the production tick and reads the plan the compositor was actually
// handed, so both halves of "leaves the ordinary route plan untouched" are
// genuinely asserted.
// #478 R2 (review finding 2): the core used to bind a follow-speaker route to
// videoFrames[routeIndex] — ordered by subscription uuid, not by who is talking — so
// a full-screen speaker scene showed whichever source sorted first. It now binds the
// DIRECTED speaker. The stub Zoom session directs "operator-1"; the frame gather
// lists "guest-1" FIRST, so the old positional binding shows the wrong person.
namespace {
class TwoGuestZoomSource final : public corevideo::core::ISource {
 public:
  TwoGuestZoomSource() { init(); }
  explicit TwoGuestZoomSource(std::vector<std::string> ids) : ids_(std::move(ids)) { init(); }
  const corevideo::core::SourceDescriptor& descriptor() const override { return descriptor_; }
  corevideo::core::SourceTick poll(int64_t) override {
    corevideo::core::SourceTick tick;
    tick.health = corevideo::core::SourceHealth::Producing;
    for (const auto& participantId : ids_) {
      corevideo::modules::VideoFrame frame;
      frame.participantId = participantId;
      frame.width = frame.height = frame.i420Width = frame.i420Height = 2;
      frame.frameId = ++frameId_;
      frame.i420 = std::make_shared<const std::vector<std::uint8_t>>(6, 128);
      tick.video.push_back(std::move(frame));
    }
    return tick;
  }
  corevideo::core::SourceIngestCounters counters() const override { return {}; }

 private:
  void init() {
    descriptor_.sourceId = "two-guest-zoom";
    descriptor_.kind = "zoom-slate";
    descriptor_.hasVideo = true;
  }
  corevideo::core::SourceDescriptor descriptor_;
  std::vector<std::string> ids_{"guest-1", "operator-1"};
  std::int64_t frameId_ = 0;
};

corevideo::rpc::Json::Array followSpeakerScene() {
  return corevideo::rpc::Json::Array{
      corevideo::rpc::Json{corevideo::rpc::Json::Object{
          {"type", corevideo::rpc::Json{"load-scene-graph"}},
          {"sceneId", corevideo::rpc::Json{"speaker"}},
          {"routes", corevideo::rpc::Json{corevideo::rpc::Json::Array{
              corevideo::rpc::Json{corevideo::rpc::Json::Object{
                  {"routeId", corevideo::rpc::Json{"follow"}},
                  {"mode", corevideo::rpc::Json{"active-speaker"}}}}}}}}}};
}

void expectEmptyFollowLayer(const corevideo::modules::CompositorRenderPlan& plan) {
  const auto* layer = findLayer(plan, "route:follow");
  ASSERT_NE(layer, nullptr) << "the layer must still exist (an empty plan is NOT 'draw nothing')";
  EXPECT_TRUE(layer->participantId.empty()) << "bound '" << layer->participantId << "'";
  EXPECT_TRUE(layer->sourceId.empty());
  EXPECT_TRUE(layer->hasFillColor);
  EXPECT_EQ(layer->fillColor, "#00000000");
  EXPECT_EQ(layer->opacity, 0.f);
}
}  // namespace

// #478 N2: the directed speaker has NO frame this tick (they left, or were dropped
// from the sources mid-talk and their video retired). Binding them painted a
// slab on Program (the bus-health slate today, #535 slice 4a; the pink
// colorFromParticipantId tile before that); the positional fallback would show
// a random source. The layer renders EMPTY instead.
TEST(TilesRenderPlan, AFollowSpeakerRouteWhoseSpeakerHasNoFrameRendersEmptyNotASlab) {
  auto modules = corevideo::modules::createStubModules();
  auto ownedCompositor = std::make_unique<RecordingCompositor>();
  auto* compositor = ownedCompositor.get();
  modules.compositor = std::move(ownedCompositor);
  MediaCore core(std::move(modules));
  core.useZoomSourcesForTest({std::make_shared<TwoGuestZoomSource>(std::vector<std::string>{"guest-1"})});
  (void)core.joinZoom(corevideo::rpc::Json::Object{});  // directs "operator-1", who has no frame

  (void)core.applyCommands(followSpeakerScene());
  core.renderDisplayTick();
  expectEmptyFollowLayer(compositor->lastPlan);
}

// #478 N2: leaving the meeting forgets the held speaker, even when a frame under the
// same id is still in the gather (Zoom reuses per-meeting user ids).
TEST(TilesRenderPlan, LeavingTheMeetingForgetsTheFollowRoutesHeldSpeaker) {
  auto modules = corevideo::modules::createStubModules();
  auto ownedCompositor = std::make_unique<RecordingCompositor>();
  auto* compositor = ownedCompositor.get();
  modules.compositor = std::move(ownedCompositor);
  MediaCore core(std::move(modules));
  core.useZoomSourcesForTest({std::make_shared<TwoGuestZoomSource>()});
  (void)core.joinZoom(corevideo::rpc::Json::Object{});
  (void)core.applyCommands(followSpeakerScene());
  core.renderDisplayTick();
  const auto* bound = findLayer(compositor->lastPlan, "route:follow");
  ASSERT_NE(bound, nullptr);
  ASSERT_EQ(bound->participantId, "operator-1");

  (void)core.leaveZoom();
  core.renderDisplayTick();
  expectEmptyFollowLayer(compositor->lastPlan);
}

TEST(TilesRenderPlan, AFollowSpeakerRouteShowsTheDirectedSpeakerNotTheFirstFrame) {
  auto modules = corevideo::modules::createStubModules();
  auto ownedCompositor = std::make_unique<RecordingCompositor>();
  auto* compositor = ownedCompositor.get();
  modules.compositor = std::move(ownedCompositor);
  MediaCore core(std::move(modules));
  core.useZoomSourcesForTest({std::make_shared<TwoGuestZoomSource>()});
  (void)core.joinZoom(corevideo::rpc::Json::Object{});
  ASSERT_EQ(core.zoomSnapshot().getString("activeSpeakerId"), "operator-1");

  (void)core.applyCommands(followSpeakerScene());
  core.renderDisplayTick();

  const auto* layer = findLayer(compositor->lastPlan, "route:follow");
  ASSERT_NE(layer, nullptr);
  EXPECT_EQ(layer->participantId, "operator-1");
  EXPECT_EQ(layer->sourceId, "zoom:operator-1");
}

TEST(TilesRenderPlan, NoTilesLayerLeavesTheOrdinaryRoutePlanUntouched) {
  auto modules = corevideo::modules::createStubModules();
  auto ownedCompositor = std::make_unique<RecordingCompositor>();
  auto* compositor = ownedCompositor.get();
  modules.compositor = std::move(ownedCompositor);
  MediaCore core(std::move(modules));

  // A scene with routes and NO tiles node at all.
  (void)core.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json{corevideo::rpc::Json::Object{
          {"type", corevideo::rpc::Json{"load-scene-graph"}},
          {"sceneId", corevideo::rpc::Json{"no-wall"}},
          {"routes", corevideo::rpc::Json{corevideo::rpc::Json::Array{
              corevideo::rpc::Json{corevideo::rpc::Json::Object{
                  {"routeId", corevideo::rpc::Json{"r0"}},
                  {"mode", corevideo::rpc::Json{"fixed"}},
                  {"participantId", corevideo::rpc::Json{"p-1"}}}},
              corevideo::rpc::Json{corevideo::rpc::Json::Object{
                  {"routeId", corevideo::rpc::Json{"r1"}},
                  {"mode", corevideo::rpc::Json{"fixed"}},
                  {"participantId", corevideo::rpc::Json{"p-2"}}}}}}}}}});

  const auto& plan = compositor->lastPlan;
  ASSERT_GE(compositor->renderCount, 1) << "no render tick ran — nothing was under test";
  EXPECT_EQ(countLayersOfKind(plan, "tiles-background"), 0);
  // The ordinary route plan is present and UNTOUCHED — that is the half a
  // default-constructed plan could never have shown.
  ASSERT_NE(findLayer(plan, "route:r0"), nullptr);
  ASSERT_NE(findLayer(plan, "route:r1"), nullptr);
  for (const auto& layer : plan.layers) {
    EXPECT_NE(layer.layerId.rfind("tile:", 0), 0u)
        << "no-wall scene emitted a tile layer: " << layer.layerId;
    EXPECT_NE(layer.layerId.rfind("tiles-bg:", 0), 0u)
        << "no-wall scene emitted a wall background: " << layer.layerId;
  }
}

// CRITICAL (on-air). A wall whose members are ALL stale must still emit its
// background, so the plan is never empty.
//
// This is not a cosmetic preference. All three compositors
// (D3D11CompositorAdapter::resolveLayers, ProgramFramePreview's
// buildProgramFramePreview, MetalCompositorAdapter::resolveLayers) treat an
// EMPTY `renderPlan.layers` as "improvise a grid of every decoded frame", and
// buildRenderPlanForScene deliberately suppresses the legacy full-canvas
// fallback while a wall is active — so an empty plan under a Tiles scene put a
// grid of arbitrary decoded sources (including the very members the staleness
// veto had just rejected) onto PROGRAM, and thus into the virtual camera,
// recordings and streams. Transient on EVERY Tiles take, permanent once every
// member freezes. Asserting non-emptiness is therefore the whole point of the
// test, not a formality.
TEST(TilesRenderPlan, AnAllStaleWallStillEmitsItsBackground) {
  MediaCore core;
  loadWall(core, {"zoom:1", "zoom:2"});
  core.setTilesMemberFrameAgesForTest(
      {{"zoom:1", true, corevideo::compositor::kTilesStaleFrameMs + 1},
       {"zoom:2", true, corevideo::compositor::kTilesStaleFrameMs + 1}});

  const auto plan = core.lastRenderPlanForTest();
  ASSERT_FALSE(plan.layers.empty())
      << "an empty plan makes all three compositors improvise a grid of every decoded frame onto PROGRAM";
  std::vector<std::string> layerIds;
  for (const auto& layer : plan.layers) {
    layerIds.push_back(layer.layerId);
  }
  EXPECT_EQ(layerIds, (std::vector<std::string>{"tiles-bg:tiles:s"}));
  EXPECT_EQ(countLayersOfKind(plan, "participant-video"), 0);

  const auto* background = findLayer(plan, "tiles-bg:tiles:s");
  ASSERT_NE(background, nullptr);
  EXPECT_TRUE(background->hasFillColor);
  EXPECT_EQ(background->fillColor, "#101418");
}

// Re-review finding A (Important, ON-AIR). A wall whose MEMBERS LIST IS EMPTY
// still owns the scene: it emits its background, and it still suppresses the
// legacy full-canvas fallback.
//
// `TilesLayerPayloadBuilder.Build` sends `members: []` whenever every guest is
// video-off or the roster is momentarily empty — an ordinary meeting state. The
// old `wallActive = present && !members.empty()` gate turned that into "no wall
// at all": no background AND no fallback suppression, so PROGRAM showed an
// improvised full-canvas grid of every decoded source (here, the stub Zoom
// source's two synthetic speakers), inherited by the virtual camera, every
// recording and every stream. Drives the REAL tick so those frames actually
// exist, and asserts they did — otherwise the fallback has no input and the
// test passes vacuously.
TEST(TilesRenderPlan, AMemberLessWallStillOwnsTheSceneAndEmitsItsBackground) {
  auto modules = corevideo::modules::createStubModules();
  auto ownedCompositor = std::make_unique<RecordingCompositor>();
  auto* compositor = ownedCompositor.get();
  modules.compositor = std::move(ownedCompositor);
  MediaCore core(std::move(modules));

  // A wall configured with NOBODY on it — every camera off.
  loadWall(core, {});
  (void)core.applyCommands(corevideo::rpc::Json::Array{});

  ASSERT_GE(compositor->lastFrameCount, 2u)
      << "the frame gather produced no frames — the fallback this test guards against "
         "had no input, so the test would pass vacuously";

  const auto plan = core.lastRenderPlanForTest();
  ASSERT_FALSE(plan.layers.empty())
      << "a members-less wall emitted an EMPTY plan — all three compositors then improvise "
         "a grid of every decoded frame onto PROGRAM";
  std::vector<std::string> layerIds;
  for (const auto& layer : plan.layers) {
    layerIds.push_back(layer.layerId);
  }
  EXPECT_EQ(layerIds, (std::vector<std::string>{"tiles-bg:tiles:s"}));
  EXPECT_EQ(countLayersOfKind(plan, "participant-video"), 0);
  // The legacy full-canvas fallback stays suppressed: no layer for either
  // synthetic speaker the gather just produced.
  for (const auto& layer : plan.layers) {
    EXPECT_NE(layer.layerId, "zoom:synthetic-speaker-1");
    EXPECT_NE(layer.layerId, "zoom:synthetic-speaker-2");
  }
}

// Re-review finding B (Important). A PREVIEW scene carrying ONLY a wall — no
// media background, no overlay, and (by construction, since the shell now
// serialises an empty route list for a gallery scene) no routes — must run the
// dedicated preview composite.
//
// hasPreviewScene() used to tally routes + background + overlays only, so a
// Tiles preview scored ZERO layers: the third composite never ran, the preview
// shared-texture handle was cleared, and the shell fell back to the
// single-source preview path — the operator never saw the wall they were about
// to take. Asserts the composite REALLY RAN (the stub records renderPreview)
// and that what it composited is the wall, not an empty plan.
TEST(TilesRenderPlan, APreviewSceneCarryingOnlyAWallStillComposites) {
  auto modules = corevideo::modules::createStubModules();
  auto ownedCompositor = std::make_unique<RecordingCompositor>();
  auto* compositor = ownedCompositor.get();
  modules.compositor = std::move(ownedCompositor);
  MediaCore core(std::move(modules));

  (void)core.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json{corevideo::rpc::Json::Object{
          {"type", corevideo::rpc::Json{"set-preview-scene"}},
          {"sceneId", corevideo::rpc::Json{"pvw-gallery"}},
          {"routes", corevideo::rpc::Json{corevideo::rpc::Json::Array{}}},
          {"tiles", corevideo::rpc::Json{corevideo::rpc::Json::Object{
              {"layerId", corevideo::rpc::Json{"tiles:pvw"}},
              {"members", corevideo::rpc::Json{corevideo::rpc::Json::Array{
                  corevideo::rpc::Json{"zoom:1"}}}},
              {"style", corevideo::rpc::Json{corevideo::rpc::Json::Object{
                  {"backgroundColor", corevideo::rpc::Json{"#101418"}}}}}}}}}}});
  (void)core.applyCommands(corevideo::rpc::Json::Array{});

  ASSERT_GE(compositor->previewRenderCount, 1)
      << "the preview bus never composited a scene whose only layer is the wall";
  const auto& previewPlan = compositor->lastPreviewPlan;
  ASSERT_FALSE(previewPlan.layers.empty());
  ASSERT_NE(findLayer(previewPlan, "tiles-bg:tiles:pvw"), nullptr)
      << "the preview composite ran but did not carry the wall's background";
}

// Re-review (reviewer's cheap suggestion): routes + a wall in ONE scene is
// accepted silently by the wire. Make it AUDIBLE — the characterization test
// below pins WHAT happens; this pins that we SAY something.
TEST(TilesRenderPlan, ASceneCarryingBothRoutesAndAWallWarnsLoudly) {
  MediaCore core;
  loadWallWithRoutes(core, {"zoom:1"}, {2});

  const auto warnings = core.sceneValidationWarningsForTest();
  const bool warned = std::any_of(warnings.begin(), warnings.end(), [](const std::string& warning) {
    return warning.find("tiles wall") != std::string::npos &&
           warning.find("route") != std::string::npos;
  });
  EXPECT_TRUE(warned) << "routes + wall in one scene passed without a validation warning";

  // A wall-only scene (the shape the shell actually sends) must stay quiet —
  // a warning that fires on the healthy path is noise and gets ignored.
  MediaCore quiet;
  loadWall(quiet, {"zoom:1"});
  for (const auto& warning : quiet.sceneValidationWarningsForTest()) {
    EXPECT_EQ(warning.find("tiles wall"), std::string::npos)
        << "wall-only scene warned about a route collision: " << warning;
  }
}

// Characterization (whole-branch review): routes and a wall arriving in ONE
// scene share a single order namespace, so they INTERLEAVE. tiles-bg takes
// wall.order and tile #i takes wall.order + 1 + i, while a route with an
// explicit rect takes its own zIndex — so a route at zIndex 2 lands exactly on
// top of the second tile's order and sorts between the wall's own layers.
//
// The shell can no longer produce this shape (BuildProductionSyncContext
// serializes an EMPTY route list for a gallery scene, by construction — see
// its comment there), but the core's wire accepts routes+tiles from any
// producer. Pinning it here means the interleave is a KNOWN, tested property
// rather than a latent surprise for whoever gives the wall its own order
// namespace or its own sub-plan later.
TEST(TilesRenderPlan, RoutesAndAWallShareOneOrderNamespace) {
  MediaCore core;
  loadWallWithRoutes(core, {"zoom:1", "zoom:2"}, {2});
  core.setTilesMemberFrameAgesForTest({{"zoom:1", true, 0}, {"zoom:2", true, 0}});

  const auto plan = core.lastRenderPlanForTest();
  const auto* background = findLayer(plan, "tiles-bg:tiles:s");
  const auto* tile1 = findLayer(plan, "tile:zoom:1");
  const auto* tile2 = findLayer(plan, "tile:zoom:2");
  const auto* route = findLayer(plan, "route:r0");
  ASSERT_NE(background, nullptr);
  ASSERT_NE(tile1, nullptr);
  ASSERT_NE(tile2, nullptr);
  ASSERT_NE(route, nullptr) << "the route survived onto the same plan as the wall";

  // wall.order defaults to 0 (no "order" key sent).
  EXPECT_EQ(background->order, 0);
  EXPECT_EQ(tile1->order, 1);
  EXPECT_EQ(tile2->order, 2);
  // THE INTERLEAVE: the route's zIndex is resolved in the SAME namespace, so
  // it collides with tile #2 rather than sitting above or below the wall.
  EXPECT_EQ(route->order, 2);
  EXPECT_GT(route->order, tile1->order);
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
  auto modules = corevideo::modules::createStubModules();
  auto ownedCompositor = std::make_unique<RecordingCompositor>();
  auto* compositor = ownedCompositor.get();
  modules.compositor = std::move(ownedCompositor);
  MediaCore core(std::move(modules));
  loadWall(core, {"zoom:1"});
  // One more ordinary tick (no commands), same real-frame-gather path as the
  // one already run inside loadWall()'s applyCommands.
  (void)core.applyCommands(corevideo::rpc::Json::Array{});

  // PRECONDITION (whole-branch review fix). Post-CRITICAL-fix, `admitted` is
  // empty on these ticks (the wall's member has no real frame, so the
  // freshness gate rejects it) and the plan holds only `tiles-bg:*` — so both
  // EXPECT_NEs below iterate a list that cannot contain a fallback layer, and
  // the test would assert absence over a near-empty list. It could then stay
  // GREEN if the plan emptied for a completely unrelated reason, which is
  // exactly the failure mode this test exists to catch.
  //
  // Assert the frame gather ACTUALLY PRODUCED FRAMES: the stub Zoom source's
  // two synthetic placeholder participants are what the legacy
  // "no routes -> show whatever frames arrived" branch would expand into
  // full-canvas layers if the wall did not suppress it. With zero frames
  // there is nothing to leak and nothing under test.
  ASSERT_GE(compositor->lastFrameCount, 2u)
      << "the frame gather produced no frames — the suppressed fallback had no input, "
         "so this test would pass vacuously";

  const auto plan = core.lastRenderPlanForTest();
  ASSERT_FALSE(plan.layers.empty()) << "an active wall must always emit at least its background";
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

// A SuperSource backdrop on the scene must survive a Tiles wall. The wall's
// background is an OPAQUE solid at order 0 across the whole canvas, while the
// scene background sits at order -100 — so emitting both painted the operator's
// backdrop out entirely (owner report, live meeting 2026-08-16). This is a T1
// regression: before the wall existed, a gallery scene emitted no such layer.
// The CRITICAL never-empty-plan rule still has to hold, and here it is the
// media background that satisfies it.
TEST(TilesRenderPlan, ASceneBackgroundSurvivesTheWallAndSuppressesItsSolidFill) {
  MediaCore core;
  loadWallWithSceneBackground(core, {"zoom:1"});
  core.setTilesMemberFrameAgesForTest({{"zoom:1", true, 0}});

  const auto plan = core.lastRenderPlanForTest();
  EXPECT_FALSE(plan.layers.empty());
  EXPECT_EQ(findLayer(plan, "tiles-bg:tiles:s"), nullptr)
      << "the wall must not paint its solid colour over a scene background";
  ASSERT_NE(findLayer(plan, "background:bg-1"), nullptr)
      << "the scene background is what keeps the plan non-empty here";
  EXPECT_EQ(findLayer(plan, "background:bg-1")->kind, "media-background");
}

// The complement: with NO scene background the wall still paints its own, which
// is what the never-empty-plan rule depends on in that case.
TEST(TilesRenderPlan, WithNoSceneBackgroundTheWallStillPaintsItsOwn) {
  MediaCore core;
  loadWall(core, {"zoom:1"});
  core.setTilesMemberFrameAgesForTest({{"zoom:1", true, 0}});

  const auto plan = core.lastRenderPlanForTest();
  EXPECT_FALSE(plan.layers.empty());
  EXPECT_NE(findLayer(plan, "tiles-bg:tiles:s"), nullptr);
}

// ---------------------------------------------------------------------------
// Preview -> Program wall hand-off (live-show defect, owner report 2026-09-09)
//
// "I am ok if panelists leave and join the video but what I can't have is a
// total rerender from what is in preview to program like it is loading for the
// first time. It needs to be a constant rendered source that is able to be cut
// to without the need to redraw it in PGM."
//
// A wall settled in PREVIEW that is TAKEN must be at its settled state on the
// first program frame that draws it. The converse still holds: a wall the
// other bus never held is not handed anything.
// ---------------------------------------------------------------------------
namespace {

// Delivers real pixels for N wall members with an ADVANCING frameId, so every
// member stays admitted (kTilesStaleFrameMs) for as long as the test ticks.
// pause()/resume() reproduces the transient every Tiles take can hit: the wall
// is still configured, but for a beat not one member has a fresh frame.
class LiveWallCaptureDevice final : public corevideo::modules::ICaptureDevice {
 public:
  explicit LiveWallCaptureDevice(std::vector<std::string> ids) : ids_(std::move(ids)) {}

  void pause() { delivering_ = false; }
  void resume() { delivering_ = true; frozen_ = false; }
  // The frozen-but-still-delivering shape a real feed actually presents: every
  // producer in this codebase re-serves its held frame with a FRESH timestamp
  // and an unchanged frameId (see FrozenFrameIdCaptureDevice and the I4 note in
  // MediaCore's age gather), so the source stays in videoFrames and only its
  // AGE grows. pause() is the different, harsher case — no frame at all.
  void freeze() { frozen_ = true; }

  std::vector<corevideo::modules::CaptureDeviceInfo> enumerate() const override { return {}; }
  std::vector<corevideo::modules::CaptureDeviceInfo> selectInput(const std::string&,
                                                                  const std::string&) override {
    return {};
  }
  std::vector<corevideo::modules::CaptureDeviceInfo> setAudioSyncOffset(const std::string&, int) override {
    return {};
  }
  std::vector<corevideo::modules::CaptureDeviceInfo> connect(const std::string&) override { return {}; }

  void captureVideoTick(int64_t timestampMs) override {
    if (!delivering_) { replaceVideo({}); return; }
    if (!frozen_) ++frameId_;
    std::vector<corevideo::modules::VideoFrame> frames;
    frames.reserve(ids_.size());
    for (const auto& id : ids_) {
      corevideo::modules::VideoFrame frame;
      frame.participantId = id;
      frame.width = frame.naturalWidth = frame.pixelWidth = 64;
      frame.height = frame.naturalHeight = frame.pixelHeight = 64;
      frame.pixelStride = 64 * 4;
      frame.timestampMs = timestampMs;
      frame.frameId = frameId_;
      frame.pixels = pixels_;
      frames.push_back(std::move(frame));
    }
    replaceVideo(std::move(frames));
  }

 private:
  std::vector<std::string> ids_;
  bool delivering_ = true;
  bool frozen_ = false;
  int64_t frameId_ = 0;
  std::shared_ptr<const std::vector<uint8_t>> pixels_ =
      std::make_shared<std::vector<uint8_t>>(64 * 64 * 4, 128);
};

corevideo::rpc::Json animatedTilesPayload(const std::string& layerId,
                                          const std::vector<std::string>& members) {
  corevideo::rpc::Json::Array memberJson;
  for (const auto& member : members) memberJson.push_back(corevideo::rpc::Json{member});
  return corevideo::rpc::Json{corevideo::rpc::Json::Object{
      {"layerId", corevideo::rpc::Json{layerId}},
      {"members", corevideo::rpc::Json{memberJson}},
      {"style", corevideo::rpc::Json{corevideo::rpc::Json::Object{
          {"fillMode", corevideo::rpc::Json{"auto"}},
          {"animateLayout", corevideo::rpc::Json{true}},
          // The clamped floor (TilesAnimator) — the shortest entrance the wire
          // can ask for, so the settle loops below stay short real-time waits.
          {"animationDurationMs", corevideo::rpc::Json{100.0}},
          {"backgroundColor", corevideo::rpc::Json{"#101418"}}}}}}};
}

corevideo::rpc::Json wallScene(const char* type, const char* sceneId,
                               const std::vector<std::string>& members) {
  return corevideo::rpc::Json{corevideo::rpc::Json::Object{
      {"type", corevideo::rpc::Json{type}},
      {"sceneId", corevideo::rpc::Json{sceneId}},
      {"routes", corevideo::rpc::Json{corevideo::rpc::Json::Array{}}},
      {"tiles", animatedTilesPayload(std::string("tiles:") + sceneId, members)}}};
}

// Same shape as wallScene(), but with a caller-chosen animationDurationMs.
// AWallTakenMidAnimationIsContinuous needs the SLOWEST duration the animator
// accepts (clamped at 2000ms in TilesAnimator) so its rect-continuity check
// (Finding E, review round 1) is not at the mercy of how many real
// milliseconds of test-process overhead land between two back-to-back
// applyCommands() calls: at the shared 100ms floor, this rig's own command
// handling was enough real wall-clock time for a critically-damped 6.6/duration-Hz
// spring to converge audibly close to its target on its own, with or without a
// bug — the same closed-form "rest" snap the reset path uses. At 2000ms the
// per-millisecond convergence is 20x slower, giving a reliable margin.
corevideo::rpc::Json wallSceneWithDuration(const char* type, const char* sceneId,
                                           const std::vector<std::string>& members, double durationMs) {
  auto tiles = animatedTilesPayload(std::string("tiles:") + sceneId, members);
  auto object = tiles.asObject();
  auto style = object.at("style").asObject();
  style.insert_or_assign("animationDurationMs", corevideo::rpc::Json{durationMs});
  object.insert_or_assign("style", corevideo::rpc::Json{style});
  return corevideo::rpc::Json{corevideo::rpc::Json::Object{
      {"type", corevideo::rpc::Json{type}},
      {"sceneId", corevideo::rpc::Json{sceneId}},
      {"routes", corevideo::rpc::Json{corevideo::rpc::Json::Array{}}},
      {"tiles", corevideo::rpc::Json{object}}}};
}

// The same wall, but carrying a LIVE background feed (tiles-source-bg:<layerId>).
corevideo::rpc::Json wallSceneWithBackground(const char* type, const char* sceneId,
                                             const std::vector<std::string>& members,
                                             const char* backgroundSourceId) {
  auto tiles = animatedTilesPayload(std::string("tiles:") + sceneId, members);
  auto object = tiles.asObject();
  auto style = object.at("style").asObject();
  style.emplace("backgroundSourceId", corevideo::rpc::Json{backgroundSourceId});
  object.insert_or_assign("style", corevideo::rpc::Json{style});
  return corevideo::rpc::Json{corevideo::rpc::Json::Object{
      {"type", corevideo::rpc::Json{type}},
      {"sceneId", corevideo::rpc::Json{sceneId}},
      {"routes", corevideo::rpc::Json{corevideo::rpc::Json::Array{}}},
      {"tiles", corevideo::rpc::Json{object}}}};
}

corevideo::rpc::Json wallLessScene(const char* type, const char* sceneId) {
  return corevideo::rpc::Json{corevideo::rpc::Json::Object{
      {"type", corevideo::rpc::Json{type}},
      {"sceneId", corevideo::rpc::Json{sceneId}},
      {"routes", corevideo::rpc::Json{corevideo::rpc::Json::Array{}}}}};
}

std::vector<const corevideo::modules::CompositorRenderPlanLayer*> tileLayers(
    const corevideo::modules::CompositorRenderPlan& plan) {
  std::vector<const corevideo::modules::CompositorRenderPlanLayer*> tiles;
  for (const auto& layer : plan.layers) {
    if (layer.kind == "participant-video" && layer.layerId.rfind("tile:", 0) == 0) {
      tiles.push_back(&layer);
    }
  }
  return tiles;
}

bool allSettled(const std::vector<const corevideo::modules::CompositorRenderPlanLayer*>& tiles) {
  return !tiles.empty() &&
         std::all_of(tiles.begin(), tiles.end(), [](const auto* tile) { return tile->opacity == 1.f; });
}

std::map<std::string, corevideo::modules::CompositorRenderPlanLayer> snapshotTiles(
    const corevideo::modules::CompositorRenderPlan& plan) {
  std::map<std::string, corevideo::modules::CompositorRenderPlanLayer> tiles;
  for (const auto* tile : tileLayers(plan)) tiles[tile->layerId] = *tile;
  return tiles;
}

// Ticks the real production path in REAL TIME until the preview wall's tiles
// have finished their entrance — the animator runs off steady_clock, so this
// has to be wall time, not tick count.
void settlePreviewWall(MediaCore& core, const RecordingCompositor& compositor, size_t expected) {
  for (int tick = 0; tick < 60; ++tick) {
    (void)core.applyCommands(corevideo::rpc::Json::Array{});
    const auto tiles = tileLayers(compositor.lastPreviewPlan);
    if (tiles.size() == expected && allSettled(tiles)) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

const std::vector<std::string>& wallMembers() {
  static const std::vector<std::string> members{"capture:g1", "capture:g2", "capture:g3"};
  return members;
}

corevideo::modules::ModuleSet wallModules(RecordingCompositor** compositor,
                                          LiveWallCaptureDevice** device) {
  auto modules = corevideo::modules::createStubModules();
  auto ownedCompositor = std::make_unique<RecordingCompositor>();
  *compositor = ownedCompositor.get();
  modules.compositor = std::move(ownedCompositor);
  auto ownedDevice = std::make_unique<LiveWallCaptureDevice>(wallMembers());
  if (device != nullptr) *device = ownedDevice.get();
  modules.captureDevice = std::move(ownedDevice);
  return modules;
}

// The take TransportCoordinator.TakeAsync actually sends: ONE sync carrying the
// new PROGRAM scene and the new PREVIEW scene together.
void take(MediaCore& core, const corevideo::rpc::Json& program, const corevideo::rpc::Json& preview) {
  (void)core.applyCommands(corevideo::rpc::Json::Array{program, preview});
}

}  // namespace

// #448. `TilesPlanAnimation::adoptSettledFrom` refused a wall whose tiles were
// still flying, because with two per-bus animators mid-flight state had no
// correct owner ("mid-flight state belongs to the bus that is flying it").
// So a wall taken MID-ANIMATION lost its motion on Program — the live-show
// defect this file is named for, caught here at its actual root: with one
// animator PER WALL (core::TilesWallSource, shared by both buses) there is
// nothing to hand over, so the cut is continuous even mid-flight.
//
// WHAT THE OLD MECHANISM ACTUALLY DID, because the direction matters to every
// assertion below: a reset does NOT replay from alpha 0. `TilesAnimator`
// treats a reset animator's next non-empty sample() as an ADOPTION — content
// already present, not entering — so the wall SNAPS STRAIGHT TO ITS FINAL
// STATE: alpha pops to 1 and mid-spring rects jump to their settled positions.
// That is why `EXPECT_GE(onAir alpha, midFlight alpha)` alone is NOT a
// regression test — a snap to 1 satisfies it just as well as continuity does,
// and the first draft of this test passed against the unfixed code. The
// falsifying assertions are the ones that bound the OTHER side: the post-take
// alpha of a tile that was mid-ramp must stay BELOW 0.9, and any tile already
// at opacity 1 must keep its mid-spring RECT (EXPECT_NEAR, 0.05). Both were
// verified red by reverting the MediaCore.cpp / TilesPlanAnimation.h changes,
// not assumed.
//
// One honest limit on that revert: the generation-equality check is not
// independently falsified by it, because the reverted take path never touches
// tilesWallSources_ at all — it reads 0 == 0 either way. The alpha and rect
// assertions are the proven-red ones; the generation assertion holds forward,
// by construction of the new API.
//
// Getting a wall genuinely MID-FLIGHT deterministically (no real-time
// polling): TilesAnimator treats an animator's truly first-ever sample() call
// as an "adoption" — its content is already-present, not entering (see
// DifferentWallCannotReuseAnotherWallsCachedGeometry / TilesAnimatorTest.cpp,
// unrelated to and unchanged by #448) — so a wall's very first tick is
// SETTLED instantly and cannot exercise this test. A member JOINING an
// ALREADY-ESTABLISHED wall does animate in, and its alpha is exactly 0 on the
// single tick it is added (entryElapsed only advances on LATER ticks) — the
// everyday case that actually produces a mid-flight take.
TEST(TilesRenderPlan, AWallTakenMidAnimationIsContinuous) {
  RecordingCompositor* compositor = nullptr;
  MediaCore core(wallModules(&compositor, nullptr));

  const double kDurationMs = 2000.0;  // the animator's own clamp ceiling — see wallSceneWithDuration
  const std::vector<std::string> initialMembers{"capture:g1", "capture:g2"};
  (void)core.applyCommands(corevideo::rpc::Json::Array{
      wallLessScene("load-scene-graph", "solo"),
      wallSceneWithDuration("set-preview-scene", "gallery", initialMembers, kDurationMs)});
  ASSERT_TRUE(allSettled(tileLayers(compositor->lastPreviewPlan)))
      << "precondition: the wall must be established before a member joins it";

  // A THIRD member joins the SAME wall (same layerId, same scene) while it is
  // already on air in preview.
  (void)core.applyCommands(corevideo::rpc::Json::Array{
      wallSceneWithDuration("set-preview-scene", "gallery", wallMembers(), kDurationMs)});

  const auto midFlight = tileLayers(compositor->lastPreviewPlan);
  ASSERT_EQ(midFlight.size(), wallMembers().size());
  ASSERT_FALSE(allSettled(midFlight))
      << "precondition: the wall must still be animating for this test to mean anything";
  const auto midFlightSnapshot = snapshotTiles(compositor->lastPreviewPlan);

  const auto generationBefore = core.tilesWallGeneration("tiles:gallery");

  take(core, wallSceneWithDuration("load-scene-graph", "gallery", wallMembers(), kDurationMs),
       wallLessScene("set-preview-scene", "solo"));

  // The wall did not restart...
  EXPECT_EQ(core.tilesWallGeneration("tiles:gallery"), generationBefore)
      << "the wall's generation moved across the take — it restarted instead of continuing";

  // ...and its tiles continued from where they were, rather than snapping to
  // a new state. Alpha is the sharpest signal, but ">= before" alone is too
  // weak: an animator that RESETS (a fresh TilesAnimator's first non-empty
  // sample call is itself treated as "already there" — see
  // AWallThatWasNeverCuedStartsCold above) pops a still-entering tile straight
  // to fully opaque, which technically satisfies ">=" too. The take happens on
  // the very next tick with negligible additional wall-clock time, so a
  // CONTINUING animation cannot have progressed far past where it was; a value
  // that jumped essentially to 1.0 is a reset wearing an alpha that happens to
  // be no smaller, not a continuation.
  const auto onAir = tileLayers(compositor->lastPlan);
  ASSERT_EQ(onAir.size(), wallMembers().size()) << "the taken wall lost tiles on its first program frame";
  for (const auto* tile : onAir) {
    ASSERT_EQ(midFlightSnapshot.count(tile->layerId), 1u);
    const auto& before = midFlightSnapshot.at(tile->layerId);
    EXPECT_GE(tile->opacity, before.opacity)
        << tile->layerId << " opacity regressed across the take";
    if (before.opacity < 1.f) {
      EXPECT_LT(tile->opacity, 0.9f)
          << tile->layerId << " snapped straight to fully opaque instead of continuing its entrance "
             "— the wall was reset (popped in), not continued";
    } else {
      // Review fix round 1, Finding E: g1/g2 are already fully opaque at the
      // midFlight tick (only g3, the newly joined member, has a ramping
      // alpha) — the layout change from 2-up to 3-up put THEM mid-spring on
      // their RECT instead. A reset snaps a tile's position straight to its
      // new target (TilesAnimator: `if (added) state.position = goal;`), so
      // an on-air rect far from the mid-flight rect is a reset wearing full
      // opacity, not a continuation — the take happens on the very next tick
      // with negligible additional wall-clock time, so a genuinely
      // CONTINUING spring cannot have travelled far from where it was.
      EXPECT_NEAR(tile->rect.x, before.rect.x, 0.05f)
          << tile->layerId << " rect.x snapped to a new position instead of continuing its spring";
      EXPECT_NEAR(tile->rect.y, before.rect.y, 0.05f)
          << tile->layerId << " rect.y snapped to a new position instead of continuing its spring";
      EXPECT_NEAR(tile->rect.width, before.rect.width, 0.05f)
          << tile->layerId << " rect.width snapped to a new position instead of continuing its spring";
      EXPECT_NEAR(tile->rect.height, before.rect.height, 0.05f)
          << tile->layerId << " rect.height snapped to a new position instead of continuing its spring";
    }
  }

  // Review fix round 1, Finding D: the verdict this task exists to prove was
  // never asserted end-to-end. A take record reading "continuous"/"cut" is
  // the whole point of #448 — read the record the take above just completed
  // and assert it directly, not just the raw generation number.
  const auto snapshot = core.sessionState();
  const auto* takes = snapshot.get("takeRecords");
  ASSERT_NE(takes, nullptr);
  ASSERT_NE(takes->get("records"), nullptr);
  const auto& records = takes->get("records")->asArray();
  ASSERT_FALSE(records.empty()) << "the take above produced no record";
  const auto& record = records.back();
  EXPECT_EQ(record.getString("wall"), "continuous")
      << "the take record still reads the wall as having reset";
  EXPECT_EQ(record.getString("verdict"), "cut")
      << "the take record still reads this as a rebuild, not a cut";
}

// Review round 2: a wall present on BOTH buses with DISAGREEING `animateLayout`
// (Program false, Preview true) has NO coverage before this test — which is
// why it took two review rounds to find. Round 1 fixed the freeze (Finding B:
// the double-advance guard keyed on wall-id equality, so this exact
// configuration was advanced by NEITHER branch) and the stale-geometry
// retention (Finding C: advance() must run unconditionally for a present
// wall) as two separate, correct fixes — but combined, they advance the SAME
// shared TilesWallSource TWICE in one tick with contradictory `enabled`:
// Program's own call (enabled=false) resets it (a real reset once it has a
// key to lose), then Preview's separate call (enabled=true) sees an EMPTY key
// and resets it AGAIN. Net per tick: generation +2, no animation ever
// actually completes, and the shared object churns forever. This test must
// FAIL against commit c11862d2 (round 1) and pass after round 2's "one
// advance per wall per tick" restructure.
// Review round 2 caught: this configuration (same wall id, disagreeing
// `animateLayout`) had NO coverage before it, which is why it took two review
// rounds to find the double-advance bug it exposed.
//
// Review round 3, Finding 2 (RULING) changed what "correct" means here: the
// original version of this test asserted the shared wall's generation settled
// after climbing once (the old `enabled = programEnabled || previewEnabled`
// behaviour) — i.e. Preview's animateLayout=true was allowed to start motion
// on Program. That is a live-show hazard (an off-air Preview draft edit
// reaching Program — CLAUDE.md: "an off-air Preview look can never take video
// ... from a Program source"), so the rule is now PROGRAM WINS: a wall shared
// by both buses uses ONLY Program's `enabled`, never an OR. This test now
// asserts that property directly: with Program's animateLayout=false, the
// shared wall's generation NEVER moves off 0, no matter how many ticks pass
// or what Preview wants — it never even acquires a real key (`advance()`
// takes the early-return branch every tick since `enabled` is `false`
// throughout). If the OR were ever restored, the first tick would establish
// a real key and settle the generation at 1 instead of 0 — this assertion
// would catch that.
TEST(TilesRenderPlan, AProgramDisabledSharedWallNeverAnimatesEvenWhenPreviewWantsIt) {
  MediaCore core;
  const std::vector<std::string> members{"zoom:1", "zoom:2"};

  // PROGRAM: layerId "tiles:s", animateLayout=false (loadWall()'s default —
  // it sends no "animateLayout" key at all).
  loadWall(core, members);
  // PREVIEW: the SAME scene id "s" -> the SAME layerId "tiles:s",
  // animateLayout=true (wallScene()/animatedTilesPayload()'s default).
  (void)core.applyCommands(corevideo::rpc::Json::Array{
      wallScene("set-preview-scene", "s", members)});

  // Review round 4, Finding 2: prove the SHARED configuration this test is
  // about actually exists before asserting on it — an all-negative test
  // (generation == 0 forever) passes VACUOUSLY if the configuration silently
  // stops existing (set-preview-scene stops populating previewTilesLayer_,
  // hasPreviewScene() goes false, or the layerId derivation changes so the
  // two buses no longer share "tiles:s"). Program's own wall must be present
  // and drawing real tiles — a plain MediaCore() has no real source to admit
  // zoom:1/zoom:2, so force admission the same way
  // EachAdmittedMemberBecomesOneTileLayer does. This only rebuilds
  // lastRenderPlan_ via buildCompositorRenderPlan (see the seam's own
  // comment) — it does NOT touch tilesWallSources_/generation, so it cannot
  // disturb the property under test below.
  core.setTilesMemberFrameAgesForTest({{"zoom:1", true, 0}, {"zoom:2", true, 0}});
  ASSERT_NE(findLayer(core.lastRenderPlanForTest(), "tile:zoom:1"), nullptr)
      << "precondition: Program's wall never rendered its tiles";
  // ...and Preview's scene must have been accepted onto the SAME wall id, not
  // silently rejected or parsed onto some other layerId.
  ASSERT_TRUE(core.previewTilesLayerForTest().present)
      << "precondition: the preview scene was not accepted";
  ASSERT_EQ(core.previewTilesLayerForTest().layerId, "tiles:s")
      << "precondition: preview did not land on the SAME wall id as Program";
  ASSERT_TRUE(core.previewTilesLayerForTest().style.animateLayout)
      << "precondition: Preview's animateLayout must be true for this test to mean anything";

  EXPECT_EQ(core.tilesWallGeneration("tiles:s"), 0u)
      << "the shared wall animated on its very first tick even though Program's "
         "animateLayout is false — Preview must never be able to start it";

  // Several more ticks with nothing changing: the generation must stay
  // pinned at 0 forever, not merely "settle" at some nonzero value (which
  // would mean Preview's flag won at least once).
  for (int tick = 0; tick < 5; ++tick) {
    (void)core.applyCommands(corevideo::rpc::Json::Array{});
    EXPECT_EQ(core.tilesWallGeneration("tiles:s"), 0u)
        << "tick " << tick << ": the shared wall animated even though Program's "
           "animateLayout is false — a Preview-only toggle must never move Program";
  }
}

// THE PROPERTY: a wall settled in preview, then taken, is at its settled state
// on the first program frame — a cut, not a redraw.
TEST(TilesRenderPlan, AWallSettledInPreviewIsAlreadySettledOnItsFirstProgramFrame) {
  RecordingCompositor* compositor = nullptr;
  MediaCore core(wallModules(&compositor, nullptr));

  (void)core.applyCommands(corevideo::rpc::Json::Array{
      wallLessScene("load-scene-graph", "solo"),
      wallScene("set-preview-scene", "gallery", wallMembers())});
  settlePreviewWall(core, *compositor, wallMembers().size());
  ASSERT_TRUE(allSettled(tileLayers(compositor->lastPreviewPlan)))
      << "precondition: the preview wall must settle before the take";
  const auto settled = snapshotTiles(compositor->lastPreviewPlan);

  take(core, wallScene("load-scene-graph", "gallery", wallMembers()),
       wallLessScene("set-preview-scene", "solo"));

  const auto onAir = tileLayers(compositor->lastPlan);
  ASSERT_EQ(onAir.size(), wallMembers().size()) << "the taken wall lost tiles on its first program frame";
  for (const auto* tile : onAir) {
    ASSERT_EQ(settled.count(tile->layerId), 1u);
    const auto& want = settled.at(tile->layerId);
    EXPECT_EQ(tile->opacity, 1.f)
        << tile->layerId << " faded in on PROGRAM — the settled preview wall was redrawn, not cut to";
    EXPECT_NEAR(tile->rect.x, want.rect.x, 1e-5f);
    EXPECT_NEAR(tile->rect.y, want.rect.y, 1e-5f);
    EXPECT_NEAR(tile->rect.width, want.rect.width, 1e-5f);
    EXPECT_NEAR(tile->rect.height, want.rect.height, 1e-5f);
  }
}

// The same property across the transient that actually produced the reported
// redraw: the wall is taken on a tick where every member's frame has momentarily
// aged out (CLAUDE.md: an all-stale wall is an ORDINARY state, and every Tiles
// take can hit it before first frames land). The wall is still the wall — when
// its frames return it must appear settled, not replay its entrance from zero.
TEST(TilesRenderPlan, AWallTakenWhileItsFramesLapseIsStillCutToNotRedrawn) {
  RecordingCompositor* compositor = nullptr;
  LiveWallCaptureDevice* device = nullptr;
  MediaCore core(wallModules(&compositor, &device));

  (void)core.applyCommands(corevideo::rpc::Json::Array{
      wallLessScene("load-scene-graph", "solo"),
      wallScene("set-preview-scene", "gallery", wallMembers())});
  settlePreviewWall(core, *compositor, wallMembers().size());
  ASSERT_TRUE(allSettled(tileLayers(compositor->lastPreviewPlan)));
  const auto settled = snapshotTiles(compositor->lastPreviewPlan);

  // Age every member out of the wall (kTilesStaleFrameMs at the synthetic
  // ~17ms tick; the frozen-frameId test above uses the same 400-tick budget).
  device->pause();
  bool wallWentEmpty = false;
  for (int tick = 0; tick < 400 && !wallWentEmpty; ++tick) {
    (void)core.applyCommands(corevideo::rpc::Json::Array{});
    wallWentEmpty = tileLayers(compositor->lastPreviewPlan).empty();
  }
  ASSERT_TRUE(wallWentEmpty) << "the preview wall never went all-stale — the transient is not under test";

  take(core, wallScene("load-scene-graph", "gallery", wallMembers()),
       wallLessScene("set-preview-scene", "solo"));
  device->resume();
  (void)core.applyCommands(corevideo::rpc::Json::Array{});

  const auto onAir = tileLayers(compositor->lastPlan);
  ASSERT_EQ(onAir.size(), wallMembers().size());
  for (const auto* tile : onAir) {
    ASSERT_EQ(settled.count(tile->layerId), 1u);
    const auto& want = settled.at(tile->layerId);
    EXPECT_EQ(tile->opacity, 1.f)
        << tile->layerId << " replayed its entrance on PROGRAM after a stale beat";
    EXPECT_NEAR(tile->rect.x, want.rect.x, 1e-5f);
    EXPECT_NEAR(tile->rect.width, want.rect.width, 1e-5f);
  }
}

// The converse: the hand-off is a match on the wall key, not the removal of the
// entrance animation. A wall the preview bus never held is handed nothing — it
// composites its OWN fresh layout, and the preview bus keeps its own wall.
TEST(TilesRenderPlan, AWallThatWasNeverInPreviewIsHandedNothing) {
  RecordingCompositor* compositor = nullptr;
  MediaCore core(wallModules(&compositor, nullptr));

  // A DIFFERENT wall (different scene id => different key, and two members
  // instead of three => a visibly different layout) sits settled in preview.
  const std::vector<std::string> otherMembers{"capture:g1", "capture:g2"};
  (void)core.applyCommands(corevideo::rpc::Json::Array{
      wallLessScene("load-scene-graph", "solo"),
      wallScene("set-preview-scene", "other-gallery", otherMembers)});
  settlePreviewWall(core, *compositor, otherMembers.size());
  ASSERT_TRUE(allSettled(tileLayers(compositor->lastPreviewPlan)));
  const auto otherWall = snapshotTiles(compositor->lastPreviewPlan);

  take(core, wallScene("load-scene-graph", "gallery", wallMembers()),
       wallScene("set-preview-scene", "other-gallery", otherMembers));

  const auto onAir = tileLayers(compositor->lastPlan);
  ASSERT_EQ(onAir.size(), wallMembers().size());
  const auto* first = onAir.front();
  ASSERT_EQ(otherWall.count(first->layerId), 1u);
  EXPECT_GT(std::abs(first->rect.width - otherWall.at(first->layerId).rect.width), 1e-4f)
      << "the taken wall inherited the OTHER wall's geometry — buses must not contaminate each other";

  // And the preview bus still owns its own settled wall after the take.
  const auto previewAfter = tileLayers(compositor->lastPreviewPlan);
  ASSERT_EQ(previewAfter.size(), otherMembers.size());
  EXPECT_TRUE(allSettled(previewAfter)) << "the preview wall lost its state to a take it was not part of";
}

// THE PROPERTY (owner report, live broadcast 2026-09-09: "Tiles background still
// refreshing on cut to program, that should be seamless"): the wall's LIVE
// background feed rode the same 1500ms kTilesStaleFrameMs admission the tiles
// do, so one beat of its frames lapsing across the take dropped the
// `tiles-source-bg:` layer entirely — program fell through to the solid colour
// and the picture popped back when frames resumed. A background that is being
// drawn must survive the beat.
TEST(TilesRenderPlan, AWallsLiveBackgroundSurvivesATakeAcrossAStaleBeat) {
  auto modules = corevideo::modules::createStubModules();
  auto ownedCompositor = std::make_unique<RecordingCompositor>();
  RecordingCompositor* compositor = ownedCompositor.get();
  modules.compositor = std::move(ownedCompositor);
  // The background source is a real feed on the same device, but NOT a wall
  // member — exactly how the operator configures it (a gallery member choice
  // promoted to the backdrop).
  std::vector<std::string> feeds = wallMembers();
  feeds.push_back("capture:bg");
  auto ownedDevice = std::make_unique<LiveWallCaptureDevice>(feeds);
  LiveWallCaptureDevice* device = ownedDevice.get();
  modules.captureDevice = std::move(ownedDevice);
  MediaCore core(std::move(modules));

  (void)core.applyCommands(corevideo::rpc::Json::Array{
      wallLessScene("load-scene-graph", "solo"),
      wallSceneWithBackground("set-preview-scene", "gallery", wallMembers(), "capture:bg")});
  settlePreviewWall(core, *compositor, wallMembers().size());
  ASSERT_NE(findLayer(compositor->lastPreviewPlan, "tiles-source-bg:tiles:gallery"), nullptr)
      << "precondition: the live background must be drawn on the preview wall before the take";

  // Lapse every feed — the background's included — past kTilesStaleFrameMs, the
  // transient CLAUDE.md records as ordinary on a Tiles take. FREEZE, not pause:
  // the frames keep arriving with a held frameId, which is what a real Zoom or
  // capture source does. The source is still there; only its age has grown.
  device->freeze();
  bool wallWentEmpty = false;
  for (int tick = 0; tick < 400 && !wallWentEmpty; ++tick) {
    (void)core.applyCommands(corevideo::rpc::Json::Array{});
    wallWentEmpty = tileLayers(compositor->lastPreviewPlan).empty();
  }
  ASSERT_TRUE(wallWentEmpty) << "the preview wall never went all-stale — the transient is not under test";

  take(core, wallSceneWithBackground("load-scene-graph", "gallery", wallMembers(), "capture:bg"),
       wallLessScene("set-preview-scene", "solo"));

  const auto* background = findLayer(compositor->lastPlan, "tiles-source-bg:tiles:gallery");
  ASSERT_NE(background, nullptr)
      << "the wall's live background vanished on the first PROGRAM frame — the cut showed the "
         "solid colour and the backdrop popped back when frames resumed";
  EXPECT_EQ(background->sourceId, "capture:bg");
  EXPECT_EQ(background->participantId, "capture:bg");
  EXPECT_EQ(background->kind, "participant-video");
}

// The converse, and the "never invent a source" rule: holding the background is
// evidence-based, not memory-based. A background source with NO frame in the
// gather — never arrived, or genuinely departed after having been drawn — is
// refused exactly as it is today. Emitting it anyway would paint a solid slab
// over the wall background (resolveLayers) — the bus-health slate today (#535
// slice 4a), the pink colorFromParticipantId() tile before that — which is
// worse than the pop this fix removes.
TEST(TilesRenderPlan, AStaleBackgroundIsHeldButAnAbsentOneIsNeverFabricated) {
  MediaCore core;
  const auto command = corevideo::rpc::Json::parse(R"({"type":"load-scene-graph","sceneId":"pinned","routes":[],
    "tiles":{"layerId":"tiles:pinned","members":["zoom:1"],
      "style":{"backgroundSourceId":"zoom:background","backgroundColor":"#123456"}}})");
  ASSERT_TRUE(command.has_value());
  core.applyCommands(corevideo::rpc::Json::Array{*command});

  // Drawn, fresh.
  core.setTilesMemberFrameAgesForTest({{"zoom:1", true, 0}, {"zoom:background", true, 0}});
  ASSERT_NE(findLayer(core.lastRenderPlanForTest(), "tiles-source-bg:tiles:pinned"), nullptr);

  // Still present, merely frozen well past kTilesStaleFrameMs — HELD.
  core.setTilesMemberFrameAgesForTest({{"zoom:1", true, 0}, {"zoom:background", true, 60000}});
  EXPECT_NE(findLayer(core.lastRenderPlanForTest(), "tiles-source-bg:tiles:pinned"), nullptr)
      << "a background frozen for a beat must not vanish from the plan";

  // Departed: the frame is gone from the gather entirely — RELEASED, not held.
  core.setTilesMemberFrameAgesForTest({{"zoom:1", true, 0}, {"zoom:background", false, 0}});
  EXPECT_EQ(findLayer(core.lastRenderPlanForTest(), "tiles-source-bg:tiles:pinned"), nullptr)
      << "a departed background source was fabricated into the plan";

  // Never arrived: no entry at all — same refusal.
  core.setTilesMemberFrameAgesForTest({{"zoom:1", true, 0}});
  EXPECT_EQ(findLayer(core.lastRenderPlanForTest(), "tiles-source-bg:tiles:pinned"), nullptr);

  // And the wall never ships an empty plan either way: its own solid background
  // is above the admission gate and is what program falls back to.
  EXPECT_NE(findLayer(core.lastRenderPlanForTest(), "tiles-bg:tiles:pinned"), nullptr);
}

namespace {
using corevideo::core::SourceRegistry;

const SourceRegistry::Source* findRegisteredSource(
    const std::shared_ptr<const SourceRegistry::Snapshot>& snapshot, const std::string& sourceId) {
  if (!snapshot) return nullptr;
  for (const auto& source : snapshot->sources) {
    if (source.token.sourceId.value == sourceId) return &source;
  }
  return nullptr;
}
}  // namespace

// Task 4: the wall is the first real consumer of SourceRegistry — it registers
// as a Kind::Composed source the tick it becomes live, with its five
// capture-only fields left nullopt (a wall has no SDK handle, no availability
// concept, and is never subscribed — SourceRegistry::Kind::Composed).
TEST(TilesRenderPlan, ALiveWallRegistersAsAComposedSourceInTheRegistry) {
  MediaCore core;
  loadWall(core, {"zoom:1", "zoom:2"});

  // Bind the shared_ptr before taking a pointer into it (SourceRegistryComposedTest's
  // own rule) - `findRegisteredSource(core.sourceRegistrySnapshotForTest(), ...)` as
  // one expression leaves `wall` dangling the instant the temporary shared_ptr's
  // refcount drops to zero at the semicolon.
  const auto snapshot = core.sourceRegistrySnapshotForTest();
  const auto* wall = findRegisteredSource(snapshot, "tiles:s");
  ASSERT_NE(wall, nullptr) << "a live wall must appear in the SourceRegistry snapshot";
  EXPECT_EQ(wall->kind, SourceRegistry::Kind::Composed);
  EXPECT_FALSE(wall->personId.has_value());
  EXPECT_FALSE(wall->externalId.has_value());
  EXPECT_FALSE(wall->availability.has_value());
  EXPECT_FALSE(wall->subscriptionRequested.has_value());
  EXPECT_FALSE(wall->subscriptionObserved.has_value());
}

// Lifetime is "named by a live scene" (parent spec section 2) — the SAME rule
// tilesWallSources_.releaseAllExcept already implements one level down. Once
// no scene on either bus names the wall, it must be genuinely gone from the
// registry, not tombstoned (a wall has no provider process to fence).
TEST(TilesRenderPlan, AWallNoSceneReferencesIsGoneFromTheRegistry) {
  MediaCore core;
  loadWall(core, {"zoom:1"});
  ASSERT_NE(findRegisteredSource(core.sourceRegistrySnapshotForTest(), "tiles:s"), nullptr);

  (void)core.applyCommands(corevideo::rpc::Json::Array{wallLessScene("load-scene-graph", "solo")});

  EXPECT_EQ(findRegisteredSource(core.sourceRegistrySnapshotForTest(), "tiles:s"), nullptr)
      << "a wall no scene still names must be gone from the registry, not merely departed";
}

// A wall released and then re-cued under the SAME scene id is a NEW source,
// never a resurrection of the old registry entry — mirroring
// core::TilesWallSource's own "a recreated wall starts at generation 0, never
// continuing a retired one's count." The registry has no generation counter
// per composed source to reuse, so the discriminator is the registry-minted
// instanceId: install() mints a fresh one from the CURRENT revision every
// time, so a genuinely new install() call can never mint the same value twice.
TEST(TilesRenderPlan, AWallReleasedAndReCuedRegistersAsANewSource) {
  MediaCore core;
  loadWall(core, {"zoom:1"});
  // Bind each snapshot before taking a pointer into it — see the comment on
  // the headline registration test above for why the unbound one-expression
  // form dangles.
  const auto firstSnapshot = core.sourceRegistrySnapshotForTest();
  const auto* first = findRegisteredSource(firstSnapshot, "tiles:s");
  ASSERT_NE(first, nullptr);
  const auto firstInstanceId = first->token.instanceId.value;

  (void)core.applyCommands(corevideo::rpc::Json::Array{wallLessScene("load-scene-graph", "solo")});
  ASSERT_EQ(findRegisteredSource(core.sourceRegistrySnapshotForTest(), "tiles:s"), nullptr);

  loadWall(core, {"zoom:1"});
  const auto secondSnapshot = core.sourceRegistrySnapshotForTest();
  const auto* second = findRegisteredSource(secondSnapshot, "tiles:s");
  ASSERT_NE(second, nullptr) << "re-cueing the same scene id must register again";
  EXPECT_NE(second->token.instanceId.value, firstInstanceId)
      << "a re-cued wall is a NEW registry entry, not the old one come back";
}

// Review round 1, Finding 1: a plain "exactly one entry" count is near-
// unfalsifiable here. `sources_` is a std::map keyed by sourceId, so ONE key
// can never hold two entries by construction, and "never zero" is already
// covered by ALiveWallRegistersAsAComposedSourceInTheRegistry above. Worse:
// delete registeredWallIds_ entirely and add() answers Conflict on every
// tick (taking the registry mutex 60x/s on the render path) while this count
// assertion STILL passes, because a refused add() neither creates a second
// entry nor bumps any counter this test reads.
//
// The falsifiable property is identity, not count: install() mints a fresh
// instanceId as `epoch + ":" + revision` on every real add() call, so the
// realistic regression this guards against — someone drops the guard and
// instead removes-and-re-adds every tick — mints a NEW instanceId every
// frame, which this assertion catches and a count assertion cannot.
TEST(TilesRenderPlan, ALiveWallKeepsTheSameRegistryIdentityAcrossManyTicks) {
  MediaCore core;
  loadWall(core, {"zoom:1"});

  const auto firstSnapshot = core.sourceRegistrySnapshotForTest();
  const auto* first = findRegisteredSource(firstSnapshot, "tiles:s");
  ASSERT_NE(first, nullptr);
  const auto firstInstanceId = first->token.instanceId.value;

  for (int tick = 0; tick < 25; ++tick) {
    (void)core.applyCommands(corevideo::rpc::Json::Array{});
  }

  const auto laterSnapshot = core.sourceRegistrySnapshotForTest();
  const auto count = std::count_if(laterSnapshot->sources.begin(), laterSnapshot->sources.end(),
      [](const auto& source) { return source.token.sourceId.value == "tiles:s"; });
  ASSERT_EQ(count, 1) << "never zero (a lost registration) across a live wall's steady state";
  const auto* later = findRegisteredSource(laterSnapshot, "tiles:s");
  ASSERT_NE(later, nullptr);
  EXPECT_EQ(later->token.instanceId.value, firstInstanceId)
      << "a live wall's registration must be the SAME entry across ticks, "
      << "never removed-and-re-added (which would mint a fresh instanceId)";
}
