// Task 6 (persistent-sources slice 1): pixel continuity across a Take — a
// probe that cannot be fooled by counters. Earlier tasks made Preview and
// Program address the SAME media source id (`background:<asset>`), so a
// background already running on Preview is already running when Program
// takes it. This test proves it at the PIXEL level: it reads
// `ProgramFrame::preview` (the CPU thumbnail the stub compositor fills) and
// asserts every program frame from the FIRST tick after a Take carries the
// background's real luma — never one tick of the cold-start placeholder.
//
// Controller ruling (amends the task-6 brief): the brief's synchronous fake +
// program-only scenario would pass even with the old bug (it never exercises
// Preview). This test instead models the real async decoder
// (`OwnedMediaFrameSource`): no frame on a source id's FIRST poll, a flat
// dark frame on every later poll (dark, not mid-grey — see
// ColdStartGreyMediaFrameSource below for why). The background is cued on
// PREVIEW first (warming that decoder), then Taken onto Program — the exact
// shape of the live-show defect fixed by the `preview:` rename removal in
// `MediaCore::buildPreviewCompositorRenderPlan`.

#include "core/MediaCore.h"
#include "modules/OwnedMediaFrameSource.h"
#include "modules/Interfaces.h"
#include "rpc/Json.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <chrono>
#include <thread>
#include <vector>

namespace {

using corevideo::core::MediaCore;

// The default stub zoom module (RealZoomCaptureSource wrapping
// SyntheticZoomCaptureSource) always hands back two synthetic "no meeting
// joined" placeholder frames — a deliberate fallback slate so the UI stays
// renderable with no meeting. With no scene routes and no wall, MediaCore's
// `buildRenderPlanForScene` then paints one full-canvas layer per such frame
// OVER the media background (the "no routes -> show whatever frames
// arrived" legacy fallback), which would corrupt the very luma this test
// measures. This test is about the media-background decoder only, so the
// Zoom side stays silent.
class NoZoomCaptureSource final : public corevideo::modules::IZoomCaptureSource {
 public:
  std::vector<corevideo::modules::VideoFrame> pollVideoFrames() override { return {}; }
  std::vector<corevideo::modules::AudioFrame> pollAudioFrames() override { return {}; }
};

// The default stub capture device (FakeCaptureDevice) ships one pre-connected
// device ("decklink-1") that polls a real test-pattern frame every tick, which
// the same legacy fallback above would also paint over the background. No
// enumerated devices here, so nothing is ever connected.
class NoCaptureDevice final : public corevideo::modules::ICaptureDevice {
 public:
  std::vector<corevideo::modules::CaptureDeviceInfo> enumerate() const override { return {}; }
  std::vector<corevideo::modules::CaptureDeviceInfo> selectInput(const std::string&, const std::string&) override { return {}; }
  std::vector<corevideo::modules::CaptureDeviceInfo> setAudioSyncOffset(const std::string&, int) override { return {}; }
  std::vector<corevideo::modules::CaptureDeviceInfo> connect(const std::string&) override { return {}; }
};

// Emulates the real async decoder's cold start: the first time a given
// source id is polled it has no frame yet (still opening); every later poll
// delivers a flat DARK (0x10 BGRA) 64x36 frame on that source's own
// advancing frameId clock. Dark, not mid-grey: the cold-start placeholder
// this test guards against (compositor::colorFromParticipantId) always draws
// each channel independently in [72,199] — a "medium" debug palette centred
// around ~135 — so a fill anywhere near that band risks landing inside the
// placeholder's range by pure hash coincidence for some asset id. 0x10 (16)
// sits far below the placeholder's entire possible range, so the two can
// never be confused regardless of which id or hash produces the placeholder.
class ColdStartGreyMediaFrameSource final : public corevideo::modules::IMediaFrameSource {
 public:
  std::vector<corevideo::modules::VideoFrame> pollMediaFrames(
      const std::vector<corevideo::modules::CompositorRenderPlanLayer>& layers, int64_t timestampMs) override {
    std::vector<corevideo::modules::VideoFrame> frames;
    for (const auto& layer : layers) {
      if (layer.mediaAssetId.empty()) continue;
      const std::string sourceId = layer.sourceId.empty() ? "media:" + layer.mediaAssetId : layer.sourceId;
      if (polled_.insert(sourceId).second) continue;  // first poll: still opening, no frame yet
      corevideo::modules::VideoFrame frame;
      frame.participantId = sourceId;
      frame.width = frame.pixelWidth = frame.naturalWidth = 64;
      frame.height = frame.pixelHeight = frame.naturalHeight = 36;
      frame.pixelStride = 64 * 4;
      frame.timestampMs = timestampMs;
      frame.frameId = ++frameIds_[sourceId];
      // BGRA, 0x10 on every color channel, fully OPAQUE (alpha 0xff) — a real
      // decoded frame carries no meaningful alpha, and the preview blend is a
      // straight src-over (blendPixelBgra): a non-opaque source alpha blends
      // toward whatever the preview canvas already held, which would corrupt
      // the very luma this test measures.
      auto pixels = std::make_shared<std::vector<std::uint8_t>>(
          static_cast<std::size_t>(64) * static_cast<std::size_t>(36) * 4u, 0x10);
      for (std::size_t i = 3; i < pixels->size(); i += 4) (*pixels)[i] = 0xff;
      frame.pixels = std::move(pixels);
      frames.push_back(std::move(frame));
    }
    return frames;
  }

 private:
  std::set<std::string> polled_;
  std::map<std::string, std::int64_t> frameIds_;
};

// Mean luma (BT.601-ish weights, matching the rest of this test suite) over
// `ProgramFrame::preview` — the 320x180 CPU BGRA thumbnail the stub
// compositor (`CpuNoopCompositor`) fills via `fillSyntheticProgramFramePreview`
// every tick. Returns -1 when the compositor left it empty (nothing to judge).
double meanLuma(const corevideo::modules::ProgramFrame& frame) {
  const auto& px = frame.preview.bgra;
  if (px.empty()) return -1.0;
  double sum = 0;
  std::size_t n = 0;
  for (std::size_t i = 0; i + 3 < px.size(); i += 4) {
    sum += 0.114 * px[i] + 0.587 * px[i + 1] + 0.299 * px[i + 2];
    ++n;
  }
  return n ? sum / n : -1.0;
}

corevideo::rpc::Json sceneWithBackground(const char* sceneId, const char* type) {
  return corevideo::rpc::Json::Object{
      {"type", type},
      {"sceneId", sceneId},
      {"background", corevideo::rpc::Json::Object{
          {"mediaAssetId", "bg"}, {"mediaAssetName", "bg"}, {"mediaAssetKind", "video"},
          {"mediaAssetPath", "C:\\media\\bg.mp4"}, {"playing", true}}},
      {"routes", corevideo::rpc::Json::Array{}}};
}

corevideo::rpc::Json sceneWithNoBackground(const char* sceneId, const char* type) {
  return corevideo::rpc::Json::Object{
      {"type", type}, {"sceneId", sceneId}, {"routes", corevideo::rpc::Json::Array{}}};
}


// T1.11 / #449. A cold-starting DECODER, handed to a real OwnedMediaFrameSource
// by its factory — so this test exercises the owner's decoder bookkeeping, not a
// stand-in for it. Every fresh decoder yields nothing on its first poll (still
// opening) and a flat dark 0x10 frame afterwards, the same 0x10 sentinel and the
// same reasoning as ColdStartGreyMediaFrameSource above.
class ColdStartClipDecoder final : public corevideo::modules::IMediaFrameSource {
 public:
  std::vector<corevideo::modules::VideoFrame> pollMediaFrames(
      const std::vector<corevideo::modules::CompositorRenderPlanLayer>& layers, int64_t timestampMs) override {
    if (layers.empty() || layers.front().mediaAssetId.empty()) return {};
    if (!opened_) { opened_ = true; return {}; }  // first poll: the reader is still opening
    const auto& layer = layers.front();
    corevideo::modules::VideoFrame frame;
    frame.participantId = layer.sourceId.empty() ? "media:" + layer.mediaAssetId : layer.sourceId;
    frame.width = frame.pixelWidth = frame.naturalWidth = 64;
    frame.height = frame.pixelHeight = frame.naturalHeight = 36;
    frame.pixelStride = 64 * 4;
    frame.timestampMs = timestampMs;
    frame.frameId = ++frameId_;
    auto pixels = std::make_shared<std::vector<std::uint8_t>>(
        static_cast<std::size_t>(64) * static_cast<std::size_t>(36) * 4u, 0x10);
    for (std::size_t i = 3; i < pixels->size(); i += 4) (*pixels)[i] = 0xff;
    frame.pixels = std::move(pixels);
    return {frame};
  }

 private:
  bool opened_ = false;
  std::int64_t frameId_ = 0;
};

// A scene whose only layer is a clip ROUTE. `playing` and the go-live
// generation are exactly what the shell sends: a Preview cue is paused at
// generation n, and the Take that puts it on Program plays it at n+1
// (MediaGoLiveLedger.RecordTake).
corevideo::rpc::Json sceneWithClipRoute(const char* sceneId, const char* type, bool playing, int generation) {
  return corevideo::rpc::Json::Object{
      {"type", type},
      {"sceneId", sceneId},
      {"routes", corevideo::rpc::Json::Array{corevideo::rpc::Json::Object{
          {"routeId", "route-1"},
          {"mode", "fixed"},
          {"mediaAssetId", "clip"},
          {"mediaAssetName", "clip"},
          {"mediaAssetKind", "video"},
          {"mediaAssetPath", "C:\\media\\clip.mp4"},
          {"mediaPlaybackKey", std::string("media:clip:live:") + std::to_string(generation)},
          {"mediaAssetPlaying", playing},
          {"rect", corevideo::rpc::Json::Object{{"x", 0}, {"y", 0}, {"width", 1}, {"height", 1}}}}}}};
}

}  // namespace

TEST(ProgramPixelContinuity, ASharedBackgroundDoesNotFlickerAcrossATake) {
  auto modules = corevideo::modules::createStubModules();
  modules.mediaFrames = std::make_unique<ColdStartGreyMediaFrameSource>();
  modules.zoom = std::make_unique<NoZoomCaptureSource>();
  modules.captureDevice = std::make_unique<NoCaptureDevice>();
  MediaCore core(std::move(modules));
  core.enableAudioOutputWorker();

  // Program: scene-a, no background, empty routes.
  // Preview: scene-b, background "bg" (video, playing).
  (void)core.applyCommands(corevideo::rpc::Json::Array{
      sceneWithNoBackground("scene-a", "load-scene-graph"),
      sceneWithBackground("scene-b", "set-preview-scene")});
  for (int i = 0; i < 10; ++i) core.renderDisplayTick();

  // Take: Program becomes scene-b (same background); Preview becomes scene-a.
  (void)core.applyCommands(corevideo::rpc::Json::Array{
      sceneWithBackground("scene-b", "load-scene-graph"),
      sceneWithNoBackground("scene-a", "set-preview-scene")});

  // Expected luma is computed from the fill itself (0x10 on every channel ->
  // weights sum to 1.0 -> luma 16), not from a pre-take sample — the whole
  // point is that the take must not restart the decoder.
  constexpr double kExpectedGreyLuma = 0.114 * 0x10 + 0.587 * 0x10 + 0.299 * 0x10;
  // The fill is deliberately dark (see ColdStartGreyMediaFrameSource above) so
  // its luma (~16) sits far outside colorFromParticipantId's entire possible
  // range (each channel in [72,199], luma centred ~135) — a designed margin,
  // not a property of one hash output. 2.0 is comfortable here.
  constexpr double kLumaTolerance = 2.0;

  for (int tick = 0; tick < 10; ++tick) {
    core.renderDisplayTick();
    const double luma = meanLuma(core.lastProgramFrameForTest());
    ASSERT_GT(luma, 0.0) << "tick " << tick << " after the take: preview was empty";
    EXPECT_NEAR(luma, kExpectedGreyLuma, kLumaTolerance) << "tick " << tick << " after the take";
  }
}


// T1.11 / #449: a clip cued in Preview and then TAKEN must never show the
// cold-start placeholder on Program. Unlike the background case above, a clip
// legitimately changes identity on go-live (the `preview:` namespace collapses
// AND the go-live generation advances), so the fix is not a shared id — it is
// the warm cue decoder being handed over (MediaCueHandoff). Same 0x10 sentinel
// and the same designed luma margin as the background test.
TEST(ProgramPixelContinuity, ACuedClipTakenToProgramNeverShowsThePlaceholder) {
  auto modules = corevideo::modules::createStubModules();
  modules.mediaFrames = std::make_unique<corevideo::modules::OwnedMediaFrameSource>(
      [] { return std::unique_ptr<corevideo::modules::IMediaFrameSource>(new ColdStartClipDecoder()); });
  modules.zoom = std::make_unique<NoZoomCaptureSource>();
  modules.captureDevice = std::make_unique<NoCaptureDevice>();
  MediaCore core(std::move(modules));
  core.enableAudioOutputWorker();

  // Program: scene-a, nothing. Preview: scene-b, the clip CUED (paused, gen 1).
  (void)core.applyCommands(corevideo::rpc::Json::Array{
      sceneWithNoBackground("scene-a", "load-scene-graph"),
      sceneWithClipRoute("scene-b", "set-preview-scene", /*playing=*/false, /*generation=*/1)});
  // Let the cue decoder open and settle on its poster. The owner's workers are
  // asynchronous, so this is a bounded wait on real work, not a fixed sleep.
  const auto warmed = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < warmed) {
    core.renderDisplayTick();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  // The Take: Program becomes scene-b, the clip PLAYING at generation 2.
  (void)core.applyCommands(corevideo::rpc::Json::Array{
      sceneWithClipRoute("scene-b", "load-scene-graph", /*playing=*/true, /*generation=*/2),
      sceneWithNoBackground("scene-a", "set-preview-scene")});

  constexpr double kExpectedGreyLuma = 0.114 * 0x10 + 0.587 * 0x10 + 0.299 * 0x10;
  constexpr double kLumaTolerance = 2.0;
  for (int tick = 0; tick < 10; ++tick) {
    core.renderDisplayTick();
    const double luma = meanLuma(core.lastProgramFrameForTest());
    ASSERT_GT(luma, 0.0) << "tick " << tick << " after the take: preview was empty";
    EXPECT_NEAR(luma, kExpectedGreyLuma, kLumaTolerance)
        << "tick " << tick << " after the take: the clip cold-started on Program";
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}
