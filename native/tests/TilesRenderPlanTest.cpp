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

// Whole-branch review fix: this test used to call `loadWall(core, {})` and read
// lastRenderPlanForTest(). An empty-members tiles node fails MediaCore's
// `tilesLayer_.present && !tilesLayer_.members.empty()` assignment gate, so
// lastRenderPlan_ was never written and the test inspected a DEFAULT-CONSTRUCTED
// plan — zero tiles-background layers is true of an empty struct, so it could
// not fail for any reason. It now drives a real no-wall scene WITH routes
// through the production tick and reads the plan the compositor was actually
// handed, so both halves of "leaves the ordinary route plan untouched" are
// genuinely asserted.
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
