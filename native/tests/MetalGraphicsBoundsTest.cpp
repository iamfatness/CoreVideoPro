// Metal graphics backend G3: graphics compositor tests that were D3D11-only.
//
// D3D11CompositorTest.cpp is D3D11-gated, and G1 wrote new Mac tests instead of
// porting its graphics cases, so these never ran on Metal. Each port keeps the
// D3D11 assertions unchanged; only the compositor factory and the helpers
// (rebuilt here from their D3D11 definitions) differ. Later G3 tasks add the
// 1080p crawl boundary, update storms and retention under cache pressure.
//
// A machine without a Metal device logs the reason and returns green (the
// vendored gtest has no GTEST_SKIP; same pattern as MetalCompositorTest.cpp).

#if COREVIDEO_WITH_METAL && !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "graphics/GraphicsInstances.h"
#include "modules/Interfaces.h"
#include "modules/ProgramFramePreview.h"

namespace {

using namespace corevideo::modules;
using corevideo::rpc::Json;

bool metalUnavailable(ICompositor* compositor) {
  if (!compositor) {
    std::fprintf(stderr, "[metal-test] unavailable: createMetalCompositor returned null\n");
    return true;
  }
  CompositorRenderPlan probe;
  probe.renderPlanId = "metal-bounds-probe";
  const auto frame = compositor->render(probe, {});
  for (const auto& warning : frame.warnings) {
    if (warning.rfind("Metal compositor unavailable", 0) == 0) {
      std::fprintf(stderr, "[metal-test] unavailable: %s\n", warning.c_str());
      return true;
    }
  }
  return false;
}

#define METAL_COMPOSITOR_OR_SKIP(var)                \
  auto var = createMetalCompositor();                \
  if (metalUnavailable(var.get())) return;

bool hasWarning(const ProgramFrame& frame, const std::string& warning) {
  return std::find(frame.warnings.begin(), frame.warnings.end(), warning) != frame.warnings.end();
}

// Readiness is asynchronous; never make the render call block to satisfy a
// test. A bounded poll that asserts a state, never a duration.
ProgramFrame renderUntilPrepared(ICompositor& compositor, const CompositorRenderPlan& plan,
                                 const std::vector<VideoFrame>& sources = {}) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  for (;;) {
    auto frame = compositor.render(plan, sources);
    if (!hasWarning(frame, "graphics-preparing")) return frame;
    if (std::chrono::steady_clock::now() >= end) throw std::runtime_error("Native graphics preparation timed out");
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

uint32_t previewPixelRgba(const ProgramFramePreviewPixels& preview, int x, int y) {
  if (preview.width <= 0 || preview.height <= 0 || preview.bgra.empty()) return 0;
  const int cx = std::max(0, std::min(x, preview.width - 1));
  const int cy = std::max(0, std::min(y, preview.height - 1));
  const size_t offset = static_cast<size_t>((cy * preview.width + cx) * 4);
  return (static_cast<uint32_t>(preview.bgra[offset + 3]) << 24) |
         (static_cast<uint32_t>(preview.bgra[offset + 2]) << 16) |
         (static_cast<uint32_t>(preview.bgra[offset + 1]) << 8) |
         static_cast<uint32_t>(preview.bgra[offset + 0]);
}

size_t gpuDistinctColorsInRegion(const ProgramFramePreviewPixels& preview, float x, float y, float w, float h) {
  std::set<uint32_t> colors;
  const int left = std::max(0, static_cast<int>(x * preview.width));
  const int top = std::max(0, static_cast<int>(y * preview.height));
  const int right = std::min(preview.width, static_cast<int>((x + w) * preview.width));
  const int bottom = std::min(preview.height, static_cast<int>((y + h) * preview.height));
  for (int py = top; py < bottom; ++py)
    for (int px = left; px < right; ++px) colors.insert(previewPixelRgba(preview, px, py));
  return colors.size();
}

CompositorRenderPlanLayer makeGpuOverlayLayer(const std::string& layerId, float rx, float ry, float rw, float rh) {
  CompositorRenderPlanLayer layer;
  layer.layerId = layerId;
  layer.kind = "overlay";
  layer.order = 10;
  layer.rect = {rx, ry, rw, rh};
  layer.opacity = 0.95f;
  layer.hasOverlayContent = true;
  return layer;
}

VideoFrame solidBgraFrame(const std::string& participantId, int width, int height, uint8_t luma) {
  VideoFrame frame;
  frame.participantId = participantId;
  frame.width = frame.naturalWidth = frame.pixelWidth = width;
  frame.height = frame.naturalHeight = frame.pixelHeight = height;
  frame.pixelStride = width * 4;
  frame.timestampMs = 16;
  frame.frameId = 1;
  auto pixels = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(width) * height * 4, luma);
  for (size_t i = 3; i < pixels->size(); i += 4) (*pixels)[i] = 255;
  frame.pixels = std::move(pixels);
  return frame;
}

CompositorRenderPlan fullRectParticipantPlan(const std::string& planId, const std::string& participantId,
                                             int width, int height) {
  CompositorRenderPlan plan;
  plan.renderPlanId = planId;
  plan.width = width;
  plan.height = height;
  CompositorRenderPlanLayer layer;
  layer.layerId = "route:" + participantId;
  layer.kind = "participant-video";
  layer.sourceId = "zoom:" + participantId;
  layer.participantId = participantId;
  layer.order = 0;
  layer.rect = {0.f, 0.f, 1.f, 1.f};
  layer.opacity = 1.f;
  plan.layers.push_back(layer);
  return plan;
}

// The Metal counterpart of D3D11CompositorTest's KeyRig, without the graphics
// key (Metal has no graphics key; spec 10 rules it out of scope).
struct MetalGraphicsRig {
  std::unique_ptr<ICompositor> compositor = createMetalCompositor();
  explicit MetalGraphicsRig(std::function<double()> tick = {}) : graphics("test-core", std::move(tick)) {}
  corevideo::graphics::GraphicsInstances graphics;
  VideoFrame video = solidBgraFrame("p-key", 320, 180, 200);
  int sequence = 0;

