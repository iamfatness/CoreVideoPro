#include "modules/ShmCapturePreparation.h"
#include "modules/WinUiCaptureDeviceAdapter.h"
#include "modules/ProgramFramePreview.h"
#include <gtest/gtest.h>
#include <future>
#include <cstring>
#include <limits>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#if defined(_WIN32) && COREVIDEO_WITH_D3D11 && !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS
#include "modules/D3DVideoFrame.h"
#include "modules/BgraSourcePreparation.h"
#include <cstdlib>
#endif
using namespace corevideo::modules;

TEST(ShmCapturePreparation, RejectsInvalidAndOverBudgetRequestsBeforeAllocation) {
  ShmCapturePreparation preparation;
  EXPECT_FALSE(preparation.registerBuffer("", "name", 64, 64));
  EXPECT_FALSE(preparation.registerBuffer("id", "name", 0, 64));
  EXPECT_FALSE(preparation.registerBuffer("id", "name", std::numeric_limits<int>::max(), std::numeric_limits<int>::max()));
  EXPECT_EQ(preparation.stats().accepted, 0u);
  EXPECT_EQ(preparation.stats().refused, 3u);
  EXPECT_EQ(preparation.stats().lastRefusalReason, "capture-budget");
  EXPECT_EQ(preparation.stats().residentBytes, 0u);
}

#ifdef _WIN32
namespace {
struct Writer {
  HANDLE handle = nullptr;
  uint8_t* view = nullptr;
  std::string name;
  int width, height;
  Writer(int w, int h) : width(w), height(h) {
    static std::atomic<uint64_t> next{1};
    name = "Local\\CoreVideo-Shm-Unit-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(next++);
    const std::wstring wide(name.begin(), name.end());
    handle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 16 + w * h * 4, wide.c_str());
    if (!handle) throw std::runtime_error("test mapping creation failed");
    view = static_cast<uint8_t*>(MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    if (!view) { CloseHandle(handle); throw std::runtime_error("test mapping view failed"); }
    reinterpret_cast<uint32_t*>(view)[1] = w;
    reinterpret_cast<uint32_t*>(view)[2] = h;
  }
  ~Writer() { UnmapViewOfFile(view); CloseHandle(handle); }
  void write(uint32_t seq, uint8_t color, bool opaque = false) {
    InterlockedExchange(reinterpret_cast<volatile LONG*>(view), seq - 1);
    std::memset(view + 16, color, static_cast<size_t>(width) * height * 4);
    if (opaque) for (size_t i = 19; i < 16 + static_cast<size_t>(width) * height * 4; i += 4) view[i] = 255;
    MemoryBarrier();
    InterlockedExchange(reinterpret_cast<volatile LONG*>(view), seq);
  }
};
bool waitFor(const std::function<bool()>& check) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!check() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  return check();
}
VideoFrame awaitColor(ShmCapturePreparation& preparation, uint8_t color) {
  VideoFrame result;
  if (!waitFor([&] {
    auto frames = preparation.latest();
    if (frames.size() == 1 && frames[0].hasPixels() && frames[0].pixels->at(0) == color) {
      result = frames[0]; return true;
    }
    return false;
  })) throw std::runtime_error("prepared color did not arrive");
  return result;
}
class EmptyCapture final : public ICaptureDevice {
 public:
  std::vector<CaptureDeviceInfo> enumerate() const override { return {}; }
  std::vector<CaptureDeviceInfo> selectInput(const std::string&, const std::string&) override { return {}; }
  std::vector<CaptureDeviceInfo> setAudioSyncOffset(const std::string&, int) override { return {}; }
  std::vector<CaptureDeviceInfo> connect(const std::string&) override { return {}; }
};
}

