// Pixel proof on Program for macOS media clips: the REAL AVFoundation decoder,
// the REAL Metal compositor and the REAL MediaCore (plan 2026-11-16 Task 3).
//
// What is judged is ProgramFrame::preview, the 320x180 BGRA thumbnail the Metal
// compositor reads back from Program on a FULL tick (applyCommands with no
// audio worker running; a full tick leaves skipCpuReadback false). Counters
// (mediaSources[]) are read too, but never instead of a pixel.
//
// Clip colours are chosen far from every slate a layer with no frame resolves
// to (kWarmingSlateRgba 0x1b1f27, kFailedSlateRgba 0x23181c, black):
// RED = BGRA(0x20,0x20,0xE0), BLUE = BGRA(0xE0,0x20,0x20), GREEN = BGRA(0x20,0xE0,0x20).

#if !COREVIDEO_STUB && COREVIDEO_WITH_METAL && COREVIDEO_WITH_AVF_MEDIA_DECODER

#include "AvfMediaFixtures.h"
#include "core/MediaCore.h"
#include "modules/Interfaces.h"
#include "rpc/Json.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using corevideo::core::MediaCore;
using corevideo::rpc::Json;
using Pixel = std::array<int, 3>;  // B, G, R

constexpr Pixel kRed = {0x20, 0x20, 0xE0};
constexpr Pixel kBlue = {0xE0, 0x20, 0x20};
constexpr Pixel kGreen = {0x20, 0xE0, 0x20};
constexpr int kTolerance = 12;

// Copied from ProgramPixelContinuityTest.cpp: no enumerated capture devices, so
// the legacy "no routes -> paint whatever frames arrived" fallback has nothing
// to paint over the media under test.
class NoCaptureDevice final : public corevideo::modules::ICaptureDevice {
 public:
  std::vector<corevideo::modules::CaptureDeviceInfo> enumerate() const override { return {}; }
  std::vector<corevideo::modules::CaptureDeviceInfo> selectInput(const std::string&, const std::string&) override { return {}; }
  std::vector<corevideo::modules::CaptureDeviceInfo> setAudioSyncOffset(const std::string&, int) override { return {}; }
  std::vector<corevideo::modules::CaptureDeviceInfo> connect(const std::string&) override { return {}; }
};

int64_t steadyMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::filesystem::path tmp(const std::string& name) {
  return std::filesystem::temp_directory_path() / ("corevideo-avfpx-" + std::to_string(steadyMs()) + "-" + name);
}
struct Cleanup { std::filesystem::path p; ~Cleanup() { std::error_code e; std::filesystem::remove(p, e); } };

// The real Metal compositor, or nullptr with the reason (no device on this machine).
std::unique_ptr<corevideo::modules::ICompositor> metalOrSkipReason(std::string& reason) {
  auto compositor = corevideo::modules::createMetalCompositor();
  if (!compositor) { reason = "createMetalCompositor returned null under COREVIDEO_WITH_METAL"; return nullptr; }
  corevideo::modules::CompositorRenderPlan probe;
  probe.renderPlanId = "avf-pixel-probe";
  const auto frame = compositor->render(probe, {});
  for (const auto& warning : frame.warnings) {
    if (warning.rfind("Metal compositor unavailable", 0) == 0) { reason = warning; return nullptr; }
  }
  if (!frame.gpuComposed) { reason = "Metal compositor did not GPU-compose the probe frame"; return nullptr; }
  return compositor;
}

// Rig: stub modules with the real compositor and the real decoder swapped in.
#define MAKE_CORE_OR_SKIP(core, testName)                                                        \
  std::string skipReason_;                                                                       \
  auto compositor_ = metalOrSkipReason(skipReason_);                                             \
  if (!compositor_) {                                                                            \
    std::fprintf(stderr, "[  SKIPPED ] AvfMediaPixel.%s: %s\n", testName, skipReason_.c_str());  \
    ASSERT_TRUE(skipReason_.find("no Metal device") != std::string::npos);                       \
    return;                                                                                      \
  }                                                                                              \
  auto modules_ = corevideo::modules::createStubModules();                                       \
  modules_.compositor = std::move(compositor_);                                                  \
  modules_.mediaDecoderFactory = decoderFactoryForTest();                                        \
  modules_.captureDevice = std::make_unique<NoCaptureDevice>();                                  \
  MediaCore core(std::move(modules_));                                                           \
  core.useZoomSourcesForTest({})