  bool unavailable() { return metalUnavailable(compositor.get()); }

  bool command(const std::string& type, Json::Object payload, const std::string& instanceId = "featured") {
    payload["schemaVersion"] = 1;
    payload["requestId"] = "metal-request-" + std::to_string(++sequence);
    payload["clientId"] = "gpu-qa";
    payload["coreEpoch"] = "test-core";
    if (payload.find("instanceId") == payload.end()) payload["instanceId"] = instanceId;
    return graphics.apply(Json::Object{{"type", type}, {"graphics", std::move(payload)}});
  }

  CompositorRenderPlan plan(int width = 320, int height = 180) {
    auto value = fullRectParticipantPlan("metal-graphics", "p-key", width, height);
    graphics.append(value, false);
    return value;
  }

  ProgramFrame render(const CompositorRenderPlan& value) {
    auto frame = compositor->render(value, {video});
    graphics.observe(frame);
    return frame;
  }

  const Json& row() { return graphics.snapshot().get("instances")->asArray().front(); }
};

#define RIG_OR_SKIP(rig) \
  if (rig.unavailable()) return;

}  // namespace

// A remapped Program monitor must hard-clip its animated key. Without the
// per-layer scissor, a building-out lower third is drawn into the cells below.
// Port of D3D11Compositor.AnimatedOverlayHonorsCompositeClipRect.
TEST(MetalGraphicsBounds, AnimatedOverlayHonorsCompositeClipRect) {
  METAL_COMPOSITOR_OR_SKIP(compositor);
  CompositorRenderPlan baselinePlan;
  baselinePlan.renderPlanId = "overlay-clip-baseline";
  baselinePlan.width = 640;
  baselinePlan.height = 360;
  const auto baseline = renderUntilPrepared(*compositor, baselinePlan, {});

  auto clippedPlan = baselinePlan;
  clippedPlan.renderPlanId = "overlay-clip-building-out";
  auto overlay = makeGpuOverlayLayer("multiview-pgm:key:lower-third", 0.08f, 0.43f, 0.38f, 0.12f);
  overlay.hasClipRect = true;
  overlay.clipRect = {0.f, 0.f, 0.5f, 0.5f};
  overlay.overlay.title = "CLIPPED EXIT";
  overlay.overlay.keyPhase = "building-out";
  overlay.overlay.keyProgress = 0.5f;
  clippedPlan.layers.push_back(overlay);
  const auto clipped = renderUntilPrepared(*compositor, clippedPlan, {});

  ASSERT_EQ(baseline.preview.width, clipped.preview.width);
  ASSERT_EQ(baseline.preview.height, clipped.preview.height);
  // Fixture invariant: the unclipped overlay rect (y 0.43..0.55) crosses the
  // clip's bottom edge (0.5), so without the scissor it inks below it.
  size_t mismatches = 0;
  for (int y = clipped.preview.height / 2; y < clipped.preview.height; ++y)
    for (int x = 0; x < clipped.preview.width / 2; ++x)
      if (previewPixelRgba(clipped.preview, x, y) != previewPixelRgba(baseline.preview, x, y)) ++mismatches;
  EXPECT_EQ(mismatches, 0u);
  // Confirmatory: the overlay did draw inside the clip (else the check above is vacuous).
  EXPECT_GT(gpuDistinctColorsInRegion(clipped.preview, 0.08f, 0.43f, 0.38f, 0.07f), 1u);
}