TEST(ShmCapturePreparation, HoldsImmutableRealPixelsAndRefusesAnExhaustedPool) {
  Writer writer(64, 64);
  ShmCapturePreparation preparation;
  ASSERT_TRUE(preparation.registerBuffer("camera", writer.name, 64, 64));
  std::vector<VideoFrame> held;
  for (uint8_t color = 1; color <= 4; ++color) {
    writer.write(color * 2, color);
    held.push_back(awaitColor(preparation, color));
  }
  writer.write(10, 5);
  ASSERT_TRUE(waitFor([&] { return preparation.stats().poolBusy > 0; }));
  EXPECT_EQ(preparation.latest()[0].frameId, held.back().frameId);
  for (size_t i = 0; i < held.size(); ++i) {
    EXPECT_EQ(held[i].pixels->at(0), i + 1);
    EXPECT_EQ(held[i].pixels->back(), i + 1);
    EXPECT_GT(held[i].captureTimestamp100ns, 0);
    EXPECT_GT(held[i].sourceEpoch, 0u);
  }
  held.erase(held.begin());
  auto fresh = awaitColor(preparation, 5);
  EXPECT_GT(fresh.frameId, held.back().frameId);
  EXPECT_EQ(fresh.sourceEpoch, held.back().sourceEpoch);
  EXPECT_LE(preparation.stats().residentBytes, ShmCapturePreparation::kBudgetBytes);
}

TEST(ShmCapturePreparation, EmptyOrInProgressMappingCannotPublishFreshPixels) {
  Writer writer(64, 64);
  ShmCapturePreparation preparation;
  ASSERT_TRUE(preparation.registerBuffer("camera", writer.name, 64, 64));
  ASSERT_TRUE(waitFor([&] { return preparation.stats().active == 1; }));
  EXPECT_TRUE(preparation.latest().empty()); // sequence zero is not an arrival
  InterlockedExchange(reinterpret_cast<volatile LONG*>(writer.view), 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(25));
  EXPECT_TRUE(preparation.latest().empty());
  writer.write(2, 7);
  auto good = awaitColor(preparation, 7);
  InterlockedExchange(reinterpret_cast<volatile LONG*>(writer.view), 3);
  std::this_thread::sleep_for(std::chrono::milliseconds(25));
  EXPECT_EQ(preparation.latest()[0].frameId, good.frameId);
  EXPECT_EQ(preparation.latest()[0].pixels->at(0), 7);
}

TEST(ShmCapturePreparation, ResizeReconnectAndUnregisterFenceOldGenerationsAndRetireLeases) {
  Writer first(64, 64), second(96, 64), third(32, 64);
  ShmCapturePreparation preparation;
  first.write(2, 11);
  ASSERT_TRUE(preparation.registerBuffer("camera", first.name, 64, 64));
  auto old = awaitColor(preparation, 11);
  second.write(2, 22);
  ASSERT_TRUE(preparation.registerBuffer("camera", second.name, 96, 64));
  for (const auto& frame : preparation.latest()) {
    if (frame.sourceEpoch == old.sourceEpoch) EXPECT_EQ(frame.frameId, old.frameId);
    else EXPECT_EQ(frame.pixelWidth, 96);
  }
  auto resized = awaitColor(preparation, 22);
  EXPECT_EQ(resized.pixelWidth, 96);
  EXPECT_NE(resized.sourceEpoch, old.sourceEpoch);
  EXPECT_GT(resized.frameId, old.frameId);
  ASSERT_TRUE(waitFor([&] { return preparation.stats().retiring == 1; }));
  third.write(2, 33);
  ASSERT_TRUE(preparation.registerBuffer("camera", third.name, 32, 64));
  ASSERT_TRUE(waitFor([&] { return preparation.stats().reason == "capture-retirement-pending"; }));
  ASSERT_EQ(preparation.latest().size(), 1u);
  EXPECT_EQ(preparation.latest()[0].frameId, resized.frameId);
  old = {};
  auto reconnected = awaitColor(preparation, 33);
  EXPECT_EQ(reconnected.pixelWidth, 32);
  EXPECT_NE(reconnected.sourceEpoch, resized.sourceEpoch);
  preparation.unregisterBuffer("camera");
  EXPECT_TRUE(preparation.latest().empty());
  resized = {}; reconnected = {};
  ASSERT_TRUE(waitFor([&] { return preparation.stats().residentBytes == 0; }));
  EXPECT_EQ(preparation.stats().retiring, 0u);
}

TEST(ShmCapturePreparation, TornCopyIsRejectedAndTheNextCompleteFrameRecovers) {
  Writer writer(64, 64);
  writer.write(2, 11);
  std::atomic<bool> first{true};
  ShmCapturePreparation preparation([&] {
    if (first.exchange(false)) writer.write(4, 22);
  });
  ASSERT_TRUE(preparation.registerBuffer("camera", writer.name, 64, 64));
  const auto recovered = awaitColor(preparation, 22);
  EXPECT_GT(preparation.stats().torn, 0u);
  EXPECT_EQ(recovered.frameId, 1); // rejected copy never acquired a delivered identity
  EXPECT_EQ(recovered.pixels->back(), 22);
  EXPECT_TRUE(waitFor([&] { return preparation.stats().state == "ready"; }));
  EXPECT_FALSE(preparation.registerBuffer("camera", writer.name, 0, 64));
  ASSERT_TRUE(waitFor([&] { return preparation.stats().state == "ready"; }));
  EXPECT_EQ(preparation.stats().lastRefusalReason, "invalid-mapping");
}

TEST(ShmCapturePreparation, BridgePreservesDeliveryClockWithoutRestampingAcquisitionOrIdentity) {
  Writer writer(64, 64);
  writer.write(2, 77);
  WinUiCaptureDeviceAdapter bridge(std::make_unique<EmptyCapture>());
  bridge.registerCaptureBuffer("camera", writer.name, 64, 64);
  ASSERT_TRUE(waitFor([&] { return bridge.shmCapturePreparationDiagnostics().prepared > 0; }));
  struct Consumer : ICaptureVideoConsumer {
    VideoFrame frame;
    void publish(VideoFrame next) override { frame = std::move(next); }
    void end(const std::string&) override {}
  } first, held;
  bridge.deliverVideo(first, 123);
  bridge.deliverVideo(held, 234);
  ASSERT_TRUE(first.frame.hasPixels());
  ASSERT_TRUE(held.frame.hasPixels());
  EXPECT_EQ(first.frame.timestampMs, 123);
  EXPECT_EQ(held.frame.timestampMs, 234);
  EXPECT_EQ(first.frame.captureTimestamp100ns, held.frame.captureTimestamp100ns);
  EXPECT_GT(first.frame.captureTimestamp100ns, 0);
  EXPECT_EQ(first.frame.frameId, held.frame.frameId);
  EXPECT_EQ(first.frame.sourceEpoch, held.frame.sourceEpoch);
  EXPECT_EQ(first.frame.pixels, held.frame.pixels);
}

TEST(ShmCapturePreparation, BlockedCopyDoesNotBlockProgramConsumerOrRealPixelComposition) {
  Writer writer(64, 64);
  writer.write(2, 77);
  std::promise<void> entered, release;
  auto gate = release.get_future().share();
  std::atomic<bool> first{true};
  WinUiCaptureDeviceAdapter bridge(std::make_unique<EmptyCapture>(), [&] {
    if (first.exchange(false)) { entered.set_value(); gate.wait(); }
  });
  bridge.registerCaptureBuffer("optional-monitor", writer.name, 64, 64);
  const auto started = entered.get_future().wait_for(std::chrono::seconds(3));
  if (started != std::future_status::ready) release.set_value();
  ASSERT_TRUE(started == std::future_status::ready);
  auto program = std::async(std::launch::async, [&] {
    struct Consumer : ICaptureVideoConsumer {
      void publish(VideoFrame) override {}
      void end(const std::string&) override {}
    } consumer;
    CompositorRenderPlan plan; plan.width = plan.height = 64;
    CompositorRenderPlanLayer layer; layer.kind = "participant-video";
    layer.sourceId = layer.participantId = "stable-program";
    layer.rect = {0, 0, 1, 1}; layer.opacity = 1;
    plan.layers.push_back(layer);
    VideoFrame source; source.participantId = "stable-program";
    source.width = source.height = source.naturalWidth = source.naturalHeight = 64;
    source.pixelWidth = source.pixelHeight = 64; source.pixelStride = 256;
    auto opaque = std::make_shared<std::vector<uint8_t>>(64 * 64 * 4, 123);
    for (size_t i = 3; i < opaque->size(); i += 4) (*opaque)[i] = 255;
    source.pixels = std::move(opaque);
    int count = 0;
    for (int i = 0; i < 20; ++i) {
      bridge.deliverVideo(consumer, i * 17);
      ProgramFramePreviewPixels pixels; ProgramFrame frame; frame.width = frame.height = 64;
      fillSyntheticProgramFramePreview(pixels, plan, {source}, frame);
      if (!pixels.bgra.empty() && pixels.bgra[0] == 123) ++count;
    }
    return count;
  });
  const bool completedWhileBlocked = program.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
  std::this_thread::sleep_for(std::chrono::milliseconds(25));
  release.set_value(); // safe teardown even if a future regression takes a preparation lock
  EXPECT_TRUE(completedWhileBlocked);
  EXPECT_EQ(program.get(), 20);
  ASSERT_TRUE(waitFor([&] { return bridge.shmCapturePreparationDiagnostics().prepared > 0; }));
  EXPECT_GE(bridge.shmCapturePreparationDiagnostics().copyMaximumNs, 25'000'000u);
}
#if COREVIDEO_WITH_D3D11 && !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS
namespace {
struct CpuPreparationFlag {
  std::string previous, monitorPrevious;
  CpuPreparationFlag() {
    const char* raw = std::getenv("COREVIDEO_CPU_SOURCE_PREPARATION");
    previous = raw ? raw : "";
    _putenv_s("COREVIDEO_CPU_SOURCE_PREPARATION", "1");
    raw = std::getenv("COREVIDEO_ISOLATE_MONITORS");
    monitorPrevious = raw ? raw : "";
    _putenv_s("COREVIDEO_ISOLATE_MONITORS", "0");
  }
  ~CpuPreparationFlag() {
    _putenv_s("COREVIDEO_CPU_SOURCE_PREPARATION", previous.c_str());
    _putenv_s("COREVIDEO_ISOLATE_MONITORS", monitorPrevious.c_str());
  }
};
CompositorRenderPlan capturePlan(int width, int height) {
  CompositorRenderPlan plan; plan.width = width; plan.height = height;
  CompositorRenderPlanLayer layer; layer.kind = "participant-video";
  layer.sourceId = layer.participantId = "capture:camera";
  layer.rect = {0, 0, 1, 1}; layer.borderStyle = "none";
  plan.layers.push_back(layer); return plan;
}
}
TEST(ShmGpuPreparation, RealMappingRendersReadyPixelsWithoutProgramCpuUploadAndKeepsIsoIdentity) {
  CpuPreparationFlag flag;
  auto compositor = createD3D11Compositor();
  ASSERT_TRUE(compositor);
  Writer writer(64, 64);
  ShmCapturePreparation preparation;
  ASSERT_TRUE(preparation.registerBuffer("camera", writer.name, 64, 64));
  writer.write(2, 91, true);
  auto original = awaitColor(preparation, 91);
  ASSERT_TRUE(waitFor([&] { auto frames = preparation.latest(); return frames.size() == 1 && frames[0].preparedGpu && frames[0].preparedGpu->acquire(false); }));
  auto ready = preparation.latest().front();
  EXPECT_EQ(ready.pixels, original.pixels);
  EXPECT_EQ(ready.frameId, original.frameId);
  EXPECT_EQ(ready.sourceEpoch, original.sourceEpoch);
  EXPECT_EQ(ready.captureTimestamp100ns, original.captureTimestamp100ns);
  EXPECT_FALSE(ready.gpuPixels); // CPU/ISO descriptors carry only weak GPU publication
  auto plan = capturePlan(64, 64);
  auto result = compositor->render(plan, {ready});
  ASSERT_FALSE(result.preview.bgra.empty());
  const size_t center = (32 * 64 + 32) * 4;
  EXPECT_NEAR(result.preview.bgra[center], 91, 1);
  EXPECT_NEAR(result.preview.bgra[center + 1], 91, 1);
  EXPECT_NEAR(result.preview.bgra[center + 2], 91, 1);
  EXPECT_EQ(compositor->sourceTexStats().cachedUploads, 0u);
  EXPECT_EQ(compositor->sourceTexStats().scratchUploads, 0u);
  EXPECT_EQ(ready.pixels->front(), 91);
  EXPECT_TRUE(preparation.stats().gpuRequested);
  EXPECT_GE(preparation.stats().gpuPrepared, 1u);
  EXPECT_EQ(preparation.stats().gpuFailed, 0u);
  writer.write(4, 173, true);
  ASSERT_TRUE(waitFor([&] { auto frames = preparation.latest(); return frames.size() == 1 &&
      frames[0].preparedGpu && frames[0].preparedGpu->acquire(false) && frames[0].pixels->front() == 173; }));
  auto fresh = preparation.latest().front();
  EXPECT_GT(fresh.frameId, ready.frameId);
  EXPECT_EQ(fresh.sourceEpoch, ready.sourceEpoch);
  result = compositor->render(plan, {fresh});
  ASSERT_FALSE(result.preview.bgra.empty());
  EXPECT_NEAR(result.preview.bgra[center + 2], 173, 1);
  EXPECT_EQ(compositor->sourceTexStats().cachedUploads, 0u);
  EXPECT_EQ(ready.pixels->front(), 91); // ISO still owns the original lease
  preparation.unregisterBuffer("camera");
  ASSERT_TRUE(waitFor([&] { return preparation.latest().empty(); }));
  EXPECT_EQ(ready.pixels->front(), 91);
}

TEST(ShmGpuPreparation, ContinuousCpuSelectionAdmitsLateCompletedPixelsWithoutChangingIsoIdentity) {
  CpuPreparationFlag flag;
  auto compositor = createD3D11Compositor(); ASSERT_TRUE(compositor);
  ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
  ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)));
  auto makeFrame = [](int64_t id) {
    VideoFrame frame; frame.participantId = "capture:camera"; frame.sourceEpoch = 1; frame.frameId = id;
    frame.captureTimestamp100ns = id * 1000;
    frame.width = frame.height = frame.pixelWidth = frame.pixelHeight = 64; frame.pixelStride = 256;
    auto bytes = std::make_shared<std::vector<uint8_t>>(64 * 64 * 4, static_cast<uint8_t>(id));
    for (size_t i = 3; i < bytes->size(); i += 4) (*bytes)[i] = 255;
    frame.pixels = bytes;
    auto token = std::make_shared<CpuSourceGpuView>(); token->sourceId = frame.participantId;
    token->sourceEpoch = frame.sourceEpoch; token->frameId = id; token->captureTimestamp100ns = frame.captureTimestamp100ns;
    token->width = token->height = 64; token->cpu = bytes; token->demand = std::make_shared<CpuSourceGpuDemand>();
    frame.preparedGpu = std::move(token); return frame;
  };
  BgraSourcePreparation owner; auto current = makeFrame(1); auto plan = capturePlan(64, 64);
  for (int64_t id = 1; id <= 120; ++id) {
    owner.offer(device.Get(), context.Get(), current);
    compositor->render(plan, {current}); // select this identity before its GPU completion
    auto next = makeFrame(id + 1);
    ASSERT_TRUE(waitFor([&] { owner.poll(context.Get(), next); return bool(current.preparedGpu->acquire(false)); }));
    EXPECT_FALSE(next.gpuPixels); EXPECT_FALSE(current.gpuPixels);
    auto rendered = compositor->render(plan, {next});
    ASSERT_EQ(rendered.sourceAdmissions.size(), 1u);
    EXPECT_EQ(rendered.sourceAdmissions.front().state, "held");
    EXPECT_EQ(rendered.sourceAdmissions.front().actualFrameId, id);
    EXPECT_EQ(rendered.sourceAdmissions.front().requestedFrameId, id + 1);
    ASSERT_FALSE(rendered.preview.bgra.empty());
    EXPECT_NEAR(rendered.preview.bgra[(32 * 64 + 32) * 4], id, 1);
    EXPECT_EQ(next.pixels->front(), id + 1); EXPECT_EQ(current.pixels->front(), id);
    current = std::move(next);
  }
  EXPECT_EQ(compositor->sourceTexStats().cachedUploads, 0u);
  EXPECT_EQ(compositor->sourceTexStats().scratchUploads, 0u);
  EXPECT_EQ(owner.stats().prepared, 120u);
  current.sourceEpoch = 2; current.preparedGpu.reset();
  auto reconnect = compositor->render(plan, {current});
  EXPECT_EQ(reconnect.sourceAdmissions.front().state, "unavailable");
  EXPECT_EQ(reconnect.sourceAdmissions.front().actualFrameId, -1);
}

TEST(ShmGpuPreparation, LateUploadCannotAttachToAnotherSourceIdentityAndOtherSourceRemainsIndependent) {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)));
  auto consumer = D3DVideoConsumers::add(device.Get()); ASSERT_TRUE(consumer);
  auto makeFrame = [](int64_t id, uint64_t epoch) {
    VideoFrame frame; frame.participantId = "capture:camera";
    frame.width = frame.height = frame.pixelWidth = frame.pixelHeight = 64;
    frame.pixelStride = 256; frame.frameId = id; frame.sourceEpoch = epoch;
    frame.pixels = std::make_shared<const std::vector<uint8_t>>(64 * 64 * 4, 131);
    return frame;
  };
  BgraSourcePreparation delayed, healthy;
  auto old = makeFrame(10, 1), replacement = makeFrame(10, 2), other = makeFrame(20, 7);
  delayed.offer(device.Get(), context.Get(), old);
  healthy.offer(device.Get(), context.Get(), other);
  ASSERT_TRUE(waitFor([&] {
    delayed.poll(context.Get(), replacement);
    healthy.poll(context.Get(), other);
    return delayed.stats().superseded == 1 && other.gpuPixels;
  }));
  EXPECT_FALSE(replacement.gpuPixels);
  EXPECT_TRUE(other.gpuPixels);
  delayed.offer(device.Get(), context.Get(), replacement);
  ASSERT_TRUE(waitFor([&] { delayed.poll(context.Get(), replacement); return bool(replacement.gpuPixels); }));
  EXPECT_EQ(replacement.gpuPixels->generation, 1u); // pool and source epochs have independent lifetimes
  EXPECT_EQ(replacement.sourceEpoch, 2u);
  EXPECT_EQ(replacement.frameId, 10);
  EXPECT_EQ(delayed.stats().prepared, 1u);
  EXPECT_EQ(healthy.stats().failed, 0u);
}

TEST(ShmGpuPreparation, PaddedRowsRemainExactAndHeldGpuImagesBoundThePoolWithoutBlockingAnotherSource) {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)));
  auto consumer = D3DVideoConsumers::add(device.Get()); ASSERT_TRUE(consumer);
  BgraSourcePreparation selected, healthy;
  std::vector<VideoFrame> held;
  auto makeFrame = [](int64_t identity) {
    VideoFrame frame; frame.participantId = "source"; frame.sourceEpoch = 3; frame.frameId = identity;
    frame.width = frame.pixelWidth = 64; frame.height = frame.pixelHeight = 32;
    frame.pixelStride = 64 * 4 + 32;
    auto bytes = std::make_shared<std::vector<uint8_t>>(frame.pixelStride * frame.pixelHeight, 0xee);
    for (int y = 0; y < 32; ++y) for (int x = 0; x < 64 * 4; ++x)
      (*bytes)[y * frame.pixelStride + x] = static_cast<uint8_t>(x * 3 + y * 7 + identity);
    frame.pixels = bytes; return frame;
  };
  for (int i = 1; i <= 3; ++i) {
    auto frame = makeFrame(i);
    selected.offer(device.Get(), context.Get(), frame);
    ASSERT_TRUE(waitFor([&] { selected.poll(context.Get(), frame); return bool(frame.gpuPixels); }));
    held.push_back(frame);
  }
  auto refused = makeFrame(4), other = makeFrame(100);
  selected.offer(device.Get(), context.Get(), refused);
  EXPECT_EQ(selected.stats().busy, 1u);
  EXPECT_FALSE(refused.gpuPixels);
  healthy.offer(device.Get(), context.Get(), other);
  ASSERT_TRUE(waitFor([&] { healthy.poll(context.Get(), other); return bool(other.gpuPixels); }));
  const auto image = std::dynamic_pointer_cast<const D3DVideoImage>(held[0].gpuPixels);
  ASSERT_TRUE(image);
  const auto* view = image->view(consumer->id); ASSERT_TRUE(view);
  D3D11_TEXTURE2D_DESC desc{}; view->texture->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = desc.MiscFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> staging;
  ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)));
  context->CopyResource(staging.Get(), view->texture.Get());
  D3D11_MAPPED_SUBRESOURCE mapped{};
  ASSERT_TRUE(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
  size_t mismatches = 0;
  for (int y = 0; y < 32; ++y) for (int x = 0; x < 64 * 4; ++x)
    if (static_cast<const uint8_t*>(mapped.pData)[y * mapped.RowPitch + x] !=
        held[0].pixels->at(y * held[0].pixelStride + x)) ++mismatches;
  context->Unmap(staging.Get(), 0);
  EXPECT_EQ(mismatches, 0u);
  // Release a different lease; the independently read image remains held.
  held.erase(held.begin() + 1);
  selected.offer(device.Get(), context.Get(), refused);
  ASSERT_TRUE(waitFor([&] { selected.poll(context.Get(), refused); return bool(refused.gpuPixels); }));
  EXPECT_EQ(selected.stats().prepared, 4u);
  EXPECT_LE(D3DVideoImage::residentBytes.load(), 512u * 1024u * 1024u);
}

TEST(ShmGpuPreparation, ReconnectFencesOldIdentityAndRetainsGpuOnlyLeaseUntilRetirement) {
  CpuPreparationFlag flag;
  auto compositor = createD3D11Compositor(); ASSERT_TRUE(compositor);
  Writer first(64, 64), replacement(96, 64);
  ShmCapturePreparation preparation;
  ASSERT_TRUE(preparation.registerBuffer("camera", first.name, 64, 64));
  first.write(2, 61, true);
  ASSERT_TRUE(waitFor([&] { auto frames = preparation.latest(); return frames.size() == 1 && frames[0].preparedGpu && frames[0].preparedGpu->acquire(false); }));
  std::shared_ptr<const GpuVideoFrame> heldGpu;
  uint64_t oldEpoch;
  {
    auto old = preparation.latest().front(); heldGpu = old.preparedGpu->acquire(false); oldEpoch = old.sourceEpoch;
  }
  ASSERT_TRUE(preparation.registerBuffer("camera", replacement.name, 96, 64));
  replacement.write(2, 182, true);
  ASSERT_TRUE(waitFor([&] { auto frames = preparation.latest(); return frames.size() == 1 &&
      frames[0].sourceEpoch != oldEpoch && frames[0].preparedGpu && frames[0].preparedGpu->acquire(false) && frames[0].pixels->front() == 182; }));
  ASSERT_TRUE(waitFor([&] { return preparation.stats().retiring == 1; }));
  EXPECT_EQ(heldGpu->width, 64);
  EXPECT_EQ(preparation.latest().front().preparedGpu->acquire(false)->width, 96);
  heldGpu.reset();
  ASSERT_TRUE(waitFor([&] { return preparation.stats().retiring == 0; }));
  preparation.unregisterBuffer("camera");
  ASSERT_TRUE(waitFor([&] { return preparation.stats().active == 0 && preparation.stats().retiring == 0; }));
}
#endif
#endif
