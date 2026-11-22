// Grading Phase 2, Tasks 6 and 7 (G5/G6): the Metal private grade preview and scopes, judged on
// the pixels of the published IOSurfaces (read back by global id, as the shell does). Ports of
// the D3D tests in IsolatedMonitorPixelTest.cpp (GradePreviewPixels.*), which are Windows-only.
//
// Fixture invariants (plan rule 1): the source is a flat 64x64 patch and Program renders it into
// a 64x64 canvas through one full-canvas layer, so the Program centre pixel and the preview centre
// pixel are the same source texel through the same fragment function; any difference is the
// grade. The warm-film look moves the (B33,G99,R177) patch by more than 3 levels on some channel.
#if COREVIDEO_WITH_METAL
#include "MetalGradeTestSupport.h"
#include <atomic>
#include <functional>
#include <chrono>
#include <thread>
#include "compositor/AdvancedGrade.h"
#include "compositor/MetalGradePreview.h"
#include "rpc/Json.h"

using namespace metal_grade_test;
using corevideo::rpc::Json;

namespace {
constexpr int kSize = 64;

MonitorRenderRequest requestWith(const VideoFrame& frame) {
  MonitorRenderRequest request;
  request.frames.push_back(frame);
  return request;
}

CompositorRenderPlan programPlan(const std::string& id) {
  CompositorRenderPlan plan; plan.renderPlanId = "grade-preview-program";
  plan.width = kSize; plan.height = kSize;
  plan.layers.push_back(fullLayer(id));
  return plan;
}

// `wantStatus` empty accepts a picture ("ready" or "held"); a test that needs one state names it,
// because the last result can still carry the previous state for the same revision.
GradePreviewSurface awaitGrade(ICompositor& compositor, MonitorRenderRequest request, const std::string& id, int64_t revision,
                               bool wantScopes = false, const std::string& wantStatus = {}) {
  static std::atomic<int64_t> sequence{0};
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < end) {
    request.sequence = ++sequence;
    compositor.submitGradePreviews(request);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    const auto result = compositor.latestGradePreviews();
    if (!result) continue;
    for (const auto& surface : result->gradePreviews)
      if (surface.demand.instanceId == id && surface.demand.revision == revision &&
          (wantStatus.empty() ? (surface.status == "ready" || surface.status == "held") : surface.status == wantStatus) &&
          surface.texture.iosurfaceId != 0 &&
          (!wantScopes || (surface.scopes.texture.iosurfaceId != 0 && surface.scopes.revision == revision)))
        return surface;
  }
  return {};
}

std::array<int, 3> centre(uint32_t iosurfaceId) {
  const auto p = surfacePixel(iosurfaceId, .5f, .5f);
  return {p[0], p[1], p[2]};
}

int maxChannelDelta(const std::array<int, 3>& a, const std::array<int, 3>& b) {
  int worst = 0;
  for (int i = 0; i < 3; ++i) worst = std::max(worst, std::abs(a[i] - b[i]));
  return worst;
}

CompositorColorGrade warmFilm(bool advanced) {
  CompositorColorGrade grade; grade.lut = "warm-film";
  if (advanced) {
    const Json line = Json::Array{Json::Object{{"x", 0}, {"y", 0}}, Json::Object{{"x", 1}, {"y", 1}}};
    std::shared_ptr<const AdvancedGradeDocument> document;
    readAdvancedGrade(Json::Object{{"version", 2}, {"colorSpace", "rec709-sdr"}, {"operations", Json::Array{
        Json::Object{{"id", "primary"}, {"kind", "primaries"}, {"exposureStops", .5},
                     {"curves", Json::Array{line, line, line, line}}}}}}, document);
    grade.advanced = document;
  }
  return grade;
}