// The raster cache: rendering the same overlay twice must reuse the texture
// (same pixels out), and a brand-color change must invalidate it.
// Port of D3D11Compositor.OverlayRasterCacheReusesAndInvalidates.
TEST(MetalGraphicsBounds, OverlayRasterCacheReusesAndInvalidates) {
  METAL_COMPOSITOR_OR_SKIP(compositor);
  auto buildPlan = [](const std::string& brandColor) {
    CompositorRenderPlan plan;
    plan.renderPlanId = "coretext-cache";
    plan.width = 1280;
    plan.height = 720;
    auto overlay = makeGpuOverlayLayer("overlay:lt:lower-third", 0.05f, 0.78f, 0.9f, 0.16f);
    overlay.overlay.title = "CACHE CHECK";
    overlay.overlay.keyPhase = "on-air";
    overlay.overlay.keyProgress = 1.f;
    overlay.overlay.brandColor = brandColor;
    plan.layers.push_back(overlay);
    return plan;
  };
  const auto first = renderUntilPrepared(*compositor, buildPlan("#44c1a1"), {});
  const auto second = renderUntilPrepared(*compositor, buildPlan("#44c1a1"), {});
  const auto recolored = renderUntilPrepared(*compositor, buildPlan("#d04040"), {});
  EXPECT_EQ(first.preview.bgra, second.preview.bgra);
  EXPECT_NE(first.preview.bgra, recolored.preview.bgra);
}

// Port of D3D11Compositor.OversizedGraphicsAreRefusedAndValidContentRecovers.
// Fixture invariant: a 65537-byte title exceeds the 64 KiB metadata bound.
TEST(MetalGraphicsBounds, OversizedGraphicsAreRefusedAndValidContentRecovers) {
  METAL_COMPOSITOR_OR_SKIP(compositor);
  CompositorRenderPlan plan;
  plan.width = 640;
  plan.height = 360;
  auto overlay = makeGpuOverlayLayer("bounded-caption", .05f, .78f, .9f, .16f);
  overlay.overlay.title = std::string(65537, 'x');
  plan.layers.push_back(overlay);
  const auto refused = compositor->render(plan, {});
  EXPECT_TRUE(refused.gpuComposed);
  EXPECT_EQ("degraded", refused.health);
  EXPECT_TRUE(hasWarning(refused, "graphics-preparation-failed"));
  plan.layers[0].overlay.title = "Recovered without restarting";
  const auto recovered = renderUntilPrepared(*compositor, plan);
  EXPECT_TRUE(recovered.gpuComposed);
  EXPECT_FALSE(hasWarning(recovered, "graphics-preparation-failed"));
  EXPECT_GT(gpuDistinctColorsInRegion(recovered.preview, .05f, .78f, .9f, .16f), 3u);
}