std::function<std::unique_ptr<corevideo::modules::IMediaDecoder>()> decoderFactoryForTest() {
  return corevideo::modules::createAvFoundationMediaDecoderFactory();
}

// One FULL tick, then a short real-time step so the decoder workers advance.
Json tick(MediaCore& core, Json::Array commands = {}) {
  auto state = core.applyCommands(commands, 0.0);
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  return state;
}

// Centre pixel of Program's thumbnail; false when the compositor left it empty.
bool centre(MediaCore& core, Pixel& out) {
  const auto& frame = core.lastProgramFrameForTest();
  // The thumbnail must be METAL'S readback of Program. When the compositor leaves `preview` empty,
  // MediaCore fills it with a CPU composite of the same plan (fillSyntheticProgramFramePreview), which
  // would let every test here pass without the GPU path being judged at all (measured: forcing
  // skipCpuReadback left all five green before this guard). Only the Metal readback stamps
  // programPixelSignature, so a zero signature means "not the GPU's pixels".
  if (!frame.gpuComposed || frame.programPixelSignature == 0) return false;
  const auto& preview = frame.preview;
  if (preview.bgra.empty() || preview.width <= 0 || preview.height <= 0) return false;
  const size_t offset = (static_cast<size_t>(preview.height / 2) * preview.width + preview.width / 2) * 4u;
  if (offset + 3 >= preview.bgra.size()) return false;
  out = {preview.bgra[offset], preview.bgra[offset + 1], preview.bgra[offset + 2]};
  return true;
}

bool near(const Pixel& pixel, const Pixel& target, int tolerance = kTolerance) {
  for (int channel = 0; channel < 3; ++channel) {
    if (std::abs(pixel[channel] - target[channel]) > tolerance) return false;
  }
  return true;
}

std::string show(const Pixel& pixel) {
  char text[48];
  std::snprintf(text, sizeof(text), "BGR(%d,%d,%d)", pixel[0], pixel[1], pixel[2]);
  return text;
}

// Ticks until the centre pixel is `target` (bounded wait on real work).
bool tickUntilCentre(MediaCore& core, const Pixel& target, int timeoutMs) {
  for (const auto deadline = steadyMs() + timeoutMs; steadyMs() < deadline;) {
    (void)tick(core);
    Pixel pixel{};
    if (centre(core, pixel) && near(pixel, target)) return true;
  }
  return false;
}

const Json* mediaRow(const Json& state, const std::string& sourceId) {
  const auto* rows = state.get("mediaSources");
  if (rows == nullptr) return nullptr;
  for (const auto& row : rows->asArray()) {
    if (row.getString("sourceId") == sourceId) return &row;
  }
  return nullptr;
}

Json rect() { return Json::Object{{"x", 0}, {"y", 0}, {"width", 1}, {"height", 1}}; }

// ONE full-canvas fixed media route.
Json mediaRoute(const std::string& assetId, const std::string& kind, const std::filesystem::path& path) {
  return Json::Object{{"routeId", "r"}, {"mode", "fixed"}, {"mediaAssetId", assetId}, {"mediaAssetName", assetId},
                      {"mediaAssetKind", kind}, {"mediaAssetPath", path.string()}, {"mediaAssetLoop", false},
                      {"rect", rect()}};
}

Json scene(const char* type, const std::string& sceneId, Json::Array routes) {
  return Json::Object{{"type", type}, {"sceneId", sceneId}, {"routes", std::move(routes)}};
}

Json backgroundScene(const std::string& sceneId, const std::filesystem::path& path) {
  return Json::Object{{"type", "load-scene-graph"}, {"sceneId", sceneId}, {"routes", Json::Array{}},
                      {"background", Json::Object{{"mediaAssetId", "bg"}, {"mediaAssetName", "BG"},
                                                  {"mediaAssetKind", "stinger"}, {"mediaAssetPath", path.string()}}}};
}

Json transport(const char* action) {
  return Json::Object{{"type", "set-media-transport"}, {"mediaAssetId", "clip"}, {"action", action}};
}

}  // namespace

