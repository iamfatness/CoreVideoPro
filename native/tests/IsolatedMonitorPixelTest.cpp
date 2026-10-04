#include "modules/Interfaces.h"
#include "modules/DeliveryCounterPattern.h"
#include "core/ComApartmentLifetime.h"
#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <future>
#include <algorithm>

#if defined(_WIN32) && !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_D3D11
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include "compositor/ComPtrLite.h"
#include "modules/D3DVideoFrame.h"

namespace {
using namespace corevideo::modules;

std::unique_ptr<ICompositor> isolatedCompositor() {
  const char* raw = std::getenv("COREVIDEO_ISOLATE_MONITORS");
  const std::string previous = raw ? raw : "";
  _putenv_s("COREVIDEO_ISOLATE_MONITORS", "1");
  auto result = createD3D11Compositor();
  _putenv_s("COREVIDEO_ISOLATE_MONITORS", previous.c_str());
  return result;
}

// Independently opens and consumes the exported pixels, rather than trusting
// the job's metadata or the compositor's submission counter.
uint32_t consumeCenter(const ProgramFrameSharedTexture& exported) {
  ComPtrLite<ID3D11Device> device;
  ComPtrLite<ID3D11DeviceContext> context;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
      device.put(), nullptr, context.put()))) return 0;
  const auto handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(
      std::stoull(exported.sharedHandleHex, nullptr, 16)));
  ComPtrLite<ID3D11Texture2D> texture, staging;
  ComPtrLite<IDXGIKeyedMutex> key;
  if (FAILED(device->OpenSharedResource(handle, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(texture.put()))) ||
      FAILED(texture->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(key.put())))) return 0;
  D3D11_TEXTURE2D_DESC desc{};
  texture->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.MiscFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  if (FAILED(device->CreateTexture2D(&desc, nullptr, staging.put())) || key->AcquireSync(1, 1000) != S_OK) return 0;
  context->CopyResource(staging.get(), texture.get());
  context->Flush();
  key->ReleaseSync(0);
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) return 0;
  const auto* p = static_cast<const uint8_t*>(mapped.pData) + (desc.Height / 2) * mapped.RowPitch + (desc.Width / 2) * 4;
  const uint32_t pixel = (uint32_t(p[3]) << 24) | (uint32_t(p[2]) << 16) | (uint32_t(p[1]) << 8) | p[0];
  context->Unmap(staging.get(), 0);
  return pixel;
}

MonitorRenderRequest requestAtSize(int size) {
  MonitorRenderRequest request;
  request.previewActive = request.multiviewActive = true;
  request.previewPlan.width = request.previewPlan.height = size;
  request.previewPlan.skipCpuReadback = true;
  CompositorRenderPlanLayer layer;
  layer.kind = "participant-video"; layer.participantId = "test";
  layer.sourceId = "test"; layer.layerId = "monitor:test";
  layer.rect = {0, 0, 1, 1}; layer.borderStyle = "none";
  request.previewPlan.layers.push_back(layer);
  request.multiviewPlan = request.programPlan = request.previewPlan;
  VideoFrame frame;
  frame.participantId = "test"; frame.frameId = 1;
  frame.width = frame.pixelWidth = frame.naturalWidth = 64;
  frame.height = frame.pixelHeight = frame.naturalHeight = 64;
  frame.pixelStride = 256;
  auto bytes = std::make_shared<std::vector<uint8_t>>(64 * 64 * 4);
  for (size_t i = 0; i < bytes->size(); i += 4) {
    (*bytes)[i] = 33; (*bytes)[i + 1] = 99; (*bytes)[i + 2] = 177; (*bytes)[i + 3] = 255;
  }
  frame.pixels = bytes;
  request.frames.push_back(frame);
  return request;
}
}