// Port of D3D11Compositor.GraphicsCueAndProgramAdoptCompleteNativeRevisionsIndependently.
TEST(MetalGraphicsBounds, GraphicsCueAndProgramAdoptCompleteNativeRevisionsIndependently) {
  METAL_COMPOSITOR_OR_SKIP(compositor);
  corevideo::graphics::GraphicsInstances graphics{"test-core"};
  int sequence = 0;
  const auto command = [&](const std::string& type, Json::Object payload) {
    payload["schemaVersion"] = 1; payload["requestId"] = "request-" + std::to_string(++sequence);
    payload["clientId"] = "gpu-qa"; payload["coreEpoch"] = "test-core"; payload["instanceId"] = "featured";
    return graphics.apply(Json::Object{{"type", type}, {"graphics", std::move(payload)}});
  };
  ASSERT_TRUE(command("graphics-instance-create", {{"scope", "global"}, {"templateId", "compact-solid"},
      {"templateVersion", 1}, {"fields", Json::Object{{"name", "First speaker"}, {"title", "Engineer"}}}}));
  ASSERT_TRUE(command("graphics-instance-cue", {{"expectedRevision", 1}}));
  ASSERT_TRUE(command("graphics-instance-trigger", {{"expectedRevision", 1}, {"action", "show"}}));
  const auto render = [&](bool preview = false) {
    CompositorRenderPlan plan;
    plan.width = 320; plan.height = 180;
    graphics.append(plan, preview);
    auto frame = compositor->render(plan, {});
    if (!preview) graphics.observe(frame);
    return frame;
  };
  const auto pumpUntil = [&](const auto& ready) {
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!ready() && std::chrono::steady_clock::now() < limit) {
      render(); std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return ready();
  };
  const auto row = [&]() -> const Json& { return graphics.snapshot().get("instances")->asArray().front(); };
  ASSERT_TRUE(pumpUntil([&] { return row().getNumber("firstDrawSubmittedFrame") > 0; }));
  const auto first = render();
  EXPECT_GT(gpuDistinctColorsInRegion(first.preview, .05f, .80f, .60f, .14f), 3u);
  ASSERT_TRUE(command("graphics-instance-update", {{"expectedRevision", 1}, {"target", "cue"},
      {"fields", Json::Object{{"name", "Second speaker"}, {"title", ""}}}}));
  ASSERT_TRUE(pumpUntil([&] { return row().getString("preparation") == "ready"; }));
  EXPECT_EQ(1., row().getNumber("programRevision"));
  EXPECT_EQ(first.preview.bgra, render().preview.bgra);
  EXPECT_EQ(first.preview.bgra, render(true).preview.bgra);
  ASSERT_TRUE(command("graphics-instance-cue", {{"expectedRevision", 2}}));
  const auto cue = render(true);
  EXPECT_NE(first.preview.bgra, cue.preview.bgra);
  ASSERT_TRUE(command("graphics-instance-update", {{"expectedRevision", 2}, {"target", "program"}}));
  EXPECT_EQ(2., row().getNumber("programRevision"));
  const auto updated = render();
  EXPECT_EQ(cue.preview.bgra, updated.preview.bgra);
  EXPECT_GT(row().getNumber("firstDrawSubmittedFrame"), 0.);
  ASSERT_TRUE(command("graphics-program-clear", {{"channel", "all"}}));
  const auto cleared = render();
  EXPECT_NE(updated.preview.bgra, cleared.preview.bgra);
  EXPECT_EQ("hidden", row().getString("visibility"));
}

// Port of D3D11Compositor.GuardedShotLayoutDrawsBothPreparedGraphicsOnItsFirstFrame,
// without the graphics key. Fixture invariant: the two instances are anchored
// upper-left and lower-left, so one inks the top half and one the bottom half
// of a 320x180 Program.
TEST(MetalGraphicsBounds, GuardedShotLayoutDrawsBothPreparedGraphicsOnItsFirstFrame) {
  double tick = 0;
  MetalGraphicsRig rig([&] { return tick; });
  RIG_OR_SKIP(rig);
  std::vector<corevideo::graphics::GraphicsInstances::LayoutCue> cues;
  for (const auto* id : {"upper", "lower"}) {
    const bool upper = std::string_view(id) == "upper";
    ASSERT_TRUE(rig.command("graphics-instance-create", {
        {"scope", "global"}, {"templateId", "compact-solid"}, {"templateVersion", 1},
        {"fields", Json::Object{{"name", upper ? "First" : "Second"}}},
        {"placement", Json::Object{{"anchor", upper ? "upper-left" : "lower-left"}, {"x", 48}, {"y", upper ? 48 : -48}, {"scale", 1}}},
        {"animations", Json::Object{{"in", Json::Object{{"type", "none"}}}, {"out", Json::Object{{"type", "none"}}}}}}, id));
    const auto revision = rig.graphics.snapshot().get("lastResult")->getNumber("currentRevision");
    ASSERT_GT(revision, 0.0);
    ASSERT_TRUE(rig.command("graphics-instance-cue", {{"cueOwner", "layout-pixels"}, {"expectedRevision", revision}}, id));
    cues.push_back({id, uint64_t(revision)});
  }
  auto frame = rig.render(rig.plan());
  const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!rig.graphics.unavailableLayoutCue("layout-pixels", cues).empty() && std::chrono::steady_clock::now() < limit) {
    frame = rig.render(rig.plan());
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_TRUE(rig.graphics.unavailableLayoutCue("layout-pixels", cues).empty())
      << rig.graphics.snapshot().get("instances")->stringify();
  ASSERT_TRUE(rig.graphics.commitLayoutTake("layout-pixels", cues, false));
  const auto taken = rig.render(rig.plan());
  ASSERT_EQ(frame.preview.bgra.size(), taken.preview.bgra.size());
  ASSERT_FALSE(taken.preview.bgra.empty());
  bool top = false, bottom = false;
  for (int y = 0; y < taken.preview.height; ++y) {
    for (int x = 0; x < taken.preview.width; ++x) {
      if (previewPixelRgba(frame.preview, x, y) == previewPixelRgba(taken.preview, x, y)) continue;
      if (y < taken.preview.height / 2) top = true;
      else bottom = true;
    }
  }
  EXPECT_TRUE(top);
  EXPECT_TRUE(bottom);
}

