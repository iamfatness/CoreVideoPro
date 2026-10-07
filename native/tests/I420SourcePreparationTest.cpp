#include "modules/I420SourcePreparation.h"
#include "modules/Interfaces.h"
#include "modules/MonitorFrameAdmission.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cstdlib>
#include <thread>
#include <stdexcept>

using namespace corevideo::modules;
TEST(CpuSourceGpuView, RejectsWrongIdentityAndCpuDescriptorsDoNotOwnGpuImages) {
  CpuSourceGpuView token;
  token.sourceId = "source"; token.sourceEpoch = 7; token.frameId = 11;
  token.captureTimestamp100ns = 1234; token.width = token.height = 64;
  token.demand = std::make_shared<CpuSourceGpuDemand>();
  auto image = std::make_shared<GpuVideoFrame>();
  image->sourceId = "source"; image->sourceEpoch = 7; image->sourceFrameId = 12;
  image->sourceCaptureTimestamp100ns = 1234; image->width = image->height = 64;
  token.ready.store(image);
  EXPECT_FALSE(token.acquire(true));
  image->sourceFrameId = 11;
  image->sourceId = "another-source";
  EXPECT_FALSE(token.acquire(true));
  image->sourceId = "source";
  EXPECT_TRUE(token.acquire(true));
  EXPECT_EQ(token.demand->selectedFrameId.load(), 11);
  EXPECT_FALSE(token.consumed.load()); // actual read admission, not lookup, consumes
  image.reset();
  EXPECT_FALSE(token.acquire(false));
}
TEST(CpuSourceGpuView, OptionalMonitorCannotAcquireProductionPreparationAfterAdmission) {
  MonitorRenderRequest request; request.previewActive = true;
  VideoFrame frame; frame.participantId = "source";
  frame.preparedGpu = std::make_shared<CpuSourceGpuView>();
  request.frames.push_back(frame);
  ASSERT_TRUE(prepareMonitorFrames(request));
  EXPECT_FALSE(request.frames.front().preparedGpu);
  EXPECT_EQ(request.unavailableInputs.size(), 1u);
  EXPECT_TRUE(frame.preparedGpu); // only optional snapshot changed
}

#include "modules/ProgramSourceAdmission.h"
#include "modules/ProgramFramePreview.h"

TEST(ProgramSourceAdmission, HoldsExactOldImageWithoutCpuLeasesAndNeverHoldsFutureEpochOrDimensions) {
  using namespace corevideo::modules;
  ProgramSourceAdmissionPolicy policy;
  VideoFrame frame; frame.participantId = "source"; frame.sourceEpoch = 1; frame.frameId = 10;
  frame.captureTimestamp100ns = 1000; frame.width = frame.height = frame.pixelWidth = frame.pixelHeight = 64; frame.pixelStride = 256;
  frame.pixels = std::make_shared<std::vector<uint8_t>>(64 * 64 * 4);
  auto gpu = std::make_shared<GpuVideoFrame>(); gpu->width = gpu->height = 64; frame.gpuPixels = gpu;
  auto first = policy.select(frame, 1, [](const auto&) { return true; });
  EXPECT_EQ(first.evidence.state, "ready"); EXPECT_FALSE(first.image.pixels);
  auto newer = frame; newer.frameId = 11; newer.captureTimestamp100ns = 1100; newer.gpuPixels.reset();
  auto held = policy.select(newer, 2, [](const auto&) { return true; });
  EXPECT_EQ(held.evidence.state, "held"); EXPECT_EQ(held.evidence.requestedFrameId, 11);
  EXPECT_EQ(held.evidence.actualFrameId, 10); EXPECT_EQ(held.image.gpuPixels, gpu);
  EXPECT_EQ(frame.pixels.use_count(), 2); // only the original caller descriptors
  auto older = newer; older.frameId = 9; older.captureTimestamp100ns = 900;
  EXPECT_EQ(policy.select(older, 3, [](const auto&) { return true; }).evidence.state, "unavailable");
  newer.sourceEpoch = 2;
  EXPECT_EQ(policy.select(newer, 4, [](const auto&) { return true; }).evidence.state, "unavailable");
  frame.sourceEpoch = 2;
  EXPECT_EQ(policy.select(frame, 5, [](const auto&) { return true; }).evidence.state, "ready");
  newer.width = newer.pixelWidth = 128;
  EXPECT_EQ(policy.select(newer, 6, [](const auto&) { return true; }).evidence.state, "unavailable");
}

TEST(ProgramSourceAdmission, RefusesWrongGpuIdentityConsumerAndExpiresUndemandedLeases) {
  using namespace corevideo::modules;
  ProgramSourceAdmissionPolicy policy;
  VideoFrame frame; frame.participantId = "source"; frame.sourceEpoch = 1; frame.frameId = 10;
  frame.captureTimestamp100ns = 1000; frame.width = frame.height = 64;
  auto gpu = std::make_shared<GpuVideoFrame>(); gpu->width = gpu->height = 64;
  gpu->sourceId = "other"; frame.gpuPixels = gpu;
  EXPECT_EQ(policy.select(frame, 1, [](const auto&) { return true; }).evidence.reason, "gpu-identity-mismatch");
  gpu->sourceId.clear();
  EXPECT_EQ(policy.select(frame, 2, [](const auto&) { return false; }).evidence.reason, "gpu-consumer-or-read-lease-unavailable");
  EXPECT_EQ(policy.select(frame, 3, [](const auto&) { return true; }).evidence.state, "ready");
  frame.gpuPixels.reset();
  EXPECT_EQ(policy.select(frame, 304, [](const auto&) { return true; }).evidence.state, "unavailable");
  ProgramFrame program; program.cpuSourceReadyOnly = true; program.frameNumber = 12;
  program.sourceAdmissions.push_back(policy.select(frame, 305, [](const auto&) { return true; }).evidence);
  auto evidence = programSourceAdmissionJson(program);
  ASSERT_TRUE(evidence.get("sources"));
  ASSERT_TRUE(evidence.get("readyOnlyRequested"));
  EXPECT_TRUE(evidence.get("readyOnlyRequested")->asBool());
  const auto& rows = evidence.get("sources")->asArray(); ASSERT_EQ(rows.size(), 1u);
  ASSERT_TRUE(rows[0].get("actualFrameId")); EXPECT_TRUE(rows[0].get("actualFrameId")->isNull());
}

TEST(ProgramSourceAdmission, FailedOrStoppedProducerClearsHeldImage) {
  ProgramSourceAdmissionPolicy policy;
  VideoFrame frame; frame.participantId = "source"; frame.sourceEpoch = 1; frame.frameId = 1;
  frame.width = frame.height = 64;
  auto gpu = std::make_shared<GpuVideoFrame>(); gpu->width = gpu->height = 64; frame.gpuPixels = gpu;
  EXPECT_EQ(policy.select(frame, 1, [](const auto&) { return true; }).evidence.state, "ready");
  frame.gpuPixels.reset();
  auto token = std::make_shared<CpuSourceGpuView>();
  token->sourceId = frame.participantId; token->sourceEpoch = frame.sourceEpoch; token->frameId = frame.frameId;
  token->width = token->height = 64; token->demand = std::make_shared<CpuSourceGpuDemand>();
  token->demand->failed.store(true); frame.preparedGpu = token;
  auto failed = policy.select(frame, 2, [](const auto&) { return true; });
  EXPECT_EQ(failed.evidence.reason, "preparation-failed"); EXPECT_FALSE(failed.image.gpuPixels);
  frame.preparedGpu.reset();
  EXPECT_EQ(policy.select(frame, 3, [](const auto&) { return true; }).evidence.state, "unavailable");
  frame.gpuPixels = gpu;
  EXPECT_EQ(policy.select(frame, 4, [](const auto&) { return true; }).evidence.state, "ready");
  frame.gpuPixels.reset(); token->demand->failed.store(false); token->demand->stopped.store(true); frame.preparedGpu = token;
  EXPECT_EQ(policy.select(frame, 5, [](const auto&) { return true; }).evidence.reason, "preparation-stopped");
}

#if defined(_WIN32) && COREVIDEO_WITH_D3D11 && !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS
#define NOMINMAX
#include <windows.h>
#include "modules/D3DI420VideoFrame.h"
namespace {
struct PreparationFlags {
  std::string cpu, monitor;
  PreparationFlags() {
    const char* value = std::getenv("COREVIDEO_CPU_SOURCE_PREPARATION"); cpu = value ? value : "";
    value = std::getenv("COREVIDEO_ISOLATE_MONITORS"); monitor = value ? value : "";
    _putenv_s("COREVIDEO_CPU_SOURCE_PREPARATION", "1"); _putenv_s("COREVIDEO_ISOLATE_MONITORS", "0");
  }
  ~PreparationFlags() { _putenv_s("COREVIDEO_CPU_SOURCE_PREPARATION", cpu.c_str()); _putenv_s("COREVIDEO_ISOLATE_MONITORS", monitor.c_str()); }
};
bool await(const std::function<bool()>& check) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  do { if (check()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
  while (std::chrono::steady_clock::now() < deadline);
  return false;
}
VideoFrame sourceFrame(const std::string& id, uint64_t epoch, int64_t identity, uint8_t luma = 128) {
  VideoFrame frame; frame.participantId = id; frame.sourceEpoch = epoch; frame.frameId = identity;
  frame.captureTimestamp100ns = identity * 1000;
  frame.width = frame.height = frame.naturalWidth = frame.naturalHeight = 64;
  frame.i420Width = frame.i420Height = 64;
  auto cpu = std::make_shared<std::vector<uint8_t>>(64 * 64 * 3 / 2, 128);
  std::fill(cpu->begin(), cpu->begin() + 64 * 64, luma); frame.i420 = cpu;
  return frame;
}
CompositorRenderPlan planFor(const std::string& id) {
  CompositorRenderPlan plan; plan.width = plan.height = 64;
  CompositorRenderPlanLayer layer; layer.kind = "participant-video";
  layer.sourceId = layer.participantId = id; layer.rect = {0, 0, 1, 1}; layer.borderStyle = "none";
  plan.layers.push_back(layer); return plan;
}
void offer(I420SourcePreparation& owner, VideoFrame& frame) {
  frame.preparedGpu = owner.offer(frame.participantId, frame.sourceEpoch, frame.frameId,
      frame.captureTimestamp100ns, frame.i420Width, frame.i420Height, frame.i420);
}
}

TEST(I420SourcePreparation, IndependentDeviceMatchesCpuColorRangeMatricesAndGradeWithZeroSourceUploads) {
  PreparationFlags flags;
  I420SourcePreparation owner(true);
  for (int mode = 0; mode < 8; ++mode) {
    _putenv_s("COREVIDEO_CPU_SOURCE_PREPARATION", "0");
    auto reference = createD3D11Compositor();
    _putenv_s("COREVIDEO_CPU_SOURCE_PREPARATION", "1");
    auto prepared = createD3D11Compositor();
    ASSERT_TRUE(reference && prepared);
    auto frame = sourceFrame("source", mode + 1, 20);
    auto pattern = std::make_shared<std::vector<uint8_t>>(*frame.i420);
    for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x)
      (*pattern)[y * 64 + x] = static_cast<uint8_t>(x * 4 + y * 7);
    for (size_t p = 4096; p < pattern->size(); ++p) (*pattern)[p] = static_cast<uint8_t>(p * 17);
    frame.i420 = pattern; frame.i420FullRange = (mode & 1) != 0; frame.i420Bt601 = (mode & 2) != 0;
    auto plan = planFor("source");
    if (mode & 4) { plan.layers[0].hasColorGrade = true; plan.layers[0].colorGrade = {.3f, .2f, .1f, .15f}; }
    auto expected = reference->render(plan, {frame});
    const auto originalCpu = frame.i420;
    offer(owner, frame); ASSERT_TRUE(frame.preparedGpu);
    ASSERT_TRUE(await([&] { return bool(frame.preparedGpu->acquire(true)); }));
    auto result = prepared->render(plan, {frame});
    ASSERT_EQ(result.preview.bgra.size(), expected.preview.bgra.size());
    ASSERT_FALSE(result.preview.bgra.empty());
    size_t mismatches = 0;
    for (size_t p = 0; p < result.preview.bgra.size(); ++p)
      if (std::abs(int(result.preview.bgra[p]) - int(expected.preview.bgra[p])) > 1) ++mismatches;
    EXPECT_EQ(mismatches, 0u);
    EXPECT_EQ(prepared->sourceTexStats().cachedUploads, 0u);
    EXPECT_EQ(prepared->sourceTexStats().scratchUploads, 0u);
    EXPECT_EQ(prepared->sourceTexStats().textureCreates, 0u);
    EXPECT_EQ(frame.i420, originalCpu);
    EXPECT_EQ(frame.frameId, 20); EXPECT_EQ(frame.sourceEpoch, mode + 1);
    EXPECT_EQ(frame.captureTimestamp100ns, 20000);
  }
}

TEST(I420SourcePreparation, CpuAndIsoReferencesDoNotPinThreeGpuSlotsOrResurrectReusedImages) {
  PreparationFlags flags; auto compositor = createD3D11Compositor(); ASSERT_TRUE(compositor);
  I420SourcePreparation owner(true);
  std::vector<VideoFrame> cpuHeld;
  std::vector<std::shared_ptr<const GpuVideoFrame>> gpuHeld;
  for (int i = 1; i <= 3; ++i) {
    cpuHeld.push_back(sourceFrame("source", 9, i, i * 30));
    offer(owner, cpuHeld.back());
    ASSERT_TRUE(await([&] {
      auto image = cpuHeld.back().preparedGpu->acquire(true);
      if (image) { cpuHeld.back().preparedGpu->consumed.store(true); gpuHeld.push_back(image); }
      return bool(image);
    }));
  }
  auto fourth = sourceFrame("source", 9, 4, 180); offer(owner, fourth);
  fourth.preparedGpu->acquire(true);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_FALSE(fourth.preparedGpu->acquire(false));
  gpuHeld.erase(gpuHeld.begin()); // CPU/ISO frame1 and token still remain
  ASSERT_TRUE(await([&] { return bool(fourth.preparedGpu->acquire(false)); }));
  EXPECT_FALSE(cpuHeld.front().preparedGpu->acquire(false));
  EXPECT_EQ(cpuHeld.front().i420->front(), 30);
  auto image = fourth.preparedGpu->acquire(false); ASSERT_TRUE(image);
  EXPECT_EQ(image->sourceFrameId, 4); EXPECT_EQ(image->sourceEpoch, 9u);
  EXPECT_LE(D3DVideoImage::residentBytes.load(), 512u * 1024u * 1024u);
}

TEST(I420SourcePreparation, DelayedSelectedSourceResourcesLeaveHealthyGpuPixelsAdvancing) {
  PreparationFlags flags; auto compositor = createD3D11Compositor(); ASSERT_TRUE(compositor);
  std::atomic<bool> faultStarted{false}, faultFinished{false}, releaseFault{false};
  I420SourcePreparation owner(true, [&](const std::string& id) {
    if (id == "bad") {
      faultStarted.store(true);
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (!releaseFault.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      faultFinished.store(true);
    }
  });
  struct ReleaseFaultOnExit {
    std::atomic<bool>& release;
    ~ReleaseFaultOnExit() { release.store(true); }
  } releaseOnExit{releaseFault}; // release before owner joins, including assertion failure
  auto good = sourceFrame("good", 1, 1, 50); offer(owner, good);
  ASSERT_TRUE(await([&] { return bool(good.preparedGpu->acquire(true)); }));
  auto plan = planFor("good");
  auto warm = compositor->render(plan, {good}); ASSERT_FALSE(warm.preview.bgra.empty());
  auto bad = sourceFrame("bad", 2, 1, 200); offer(owner, bad); bad.preparedGpu->acquire(true);
  ASSERT_TRUE(await([&] { return faultStarted.load(); }));
  plan.layers[0].rect = {.5f, 0, .5f, 1};
  auto layer = plan.layers[0]; layer.sourceId = layer.participantId = "bad"; layer.rect = {0, 0, .5f, 1};
  plan.layers.push_back(layer);
  for (int i = 2; i <= 5; ++i) {
    good = sourceFrame("good", 1, i, i * 30); offer(owner, good);
    ASSERT_TRUE(await([&] { return bool(good.preparedGpu->acquire(true)); }));
    auto result = compositor->render(plan, {good, bad});
    ASSERT_FALSE(result.preview.bgra.empty());
    EXPECT_NEAR(result.preview.bgra[(32 * 64 + 48) * 4], i * 30, 1);
  }
  EXPECT_FALSE(faultFinished.load());
  EXPECT_EQ(owner.stats().failed, 0u);
}

TEST(I420SourcePreparation, ReconnectFencesSameFrameIdAndRetiresOnlyAfterExternalGpuLeaseReleases) {
  PreparationFlags flags; auto compositor = createD3D11Compositor(); ASSERT_TRUE(compositor);
  I420SourcePreparation owner(true);
  auto old = sourceFrame("source", 4, 20, 50); offer(owner, old);
  std::shared_ptr<const GpuVideoFrame> lease;
  ASSERT_TRUE(await([&] { lease = old.preparedGpu->acquire(true); return bool(lease); }));
  auto fresh = sourceFrame("source", 5, 20, 190); offer(owner, fresh);
  EXPECT_FALSE(old.preparedGpu->acquire(false));
  ASSERT_TRUE(await([&] { return bool(fresh.preparedGpu->acquire(true)); }));
  EXPECT_EQ(lease->sourceEpoch, 4u); EXPECT_EQ(lease->sourceFrameId, 20);
  EXPECT_EQ(old.i420->front(), 50); EXPECT_EQ(fresh.i420->front(), 190);
  EXPECT_EQ(owner.stats().active, 2u);
  lease.reset();
  ASSERT_TRUE(await([&] { return owner.stats().active == 1; }));
  EXPECT_EQ(old.i420->front(), 50); // retained CPU/ISO does not defer GPU retirement
  EXPECT_FALSE(owner.offer("source", 4, 21, 21000, 64, 64, old.i420));
}

TEST(I420SourcePreparation, InvalidDuplicateAndBoundedPendingOffersPreserveCpuFrames) {
  I420SourcePreparation disabled(false), owner(true);
  auto frame = sourceFrame("source", 1, 1);
  EXPECT_FALSE(disabled.offer("source", 1, 1, 1000, 64, 64, frame.i420));
  EXPECT_FALSE(owner.offer("source", 0, 1, 1000, 64, 64, frame.i420));
  EXPECT_FALSE(owner.offer("source", 1, -1, 1000, 64, 64, frame.i420));
  EXPECT_FALSE(owner.offer("source", 1, 1, 1000, 63, 64, frame.i420));
  auto shortCpu = std::make_shared<const std::vector<uint8_t>>(1, 128);
  EXPECT_FALSE(owner.offer("source", 1, 1, 1000, 64, 64, shortCpu));
  std::vector<std::shared_ptr<CpuSourceGpuView>> held;
  for (size_t id = 1; id <= I420SourcePreparation::kPendingPerSource; ++id) {
    auto token = owner.offer("source", 1, id, id * 1000, 64, 64, frame.i420);
    ASSERT_TRUE(token); held.push_back(token);
  }
  EXPECT_FALSE(owner.offer("source", 1, 1, 1000, 64, 64, frame.i420));
  EXPECT_FALSE(owner.offer("source", 1, 29, 29000, 32, 64, frame.i420));
  EXPECT_FALSE(owner.offer("source", 1, 29, 29000, 64, 64, frame.i420));
  EXPECT_EQ(owner.stats().active, 0u); // no Program demand, no GPU allocation
  EXPECT_EQ(owner.stats().refused, 7u);
  EXPECT_EQ(frame.i420->front(), 128);
  held.clear();
  EXPECT_TRUE(owner.offer("source", 1, 29, 29000, 64, 64, frame.i420));
}

TEST(I420SourcePreparation, FailedResourceCreationIsUnavailableWithoutBlockingHealthySource) {
  PreparationFlags flags; auto compositor = createD3D11Compositor(); ASSERT_TRUE(compositor);
  I420SourcePreparation owner(true, [](const std::string& id) {
    if (id == "bad") throw std::runtime_error("owned resource failure");
  });
  auto bad = sourceFrame("bad", 1, 1, 90); offer(owner, bad);
  bad.preparedGpu->acquire(true);
  ASSERT_TRUE(await([&] { return owner.stats().failed == 1; }));
  EXPECT_FALSE(bad.preparedGpu->acquire(false));
  auto fallback = compositor->render(planFor("bad"), {bad});
  ASSERT_FALSE(fallback.preview.bgra.empty());
  EXPECT_EQ(fallback.health, "degraded");
  ASSERT_EQ(fallback.sourceAdmissions.size(), 1u);
  EXPECT_EQ(fallback.sourceAdmissions.front().state, "unavailable");
  EXPECT_EQ(fallback.sourceAdmissions.front().reason, "preparation-failed");
  EXPECT_EQ(compositor->sourceTexStats().cachedUploads, 0u);
  EXPECT_EQ(compositor->sourceTexStats().scratchUploads, 0u);
  EXPECT_EQ(compositor->sourceTexStats().textureCreates, 0u);
  auto good = sourceFrame("good", 1, 1, 180); offer(owner, good);
  ASSERT_TRUE(await([&] { return bool(good.preparedGpu->acquire(true)); }));
  auto ready = compositor->render(planFor("good"), {good});
  ASSERT_FALSE(ready.preview.bgra.empty());
  EXPECT_NEAR(ready.preview.bgra.front(), 180, 1);
  EXPECT_EQ(owner.stats().active, 1u);
  EXPECT_EQ(owner.stats().failed, 1u);
}

TEST(I420SourcePreparation, StrictProgramHoldsActualPixelsAcrossInlinePreviewAndRejectsReconnect) {
  PreparationFlags flags;
  auto compositor = createD3D11Compositor(); ASSERT_TRUE(compositor);
  I420SourcePreparation owner(true);
  auto first = sourceFrame("source", 1, 1, 70);
  first.i420FullRange = false; first.i420Bt601 = true;
  offer(owner, first);
  ASSERT_TRUE(await([&] { return bool(first.preparedGpu->acquire(true)); }));
  const auto baseline = compositor->render(planFor("source"), {first});
  ASSERT_EQ(baseline.sourceAdmissions.size(), 1u);
  EXPECT_EQ(baseline.sourceAdmissions.front().state, "ready");
  auto next = sourceFrame("source", 1, 2, 180);
  const auto originalCpu = next.i420;
  (void)compositor->renderPreview(planFor("source"), {next});
  const auto before = compositor->sourceTexStats();
  const auto held = compositor->render(planFor("source"), {next});
  const auto after = compositor->sourceTexStats();
  EXPECT_EQ(held.preview.bgra, baseline.preview.bgra);
  ASSERT_EQ(held.sourceAdmissions.size(), 1u);
  EXPECT_EQ(held.sourceAdmissions.front().state, "held");
  EXPECT_EQ(held.sourceAdmissions.front().actualFrameId, 1);
  EXPECT_EQ(held.sourceAdmissions.front().requestedFrameId, 2);
  EXPECT_EQ(held.health, "degraded");
  EXPECT_EQ(after.cachedUploads, before.cachedUploads);
  EXPECT_EQ(after.scratchUploads, before.scratchUploads);
  EXPECT_EQ(after.textureCreates, before.textureCreates);
  EXPECT_EQ(next.i420, originalCpu);
  auto reconnect = sourceFrame("source", 2, 1, 210);
  const auto missing = compositor->render(planFor("source"), {reconnect});
  ASSERT_EQ(missing.sourceAdmissions.size(), 1u);
  EXPECT_EQ(missing.sourceAdmissions.front().state, "unavailable");
  EXPECT_EQ(missing.sourceAdmissions.front().actualFrameId, -1);
  EXPECT_EQ(compositor->sourceTexStats().cachedUploads, after.cachedUploads);
  offer(owner, reconnect);
  ASSERT_TRUE(await([&] { return bool(reconnect.preparedGpu->acquire(true)); }));
  const auto recovered = compositor->render(planFor("source"), {reconnect});
  ASSERT_EQ(recovered.sourceAdmissions.size(), 1u);
  EXPECT_EQ(recovered.sourceAdmissions.front().state, "ready");
  EXPECT_EQ(recovered.sourceAdmissions.front().actualEpoch, 2u);
  EXPECT_EQ(recovered.sourceAdmissions.front().actualFrameId, 1);
  ASSERT_FALSE(recovered.preview.bgra.empty());
  EXPECT_NEAR(recovered.preview.bgra.front(), 210, 1);
}
#endif