VideoFrame patch(bool i420, bool fullRange = true, bool bt601 = false) {
  auto frame = i420 ? i420Frame("cam", kSize, kSize, 110, 100, 170, fullRange, bt601)
                    : bgraFrame("cam", kSize, kSize, 33, 99, 177);
  frame.sourceEpoch = 8; frame.captureTimestamp100ns = 1234; frame.frameId = 41;
  return frame;
}

// Port of IsolatedMonitorPixelTest.cpp gradePixelParity (:131-185).
void draftMatchesProgram(bool i420, bool advanced, bool fullRange = true, bool bt601 = false) {
  METAL_GRADE_OR_SKIP(compositor);
  ASSERT_TRUE(compositor->supportsGradePreview());
  const auto frame = patch(i420, fullRange, bt601);
  auto plan = programPlan("cam"); plan.skipCpuReadback = false;
  const auto neutral = programCentre(compositor->render(plan, {frame}));
  ASSERT_GE(neutral[0], 0);
  const auto grade = warmFilm(advanced);
  auto request = requestWith(frame);
  request.gradePreviews.push_back({"draft", "cam", 1, grade});
  const auto surface = awaitGrade(*compositor, request, "draft", 1);
  ASSERT_NE(surface.texture.iosurfaceId, 0u) << "a Metal preview is an IOSurface";
  EXPECT_TRUE(surface.texture.sharedHandleHex.empty()) << "never a DXGI handle on Mac";
  EXPECT_EQ(surface.sourceEpoch, 8u); EXPECT_EQ(surface.sourceFrameId, 41); EXPECT_EQ(surface.captureTimestamp100ns, 1234);
  EXPECT_EQ(surface.status, "ready");
  EXPECT_EQ(surface.texture.width, kSize); EXPECT_EQ(surface.texture.height, kSize);
  const auto preview = centre(surface.texture.iosurfaceId);
  EXPECT_GT(maxChannelDelta(preview, neutral), 3) << "the draft grade changed the private picture";
  // Previewing a draft leaves Program untouched.
  EXPECT_EQ(programCentre(compositor->render(plan, {frame})), neutral);
  // The same grade applied on Program produces the preview's pixel.
  plan.layers[0].hasColorGrade = true; plan.layers[0].colorGrade = grade;
  const auto applied = programCentre(compositor->render(plan, {frame}));
  EXPECT_LE(maxChannelDelta(preview, applied), 1) << "preview " << preview[2] << "," << preview[1] << "," << preview[0]
      << " vs Program " << applied[2] << "," << applied[1] << "," << applied[0];
  compositor->submitGradePreviews({});
}
}  // namespace

TEST(MetalGradePreview, BgraDraftMatchesProgramWithoutTouchingIt) { draftMatchesProgram(false, false); }
TEST(MetalGradePreview, I420DraftMatchesProgramWithoutTouchingIt) {
  for (bool full : {true, false}) for (bool bt601 : {false, true}) draftMatchesProgram(true, false, full, bt601);
}
TEST(MetalGradePreview, AdvancedDraftMatchesProgramForBgraAndI420) {
  draftMatchesProgram(false, true);
  draftMatchesProgram(true, true, false, true);
}

// Compare original is monitor-only: the private picture is the ungraded source while Program and
// the draft are unchanged (ColorGradeEditorViewModelTests.cs:68 on the shell side).
TEST(MetalGradePreview, CompareOriginalShowsTheUngradedSource) {
  METAL_GRADE_OR_SKIP(compositor);
  const auto frame = patch(false);
  auto plan = programPlan("cam"); plan.skipCpuReadback = false;
  const auto neutral = programCentre(compositor->render(plan, {frame}));
  auto request = requestWith(frame);
  GradePreviewDemand demand{"cmp", "cam", 1, warmFilm(false)};
  demand.compareOriginal = true;
  request.gradePreviews.push_back(demand);
  const auto surface = awaitGrade(*compositor, request, "cmp", 1);
  ASSERT_NE(surface.texture.iosurfaceId, 0u);
  EXPECT_LE(maxChannelDelta(centre(surface.texture.iosurfaceId), neutral), 1) << "compare shows the original";
  request.gradePreviews[0].revision = 2; request.gradePreviews[0].compareOriginal = false;
  const auto graded = awaitGrade(*compositor, request, "cmp", 2);
  EXPECT_GT(maxChannelDelta(centre(graded.texture.iosurfaceId), neutral), 3) << "turning compare off shows the draft";
  EXPECT_EQ(programCentre(compositor->render(plan, {frame})), neutral);
  compositor->submitGradePreviews({});
}