TEST(IsolatedMonitorPixels, IndependentDeviceReceivesPreviewAndMultiviewAcrossResizeAndRetirement) {
  auto compositor = isolatedCompositor();
  ASSERT_TRUE(compositor != nullptr);
  ASSERT_TRUE(compositor->hasIsolatedMonitors());
  int64_t sequence = 0;
  for (int size : {64, 96, 64}) {
    auto request = requestAtSize(size);
    std::shared_ptr<const MonitorRenderResult> result;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    do {
      request.sequence = ++sequence;
      compositor->submitMonitors(request);
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      result = compositor->latestMonitors();
    } while ((!result || result->preview.width != size || result->multiview.width != size ||
              result->preview.sharedHandleHex.empty() || result->multiview.sharedHandleHex.empty()) &&
             std::chrono::steady_clock::now() < deadline);
    ASSERT_TRUE(result != nullptr);
    ASSERT_FALSE(result->preview.sharedHandleHex.empty());
    ASSERT_FALSE(result->multiview.sharedHandleHex.empty());
    EXPECT_EQ(result->preview.width, size);
    EXPECT_EQ(result->multiview.width, size);
    EXPECT_EQ(consumeCenter(result->preview), 0xffb16321u);
    EXPECT_EQ(consumeCenter(result->multiview), 0xffb16321u);
    EXPECT_TRUE(result->sources.empty()); // composite demand does not export individual sources
  }
  MonitorRenderRequest retired;
  retired.sequence = ++sequence;
  compositor->submitMonitors(retired);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (compositor->latestMonitors()->sequence != sequence && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  EXPECT_EQ(compositor->latestMonitors()->sequence, sequence);
  EXPECT_TRUE(compositor->latestMonitors()->preview.sharedHandleHex.empty());
  EXPECT_TRUE(compositor->latestMonitors()->multiview.sharedHandleHex.empty());
  EXPECT_EQ(compositor->monitorDiagnostics().failed, 0u);
}

TEST(GpuCaptureIngress, IndependentProducerImageComposesWithoutCpuPixelsOrAnUpload) {
  const char* raw = std::getenv("COREVIDEO_GPU_CAPTURE");
  const std::string previous = raw ? raw : "";
  _putenv_s("COREVIDEO_GPU_CAPTURE", "1");
  auto compositor = isolatedCompositor();
  _putenv_s("COREVIDEO_GPU_CAPTURE", previous.c_str());
  ASSERT_TRUE(compositor != nullptr);
  ComPtrLite<ID3D11Device> producer;
  ComPtrLite<ID3D11DeviceContext> context;
  ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
      producer.put(), nullptr, context.put())));
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = desc.Height = 64; desc.MipLevels = desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_RENDER_TARGET;
  ComPtrLite<ID3D11Texture2D> source;
  ComPtrLite<ID3D11RenderTargetView> target;
  ASSERT_TRUE(SUCCEEDED(producer->CreateTexture2D(&desc, nullptr, source.put())));
  ASSERT_TRUE(SUCCEEDED(producer->CreateRenderTargetView(source.get(), nullptr, target.put())));
  const float original[] = {177.f / 255, 99.f / 255, 33.f / 255, 1};
  context->ClearRenderTargetView(target.get(), original);
  D3DVideoFramePool pool;
  ASSERT_TRUE(pool.initialize(producer.get(), 64, 64, 1));
  const int slot = pool.beginCopy(context.get(), source.get());
  ASSERT_GE(slot, 0);
  context->Flush();
  std::shared_ptr<const GpuVideoFrame> image;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!(image = pool.completed(context.get(), slot)) && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  ASSERT_TRUE(image != nullptr);
  const float overwritten[] = {0, 0, 0, 1};
  context->ClearRenderTargetView(target.get(), overwritten);
  context->Flush();
  auto request = requestAtSize(64);
  auto frame = request.frames.front();
  frame.pixels.reset();
  frame.gpuPixels = image;
  request.programPlan.skipCpuReadback = false;
  const auto result = compositor->render(request.programPlan, {frame});
  ASSERT_FALSE(result.preview.bgra.empty());
  const auto offset = static_cast<size_t>((result.preview.height / 2) * result.preview.width + result.preview.width / 2) * 4;
  EXPECT_EQ(result.preview.bgra[offset], 33);
  EXPECT_EQ(result.preview.bgra[offset + 1], 99);
  EXPECT_EQ(result.preview.bgra[offset + 2], 177);
  EXPECT_EQ(compositor->sourceTexStats().cachedUploads, 0u);
  EXPECT_EQ(compositor->sourceTexStats().scratchUploads, 0u);
  // All three leases held => no writer may overwrite one to make room.
  std::vector<std::shared_ptr<const GpuVideoFrame>> held{image};
  for (int i = 0; i < 2; ++i) {
    const int next = pool.beginCopy(context.get(), source.get());
    ASSERT_GE(next, 0);
    context->Flush();
    std::shared_ptr<const GpuVideoFrame> lease;
    while (!(lease = pool.completed(context.get(), next)) && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_TRUE(lease != nullptr);
    held.push_back(std::move(lease));
  }
  EXPECT_EQ(pool.beginCopy(context.get(), source.get()), -1);
  held.pop_back();
  EXPECT_GE(pool.beginCopy(context.get(), source.get()), 0);
  context->Flush();

  // A separately colored private image proves the monitor actually samples its
  // branch, while a fake production lease must never survive monitor admission.
  const auto monitorDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  const auto monitorRegistered = [] {
    const auto consumers = D3DVideoConsumers::snapshot();
    return std::any_of(consumers.begin(), consumers.end(), [](const auto& consumer) { return consumer->monitor; });
  };
  while (!monitorRegistered() && std::chrono::steady_clock::now() < monitorDeadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  ASSERT_TRUE(monitorRegistered());
  D3DVideoFramePool monitorPool;
  ASSERT_TRUE(monitorPool.initialize(producer.get(), 64, 64, 2, true));
  const float privateColor[] = {0, 1, 0, 1};
  context->ClearRenderTargetView(target.get(), privateColor);
  const int privateSlot = monitorPool.beginCopy(context.get(), source.get());
  ASSERT_GE(privateSlot, 0);
  context->Flush();
  std::shared_ptr<const GpuVideoFrame> privateImage;
  while (!(privateImage = monitorPool.completed(context.get(), privateSlot)) &&
      std::chrono::steady_clock::now() < monitorDeadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  ASSERT_TRUE(privateImage != nullptr);
  auto productionOnly = std::make_shared<GpuVideoFrame>();
  productionOnly->width = productionOnly->height = 64;
  std::weak_ptr<const GpuVideoFrame> productionLease = productionOnly;
  auto monitorRequest = requestAtSize(64);
  monitorRequest.frames.front().pixels.reset();
  monitorRequest.frames.front().gpuPixels = std::move(productionOnly);
  monitorRequest.frames.front().monitorGpuPixels = privateImage;
  monitorRequest.sequence = 99;
  compositor->submitMonitors(std::move(monitorRequest));
  EXPECT_TRUE(productionLease.expired());
  std::shared_ptr<const MonitorRenderResult> monitorResult;
  do {
    monitorResult = compositor->latestMonitors();
    if (monitorResult && monitorResult->sequence == 99 && !monitorResult->preview.sharedHandleHex.empty()) break;
    auto again = requestAtSize(64);
    again.sequence = 99;
    again.frames.front().pixels.reset();
    again.frames.front().gpuPixels = image;
    again.frames.front().monitorGpuPixels = privateImage;
    compositor->submitMonitors(std::move(again));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  } while (std::chrono::steady_clock::now() < monitorDeadline);
  ASSERT_TRUE(monitorResult != nullptr);
  ASSERT_FALSE(monitorResult->preview.sharedHandleHex.empty());
  EXPECT_EQ(consumeCenter(monitorResult->preview), 0xff00ff00u);
}

TEST(IsolatedMonitorPixels, CameraIdentityBelongsToTheDeliveredNv12Packet) {
  const char* counter = std::getenv("COREVIDEO_QA_PROGRAM_COUNTER");
  const std::string previousCounter = counter ? counter : "";
  _putenv_s("COREVIDEO_QA_PROGRAM_COUNTER", "1");
  auto compositor = isolatedCompositor();
  _putenv_s("COREVIDEO_QA_PROGRAM_COUNTER", previousCounter.c_str());
  ASSERT_TRUE(compositor != nullptr);
  compositor->configureProgramBuffer(2);
  compositor->prepareProgramBuffer(1920, 1080);
  struct Observation {
    int64_t sequence = 0, deliveredAt = 0;
    std::shared_ptr<const std::vector<uint8_t>> pixels;
  } observed;
  std::promise<void> arrived;
  auto arrival = arrived.get_future();
  compositor->setIdentifiedVcamFrameSink([&](auto bytes, int, int, int64_t sequence, int64_t deliveredAt) {
    observed = {sequence, deliveredAt, std::move(bytes)};
    arrived.set_value();
  });
  auto request = requestAtSize(64);
  request.programPlan.width = 1920;
  request.programPlan.height = 1080;
  request.programPlan.fullProgramReadback = true;
  const auto anchor = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  compositor->setProgramProductionTiming(0,
      std::chrono::duration_cast<std::chrono::nanoseconds>(anchor.time_since_epoch()).count());
  const auto produced = compositor->render(request.programPlan, request.frames);
  ProgramFrame delivered;
  const bool received = compositor->takeDeliveredProgramFrame(delivered, 2000);
  const bool notified = arrival.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  compositor->setVcamFrameSink({}); // also clears the identified sink; callee teardown barrier
  ASSERT_TRUE(received);
  ASSERT_TRUE(notified);
  EXPECT_EQ(observed.sequence, produced.frameNumber);
  EXPECT_EQ(observed.sequence, delivered.frameNumber);
  EXPECT_EQ(observed.deliveredAt, delivered.deliveredAt100ns);
  EXPECT_GT(observed.deliveredAt, 0);
  EXPECT_TRUE(observed.pixels != nullptr);
  EXPECT_TRUE(observed.pixels == delivered.programNv12Shared);
  const auto decoded = decodeDeliveryCounter(observed.pixels->data(), observed.pixels->size(), 1920, 1080, 1920);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, static_cast<uint32_t>(observed.sequence));
}

#if COREVIDEO_WITH_WGC
namespace {
// Own a small non-activating moving window: WGC need not produce another frame
// for a static desktop. This generates changes without operator interaction.
class WgcTestMotion {
 public:
  WgcTestMotion() {
    std::promise<bool> initialized; auto ready = initialized.get_future();
    thread_ = std::thread([this, initialized = std::move(initialized)]() mutable {
      POINT origin{20, 20};
      EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
        MONITORINFO info{}; info.cbSize = sizeof(info);
        if (GetMonitorInfo(monitor, &info)) {
          auto* point = reinterpret_cast<POINT*>(data);
          point->x = info.rcMonitor.left + 20; point->y = info.rcMonitor.top + 20;
        }
        return FALSE;
      }, reinterpret_cast<LPARAM>(&origin));
      const auto window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
          L"STATIC", L"CoreVideo capture validation", WS_POPUP,
          origin.x, origin.y, 160, 90, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
      if (window) ShowWindow(window, SW_SHOWNOACTIVATE);
      initialized.set_value(window != nullptr);
      unsigned sequence = 0;
      while (window && !stopping_.load()) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
          TranslateMessage(&message); DispatchMessageW(&message);
        }
        const auto dc = GetDC(window);
        RECT rect{0, 0, 160, 90};
        const auto brush = CreateSolidBrush(RGB(++sequence % 256, 64, 192));
        FillRect(dc, &rect, brush); DeleteObject(brush); ReleaseDC(window, dc);
        GdiFlush(); // flush this thread's batched GDI writes before sleeping
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
      }
      if (window) DestroyWindow(window);
    });
    valid_ = ready.get();
  }
  ~WgcTestMotion() { stopping_.store(true); if (thread_.joinable()) thread_.join(); }
  bool valid() const { return valid_; }
 private:
  std::atomic<bool> stopping_{false}; bool valid_ = false; std::thread thread_;
};
}
TEST(GpuCaptureIngress, OptInRealWgcFrameUsesPreparedGpuViewAndIndependentCpuConsumers) {
  const char* enabled = std::getenv("COREVIDEO_CAPTURE_TESTS");
  if (!enabled || std::string(enabled) != "1") {
    std::fprintf(stderr, "[capture-test] SKIPPED real WGC capture; enable COREVIDEO_CAPTURE_TESTS=1 on an interactive rig\n");
    return;
  }
  WgcTestMotion motion;
  ASSERT_TRUE(motion.valid());
  corevideo::core::ComApartmentLifetime apartment;
  const char* raw = std::getenv("COREVIDEO_GPU_CAPTURE");
  const std::string previous = raw ? raw : "";
  _putenv_s("COREVIDEO_GPU_CAPTURE", "1");
  auto compositor = isolatedCompositor();
  auto capture = createWgcScreenCaptureDevice();
  ASSERT_TRUE(compositor != nullptr);
  ASSERT_TRUE(capture != nullptr);
  const auto devices = capture->enumerate();
  ASSERT_FALSE(devices.empty());
  capture->connect(devices.front().id);
  _putenv_s("COREVIDEO_GPU_CAPTURE", previous.c_str());
  struct Consumer : ICaptureVideoConsumer {
    VideoFrame frame;
    void publish(VideoFrame value) override { frame = std::move(value); }
    void end(const std::string&) override {}
  } consumer;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!consumer.frame.hasGpuPixels() && std::chrono::steady_clock::now() < deadline) {
    capture->deliverVideo(consumer, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(consumer.frame.hasGpuPixels());
  EXPECT_FALSE(consumer.frame.hasPixels());
  std::vector<VideoFrame> cpuFrames;
  const auto cpuDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (cpuFrames.empty() && std::chrono::steady_clock::now() < cpuDeadline) {
    cpuFrames = capture->takeCpuVideoFrames();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_FALSE(cpuFrames.empty());
  EXPECT_TRUE(cpuFrames.front().hasPixels());
  EXPECT_EQ(cpuFrames.front().sourceEpoch, consumer.frame.sourceEpoch);
  EXPECT_GT(cpuFrames.front().captureTimestamp100ns, 0);
  EXPECT_EQ(cpuFrames.front().participantId, consumer.frame.participantId);
  EXPECT_EQ(consumer.frame.gpuPixels->width, consumer.frame.pixelWidth);
  EXPECT_EQ(consumer.frame.gpuPixels->height, consumer.frame.pixelHeight);
  auto request = requestAtSize(64);
  auto& layer = request.programPlan.layers.front();
  layer.participantId = layer.sourceId = consumer.frame.participantId;
  request.programPlan.skipCpuReadback = false;
  const auto output = compositor->render(request.programPlan, {consumer.frame});
  EXPECT_TRUE(output.gpuComposed);
  EXPECT_FALSE(output.preview.bgra.empty());
  EXPECT_EQ(compositor->sourceTexStats().cachedUploads, 0u);
  std::fprintf(stderr, "[capture-test] real WGC %dx%d GPU view consumed; separate CPU arrival verified; uploads=%llu\n",
      consumer.frame.pixelWidth, consumer.frame.pixelHeight,
      static_cast<unsigned long long>(compositor->sourceTexStats().cachedUploads));
  const auto mirroredId = consumer.frame.frameId;
  capture->setVideoConsumerDemand({});
  capture->takeCpuVideoFrames(); // drain work admitted before demand release
  const auto gpuOnlyDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < gpuOnlyDeadline) {
    capture->deliverVideo(consumer, 0);
    if (consumer.frame.frameId > mirroredId && consumer.frame.hasGpuPixels() && !consumer.frame.hasPixels()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_GT(consumer.frame.frameId, mirroredId);
  ASSERT_TRUE(consumer.frame.hasGpuPixels());
  ASSERT_FALSE(consumer.frame.hasPixels());
  const auto gpuOnly = compositor->render(request.programPlan, {consumer.frame});
  EXPECT_TRUE(gpuOnly.gpuComposed);
  EXPECT_FALSE(gpuOnly.preview.bgra.empty());
  EXPECT_EQ(compositor->sourceTexStats().cachedUploads, 0u);
  const auto gpuOnlyId = consumer.frame.frameId;
  // Exhaust only the optional monitor pool. Production must continue receiving
  // GPU-only pictures rather than borrowing those slots or falling back to CPU.
  std::vector<std::shared_ptr<const GpuVideoFrame>> heldMonitors;
  const auto retainDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (heldMonitors.size() < 3 && std::chrono::steady_clock::now() < retainDeadline) {
    capture->deliverVideo(consumer, 0);
    const auto& image = consumer.frame.monitorGpuPixels;
    if (image && std::find(heldMonitors.begin(), heldMonitors.end(), image) == heldMonitors.end())
      heldMonitors.push_back(image);
    if (consumer.frame.hasGpuPixels()) compositor->render(request.programPlan, {consumer.frame});
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_EQ(heldMonitors.size(), 3u);
  auto lastId = consumer.frame.frameId;
  int advanced = 0;
  bool allGpuOnly = true;
  int polls = 0; int64_t renderUs = 0, captureUs = 0;
  const auto pressureDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (advanced < 20 && std::chrono::steady_clock::now() < pressureDeadline) {
    const auto captureStart = std::chrono::steady_clock::now();
    capture->deliverVideo(consumer, 0);
    captureUs += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - captureStart).count();
    ++polls;
    if (consumer.frame.frameId > lastId) {
      lastId = consumer.frame.frameId;
      ++advanced;
      allGpuOnly = allGpuOnly && consumer.frame.hasGpuPixels() && !consumer.frame.hasPixels();
    }
    // Production keeps rendering held pictures. That also retires completed
    // GPU reads; only rendering on source changes can strand the test's leases.
    const auto renderStart = std::chrono::steady_clock::now();
    if (consumer.frame.hasGpuPixels()) compositor->render(request.programPlan, {consumer.frame});
    renderUs += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - renderStart).count();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  heldMonitors.clear();
  std::fprintf(stderr, "[capture-test] pressure advanced=%d latest=%lld allGpuOnly=%d\n",
      advanced, static_cast<long long>(lastId), allGpuOnly ? 1 : 0);
  std::fprintf(stderr, "[capture-test] polls=%d captureUs=%lld renderUs=%lld\n", polls,
      static_cast<long long>(captureUs), static_cast<long long>(renderUs));
  EXPECT_EQ(advanced, 20);
  EXPECT_TRUE(allGpuOnly);
  EXPECT_EQ(compositor->sourceTexStats().cachedUploads, 0u);
  capture->setVideoConsumerDemand({{consumer.frame.participantId, SourceVideoConsumer::Iso,
      "recording", SourceVideoRepresentation::Cpu}});
  cpuFrames.clear();
  const auto isoDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < isoDeadline) {
    capture->deliverVideo(consumer, 0);
    if (consumer.frame.hasGpuPixels()) compositor->render(request.programPlan, {consumer.frame});
    auto arrived = capture->takeCpuVideoFrames();
    for (auto& frame : arrived) if (frame.frameId > lastId) cpuFrames.push_back(std::move(frame));
    if (!cpuFrames.empty()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_GT(consumer.frame.frameId, gpuOnlyId);
  ASSERT_FALSE(cpuFrames.empty());
  EXPECT_TRUE(cpuFrames.front().hasPixels());
  EXPECT_EQ(cpuFrames.front().sourceEpoch, consumer.frame.sourceEpoch);
  EXPECT_GT(cpuFrames.front().captureTimestamp100ns, 0);
  EXPECT_FALSE(consumer.frame.hasPixels());
  EXPECT_TRUE(consumer.frame.hasGpuPixels());
  std::fprintf(stderr, "[capture-test] GPU-only/ISO transitions passed; %d new production frames with all monitor slots retained\n", advanced);
  capture->disconnect(devices.front().id);
}
#endif
#endif