// Any pixel of Program's GPU thumbnail by fraction; same GPU-readback guard as centre().
static bool at(MediaCore& core, float fx, float fy, Pixel& out) {
  const auto& frame = core.lastProgramFrameForTest();
  if (!frame.gpuComposed || frame.programPixelSignature == 0) return false;
  const auto& preview = frame.preview;
  if (preview.bgra.empty() || preview.width <= 0 || preview.height <= 0) return false;
  const int x = std::min(preview.width - 1, static_cast<int>(fx * preview.width));
  const int y = std::min(preview.height - 1, static_cast<int>(fy * preview.height));
  const size_t offset = (static_cast<size_t>(y) * preview.width + x) * 4u;
  out = {preview.bgra[offset], preview.bgra[offset + 1], preview.bgra[offset + 2]};
  return true;
}

// N3 (plan 2026-11-21 Task 2). A portrait phone clip (storage 64x36, 90 degree
// preferredTransform) routed full canvas with fit "fit" on 16:9 Program is
// PILLARBOXED and UPRIGHT. Fixture facts: the display picture is 36x64, so on
// the 320x180 thumbnail it spans x 0.342..0.658; its display quadrants are TL
// BLUE, TR RED, BL WHITE, BR GREEN (storage TL RED rotated to display TR).
TEST(AvfMediaPixel, APortraitClipIsPillarboxedUprightOnProgram) {
  constexpr Pixel kWhite = {0xD0, 0xD0, 0xD0};
  const auto p = tmp("portrait.mp4"); Cleanup c{p};
  ASSERT_EQ(corevideo::testing::writeQuadrantClip(p.string(), 64, 36, 90, 60), "");
  MAKE_CORE_OR_SKIP(core, "APortraitClipIsPillarboxedUprightOnProgram");
  auto route = Json::Object{{"routeId", "r"}, {"mode", "fixed"}, {"mediaAssetId", "clip"}, {"mediaAssetName", "clip"},
                            {"mediaAssetKind", "stinger"}, {"mediaAssetPath", p.string()}, {"mediaAssetLoop", true},
                            {"fitMode", "fit"}, {"rect", rect()}};
  (void)tick(core, {scene("load-scene-graph", "s", {Json(route)})});
  Pixel tl{}, tr{}, bl{}, br{}, edge{};
  bool upright = false;
  for (const auto deadline = steadyMs() + 3000; steadyMs() < deadline && !upright;) {
    (void)tick(core);
    if (!at(core, 0.421f, 0.25f, tl) || !at(core, 0.579f, 0.25f, tr) || !at(core, 0.421f, 0.75f, bl) ||
        !at(core, 0.579f, 0.75f, br) || !at(core, 0.10f, 0.5f, edge)) continue;
    upright = near(tl, kBlue) && near(tr, kRed) && near(bl, kWhite) && near(br, kGreen);
  }
  EXPECT_TRUE(upright) << "display TL " << show(tl) << " TR " << show(tr) << " BL " << show(bl) << " BR " << show(br)
                       << " (want BLUE, RED, WHITE, GREEN)";
  EXPECT_TRUE(edge[0] < 0x30 && edge[1] < 0x30 && edge[2] < 0x30)
      << "the side of a portrait picture on 16:9 Program is pillarbox, got " << show(edge);
}

TEST(AvfMediaPixel, AClipOnProgramRollsFromItsFirstFrameAndNeverShowsASlate) {
  // Fixture: 640x360 @30, 30 RED frames (1 s) then 60 BLUE (2 s).
  const auto p = tmp("roll.mp4"); Cleanup c{p};
  ASSERT_EQ(corevideo::testing::writeFlatColourClip(p.string(), 640, 360, 30,
                {{30, 0x20, 0x20, 0xE0}, {60, 0xE0, 0x20, 0x20}}), "");
  MAKE_CORE_OR_SKIP(core, "AClipOnProgramRollsFromItsFirstFrameAndNeverShowsASlate");
  (void)tick(core, {scene("load-scene-graph", "s", {mediaRoute("clip", "stinger", p)})});

  // PLAN FIXTURE CORRECTED (rule 4 corollary, see the task report). The plan judged "every sampled
  // centre pixel from the first tick whose preview is non-empty". Program's thumbnail is non-empty
  // from the first tick, and a clip loaded onto an EMPTY Program has no outgoing picture to hold
  // (the cold-cut hold needs a Take from a loaded scene, MediaCore.cpp "#449"), so the ticks before
  // the reader's asynchronous open delivers its first sample show the warming slate under any
  // non-blocking decoder. The judged window therefore starts at the first CONTENT tick, the same
  // rule the plan uses for its loop test; the Take path is judged from its first tick in
  // ACuedClipTakenToProgramStartsOnItsPosterFrame below.
  int ticksBeforeContent = 0;
  bool red = false;
  for (const auto deadline = steadyMs() + 3000; steadyMs() < deadline; ++ticksBeforeContent) {
    (void)tick(core);
    Pixel pixel{};
    if (centre(core, pixel) && near(pixel, kRed)) { red = true; break; }
    if (ticksBeforeContent == 0 && centre(core, pixel)) {
      std::fprintf(stderr, "[avf-pixel] before the first sample Program shows %s\n", show(pixel).c_str());
    }
    ASSERT_FALSE(centre(core, pixel) && near(pixel, kBlue)) << "the clip did not start on its first frame";
  }
  ASSERT_TRUE(red) << "the clip's first (RED) frame never reached Program";
  std::fprintf(stderr, "[avf-pixel] first content after %d ticks\n", ticksBeforeContent);

  bool blue = false;
  int index = 0;
  for (const auto deadline = steadyMs() + 2500; steadyMs() < deadline; ++index) {
    (void)tick(core);
    Pixel pixel{};
    ASSERT_TRUE(centre(core, pixel)) << "tick " << index << ": empty Program thumbnail";
    ASSERT_TRUE(near(pixel, kRed) || near(pixel, kBlue)) << "tick " << index << " after content: " << show(pixel);
    if (near(pixel, kBlue)) { blue = true; break; }
  }
  EXPECT_TRUE(blue) << "the clip never rolled past its first second";
  const auto state = core.sessionState();
  const auto* row = mediaRow(state, "media:clip");
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(row->getString("state"), "live");
  EXPECT_TRUE(row->get("onProgram")->asBool());
}

TEST(AvfMediaPixel, APausedClipHoldsTheExactOnAirPixels) {
  const auto p = tmp("pause.mp4"); Cleanup c{p};
  ASSERT_EQ(corevideo::testing::writeFlatColourClip(p.string(), 640, 360, 30,
                {{30, 0x20, 0x20, 0xE0}, {60, 0xE0, 0x20, 0x20}}), "");
  MAKE_CORE_OR_SKIP(core, "APausedClipHoldsTheExactOnAirPixels");
  (void)tick(core, {scene("load-scene-graph", "s", {mediaRoute("clip", "stinger", p)})});
  ASSERT_TRUE(tickUntilCentre(core, kBlue, 4000)) << "BLUE never came on air";

  (void)tick(core, {transport("pause")});
  (void)tick(core);
  const auto held = core.lastProgramFrameForTest().preview.bgra;
  ASSERT_FALSE(held.empty());
  auto state = core.sessionState();
  const auto* paused = mediaRow(state, "media:clip");
  ASSERT_NE(paused, nullptr);
  const auto presentedAtPause = paused->getNumber("presentedVideoFrames", -1);
  const auto positionAtPause = paused->getNumber("positionMs", -1);
  ASSERT_GE(presentedAtPause, 1.0);
  const auto pauseStarted = steadyMs();
  for (int index = 0; index < 30 || steadyMs() - pauseStarted < 500; ++index) {
    state = tick(core);
    ASSERT_TRUE(core.lastProgramFrameForTest().preview.bgra == held) << "tick " << index << ": the held picture changed";
    const auto* row = mediaRow(state, "media:clip");
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(row->getString("state"), "paused");
    EXPECT_EQ(row->getNumber("presentedVideoFrames", -1), presentedAtPause) << "tick " << index;
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
  }
  ASSERT_GE(steadyMs() - pauseStarted, 500);

  (void)tick(core, {transport("play")});
  bool resumed = false;
  double positionAfterPlay = -1;
  for (const auto deadline = steadyMs() + 1000; steadyMs() < deadline && !resumed;) {
    state = tick(core);
    const auto* row = mediaRow(state, "media:clip");
    ASSERT_NE(row, nullptr);
    if (row->getNumber("presentedVideoFrames", -1) > presentedAtPause && row->getString("state") == "live") {
      resumed = true;
      positionAfterPlay = row->getNumber("positionMs", -1);
    }
  }
  EXPECT_TRUE(resumed) << "Play did not put a new frame on air within 1 s";
  // Resume CONTINUES: a reopened reader restarts the clip at RED and at position 0.
  EXPECT_GE(positionAfterPlay, positionAtPause) << "the clip went backwards on resume";
  EXPECT_LE(positionAfterPlay, positionAtPause + 1000.0) << "the clip skipped the time it spent paused";
  // ADDED after the plan's mutation "resume reopens the reader" stayed GREEN against the plan's own
  // assertions (and against position continuity, which the plan suggested): the first frames on air
  // after Play are the up-to-three the owner had queued BEFORE the pause, so "a new frame was
  // presented" proves nothing about the reader. A reopened reader's output arrives after those, from
  // the top of the file. The clip is paused within its first BLUE moments and BLUE lasts 2 s, so for
  // the next 600 ms every tick must still be BLUE; RED here is a restart.
  const auto continueUntil = steadyMs() + 600;
  for (int index = 0; steadyMs() < continueUntil; ++index) {
    (void)tick(core);
    Pixel pixel{};
    ASSERT_TRUE(centre(core, pixel));
    ASSERT_TRUE(near(pixel, kBlue)) << "tick " << index << " after resume Program shows " << show(pixel)
                                    << " (RED = the clip restarted from the top)";
  }
}

TEST(AvfMediaPixel, ACuedClipTakenToProgramStartsOnItsPosterFrame) {
  // The real-decoder twin of ProgramPixelContinuity.ACuedClipTakenToProgramNeverShowsThePlaceholder.
  // The route wire has no source-less fill colour, so the outgoing picture is a still route.
  const auto clip = tmp("cue.mp4"); Cleanup c1{clip};
  const auto green = tmp("green.png"); Cleanup c2{green};
  ASSERT_EQ(corevideo::testing::writeFlatColourClip(clip.string(), 640, 360, 30,
                {{30, 0x20, 0x20, 0xE0}, {60, 0xE0, 0x20, 0x20}}), "");
  ASSERT_EQ(corevideo::testing::writeFlatColourPng(green.string(), 64, 64, 0x20, 0xE0, 0x20), "");
  MAKE_CORE_OR_SKIP(core, "ACuedClipTakenToProgramStartsOnItsPosterFrame");
  const auto greenScene = [&](const char* type) {
    return scene(type, "green-scene", {mediaRoute("green", "lower-third", green)});
  };
  const auto clipScene = [&](const char* type) {
    return scene(type, "clip-scene", {mediaRoute("clip", "stinger", clip)});
  };
  (void)tick(core, {greenScene("load-scene-graph")});
  ASSERT_TRUE(tickUntilCentre(core, kGreen, 3000)) << "the outgoing still never rendered";
  (void)tick(core, {clipScene("set-preview-scene")});
  bool cued = false;
  for (const auto deadline = steadyMs() + 3000; steadyMs() < deadline && !cued;) {
    const auto state = tick(core);
    const auto* row = mediaRow(state, "media:clip");
    cued = row && row->getString("state") == "cued" && row->getNumber("decodedVideoFrames", 0) >= 1;
  }
  ASSERT_TRUE(cued) << "the cue never decoded its poster";

  (void)core.applyCommands(Json::Array{
      Json::Object{{"type", "begin-take-transition"}, {"operationId", "take-1"}, {"revision", 1},
                   {"mode", "cut"}, {"durationMs", 0}},
      clipScene("load-scene-graph"), greenScene("set-preview-scene")});
  int redTail = 0;
  for (int index = 0; index < 10; ++index) {
    if (index > 0) (void)tick(core);
    Pixel pixel{};
    ASSERT_TRUE(centre(core, pixel)) << "tick " << index;
    ASSERT_TRUE(near(pixel, kGreen) || near(pixel, kRed)) << "tick " << index << " after the take: " << show(pixel);
    redTail = near(pixel, kRed) ? redTail + 1 : 0;
  }
  EXPECT_GE(redTail, 5) << "the last 5 ticks must show the clip's first (RED) second";
  // ADDED after the mutation "playing frames carry no pixels" stayed GREEN here: the ten ticks above
  // sit inside the clip's first second and the held cue poster is RED too, so they cannot tell a
  // rolling clip from a frozen poster. The clip must reach its BLUE second.
  EXPECT_TRUE(tickUntilCentre(core, kBlue, 2500)) << "the taken clip never rolled past its poster";
}