// Graphics-shell G6 prerequisite: a crawl's pending list is adopted exactly at
// the loop boundary on a 1080p Metal Program. Port of the Program half of
// D3D11Compositor.CrawlLoopSwitchKeepsFrameMatchedProgramAndKeyAt1080p (no
// graphics key on Metal). Same manifest, but "font":"Arial": Arial exists on
// both platforms, and Segoe UI would test the substitution, not the boundary.
// Crawl time comes from the injected tick, never the wall clock.
TEST(MetalGraphicsBounds, CrawlLoopSwitchAdoptsAtTheBoundaryOnA1080pProgram) {
  double tick = 0;
  MetalGraphicsRig rig([&] { return tick; });
  RIG_OR_SKIP(rig);
  rig.video = solidBgraFrame("p-key", 1920, 1080, 200);
  auto plan = [&] {
    rig.graphics.advanceCrawls();
    auto value = fullRectParticipantPlan("crawl-boundary", "p-key", 1920, 1080);
    value.fullProgramReadback = true;
    rig.graphics.append(value, false);
    return value;
  };
  auto waitFor = [&](const auto& ready) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
      rig.render(plan());
      if (ready()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < until);
    return false;
  };
  ASSERT_TRUE(rig.command("graphics-template-install", {{"manifest", R"({
    "schemaVersion":1,"templateId":"metal-crawl","templateVersion":1,"name":"Crawl","family":"title",
    "canvas":{"width":1920,"height":1080},"bounds":{"width":320,"height":96},"brandTokens":[],
    "fields":[{"id":"crawl-items","type":"string","default":"[\"MMMMMMM\"]","maxLength":4096}],
    "animations":{"in":{"type":"none"},"out":{"type":"none"}},
    "layers":[{"id":"clip","type":"group","x":40,"y":10,"width":100,"height":64,"children":[
      {"id":"crawl","type":"crawl","x":0,"y":0,"width":120,"height":64,"speed":1000,
       "font":"Arial","fontSize":48,"color":"#FFFFFF","items":{"field":"crawl-items"}}]}]
  })"}}));
  ASSERT_TRUE(waitFor([&] {
    const auto& installs = rig.graphics.snapshot().get("templateInstalls")->asArray();
    return !installs.empty() && installs.back().getString("state") == "installed";
  }));
  // The full-resolution Program readback is a tap that fills on some frames
  // only and publishes whatever its thread last copied, so one harvest can
  // predate the state under test. It is used only for the ink-in-clip check,
  // on a static state: harvest until two consecutive readbacks are identical
  // (bounded at 5 s). Frame-to-frame equality uses the Program preview, as the
  // D3D11 test does.
  auto full = [&] {
    ProgramFrame latest;
    std::vector<uint8_t> previous;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < until) {
      auto frame = rig.render(plan());
      if (frame.programFullBgra.bgra.empty()) continue;
      const bool stable = frame.programFullBgra.bgra == previous;
      previous = frame.programFullBgra.bgra;
      latest = std::move(frame);
      if (stable) break;
    }
    return latest;
  };
  const auto blank = full();
  ASSERT_EQ(size_t(1920) * 1080 * 4, blank.programFullBgra.bgra.size()) << "no full-resolution Program readback";
  if (blank.programFullBgra.bgra.size() != size_t(1920) * 1080 * 4) return;
  ASSERT_TRUE(rig.command("graphics-instance-create", {{"scope", "global"}, {"templateId", "metal-crawl"}, {"templateVersion", 1}}));
  ASSERT_TRUE(rig.command("graphics-instance-cue", {{"expectedRevision", 1}}));
  ASSERT_TRUE(waitFor([&] { return rig.row().getString("cuePreparation") == "ready"; })) << "cue never prepared";
  rig.graphics.promoteCuedForTake();
  ASSERT_EQ(1., rig.row().getNumber("programRevision"));
  const auto initial = plan();
  ASSERT_TRUE(initial.layers.back().graphicsPreparedResource && initial.layers.back().graphicsPreparedResource->crawl);
  if (!initial.layers.back().graphicsPreparedResource || !initial.layers.back().graphicsPreparedResource->crawl) return;
  const double boundary = initial.layers.back().graphicsPreparedResource->crawl->passWidth;
  // The update lands at tick 130, so the boundary must lie beyond it.
  ASSERT_GT(boundary, 130.);

  // All crawl ink lies inside the manifest's clip group (x 40..140, y 10..74
  // of the 320x96 bounds) placed by the layer rect, as the D3D11 test computes
  // it, with a 1 px tolerance: the adapter's scissor rounds the same edges in
  // float, so an integral edge can land one pixel out. And there is some ink.
  auto inkInsideClip = [&](const CompositorRenderPlan& value, const ProgramFrame& program) {
    const auto& rect = value.layers.back().rect;
    const double left = (rect.x + rect.width * 40. / 320.) * 1920.;
    const double right = (rect.x + rect.width * 140. / 320.) * 1920.;
    const double top = (rect.y + rect.height * 10. / 96.) * 1080.;
    const double bottom = (rect.y + rect.height * 74. / 96.) * 1080.;
    const auto& pixels = program.programFullBgra.bgra;
    if (pixels.size() != blank.programFullBgra.bgra.size()) return false;
    size_t ink = 0;
    for (int y = 0; y < 1080; ++y)
      for (int x = 0; x < 1920; ++x) {
        const size_t at = (size_t(y) * 1920 + x) * 4;
        if (std::equal(pixels.begin() + at, pixels.begin() + at + 4, blank.programFullBgra.bgra.begin() + at)) continue;
        if (x < std::floor(left) - 1 || x >= std::ceil(right) + 1 || y < std::floor(top) - 1 || y >= std::ceil(bottom) + 1) {
          std::printf("[crawl-boundary] ink outside the clip at %d,%d\n", x, y);
          return false;
        }
        ++ink;
      }
    return ink > 0;
  };

  // Crawl time is the injected tick in ms (speed 1000 px/s, passWidth px).
  tick = 65;
  // The take adopts the cue revision; the 1080p Program raster then prepares
  // asynchronously. Sample once it has drawn (bounded; asserts a state).
  ASSERT_TRUE(waitFor([&] { return rig.row().getNumber("firstDrawSubmittedFrame") > 0; })) << "Program never drew";
  const auto first = rig.render(plan());
  const auto firstFull = full();
  EXPECT_TRUE(inkInsideClip(plan(), firstFull));
  // The crawl moves with the tick within one revision. (The list change below
  // cannot show this: M and W differ wherever the crawl is drawn.)
  tick = 130;
  const auto moved = rig.render(plan());
  EXPECT_NE(first.preview.bgra, moved.preview.bgra) << "the crawl did not move with its clock";

  ASSERT_TRUE(rig.command("graphics-instance-update", {{"expectedRevision", 1}, {"target", "program"},
      {"fields", Json::Object{{"crawl-items", "[\"WWWWWWW\"]"}}}}));
  ASSERT_TRUE(waitFor([&] { return rig.row().getString("preparation") == "ready"; })) << "update never prepared";
  ASSERT_EQ(1., rig.row().getNumber("programRevision"));
  const auto before = rig.render(plan());
  EXPECT_EQ(moved.preview.bgra, before.preview.bgra) << "the pending list aired before the boundary";

  tick = boundary + 65;
  const auto after = rig.render(plan());
  EXPECT_EQ(2., rig.row().getNumber("programRevision"));
  EXPECT_NE(first.preview.bgra, after.preview.bgra);
  EXPECT_EQ(before.frameNumber + 1, after.frameNumber);
  const auto afterFull = full();
  EXPECT_NE(firstFull.programFullBgra.bgra, afterFull.programFullBgra.bgra) << "the readback never showed the new list";
  EXPECT_TRUE(inkInsideClip(plan(), afterFull));
}

#endif  // COREVIDEO_WITH_METAL && !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS
