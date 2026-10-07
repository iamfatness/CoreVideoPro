#include "modules/I420SourcePreparation.h"
#include "modules/I420CaptureArrival.h"
#include "modules/CpuVideoArrival.h"
#include "modules/Interfaces.h"
#include "modules/MonitorFrameAdmission.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cstdlib>
#include <thread>
#include <stdexcept>
#include <future>

using namespace corevideo::modules;
TEST(I420CaptureArrival, HeldSnapshotsPreserveCpuIdentityAndReconnectOrResizeChangesEpoch) {
  I420CaptureArrival first("capture:camera", {});
  auto cpu = std::make_shared<std::vector<uint8_t>>(64 * 64 * 3 / 2, 128);
  auto arrival = first.publish(cpu, 64, 64, true, true);
  EXPECT_EQ(arrival.i420, cpu); EXPECT_EQ(arrival.frameId, 1);
  EXPECT_TRUE(arrival.sourceEpoch > 0); EXPECT_TRUE(arrival.captureTimestamp100ns > 0);
  EXPECT_TRUE(arrival.i420FullRange); EXPECT_TRUE(arrival.i420Bt601);
  EXPECT_FALSE(arrival.preparedGpu);
  auto held = arrival;
  auto next = first.publish(cpu, 64, 64, false, false);
  EXPECT_EQ(next.frameId, 2); EXPECT_EQ(next.sourceEpoch, arrival.sourceEpoch);
  EXPECT_EQ(held.frameId, 1); EXPECT_EQ(held.captureTimestamp100ns, arrival.captureTimestamp100ns);
  auto resizedCpu = std::make_shared<std::vector<uint8_t>>(128 * 64 * 3 / 2, 128);
  auto resized = first.publish(resizedCpu, 128, 64, false, true);
  EXPECT_TRUE(resized.sourceEpoch > arrival.sourceEpoch); EXPECT_EQ(resized.frameId, 3);
  I420CaptureArrival reconnected("capture:camera", {});
  auto fresh = reconnected.publish(cpu, 64, 64, false, false);
  EXPECT_TRUE(fresh.sourceEpoch > resized.sourceEpoch); EXPECT_EQ(fresh.frameId, 1);
  EXPECT_EQ(arrival.i420->front(), 128);
}

TEST(I420CaptureArrival, InvalidArrivalsDoNotConsumeIdentityOrPublishPartialPlanes) {
  I420CaptureArrival arrival("capture:camera", {});
  auto cpu = std::make_shared<std::vector<uint8_t>>(64 * 64 * 3 / 2, 128);
  EXPECT_FALSE(arrival.publish({}, 64, 64, false, false).hasContent());
  EXPECT_FALSE(arrival.publish(cpu, 63, 64, false, false).hasContent());
  EXPECT_FALSE(arrival.publish(cpu, 128, 64, false, false).hasContent());
  EXPECT_FALSE(arrival.publish(cpu, 8000, 64, false, false).hasContent());
  EXPECT_EQ(arrival.publish(cpu, 64, 64, false, false).frameId, 1);
}

TEST(CpuVideoArrival, HeldPollingPreservesPlaybackTimeCpuBytesAndObservationIdentity) {
  using namespace corevideo::modules;
  auto owner = std::make_shared<CpuSourcePreparation>(false);
  CpuVideoArrival tap("media:clip", owner);
  VideoFrame first; first.frameId = 12; first.timestampMs = 750;
  first.width = first.pixelWidth = 3; first.height = first.pixelHeight = 2; first.pixelStride = 16;
  first.pixels = std::make_shared<const std::vector<uint8_t>>(32, 90);
  tap.prepare(first);
  const auto original = first.pixels;
  auto held = first; held.timestampMs = 900; tap.prepare(held);
  EXPECT_EQ(held.pixels, original); EXPECT_EQ(held.timestampMs, 900);
  EXPECT_EQ(held.sourceEpoch, first.sourceEpoch); EXPECT_EQ(held.captureTimestamp100ns, first.captureTimestamp100ns);
  auto rewind = first; rewind.frameId = 1; tap.prepare(rewind);
  EXPECT_GT(rewind.sourceEpoch, first.sourceEpoch);
  auto newPixelsSameId = rewind; newPixelsSameId.pixels = std::make_shared<const std::vector<uint8_t>>(32, 190);
  tap.prepare(newPixelsSameId); EXPECT_GT(newPixelsSameId.sourceEpoch, rewind.sourceEpoch);
  CpuVideoArrival disabled("media:clip", {});
  auto legacy = first; legacy.sourceEpoch = 0; legacy.captureTimestamp100ns = 0;
  disabled.prepare(legacy); EXPECT_EQ(legacy.sourceEpoch, 0u); EXPECT_EQ(legacy.captureTimestamp100ns, 0);
}

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

TEST(ProgramSourceAdmission, AbandonedCpuSelectionCannotBlockALaterCompletion) {
  ProgramSourceAdmissionPolicy policy;
  auto frameFor = [](int64_t id) {
    VideoFrame frame; frame.participantId = "source"; frame.sourceEpoch = 1; frame.frameId = id;
    frame.captureTimestamp100ns = id * 100; frame.width = frame.height = 64;
    auto token = std::make_shared<CpuSourceGpuView>(); token->sourceId = frame.participantId;
    token->sourceEpoch = 1; token->frameId = id; token->captureTimestamp100ns = frame.captureTimestamp100ns;
    token->width = token->height = 64; token->demand = std::make_shared<CpuSourceGpuDemand>(); frame.preparedGpu = token;
    return frame;
  };
  auto abandoned = frameFor(1); policy.select(abandoned, 1, [](const auto&) { return true; });
  abandoned.preparedGpu->superseded.store(true);
  auto pending = frameFor(2); policy.select(pending, 2, [](const auto&) { return true; });
  auto gpu = std::make_shared<GpuVideoFrame>(); gpu->sourceId = "source"; gpu->sourceEpoch = 1;
  gpu->sourceFrameId = 2; gpu->sourceCaptureTimestamp100ns = 200; gpu->width = gpu->height = 64;
  pending.preparedGpu->ready.store(gpu); pending.preparedGpu->completionPublished.store(true);
  auto result = policy.select(frameFor(3), 3, [](const auto&) { return true; });
  EXPECT_EQ(result.evidence.state, "held"); EXPECT_EQ(result.evidence.actualFrameId, 2);
  EXPECT_EQ(result.evidence.requestedFrameId, 3);
}

#if defined(_WIN32) && COREVIDEO_WITH_D3D11 && !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS
#define NOMINMAX
#include <windows.h>
#include "modules/D3DI420VideoFrame.h"
#include "modules/StillMediaFrameCache.h"
#include "modules/BrowserSourceHostAdapter.h"
#include "core/MediaTransports.h"
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

TEST(I420SourcePreparation, FutureDecodeArrivalsCannotEvictTheNextCpuPlayoutSelection) {
  PreparationFlags flags; auto compositor = createD3D11Compositor(); ASSERT_TRUE(compositor);
  I420SourcePreparation owner(true);
  std::vector<VideoFrame> playout;
  for (int id = 1; id <= 6; ++id) playout.push_back(sourceFrame("delayed", 1, id, id * 30));
  offer(owner, playout[0]);
  ASSERT_TRUE(await([&] { return bool(playout[0].preparedGpu->acquire(true)); }));
  auto first = compositor->render(planFor("delayed"), {playout[0]});
  ASSERT_EQ(first.sourceAdmissions.front().actualFrameId, 1);
  // Decode arrives before CPU playout/trim. Retain original CPU descriptors,
  // with more arrivals than GPU slots; do not select any of them yet.
  for (size_t index = 1; index < playout.size(); ++index) offer(owner, playout[index]);
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  ProgramFrame selected;
  ASSERT_TRUE(await([&] {
    selected = compositor->render(planFor("delayed"), {playout[1]});
    return !selected.sourceAdmissions.empty() && selected.sourceAdmissions.front().actualFrameId == 2;
  })) << "Future preparation recycled frame 2 before CPU playout selected it";
  ASSERT_FALSE(selected.preview.bgra.empty());
  EXPECT_NEAR(selected.preview.bgra.front(), 60, 1);
  EXPECT_EQ(playout[1].i420->front(), 60);
  EXPECT_EQ(compositor->sourceTexStats().cachedUploads, 0u);
  EXPECT_EQ(compositor->sourceTexStats().scratchUploads, 0u);
}

TEST(CpuSourcePreparation, BgraStrideAlphaAndDelayedSelectionMatchIndependentCpuPixels) {
  PreparationFlags flags;
  _putenv_s("COREVIDEO_CPU_SOURCE_PREPARATION", "0");
  auto reference = createD3D11Compositor();
  _putenv_s("COREVIDEO_CPU_SOURCE_PREPARATION", "1");
  auto compositor = createD3D11Compositor(); ASSERT_TRUE(reference); ASSERT_TRUE(compositor);
  auto owner = std::make_shared<CpuSourcePreparation>(true);
  CpuVideoArrival arrival("media:clip", owner);
  VideoFrame cpu; cpu.participantId = "media:clip"; cpu.frameId = 1;
  cpu.width = cpu.pixelWidth = cpu.naturalWidth = 63;
  cpu.height = cpu.pixelHeight = cpu.naturalHeight = 65; cpu.pixelStride = 63 * 4 + 8;
  auto bytes = std::make_shared<std::vector<uint8_t>>(cpu.pixelStride * cpu.pixelHeight, 231);
  for (int y = 0; y < cpu.pixelHeight; ++y) for (int x = 0; x < cpu.pixelWidth; ++x) {
    const auto p = y * cpu.pixelStride + x * 4;
    (*bytes)[p] = 30; (*bytes)[p + 1] = 100; (*bytes)[p + 2] = 190; (*bytes)[p + 3] = 127;
  }
  cpu.pixels = bytes;
  auto expected = reference->render(planFor("media:clip"), {cpu});
  arrival.prepare(cpu); ASSERT_TRUE(cpu.preparedGpu);
  ASSERT_TRUE(await([&] { return bool(cpu.preparedGpu->acquire(true)); }));
  auto actual = compositor->render(planFor("media:clip"), {cpu});
  EXPECT_EQ(actual.preview.bgra, expected.preview.bgra);
  EXPECT_EQ(actual.sourceAdmissions.front().state, "ready");
  EXPECT_EQ(compositor->sourceTexStats().cachedUploads, 0u);
  EXPECT_EQ(compositor->sourceTexStats().scratchUploads, 0u);
  EXPECT_EQ(cpu.pixels, bytes); EXPECT_EQ(cpu.pixels->back(), 231);
  std::vector<VideoFrame> queued;
  for (int id = 2; id <= 7; ++id) {
    auto frame = cpu; frame.frameId = id;
    auto next = std::make_shared<std::vector<uint8_t>>(*bytes); (*next)[0] = static_cast<uint8_t>(id * 20);
    frame.pixels = next; arrival.prepare(frame); queued.push_back(std::move(frame));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  ASSERT_TRUE(await([&] {
    actual = compositor->render(planFor("media:clip"), {queued.front()});
    return actual.sourceAdmissions.front().actualFrameId == 2;
  }));
  expected = reference->render(planFor("media:clip"), {queued.front()});
  EXPECT_EQ(actual.preview.bgra, expected.preview.bgra);
  EXPECT_EQ(queued.front().pixels->back(), 231);
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

TEST(I420CaptureArrival, NativeCaptureTapPreparesRealPixelsAndFencesHeldSnapshotsOnReconnect) {
  PreparationFlags flags;
  _putenv_s("COREVIDEO_CPU_SOURCE_PREPARATION", "0");
  auto reference = createD3D11Compositor();
  _putenv_s("COREVIDEO_CPU_SOURCE_PREPARATION", "1");
  auto prepared = createD3D11Compositor(); ASSERT_TRUE(reference && prepared);
  auto owner = std::make_shared<I420SourcePreparation>(true);
  I420CaptureArrival arrival("capture:camera", owner);
  auto plan = planFor("capture:camera");
  VideoFrame held;
  for (int index = 0; index < 3; ++index) {
    auto cpu = std::make_shared<std::vector<uint8_t>>(64 * 64 * 3 / 2, 128);
    std::fill(cpu->begin(), cpu->begin() + 4096, 40 + index * 60);
    auto frame = arrival.publish(cpu, 64, 64, index == 1, index == 2);
    ASSERT_TRUE(frame.preparedGpu);
    ASSERT_TRUE(await([&] { return bool(frame.preparedGpu->acquire(true)); }));
    auto cpuOnly = frame; cpuOnly.preparedGpu.reset();
    auto expected = reference->render(plan, {cpuOnly});
    auto result = prepared->render(plan, {frame});
    ASSERT_EQ(result.preview.bgra.size(), expected.preview.bgra.size());
    ASSERT_FALSE(result.preview.bgra.empty());
    size_t mismatches = 0;
    for (size_t pixel = 0; pixel < result.preview.bgra.size(); ++pixel)
      if (std::abs(int(result.preview.bgra[pixel]) - int(expected.preview.bgra[pixel])) > 1) ++mismatches;
    EXPECT_EQ(mismatches, 0u); EXPECT_EQ(frame.i420, cpu);
    held = frame;
  }
  I420CaptureArrival reconnect("capture:camera", owner);
  auto fresh = reconnect.publish(held.i420, 64, 64, held.i420FullRange, held.i420Bt601);
  ASSERT_TRUE(fresh.preparedGpu); EXPECT_EQ(fresh.frameId, 1);
  EXPECT_TRUE(fresh.sourceEpoch > held.sourceEpoch);
  EXPECT_FALSE(held.preparedGpu->acquire(false));
  ASSERT_TRUE(await([&] { return bool(fresh.preparedGpu->acquire(true)); }));
  auto result = prepared->render(plan, {fresh}); ASSERT_FALSE(result.preview.bgra.empty());
  EXPECT_EQ(prepared->sourceTexStats().cachedUploads, 0u);
  EXPECT_EQ(prepared->sourceTexStats().scratchUploads, 0u);
  EXPECT_EQ(prepared->sourceTexStats().textureCreates, 0u);
}

TEST(I420SourcePreparation, SubmittedCompletionSurvivesNewerSelectionAndUnsubmittedTokenIsAbandoned) {
  PreparationFlags flags; auto compositor = createD3D11Compositor(); ASSERT_TRUE(compositor);
  std::atomic<bool> uploaded{false}, release{false}, releaseCurrent{false};
  I420SourcePreparation owner(true, {}, [&](const std::string&) {
    const bool later = uploaded.exchange(true);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!(later ? releaseCurrent.load() : release.load()) && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  });
  auto first = sourceFrame("camera", 1, 1, 40); offer(owner, first);
  ProgramSourceAdmissionPolicy policy;
  auto canRead = [](const auto& image) { return bool(image); };
  policy.select(first, 1, canRead);
  ASSERT_TRUE(await([&] { return uploaded.load(); }));
  auto abandoned = sourceFrame("camera", 1, 2, 100); offer(owner, abandoned);
  auto current = sourceFrame("camera", 1, 3, 200); offer(owner, current);
  policy.select(current, 2, canRead); // updates selection while frame 1 is in flight
  release.store(true);
  ASSERT_TRUE(await([&] { return first.preparedGpu->completionPublished.load(); }));
  ASSERT_TRUE(first.preparedGpu->acquire(false));
  auto held = policy.select(current, 3, canRead);
  EXPECT_EQ(held.evidence.actualFrameId, 1); EXPECT_EQ(held.evidence.state, "held");
  EXPECT_EQ(held.evidence.reason, "previous-selection-completed");
  auto pixels = compositor->render(planFor("camera"), {held.image});
  ASSERT_FALSE(pixels.preview.bgra.empty());
  EXPECT_NEAR(pixels.preview.bgra[(32 * 64 + 32) * 4], 40, 1);
  ASSERT_TRUE(await([&] { return abandoned.preparedGpu->superseded.load(); }));
  EXPECT_FALSE(abandoned.preparedGpu->completionPublished.load());
  releaseCurrent.store(true);
  ASSERT_TRUE(await([&] { return bool(current.preparedGpu->acquire(true)); }));
  EXPECT_EQ(current.i420->front(), 200); EXPECT_EQ(first.i420->front(), 40);
  EXPECT_EQ(compositor->sourceTexStats().cachedUploads, 0u);
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
  // A pinned retired generation must prevent a third allocation for this source.
  auto third = sourceFrame("source", 6, 20, 220); offer(owner, third);
  EXPECT_FALSE(third.preparedGpu);
  EXPECT_EQ(owner.stats().active, 2u);
  EXPECT_TRUE(fresh.preparedGpu->acquire(false));
  lease.reset();
  ASSERT_TRUE(await([&] { return owner.stats().active == 1; }));
  EXPECT_EQ(old.i420->front(), 50); // retained CPU/ISO does not defer GPU retirement
  EXPECT_FALSE(owner.offer("source", 4, 21, 21000, 64, 64, old.i420));
  offer(owner, third); ASSERT_TRUE(third.preparedGpu);
  ASSERT_TRUE(await([&] { return bool(third.preparedGpu->acquire(true)); }));
  EXPECT_EQ(third.i420->front(), 220);
}

namespace {
class PreparedStillDecoder final : public IStillImageDecoder {
 public:
  std::atomic<uint64_t> version{1};
  std::optional<uint64_t> fileVersion(const std::string&) override { return version.load(); }
  bool decode(const std::string&, StillImagePixels& out, std::string&) override {
    out.width = out.height = 64;
    auto bytes = std::make_shared<std::vector<uint8_t>>(64 * 64 * 4, 0);
    for (size_t index = 0; index < bytes->size(); index += 4) {
      (*bytes)[index] = 40; (*bytes)[index + 1] = 90;
      (*bytes)[index + 2] = 150; (*bytes)[index + 3] = 127;
    }
    out.bgra = bytes; return true;
  }
};
class PreparedMediaDecoder final : public IMediaDecoder {
 public:
  explicit PreparedMediaDecoder(std::shared_ptr<const std::vector<uint8_t>> bytes) : bytes_(std::move(bytes)) {}
  std::vector<VideoFrame> pollMediaFrames(const MediaDecodeRequest& request, int64_t) override {
    VideoFrame frame; frame.participantId = request.sourceId;
    frame.width = frame.height = frame.pixelWidth = frame.pixelHeight = 64;
    frame.pixelStride = 256; frame.frameId = 17; frame.timestampMs = 1234;
    frame.pixels = bytes_; return {frame};
  }
 private:
  std::shared_ptr<const std::vector<uint8_t>> bytes_;
};
}

TEST(CpuSourcePreparation, StillWorkerPreparesAliasesAndFencesChangedFileWithoutChangingCpuPixels) {
  PreparationFlags flags; auto compositor = createD3D11Compositor(); ASSERT_TRUE(compositor);
  auto owner = std::make_shared<CpuSourcePreparation>(true);
  auto decoder = std::make_unique<PreparedStillDecoder>(); auto* fake = decoder.get();
  StillMediaFrameCache cache(std::move(decoder), StillMediaFrameCache::kDefaultCacheBudgetBytes, owner);
  cache.setDesired({{"media:logo", "logo.png"}, {"media:alias", "logo.png"}});
  ASSERT_TRUE(cache.waitForIdle(5000));
  auto frames = cache.collectFrames(100); ASSERT_EQ(frames.size(), 2u);
  EXPECT_EQ(frames[0].pixels, frames[1].pixels);
  for (auto& frame : frames) {
    ASSERT_TRUE(frame.preparedGpu);
    ASSERT_TRUE(await([&] { return bool(frame.preparedGpu->acquire(true)); }));
    auto result = compositor->render(planFor(frame.participantId), {frame});
    ASSERT_FALSE(result.preview.bgra.empty());
    EXPECT_EQ(result.sourceAdmissions.front().state, "ready");
    EXPECT_EQ(frame.pixels->at(3), 127);
  }
  EXPECT_NE(frames[0].preparedGpu->sourceId, frames[1].preparedGpu->sourceId);
  EXPECT_EQ(compositor->sourceTexStats().cachedUploads, 0u);
  EXPECT_EQ(compositor->sourceTexStats().scratchUploads, 0u);
  auto held = cache.collectFrames(200);
  EXPECT_EQ(held[0].pixels, frames[0].pixels);
  EXPECT_EQ(held[0].captureTimestamp100ns, frames[0].captureTimestamp100ns);
  EXPECT_EQ(held[0].timestampMs, 200);
  fake->version.store(2);
  cache.setDesired({{"media:logo", "logo.png"}, {"media:alias", "logo.png"}});
  ASSERT_TRUE(cache.waitForIdle(5000));
  auto updated = cache.collectFrames(300); ASSERT_EQ(updated.size(), 2u);
  EXPECT_GT(updated[0].sourceEpoch, frames[0].sourceEpoch);
  EXPECT_NE(updated[0].pixels, frames[0].pixels);
  EXPECT_EQ(frames[0].pixels->at(3), 127);
  ASSERT_TRUE(updated[0].preparedGpu);
  ASSERT_TRUE(await([&] { return bool(updated[0].preparedGpu->acquire(true)); }));
}

TEST(CpuSourcePreparation, MissingGpuConsumerFailsFutureArrivalsWithAttributableTokens) {
  CpuSourcePreparation owner(true);
  auto first = sourceFrame("source", 1, 1, 80); offer(owner, first);
  ASSERT_TRUE(first.preparedGpu); first.preparedGpu->acquire(true);
  ASSERT_TRUE(await([&] { return !owner.stats().supported; }));
  auto next = sourceFrame("source", 1, 2, 170); offer(owner, next);
  ASSERT_TRUE(next.preparedGpu); ASSERT_TRUE(next.preparedGpu->demand);
  EXPECT_TRUE(next.preparedGpu->demand->failed.load());
  EXPECT_EQ(next.preparedGpu->sourceId, "source");
  EXPECT_EQ(next.preparedGpu->sourceEpoch, 1u); EXPECT_EQ(next.preparedGpu->frameId, 2);
  EXPECT_FALSE(next.preparedGpu->acquire(true)); EXPECT_EQ(next.i420->front(), 170);
}

TEST(CpuSourcePreparation, RecreatedConsumerRecoversHeldStillWithOriginalIdentityAndPixels) {
  PreparationFlags flags;
  auto first = createD3D11Compositor(); ASSERT_TRUE(first);
  auto owner = std::make_shared<CpuSourcePreparation>(true);
  StillMediaFrameCache cache(std::make_unique<PreparedStillDecoder>(),
      StillMediaFrameCache::kDefaultCacheBudgetBytes, owner);
  cache.setDesired({{"media:logo", "logo.png"}});
  ASSERT_TRUE(cache.waitForIdle(5000));
  auto frames = cache.collectFrames(100); ASSERT_EQ(frames.size(), 1u);
  ASSERT_TRUE(frames[0].preparedGpu);
  ASSERT_TRUE(await([&] { return bool(frames[0].preparedGpu->acquire(true)); }));
  const auto original = frames[0];
  const auto retained = original.preparedGpu->acquire(false);
  ASSERT_TRUE(retained);
  const auto expected = first->render(planFor("media:logo"), frames);
  ASSERT_EQ(expected.sourceAdmissions.front().state, "ready");
  // A real replacement compositor registers a different consumer device/id.
  // Retain the old image across recreation to exercise immutable read leases.
  first.reset();
  auto reopened = createD3D11Compositor(); ASSERT_TRUE(reopened);
  ProgramFrame actual;
  ASSERT_TRUE(await([&] {
    frames = cache.collectFrames(200);
    actual = reopened->render(planFor("media:logo"), frames);
    return !actual.sourceAdmissions.empty() && actual.sourceAdmissions.front().state == "ready";
  })) << "Held still never rebuilt views for the recreated consumer";
  EXPECT_NE(frames[0].preparedGpu, original.preparedGpu);
  EXPECT_EQ(frames[0].pixels, original.pixels);
  EXPECT_EQ(frames[0].frameId, original.frameId);
  EXPECT_EQ(frames[0].sourceEpoch, original.sourceEpoch);
  EXPECT_EQ(frames[0].captureTimestamp100ns, original.captureTimestamp100ns);
  EXPECT_EQ(actual.preview.bgra, expected.preview.bgra);
  EXPECT_EQ(retained->sourceFrameId, original.frameId);
  EXPECT_EQ(reopened->sourceTexStats().cachedUploads, 0u);
  EXPECT_EQ(reopened->sourceTexStats().scratchUploads, 0u);
  EXPECT_EQ(reopened->sourceTexStats().textureCreates, 0u);
}

TEST(CpuSourcePreparation, I420ConsumerChurnKeepsOneRetiringGenerationAndMonitorRegistrationDoesNotFence) {
  PreparationFlags flags;
  auto first = createD3D11Compositor(); ASSERT_TRUE(first);
  auto owner = std::make_shared<CpuSourcePreparation>(true);
  CpuVideoArrival arrival("media:i420", owner);
  auto frame = sourceFrame("media:i420", 1, 7, 170); arrival.prepare(frame);
  std::shared_ptr<const GpuVideoFrame> oldLease;
  ASSERT_TRUE(await([&] { oldLease = frame.preparedGpu->acquire(true); return bool(oldLease); }));
  const auto oldToken = frame.preparedGpu;
  auto expected = first->render(planFor(frame.participantId), {frame});
  // Monitor-only consumers are not allowed to steal production source views.
  ComPtr<ID3D11Device> monitorDevice; ComPtr<ID3D11DeviceContext> monitorContext;
  ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
      &monitorDevice, nullptr, &monitorContext)));
  auto monitor = D3DVideoConsumers::add(monitorDevice.Get(), true); ASSERT_TRUE(monitor);
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  EXPECT_FALSE(oldToken->demand->stopped.load()); EXPECT_TRUE(oldToken->acquire(false));
  auto second = createD3D11Compositor(); ASSERT_TRUE(second);
  ASSERT_TRUE(await([&] { return oldToken->demand->stopped.load(); }));
  const auto originalCpu = frame.i420;
  const auto originalEpoch = frame.sourceEpoch;
  const auto originalObservation = frame.captureTimestamp100ns;
  arrival.refreshStopped(frame); ASSERT_NE(frame.preparedGpu, oldToken);
  ASSERT_TRUE(await([&] { return bool(frame.preparedGpu->acquire(true)); }));
  const auto secondToken = frame.preparedGpu;
  auto actual = second->render(planFor(frame.participantId), {frame});
  EXPECT_EQ(actual.preview.bgra, expected.preview.bgra);
  EXPECT_EQ(owner->stats().active, 2u); // old independent read still owns storage
  auto third = createD3D11Compositor(); ASSERT_TRUE(third);
  ASSERT_TRUE(await([&] { return secondToken->demand->stopped.load(); }));
  arrival.refreshStopped(frame);
  ASSERT_TRUE(frame.preparedGpu);
  EXPECT_NE(frame.preparedGpu, secondToken);
  EXPECT_EQ(frame.preparedGpu->demand->capacity.load(), CpuPreparationCapacity::RetiringGenerations);
  auto refused = third->render(planFor(frame.participantId), {frame});
  EXPECT_EQ(refused.sourceAdmissions.front().reason, "preparation-retiring-generation-capacity");
  EXPECT_EQ(refused.sourceAdmissions.front().state, "unavailable");
  EXPECT_LE(owner->stats().active, 2u);
  EXPECT_EQ(oldLease->sourceFrameId, 7);
  // Destroy the old compositor's source cache as well as the independent read.
  first.reset(); oldLease.reset();
  ASSERT_TRUE(await([&] {
    arrival.refreshStopped(frame);
    return frame.preparedGpu && !frame.preparedGpu->demand->stopped.load();
  }));
  ASSERT_TRUE(await([&] { return bool(frame.preparedGpu->acquire(true)); }));
  actual = third->render(planFor(frame.participantId), {frame});
  EXPECT_EQ(actual.sourceAdmissions.front().state, "ready");
  EXPECT_EQ(actual.preview.bgra, expected.preview.bgra);
  EXPECT_EQ(frame.i420, originalCpu); EXPECT_EQ(frame.sourceEpoch, originalEpoch);
  EXPECT_EQ(frame.captureTimestamp100ns, originalObservation); EXPECT_EQ(frame.frameId, 7);
  EXPECT_EQ(third->sourceTexStats().cachedUploads, 0u);
  EXPECT_EQ(third->sourceTexStats().scratchUploads, 0u);
  EXPECT_EQ(third->sourceTexStats().textureCreates, 0u);
  EXPECT_LE(D3DVideoImage::residentBytes.load(), 512u * 1024u * 1024u);
}

TEST(BrowserSourcePreparation, OptInRealHostPreparesWithoutRenderPollingAndSurvivesBlockedReaderRemoval) {
  const char* enabled = std::getenv("COREVIDEO_CAPTURE_TESTS");
  if (!enabled || std::string(enabled) != "1") {
    std::fprintf(stderr, "[capture-test] SKIPPED real browser host; enable COREVIDEO_CAPTURE_TESTS=1\n");
    return;
  }
  PreparationFlags flags;
  _putenv_s("COREVIDEO_CPU_SOURCE_PREPARATION", "0");
  auto reference = createD3D11Compositor();
  _putenv_s("COREVIDEO_CPU_SOURCE_PREPARATION", "1");
  auto compositor = createD3D11Compositor(); ASSERT_TRUE(reference); ASSERT_TRUE(compositor);
  auto owner = std::make_shared<CpuSourcePreparation>(true);
  std::atomic<bool> pause{false}, blocked{false}, release{false};
  BrowserSourceHostAdapter browser(std::string(), owner, [&] {
    if (!pause.load()) return;
    blocked.store(true);
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!release.load() && std::chrono::steady_clock::now() < limit)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  });
  struct ReleaseReader {
    std::atomic<bool>& release;
    ~ReleaseReader() { release.store(true); }
  } releaseOnExit{release}; // release before browser joins on assertion failures
  std::string error;
  auto id = browser.addSource("data:text/html,%3Cbody%20style=%22margin:0;background:rgb(40,90,180)%22%3E", 64, 64, 60, error);
  ASSERT_FALSE(id.empty()) << error;
  // Real WebView2 host and SHM reader must advance without any render poll.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  bool received = false;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto stats = browser.telemetry();
    if (!stats.empty() && stats[0].framesReceived > 0) { received = true; break; }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(received) << "Real browser host did not publish before render polling";
  const auto center = (32 * 64 + 32) * 4;
  std::vector<VideoFrame> frames;
  ASSERT_TRUE(await([&] {
    frames = browser.pollVideoFrames(123);
    return frames.size() == 1 && frames[0].pixels && frames[0].pixels->size() > center + 2 &&
        std::abs(int(frames[0].pixels->at(center)) - 180) <= 1 &&
        std::abs(int(frames[0].pixels->at(center + 1)) - 90) <= 1 &&
        std::abs(int(frames[0].pixels->at(center + 2)) - 40) <= 1;
  })) << "WebView2 never painted the known page colors after its initial blank frame";
  auto frame = frames.front(); ASSERT_TRUE(frame.preparedGpu);
  ASSERT_TRUE(await([&] { return bool(frame.preparedGpu->acquire(true)); }));
  auto cpuOnly = frame; cpuOnly.preparedGpu.reset();
  const auto expected = reference->render(planFor(frame.participantId), {cpuOnly});
  const auto actual = compositor->render(planFor(frame.participantId), {frame});
  ASSERT_EQ(actual.sourceAdmissions.front().state, "ready");
  EXPECT_EQ(actual.preview.bgra, expected.preview.bgra);
  EXPECT_NEAR(frame.pixels->at(center), 180, 1);
  EXPECT_NEAR(frame.pixels->at(center + 1), 90, 1);
  EXPECT_NEAR(frame.pixels->at(center + 2), 40, 1);
  EXPECT_EQ(compositor->sourceTexStats().cachedUploads, 0u);
  EXPECT_EQ(compositor->sourceTexStats().scratchUploads, 0u);
  EXPECT_EQ(compositor->sourceTexStats().textureCreates, 0u);
  auto reopened = createD3D11Compositor(); ASSERT_TRUE(reopened);
  ASSERT_TRUE(await([&] {
    auto latest = browser.pollVideoFrames(200);
    if (latest.empty()) return false;
    auto result = reopened->render(planFor(frame.participantId), latest);
    return !result.sourceAdmissions.empty() && result.sourceAdmissions.front().state == "ready" && result.preview.bgra == expected.preview.bgra;
  }));
  EXPECT_EQ(reopened->sourceTexStats().cachedUploads, 0u);
  EXPECT_EQ(reopened->sourceTexStats().scratchUploads, 0u);
  EXPECT_EQ(reopened->sourceTexStats().textureCreates, 0u);
  pause.store(true);
  ASSERT_TRUE(await([&] { return blocked.load(); }));
  auto collected = std::async(std::launch::async, [&] { return browser.pollVideoFrames(250); });
  ASSERT_TRUE(collected.wait_for(std::chrono::milliseconds(250)) == std::future_status::ready)
      << "Render polling waited behind the blocked SHM reader";
  const auto held = collected.get(); ASSERT_EQ(held.size(), 1u);
  EXPECT_TRUE(browser.removeSource(id)); EXPECT_TRUE(browser.pollVideoFrames(300).empty());
  release.store(true);
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  EXPECT_TRUE(browser.pollVideoFrames(400).empty()); // stale reader cannot republish removed source
  EXPECT_EQ(held[0].pixels->at(center), 180);
  EXPECT_EQ(frame.pixels->at(center), 180); // removal did not mutate old CPU/ISO
}

TEST(CpuSourcePreparation, OptInActualNdiReceiverPreparesOriginalPixelsAndReconnectsWithoutProgramUploads) {
  const char* enabled = std::getenv("COREVIDEO_REQUIRE_NDI_TEST");
  if (!enabled || std::string(enabled) != "1") {
    std::fprintf(stderr, "[capture-test] SKIPPED prepared NDI receiver; enable COREVIDEO_REQUIRE_NDI_TEST=1\n");
    return;
  }
#if COREVIDEO_WITH_NDI_INGEST && COREVIDEO_WITH_NDI_OUTPUT
  PreparationFlags flags;
  _putenv_s("COREVIDEO_CPU_SOURCE_PREPARATION", "0");
  auto reference = createD3D11Compositor(); ASSERT_TRUE(reference);
  _putenv_s("COREVIDEO_CPU_SOURCE_PREPARATION", "1");
  auto compositor = createD3D11Compositor(); ASSERT_TRUE(compositor);
  auto owner = std::make_shared<CpuSourcePreparation>(true);
  auto sender = createNdiOutputSender();
  auto receiver = createNdiReceiveCaptureDevice(owner);
  ASSERT_TRUE(sender && sender->runtimeAvailableAtConstruction() && receiver)
      << "Actual NDI runtime/receiver unavailable; this is missing evidence";
  OutputDestinationSettings settings;
  settings.id = settings.protocol = "ndi";
  settings.ndiName = "CVPPrepareTest-" + std::to_string(GetCurrentProcessId()); settings.fps = 60;
  ProgramFrame output; output.width = output.preview.width = 1920;
  output.height = output.preview.height = 1080;
  output.preview.bgra.resize(1920u * 1080u * 4);
  for (size_t i = 0; i < output.preview.bgra.size(); i += 4) {
    output.preview.bgra[i] = 180; output.preview.bgra[i + 1] = 90;
    output.preview.bgra[i + 2] = 40; output.preview.bgra[i + 3] = 255;
  }
  struct Video final : ICaptureVideoConsumer {
    VideoFrame latest;
    void publish(VideoFrame frame) override { latest = std::move(frame); }
    void end(const std::string&) override {}
  } sink;
  std::string deviceId;
  auto pump = [&] {
    ++output.frameNumber;
    sender->sync({"ndi"}, &output, output.frameNumber * 16.667, {settings});
    if (deviceId.empty()) for (const auto& device : receiver->enumerate()) {
      if (device.name.find(settings.ndiName) != std::string::npos) {
        deviceId = device.id; receiver->connect(deviceId); break;
      }
    }
    receiver->deliverVideo(sink, output.frameNumber * 16);
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  };
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (std::chrono::steady_clock::now() < deadline && !sink.latest.preparedGpu) pump();
  ASSERT_TRUE(sink.latest.preparedGpu) << "Actual NDI arrival never carried preparation";
  const auto original = sink.latest;
  ASSERT_EQ(original.pixelWidth, 1920); ASSERT_EQ(original.pixelHeight, 1080);
  ASSERT_TRUE(await([&] { return bool(original.preparedGpu->acquire(true)); }));
  auto pixels = compositor->render(planFor(original.participantId), {original});
  ASSERT_EQ(pixels.sourceAdmissions.front().state, "ready");
  const auto center = (32 * 64 + 32) * 4;
  auto cpuOnly = original; cpuOnly.preparedGpu.reset();
  auto expected = reference->render(planFor(original.participantId), {cpuOnly});
  std::fprintf(stderr, "[capture-test] NDI CPU BGR=%u,%u,%u; composed BGR=%u,%u,%u\n",
      original.pixels->at(0), original.pixels->at(1), original.pixels->at(2),
      pixels.preview.bgra.at(center), pixels.preview.bgra.at(center + 1), pixels.preview.bgra.at(center + 2));
  EXPECT_EQ(pixels.preview.bgra, expected.preview.bgra);
  // NDI transport may convert/compress the sender's nominal RGB; preparation
  // must exactly preserve the decoded CPU reference, not undo that conversion.
  EXPECT_NEAR(original.pixels->at(0), 180, 8);
  EXPECT_NEAR(original.pixels->at(1), 90, 8);
  EXPECT_NEAR(original.pixels->at(2), 40, 8);
  const auto originalBytes = *original.pixels;
  const auto oldEpoch = original.sourceEpoch;
  receiver->disconnect(deviceId); receiver->connect(deviceId);
  ASSERT_TRUE(await([&] {
    pump();
    return sink.latest.preparedGpu && sink.latest.sourceEpoch > oldEpoch;
  }));
  auto fresh = sink.latest;
  ASSERT_TRUE(await([&] { return bool(fresh.preparedGpu->acquire(true)); }));
  pixels = compositor->render(planFor(fresh.participantId), {fresh});
  EXPECT_EQ(pixels.sourceAdmissions.front().state, "ready");
  EXPECT_EQ(pixels.sourceAdmissions.front().actualEpoch, fresh.sourceEpoch);
  EXPECT_EQ(compositor->sourceTexStats().cachedUploads, 0u);
  EXPECT_EQ(compositor->sourceTexStats().scratchUploads, 0u);
  EXPECT_EQ(compositor->sourceTexStats().textureCreates, 0u);
  EXPECT_EQ(*original.pixels, originalBytes);
  receiver->disconnect(deviceId);
  sender->sync({}, nullptr, output.frameNumber * 16.667 + 1);
#else
  EXPECT_TRUE(false) << "NDI adapter/output gates unavailable; this is missing evidence";
#endif
}

TEST(CpuSourcePreparation, MediaDecoderWorkerPublishesPreparedPixelsWithoutChangingPlaybackOrCpuIdentity) {
  PreparationFlags flags; auto compositor = createD3D11Compositor(); ASSERT_TRUE(compositor);
  auto owner = std::make_shared<CpuSourcePreparation>(true);
  auto bytes = std::make_shared<const std::vector<uint8_t>>(64 * 64 * 4, 255);
  corevideo::core::MediaTransports transports([bytes] { return std::make_unique<PreparedMediaDecoder>(bytes); }, owner);
  corevideo::core::MediaTransportDesired desired;
  desired.sourceId = "media:clip"; desired.assetId = "clip"; desired.path = "clip.mp4";
  desired.kind = "video"; desired.onPreview = true;
  auto changes = transports.apply({desired}, 0); ASSERT_EQ(changes.size(), 1u);
  std::optional<VideoFrame> frame;
  int64_t selectedAt100ns = 0;
  ASSERT_TRUE(await([&] {
    selectedAt100ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count() / 100;
    frame = corevideo::core::MediaTransports::selectVideo(*changes[0].entry, selectedAt100ns);
    return frame.has_value();
  }));
  EXPECT_EQ(frame->pixels, bytes); EXPECT_EQ(frame->frameId, 17);
  EXPECT_EQ(frame->timestampMs, selectedAt100ns / 10000);
  {
    std::lock_guard<std::mutex> lock(changes[0].entry->mutex);
    EXPECT_EQ(changes[0].entry->video.hold().timestampMs, 1234);
  }
  ASSERT_TRUE(frame->preparedGpu);
  ASSERT_TRUE(await([&] { return bool(frame->preparedGpu->acquire(true)); }));
  auto result = compositor->render(planFor("media:clip"), {*frame});
  ASSERT_FALSE(result.preview.bgra.empty()); EXPECT_EQ(result.preview.bgra.front(), 255);
  EXPECT_EQ(result.sourceAdmissions.front().actualFrameId, 17);
  EXPECT_EQ(compositor->sourceTexStats().cachedUploads, 0u);
  EXPECT_EQ(compositor->sourceTexStats().scratchUploads, 0u);
  EXPECT_EQ(bytes->front(), 255);
  const auto oldToken = frame->preparedGpu;
  const auto oldEpoch = frame->sourceEpoch;
  const auto oldObservation = frame->captureTimestamp100ns;
  // Force the existing idle-retirement signal, then exercise the real decoder
  // owner's refresh while the clip remains cued with no new video identity.
  oldToken->demand->stopped.store(true);
  ASSERT_TRUE(await([&] {
    frame = corevideo::core::MediaTransports::selectVideo(*changes[0].entry, selectedAt100ns);
    return frame && frame->preparedGpu != oldToken;
  }));
  EXPECT_EQ(frame->pixels, bytes); EXPECT_EQ(frame->frameId, 17);
  EXPECT_EQ(frame->sourceEpoch, oldEpoch); EXPECT_EQ(frame->captureTimestamp100ns, oldObservation);
  ASSERT_TRUE(await([&] { return bool(frame->preparedGpu->acquire(true)); }));
  auto reopened = compositor->render(planFor("media:clip"), {*frame});
  EXPECT_EQ(reopened.sourceAdmissions.front().state, "ready");
  EXPECT_EQ(reopened.preview.bgra, result.preview.bgra);
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
  auto refused = owner.offer("source", 1, 29, 29000, 64, 64, frame.i420);
  ASSERT_TRUE(refused); EXPECT_TRUE(refused->demand->stopped.load());
  EXPECT_EQ(refused->demand->capacity.load(), CpuPreparationCapacity::PendingTokens);
  frame.frameId = 29; frame.captureTimestamp100ns = 29000; frame.preparedGpu = refused;
  ProgramSourceAdmissionPolicy admission;
  const auto unavailable = admission.select(frame, 1, [](const auto&) { return true; });
  EXPECT_EQ(unavailable.evidence.reason, "preparation-pending-token-capacity");
  EXPECT_EQ(unavailable.evidence.state, "unavailable");
  EXPECT_EQ(owner.stats().active, 0u); // no Program demand, no GPU allocation
  EXPECT_EQ(owner.stats().refused, 7u);
  EXPECT_EQ(frame.i420->front(), 128);
  held.clear();
  EXPECT_TRUE(owner.offer("source", 1, 29, 29000, 64, 64, frame.i420));
}

TEST(CpuSourcePreparation, SourceCapacityRefusalCarriesIdentityWithoutRetainingCpuPixels) {
  CpuSourcePreparation owner(true);
  auto frame = sourceFrame("capacity", 1, 1);
  std::vector<std::shared_ptr<CpuSourceGpuView>> tokens;
  for (size_t i = 0; i < CpuSourcePreparation::kMaxSources; ++i) {
    auto token = owner.offer("source-" + std::to_string(i), 1, 1, 1000, 64, 64, frame.i420);
    ASSERT_TRUE(token); tokens.push_back(token);
  }
  frame.captureTimestamp100ns = 1000;
  frame.preparedGpu = owner.offer(frame.participantId, 1, 1, 1000, 64, 64, frame.i420);
  ASSERT_TRUE(frame.preparedGpu);
  ProgramSourceAdmissionPolicy policy;
  const auto result = policy.select(frame, 1, [](const auto&) { return true; });
  EXPECT_EQ(result.evidence.reason, "preparation-source-capacity");
  EXPECT_EQ(result.evidence.requestedFrameId, 1);
  EXPECT_EQ(result.evidence.state, "unavailable");
  EXPECT_EQ(owner.stats().sources, CpuSourcePreparation::kMaxSources);
  EXPECT_EQ(owner.stats().active, 0u);
  const auto token = frame.preparedGpu;
  frame.i420.reset();
  EXPECT_TRUE(token->cpu.expired());
}