TEST(AvfMediaPixel, ALoopingVideoBackgroundKeepsChangingPastItsEnd) {
  // Fixture: 64x64 @30, 15 RED then 15 BLUE = 1 s. A scene background always loops.
  const auto p = tmp("loop.mp4"); Cleanup c{p};
  ASSERT_EQ(corevideo::testing::writeFlatColourClip(p.string(), 64, 64, 30,
                {{15, 0x20, 0x20, 0xE0}, {15, 0xE0, 0x20, 0x20}}), "");
  MAKE_CORE_OR_SKIP(core, "ALoopingVideoBackgroundKeepsChangingPastItsEnd");
  (void)tick(core, {backgroundScene("s", p)});
  bool content = false;
  int transitions = 0;
  int last = 0;  // 1 = RED, 2 = BLUE
  int index = 0;
  for (const auto end = steadyMs() + 3000; steadyMs() < end; ++index) {
    const auto state = tick(core);
    Pixel pixel{};
    if (!centre(core, pixel)) continue;
    const int colour = near(pixel, kRed) ? 1 : near(pixel, kBlue) ? 2 : 0;
    if (!content && colour == 0) continue;  // before the first content tick
    content = true;
    ASSERT_TRUE(colour != 0) << "tick " << index << " after content: " << show(pixel);
    if (last != 0 && colour != last) ++transitions;
    last = colour;
    const auto* row = mediaRow(state, "background:bg");
    ASSERT_NE(row, nullptr);
    EXPECT_TRUE(row->get("loop")->asBool());
    EXPECT_EQ(row->getString("state"), "live");
  }
  ASSERT_TRUE(content) << "the background never rendered";
  // 3 s of a 1 s loop: R->B, B->R, R->B, B->R, R->B = 5; 4 proves it looped twice.
  EXPECT_GE(transitions, 4) << "the background stopped changing (it did not loop)";
}

TEST(AvfMediaPixel, AStillSceneBackgroundRenders) {
  // Spec correction 4: no scene background, still or video, rendered on Mac before this slice.
  const auto p = tmp("bg.png"); Cleanup c{p};
  ASSERT_EQ(corevideo::testing::writeFlatColourPng(p.string(), 64, 64, 0x20, 0xE0, 0x20), "");
  MAKE_CORE_OR_SKIP(core, "AStillSceneBackgroundRenders");
  (void)tick(core, {backgroundScene("s", p)});
  bool green = false;
  Pixel pixel{};
  for (const auto deadline = steadyMs() + 2000; steadyMs() < deadline && !green;) {
    (void)tick(core);
    green = centre(core, pixel) && near(pixel, kGreen, 6);
  }
  EXPECT_TRUE(green) << "the still background never rendered; last centre " << show(pixel);
}

TEST(AvfMediaPixel, ANonLoopingClipEndsOnProgramOnItsLastFrame) {
  // #1214. Fixture: 640x360 @30, 27 RED, then ONE frame each of BLUE, GREEN and WHITE (1 s). The
  // clip's last frame is WHITE, and the two before it are individually identifiable too, so a clip
  // that ends short says by how many frames.
  constexpr Pixel kWhite = {0xD0, 0xD0, 0xD0};
  const auto p = tmp("end.mp4"); Cleanup c{p};
  ASSERT_EQ(corevideo::testing::writeFlatColourClip(p.string(), 640, 360, 30,
                {{27, 0x20, 0x20, 0xE0}, {1, 0xE0, 0x20, 0x20}, {1, 0x20, 0xE0, 0x20}, {1, 0xD0, 0xD0, 0xD0}}), "");
  MAKE_CORE_OR_SKIP(core, "ANonLoopingClipEndsOnProgramOnItsLastFrame");
  (void)tick(core, {scene("load-scene-graph", "s", {mediaRoute("clip", "stinger", p)})});
  ASSERT_TRUE(tickUntilCentre(core, kRed, 3000)) << "the clip never came on air";
  const auto name = [&](const Pixel& pixel) -> std::string {
    if (near(pixel, kRed)) return "RED (27 or more frames short)";
    if (near(pixel, kBlue)) return "BLUE (2 frames short)";
    if (near(pixel, kGreen)) return "GREEN (1 frame short)";
    if (near(pixel, kWhite)) return "WHITE (the last frame)";
    return show(pixel);
  };
  int64_t endedAtMs = -1;
  std::string onAirWhenEnded;
  Pixel pixel{};
  for (const auto deadline = steadyMs() + 4000; steadyMs() < deadline;) {
    const auto state = tick(core);
    ASSERT_TRUE(centre(core, pixel));
    const auto* row = mediaRow(state, "media:clip");
    ASSERT_NE(row, nullptr);
    if (endedAtMs < 0 && row->getString("state") == "ended") { endedAtMs = steadyMs(); onAirWhenEnded = name(pixel); }
    if (endedAtMs >= 0 && steadyMs() - endedAtMs > 500) break;
  }
  std::fprintf(stderr, "[avf-pixel] on air when Ended: %s; held 500 ms later: %s\n", onAirWhenEnded.c_str(),
               name(pixel).c_str());
  ASSERT_GE(endedAtMs, 0) << "the clip never reported ended";
  EXPECT_TRUE(near(pixel, kWhite)) << "Program froze on " << name(pixel) << ", not the clip's last frame";
}