// A source that stops arriving keeps its last picture as "held" with its own epoch (the input
// cache, MonitorInputCache.h:20-40); a source never seen is "unavailable / source-unavailable".
TEST(MetalGradePreview, AVanishedSourceIsHeldAndAnUnknownOneIsUnavailable) {
  METAL_GRADE_OR_SKIP(compositor);
  auto request = requestWith(patch(false));
  request.gradePreviews.push_back({"held", "cam", 1, warmFilm(false)});
  ASSERT_NE(awaitGrade(*compositor, request, "held", 1, false, "ready").texture.iosurfaceId, 0u);
  request.frames.clear();
  // Wait for "held" itself: the previous "ready" result for this revision can still be the latest.
  const auto held = awaitGrade(*compositor, request, "held", 1, false, "held");
  EXPECT_EQ(held.status, "held"); EXPECT_EQ(held.sourceEpoch, 8u); EXPECT_EQ(held.sourceFrameId, 41);
  request.gradePreviews = {{"nobody", "ghost", 1, {}}};
  GradePreviewSurface unknown;
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < end && unknown.demand.instanceId.empty()) {
    compositor->submitGradePreviews(request);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (const auto result = compositor->latestGradePreviews())
      for (const auto& s : result->gradePreviews) if (s.demand.instanceId == "nobody") unknown = s;
  }
  EXPECT_EQ(unknown.status, "unavailable"); EXPECT_EQ(unknown.reason, "source-unavailable");
  EXPECT_EQ(unknown.texture.iosurfaceId, 0u);
  compositor->submitGradePreviews({});
}

// Two editors never share a surface; an editor that closes releases its surface, and the end of
// all demand releases everything (IOSurfaceLookup of a freed global surface fails).
TEST(MetalGradePreview, EachEditorOwnsItsSurfaceAndClosingReleasesIt) {
  METAL_GRADE_OR_SKIP(compositor);
  auto request = requestWith(patch(false));
  request.frames.push_back(bgraFrame("cam2", 32, 18, 200, 10, 10));
  request.gradePreviews.push_back({"a", "cam", 1, {}});
  request.gradePreviews.push_back({"b", "cam2", 1, {}});
  const auto a = awaitGrade(*compositor, request, "a", 1);
  const auto b = awaitGrade(*compositor, request, "b", 1);
  ASSERT_NE(a.texture.iosurfaceId, 0u); ASSERT_NE(b.texture.iosurfaceId, 0u);
  EXPECT_NE(a.texture.iosurfaceId, b.texture.iosurfaceId);
  EXPECT_EQ(b.texture.width, 32); EXPECT_EQ(b.texture.height, 18);
  // Metal frees an IOSurface-backed texture's surface asynchronously after the texture goes, so
  // each release is polled (bounded), never assumed instant.
  auto releasedWithin = [](uint32_t id, std::function<void()> keepAlive) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < end) {
      if (keepAlive) keepAlive();
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      IOSurfaceRef still = IOSurfaceLookup(id);
      if (!still) return true;
      CFRelease(still);
    }
    return false;
  };
  request.gradePreviews.pop_back();
  EXPECT_TRUE(releasedWithin(b.texture.iosurfaceId, [&] { compositor->submitGradePreviews(request); }))
      << "a closed editor's surface is released";
  IOSurfaceRef aLive = IOSurfaceLookup(a.texture.iosurfaceId);
  EXPECT_NE(aLive, nullptr) << "the open editor keeps its surface";
  if (aLive) CFRelease(aLive);
  compositor->submitGradePreviews({});
  EXPECT_TRUE(releasedWithin(a.texture.iosurfaceId, {})) << "ending all demand releases the backend";
}

// The render thread's call (spec §5.7): submit never waits, even while the worker is stuck, and a
// stuck worker is handed only the newest job (superseded, never queued).
TEST(MetalGradePreview, SubmitNeverWaitsForAStalledWorker) {
  METAL_GRADE_OR_SKIP(compositor);
  std::atomic<bool> release{false};
  std::atomic<int> stalls{0};
  MetalGradePreview::setRenderStallForTest([&] {
    ++stalls;
    while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  });
  auto request = requestWith(patch(false));
  request.gradePreviews.push_back({"stall", "cam", 1, {}});
  compositor->submitGradePreviews(request);
  const auto wait = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (stalls.load() == 0 && std::chrono::steady_clock::now() < wait) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  ASSERT_EQ(stalls.load(), 1) << "the worker is inside a job";
  double worstMs = 0;
  for (int i = 0; i < 20; ++i) {
    request.sequence = 100 + i;
    const auto t0 = std::chrono::steady_clock::now();
    compositor->submitGradePreviews(request);
    worstMs = std::max(worstMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  }
  std::fprintf(stderr, "[grade-preview-submit] worst submit while the worker is stalled: %.3f ms\n", worstMs);
  EXPECT_LT(worstMs, 2.0) << "submitGradePreviews must not wait for the worker";
  EXPECT_GE(compositor->gradePreviewDiagnostics().superseded, 19u) << "pending capacity 1: older jobs are superseded";
  release = true;
  MetalGradePreview::setRenderStallForTest({});
  EXPECT_NE(awaitGrade(*compositor, request, "stall", 1).texture.iosurfaceId, 0u) << "the worker recovers";
  compositor->submitGradePreviews({});
}

// Plan P4: under monitor shedding only every d-th request reaches the worker; after an
// over-budget job the next one is skipped; ending demand is never shed.
TEST(MetalGradePreview, ShedDivisorAndGpuBudgetThinTheRequests) {
  auto preview = MetalGradePreview::create();
  ASSERT_TRUE(preview != nullptr);
  MetalGradePreview::setRenderStallForTest([] {});   // keep jobs trivial; shedding is counted at submit
  auto request = requestWith(patch(false));
  request.gradePreviews.push_back({"shed", "cam", 1, {}});
  preview->setMonitorShedDivisor(3);
  for (int i = 0; i < 9; ++i) preview->submit(request);
  EXPECT_EQ(preview->shedCount(), 6u) << "divisor 3 forwards exactly one of every three requests";
  preview->setMonitorShedDivisor(1);
  preview->setLastGpuMsForTest(MetalGradePreview::kGpuBudgetMs + 1);
  preview->submit(request);
  EXPECT_EQ(preview->shedCount(), 7u) << "the request after an over-budget job is skipped";
  preview->submit(request);
  EXPECT_EQ(preview->shedCount(), 7u) << "and only that one";
  preview->setMonitorShedDivisor(3);
  const auto before = preview->diagnostics().submitted;
  preview->submit({});
  EXPECT_EQ(preview->diagnostics().submitted, before + 1) << "ending demand is never shed";
  MetalGradePreview::setRenderStallForTest({});
}

TEST(MetalGradePreview, ALowerThirdDemandIsNotBuiltOnMetal) {
  METAL_GRADE_OR_SKIP(compositor);
  MonitorRenderRequest request;
  GradePreviewDemand demand{"lt", "preview:lower-third", 1, {}};
  demand.lowerThirdPreview = CompositorOverlayContent{};
  request.gradePreviews.push_back(demand);
  GradePreviewSurface surface;
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < end && surface.demand.instanceId.empty()) {
    compositor->submitGradePreviews(request);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (const auto result = compositor->latestGradePreviews())
      for (const auto& s : result->gradePreviews) if (s.demand.instanceId == "lt") surface = s;
  }
  EXPECT_EQ(surface.status, "unavailable");
  EXPECT_EQ(surface.reason, "native-appearance-preview-not-built");
  compositor->submitGradePreviews({});
}

// ── Task 7 (G6): scopes ─────────────────────────────────────────────────────────────────────
// Ports of IsolatedMonitorPixelTest.cpp:263-360, judged on the published 1536x512 IOSurface.
// Expected cells come from independent Rec.709 equations over the fixture patch, never from the
// worker. Panel layout (D3DGradeScopes.h:33-72): x in [0,1/3) histogram, [1/3,2/3) waveform,
// [2/3,1] vectorscope; image y grows downward, the scope's y upward.
namespace {
GradePreviewSurface awaitScopes(ICompositor& compositor, MonitorRenderRequest& request, const std::string& id, int64_t revision) {
  return awaitGrade(compositor, request, id, revision, /*wantScopes=*/true);
}
int green(uint32_t id, float fx, float fy) { return surfacePixel(id, fx, fy)[1]; }
}  // namespace

TEST(MetalGradeScopes, KnownNeutralRgbAndSkinPatchesLandInThePredictedCells) {
  METAL_GRADE_OR_SKIP(compositor);
  auto request = requestWith(bgraFrame("cam", kSize, kSize, 0, 0, 0));
  GradePreviewDemand demand{"patches", "cam", 1, {}};
  demand.scopesEnabled = true; demand.histogramMode = 0; demand.waveformMode = 0;
  request.gradePreviews.push_back(demand);
  const int patches[][3] = {{0, 0, 0}, {255, 255, 255}, {128, 128, 128}, {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {190, 134, 110}};
  int64_t revision = 0;
  for (const auto& rgbPatch : patches) {
    ++revision;
    request.frames[0] = bgraFrame("cam", kSize, kSize, uint8_t(rgbPatch[2]), uint8_t(rgbPatch[1]), uint8_t(rgbPatch[0]), revision);
    request.gradePreviews[0].revision = revision;
    const auto surface = awaitScopes(*compositor, request, "patches", revision);
    ASSERT_NE(surface.scopes.texture.iosurfaceId, 0u) << "patch " << revision;
    EXPECT_EQ(surface.scopes.texture.width, 1536); EXPECT_EQ(surface.scopes.texture.height, 512);
    EXPECT_EQ(surface.scopes.sourceFrameId, revision); EXPECT_FALSE(surface.scopes.original);
    const float r = rgbPatch[0] / 255.f, g = rgbPatch[1] / 255.f, b = rgbPatch[2] / 255.f;
    const float luma = .2126f * r + .7152f * g + .0722f * b;
    const int bin = std::min(255, int(luma * 255));
    const int cb = std::min(255, std::max(0, int(((b - luma) / 1.8556f + .5f) * 255)));
    const int cr = std::min(255, std::max(0, int(((r - luma) / 1.5748f + .5f) * 255)));
    const uint32_t id = surface.scopes.texture.iosurfaceId;
    EXPECT_GT(green(id, (bin + .5f) / 768.f, .5f), 180) << "luma histogram bar, patch " << revision;
    EXPECT_GT(green(id, .5f, 1 - (bin + .5f) / 256.f), 240) << "luma waveform trace, patch " << revision;
    const auto vector = surfacePixel(id, (2 + (cb + .5f) / 256.f) / 3.f, 1 - (cr + .5f) / 256.f);
    EXPECT_GT(vector[1], 240) << "vectorscope density, patch " << revision;
    EXPECT_GT(vector[0], 150) << "vectorscope density (blue of .35,1,.65), patch " << revision;
    // An independent negative: a histogram bin far from this patch's luma carries no bar.
    const int far = bin < 128 ? 250 : 5;
    EXPECT_LT(green(id, (far + .5f) / 768.f, .5f), 60) << "no bar away from the patch, patch " << revision;
  }
  compositor->submitGradePreviews({});
}

TEST(MetalGradeScopes, GradedAndOriginalAnalysisMoveTheRedHistogramToThePredictedCode) {
  METAL_GRADE_OR_SKIP(compositor);
  const Json half = Json::Array{Json::Object{{"x", 0}, {"y", 0}}, Json::Object{{"x", 1}, {"y", .5}}};
  const Json line = Json::Array{Json::Object{{"x", 0}, {"y", 0}}, Json::Object{{"x", 1}, {"y", 1}}};
  CompositorColorGrade grade;
  ASSERT_TRUE(readAdvancedGrade(Json::Object{{"version", 2}, {"colorSpace", "rec709-sdr"}, {"operations", Json::Array{
      Json::Object{{"id", "red-half"}, {"kind", "curves"}, {"curves", Json::Array{line, half, line, line}}}}}}, grade.advanced));
  auto request = requestWith(patch(false));      // R=177
  GradePreviewDemand demand{"curves", "cam", 1, grade};
  demand.scopesEnabled = true;                   // histogram RGB overlay (mode 1)
  request.gradePreviews.push_back(demand);
  auto surface = awaitScopes(*compositor, request, "curves", 1);
  ASSERT_NE(surface.scopes.texture.iosurfaceId, 0u);
  const int gradedRed = surfacePixel(surface.texture.iosurfaceId, .5f, .5f)[2];
  EXPECT_NEAR(gradedRed, 89, 2) << "the red-half curve maps 177 to ~88.5";
  EXPECT_GT(surfacePixel(surface.scopes.texture.iosurfaceId, (gradedRed + .5f) / 256.f / 3.f, .5f)[2], 128)
      << "graded analysis: red bar at the graded code";
  EXPECT_LT(surfacePixel(surface.scopes.texture.iosurfaceId, 177.5f / 256.f / 3.f, .5f)[2], 60)
      << "graded analysis: nothing at the original code";
  request.gradePreviews[0].revision = 2; request.gradePreviews[0].scopesOriginal = true;
  surface = awaitScopes(*compositor, request, "curves", 2);
  ASSERT_EQ(surface.scopes.revision, 2); EXPECT_TRUE(surface.scopes.original);
  EXPECT_GT(surfacePixel(surface.scopes.texture.iosurfaceId, 177.5f / 256.f / 3.f, .5f)[2], 128)
      << "Analyze original: red bar at the source code 177";
  compositor->submitGradePreviews({});
}

TEST(MetalGradeScopes, AOnePixelRoiSamplesOnlyThatPixelAndProgramIsUntouched) {
  METAL_GRADE_OR_SKIP(compositor);
  auto pixels = std::make_shared<std::vector<uint8_t>>(size_t(kSize) * kSize * 4, 0);
  for (int y = 0; y < kSize; ++y) for (int x = 0; x < kSize; ++x) {
    const size_t i = (size_t(y) * kSize + x) * 4; (*pixels)[i + 2] = x >= 32 ? 255 : 0; (*pixels)[i + 3] = 255;
  }
  auto frame = bgraFrame("cam", kSize, kSize, 0, 0, 0, 500); frame.pixels = pixels;
  auto plan = programPlan("cam"); plan.skipCpuReadback = false;
  const auto before = programCentre(compositor->render(plan, {frame}));
  auto request = requestWith(frame);
  GradePreviewDemand demand{"roi", "cam", 1, {}};
  demand.scopesEnabled = true; demand.histogramMode = demand.waveformMode = 0;
  demand.scopeRoi = {true, .75, .5, 1. / 64, 1. / 64, 1};
  request.gradePreviews.push_back(demand);
  const auto surface = awaitScopes(*compositor, request, "roi", 1);
  const uint32_t id = surface.scopes.texture.iosurfaceId;
  ASSERT_NE(id, 0u);
  EXPECT_EQ(surface.scopes.roiPixels.x, 48); EXPECT_EQ(surface.scopes.roiPixels.y, 32);
  EXPECT_EQ(surface.scopes.roiPixels.width, 1); EXPECT_EQ(surface.scopes.roiPixels.height, 1);
  // Red luma = .2126*255 = 54.2 -> bin 54. The black half must not appear at all.
  EXPECT_GT(green(id, 54.5f / 768, .5f), 180) << "histogram: the selected red pixel";
  EXPECT_GT(green(id, .36f, 1 - 54.5f / 256), 240) << "waveform spans the region's full width (left)";
  EXPECT_GT(green(id, .64f, 1 - 54.5f / 256), 240) << "waveform spans the region's full width (right)";
  EXPECT_LT(green(id, .5f / 768, .5f), 80) << "histogram: no black from outside the region";
  EXPECT_EQ(programCentre(compositor->render(plan, {frame})), before);
  compositor->submitGradePreviews({});
}

TEST(MetalGradeScopes, ACircleMaskRemovesCornerColoursFromAllThreeScopes) {
  METAL_GRADE_OR_SKIP(compositor);
  auto pixels = std::make_shared<std::vector<uint8_t>>(size_t(kSize) * kSize * 4, 0);
  for (int y = 0; y < kSize; ++y) for (int x = 0; x < kSize; ++x) {
    const double nx = (x + .5) / 32 - 1, ny = (y + .5) / 32 - 1; const bool corner = nx * nx + ny * ny > 1.08;
    const size_t i = (size_t(y) * kSize + x) * 4; (*pixels)[i + 1] = corner ? 0 : 255; (*pixels)[i + 2] = corner ? 255 : 0; (*pixels)[i + 3] = 255;
  }
  auto frame = bgraFrame("cam", kSize, kSize, 0, 0, 0, 501); frame.pixels = pixels;
  auto plan = programPlan("cam"); plan.skipCpuReadback = false;
  const auto before = programCentre(compositor->render(plan, {frame}));
  auto request = requestWith(frame);
  GradePreviewDemand demand{"circle", "cam", 1, {}};
  demand.scopesEnabled = true; demand.scopeRoi.enabled = true; demand.histogramMode = demand.waveformMode = 0;
  request.gradePreviews.push_back(demand);
  auto measure = [&](int64_t revision, const std::string& shape) {
    request.gradePreviews[0].revision = revision; request.gradePreviews[0].scopeRoi.shape = shape;
    const auto surface = awaitScopes(*compositor, request, "circle", revision);
    const uint32_t id = surface.scopes.texture.iosurfaceId;
    const float redLuma = .2126f;
    const int cb = int(((-redLuma) / 1.8556f + .5f) * 255), cr = std::min(255, int(((1 - redLuma) / 1.5748f + .5f) * 255));
    return std::array<int, 3>{green(id, 54.5f / 768, .9f), green(id, (1 + .05f) / 3, 1 - 54.5f / 256),
                              green(id, (2 + (cb + .5f) / 256) / 3, 1 - (cr + .5f) / 256)};
  };
  const auto rectangle = measure(1, "rectangle"), circle = measure(2, "circle");
  for (int i = 0; i < 3; ++i) {
    std::fprintf(stderr, "[metal-circle-mask] scope=%d rectangle=%d circle=%d\n", i, rectangle[i], circle[i]);
    EXPECT_GT(rectangle[i], 30) << "the red corners are measured with a rectangle, scope " << i;
    EXPECT_LT(circle[i], 30) << "the circle masks the red corners, scope " << i;
  }
  EXPECT_EQ(programCentre(compositor->render(plan, {frame})), before);
  compositor->submitGradePreviews({});
}
#endif