TEST(CpuSourcePreparation, ActiveCapacityReportsBlockAndRecoversWithoutChangingCpuSelection) {
  PreparationFlags flags;
  auto compositor = createD3D11Compositor(); ASSERT_TRUE(compositor);
  CpuSourcePreparation owner(true);
  std::vector<VideoFrame> frames;
  for (size_t i = 0; i < CpuSourcePreparation::kMaxActive; ++i) {
    auto frame = sourceFrame("active-" + std::to_string(i), 1, 1);
    offer(owner, frame); frames.push_back(std::move(frame));
    ASSERT_TRUE(await([&] { return bool(frames.back().preparedGpu->acquire(true)); }));
  }
  auto extra = sourceFrame("extra", 1, 1, 180); offer(owner, extra);
  const auto token = extra.preparedGpu;
  const auto originalCpu = extra.i420;
  token->acquire(true);
  ASSERT_TRUE(await([&] { return token->demand->capacity.load() == CpuPreparationCapacity::ActiveGenerations; }));
  auto blocked = compositor->render(planFor("extra"), {extra});
  EXPECT_EQ(blocked.sourceAdmissions.front().reason, "preparation-active-generation-capacity");
  EXPECT_EQ(blocked.sourceAdmissions.front().state, "unavailable");
  EXPECT_EQ(owner.stats().active, CpuSourcePreparation::kMaxActive);
  frames.front().preparedGpu->demand->stopped.store(true);
  ASSERT_TRUE(await([&] { return bool(token->acquire(true)); }));
  auto recovered = compositor->render(planFor("extra"), {extra});
  EXPECT_EQ(recovered.sourceAdmissions.front().state, "ready");
  EXPECT_NEAR(recovered.preview.bgra.front(), 180, 1);
  EXPECT_EQ(token->demand->capacity.load(), CpuPreparationCapacity::None);
  EXPECT_EQ(extra.i420, originalCpu); EXPECT_EQ(extra.preparedGpu, token);
  EXPECT_EQ(extra.frameId, 1); EXPECT_EQ(extra.sourceEpoch, 1u);
  EXPECT_EQ(compositor->sourceTexStats().cachedUploads, 0u);
  EXPECT_EQ(compositor->sourceTexStats().scratchUploads, 0u);
  EXPECT_EQ(compositor->sourceTexStats().textureCreates, 0u);
  EXPECT_LE(owner.stats().active, CpuSourcePreparation::kMaxActive);
}

TEST(CpuSourcePreparation, ActualBgraResidencyRefusalReleasesPartialAllocationAndHeldFrameRecovers) {
  PreparationFlags flags;
  auto compositor = createD3D11Compositor(); ASSERT_TRUE(compositor);
  const auto startingBytes = D3DVideoImage::residentBytes.load();
  auto owner = std::make_shared<CpuSourcePreparation>(true);
  // Five real 4K three-slot pools consume about 475 MiB. The sixth must be
  // refused by the shared 512 MiB reservation, below the 16-generation limit.
  constexpr int width = 3840, height = 2160;
  auto cpu = std::make_shared<const std::vector<uint8_t>>(size_t(width) * height * 4, 180);
  std::vector<VideoFrame> frames;
  for (int i = 0; i < 5; ++i) {
    VideoFrame frame;
    frame.participantId = "residency-" + std::to_string(i); frame.sourceEpoch = 1;
    frame.frameId = 1; frame.captureTimestamp100ns = 1000;
    frame.width = frame.pixelWidth = width; frame.height = frame.pixelHeight = height;
    frame.pixelStride = width * 4; frame.pixels = cpu;
    frame.preparedGpu = owner->offerBgra(frame.participantId, 1, 1, 1000, width, height, width * 4, cpu);
    ASSERT_TRUE(frame.preparedGpu); frames.push_back(std::move(frame));
    ASSERT_TRUE(await([&] { return bool(frames.back().preparedGpu->acquire(true)); }));
  }
  const auto fullBytes = D3DVideoImage::residentBytes.load();
  EXPECT_EQ(fullBytes - startingBytes, size_t(5) * 3 * width * height * 4);
  CpuVideoArrival arrival("residency-extra", owner);
  auto extra = frames.front(); extra.participantId = "residency-extra"; extra.preparedGpu.reset();
  arrival.prepare(extra); const auto epoch = extra.sourceEpoch;
  const auto observation = extra.captureTimestamp100ns;
  extra.preparedGpu->acquire(true);
  ASSERT_TRUE(await([&] { return extra.preparedGpu->demand->stopped.load(); }));
  auto refused = compositor->render(planFor(extra.participantId), {extra});
  EXPECT_EQ(refused.sourceAdmissions.front().reason, "preparation-residency-capacity");
  EXPECT_EQ(refused.sourceAdmissions.front().state, "unavailable");
  EXPECT_EQ(owner->stats().failed, 0u);
  EXPECT_EQ(D3DVideoImage::residentBytes.load(), fullBytes); // partial slots released
  frames.front().preparedGpu->demand->stopped.store(true);
  ASSERT_TRUE(await([&] {
    arrival.refreshStopped(extra);
    return bool(extra.preparedGpu->acquire(true));
  }));
  auto recovered = compositor->render(planFor(extra.participantId), {extra});
  EXPECT_EQ(recovered.sourceAdmissions.front().state, "ready");
  EXPECT_EQ(extra.pixels, cpu); EXPECT_EQ(extra.sourceEpoch, epoch);
  EXPECT_EQ(extra.frameId, 1); EXPECT_EQ(extra.captureTimestamp100ns, observation);
  EXPECT_EQ(compositor->sourceTexStats().cachedUploads, 0u);
  EXPECT_EQ(compositor->sourceTexStats().scratchUploads, 0u);
  EXPECT_EQ(compositor->sourceTexStats().textureCreates, 0u);
  EXPECT_LE(D3DVideoImage::residentBytes.load(), 512u * 1024u * 1024u);
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