// N3 (plan 2026-11-21 Task 1). An audio bed routed full canvas over a GREEN
// still background: Program keeps showing the background (a frameless layer
// would paint the warming/failed slate over everything), nothing warns that
// the file "could not be opened", and the transport ends when the audio does.
// Fixture: a 1.0 s 440 Hz AAC .m4a (no video track) and a GREEN 64x64 PNG.
TEST(AvfMediaPixel, AnAudioBedOnProgramDrawsNothingOverTheSceneAndEnds) {
  const auto bed = tmp("bed.m4a"); Cleanup c1{bed};
  const auto bg = tmp("bg.png"); Cleanup c2{bg};
  ASSERT_EQ(corevideo::testing::writeToneOnlyAudio(bed.string(), 1000, 440.0), "");
  ASSERT_EQ(corevideo::testing::writeFlatColourPng(bg.string(), 64, 64, 0x20, 0xE0, 0x20), "");
  MAKE_CORE_OR_SKIP(core, "AnAudioBedOnProgramDrawsNothingOverTheSceneAndEnds");
  const Json load = Json::Object{
      {"type", "load-scene-graph"}, {"sceneId", "s"}, {"routes", Json::Array{mediaRoute("clip", "audio-bed", bed)}},
      {"background", Json::Object{{"mediaAssetId", "bg"}, {"mediaAssetName", "BG"}, {"mediaAssetKind", "stinger"},
                                  {"mediaAssetPath", bg.string()}}}};
  (void)tick(core, {load});
  ASSERT_TRUE(tickUntilCentre(core, kGreen, 3000)) << "the background never rendered";
  int64_t endedAtMs = -1;
  const auto start = steadyMs();
  int ticks = 0;
  for (const auto deadline = start + 4000; steadyMs() < deadline; ++ticks) {
    const auto state = tick(core);
    Pixel pixel{};
    ASSERT_TRUE(centre(core, pixel));
    ASSERT_TRUE(near(pixel, kGreen)) << "tick " << ticks << ": the audio bed painted " << show(pixel)
                                     << " over the scene";
    const auto text = state.stringify();
    ASSERT_EQ(text.find("could not be opened"), std::string::npos) << "an audio bed is not an open failure";
    const auto* row = mediaRow(state, "media:clip");
    ASSERT_NE(row, nullptr);
    if (endedAtMs < 0 && row->getString("state") == "ended") endedAtMs = steadyMs() - start;
    if (endedAtMs >= 0 && steadyMs() - start - endedAtMs > 300) break;
  }
  std::fprintf(stderr, "[avf-pixel] audio bed ended %lld ms after it came on air\n", static_cast<long long>(endedAtMs));
  ASSERT_GE(endedAtMs, 0) << "the audio bed never ended";
  EXPECT_LE(endedAtMs, 2500) << "ended long after its 1 s of audio";
}

#endif  // !COREVIDEO_STUB && COREVIDEO_WITH_METAL && COREVIDEO_WITH_AVF_MEDIA_DECODER
