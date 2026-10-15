#include "modules/Interfaces.h"
#include "MonitorTestMode.h"
#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include <cmath>
#include <atomic>
#include <future>

#if defined(_WIN32) && !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_D3D11
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <mmsystem.h>
#include "modules/D3DProgramBuffer.h"
#include "modules/D3DProgramCopyOwner.h"
#include "modules/MonitorRenderWorker.h"
#include "modules/MonitorInputEvidence.h"

namespace {
// JsonRpcServer negotiates this resolution before running the real render
// cadence. Filtered hardware tests must establish the same environment instead
// of depending on a previously run server test to leave the timer configured.
struct ProgramBufferTimerResolution {
  MMRESULT result = timeBeginPeriod(1);
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  ~ProgramBufferTimerResolution() { if (timer) CloseHandle(timer); if (result == TIMERR_NOERROR) timeEndPeriod(1); }
  void waitUntil(std::chrono::steady_clock::time_point target) {
    const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(target - std::chrono::steady_clock::now()).count();
    if (remaining <= 0) return;
    LARGE_INTEGER due; due.QuadPart = -(std::max)(int64_t(1), (remaining + 99) / 100);
    if (!timer || !SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) || WaitForSingleObject(timer, 50) != WAIT_OBJECT_0)
      throw std::runtime_error("phase fixture high-resolution wait failed");
  }
};
}

namespace corevideo::modules {
struct D3DProgramDisplayReaderTestAccess {
  static void fault(D3DProgramDisplayReader& reader,
      std::function<std::optional<HRESULT>(ProgramDisplayReason)> fault) { reader.fault_ = std::move(fault); }
  static void clock(D3DProgramDisplayReader& reader, std::function<int64_t()> clock) { reader.clock_ = std::move(clock); }
};
struct D3DProgramBufferTestAccess {
  static void beforeExport(D3DProgramBuffer& buffer, std::function<void(bool)> hook) { buffer.beforeExport_ = std::move(hook); }
  static void beforePrepare(D3DProgramBuffer& buffer, std::function<void(int64_t)> hook) { buffer.beforePrepare_ = std::move(hook); }
  static unsigned readLeases(D3DProgramBuffer& buffer) {
    std::lock_guard<std::mutex> lock(buffer.mutex_);
    unsigned leases = 0; for (const auto& slot : buffer.slots_) leases += slot->readers;
    return leases;
  }
  static void fail(D3DProgramBuffer& buffer) { buffer.fail("test-injected"); }
};
}

TEST(D3DProgramBuffer, DeadlineCopiesContinueWhileOptionalOwnerIsBlockedAndKeepExactPixels) {
  ProgramBufferTimerResolution timer;
  using namespace corevideo::modules;
  ComPtrLite<ID3D11Device> producer, monitor;
  ComPtrLite<ID3D11DeviceContext> producerContext, monitorContext;
  ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, producer.put(), nullptr, producerContext.put())));
  ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, monitor.put(), nullptr, monitorContext.put())));
  auto registration = D3DVideoConsumers::add(monitor.get(), true);
  ComPtrLite<IDXGIDevice> dxgi; ComPtrLite<IDXGIAdapter> adapter;
  ASSERT_TRUE(SUCCEEDED(producer->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(dxgi.put()))));
  ASSERT_TRUE(SUCCEEDED(dxgi->GetAdapter(adapter.put())));
  std::atomic<bool> entered{false}, released{false};
  std::atomic<int> firstPixel{-1}, lastPixel{-1};
  D3D11_TEXTURE2D_DESC desc{}; desc.Width = desc.Height = 64; desc.MipLevels = desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtrLite<ID3D11Texture2D> staging;
  ASSERT_TRUE(SUCCEEDED(monitor->CreateTexture2D(&desc, nullptr, staging.put())));
  MonitorRenderWorker optional([&](const MonitorRenderRequest& request) {
    if (!entered.exchange(true)) while (!released.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const auto image = std::dynamic_pointer_cast<const D3DVideoImage>(request.readyProgramImage);
    const auto* view = image ? image->view(registration->id) : nullptr;
    if (!view) throw std::runtime_error("completed Program view missing");
    monitorContext->CopyResource(staging.get(), view->texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(monitorContext->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) throw std::runtime_error("pixel read failed");
    const int pixel = static_cast<const uint8_t*>(mapped.pData)[0]; monitorContext->Unmap(staging.get(), 0);
    if (firstPixel.load() < 0) firstPixel.store(pixel);
    lastPixel.store(pixel);
    return MonitorRenderResult{};
  });
  struct Release { std::atomic<bool>& gate; ~Release() { gate.store(true); } } release{released};
  auto owner = std::make_shared<std::unique_ptr<D3DProgramCopyOwner>>();
  MonitorRenderWorker critical([&optional, owner](const MonitorRenderRequest& request) {
    optional.submit((*owner)->copy(request));
    MonitorRenderResult result; result.programDisplayReader = (*owner)->observation(); return result;
  }, [owner, &adapter] { *owner = std::make_unique<D3DProgramCopyOwner>(adapter.get(), 64, 64); }, {}, false);
  ASSERT_TRUE(critical.waitForInitialization(std::chrono::seconds(2)));
  D3DProgramDisplaySnapshots input; ASSERT_TRUE(input.initialize(producer.get(), 64, 64, 904));
  D3D11_QUERY_DESC queryDesc{D3D11_QUERY_EVENT, 0}; ComPtrLite<ID3D11Query> produced;
  ASSERT_TRUE(SUCCEEDED(producer->CreateQuery(&queryDesc, produced.put())));
  const auto anchor = std::chrono::steady_clock::now() + std::chrono::milliseconds(30);
  for (int n = 1; n <= 24; ++n) {
    const auto target = anchor + std::chrono::nanoseconds((n - 1) * 1000000000LL / 60);
    std::this_thread::sleep_until(target);
    auto slot = input.reserve(); ASSERT_TRUE(slot != nullptr);
    std::vector<uint8_t> pixels(64 * 64 * 4, static_cast<uint8_t>(n));
    producerContext->UpdateSubresource(slot->texture.get(), 0, nullptr, pixels.data(), 64 * 4, 0);
    producerContext->End(produced.get()); producerContext->Flush();
    BOOL ready = FALSE;
    const auto deadline = target + std::chrono::nanoseconds(1000000000LL / 60);
    while (producerContext->GetData(produced.get(), &ready, sizeof(ready), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_FALSE &&
        std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::microseconds(100));
    ASSERT_TRUE(ready);
    ProgramFrame frame; frame.deliverySequence = frame.frameNumber = n;
    frame.timelineTimestamp100ns = std::chrono::duration_cast<std::chrono::nanoseconds>(target.time_since_epoch()).count()/100;
    input.stamp(slot, frame);
    MonitorRenderRequest request; request.sequence = n; request.multiviewActive = request.bufferedProgram = true;
    request.programDisplayTargetNs = std::chrono::duration_cast<std::chrono::nanoseconds>(target.time_since_epoch()).count();
    request.programDisplayDeadlineNs = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline.time_since_epoch()).count();
    request.deliveredProgramOwner = slot; critical.submit(std::move(request));
    while ((!critical.latest() || critical.latest()->sequence < n) && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    ASSERT_TRUE(critical.latest() && critical.latest()->sequence == n);
    const auto observed = critical.latest()->programDisplayReader;
    EXPECT_EQ(observed.copies, static_cast<uint64_t>(n)); EXPECT_EQ(observed.completedCopies, static_cast<uint64_t>(n));
    EXPECT_EQ(observed.refused, 0u); EXPECT_EQ(observed.skipped, 0u); EXPECT_EQ(observed.gpuReadinessDeadlineMisses, 0u);
  }
  ASSERT_TRUE(entered.load()); EXPECT_EQ(optional.diagnostics().completed, 0u);
  released.store(true);
  const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (lastPixel.load() != 24 && std::chrono::steady_clock::now() < limit) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  EXPECT_EQ(firstPixel.load(), 1); EXPECT_EQ(lastPixel.load(), 24); EXPECT_EQ(optional.diagnostics().failed, 0u);
}

TEST(D3DProgramBuffer, PrivateOutputPressureNeedsIndependentConsumerRetirementAndKeepsOriginalDeadline) {
  ProgramBufferTimerResolution timer;
  using namespace corevideo::modules;
  ComPtrLite<ID3D11Device> producer, critical, monitor;
  ComPtrLite<ID3D11DeviceContext> producerContext, criticalContext, monitorContext;
  for (auto pair : {std::make_pair(&producer, &producerContext), std::make_pair(&critical, &criticalContext), std::make_pair(&monitor, &monitorContext)})
    ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, pair.first->put(), nullptr, pair.second->put())));
  auto criticalRegistration = D3DVideoConsumers::add(critical.get(), true);
  auto monitorRegistration = D3DVideoConsumers::add(monitor.get(), true);
  for (bool independentlyRetire : {false, true}) {
    D3DProgramDisplaySnapshots input; ASSERT_TRUE(input.initialize(producer.get(), 64, 64, 904));
    D3DProgramDisplayReader reader; reader.prewarm(critical.get(), criticalContext.get(), 64, 64, true);
    D3DVideoReadLeases leases(monitor.get(), monitorContext.get());
    std::vector<std::shared_ptr<D3DProgramDisplaySnapshots::Snapshot>> inputs;
    for (int n = 1; n <= 3; ++n) {
      auto slot = input.reserve(); ASSERT_TRUE(slot != nullptr); inputs.push_back(slot);
      ProgramFrame frame; frame.frameNumber = frame.deliverySequence = n; input.stamp(slot, frame);
      reader.copy(critical.get(), criticalContext.get(), slot);
      MonitorRenderRequest request; request.programDisplayDeadlineNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
          (std::chrono::steady_clock::now()+std::chrono::seconds(1)).time_since_epoch()).count();
      reader.completeBeforeAuxiliary(criticalContext.get(), request);
      ASSERT_TRUE(reader.completedImage() != nullptr); ASSERT_TRUE(leases.hold(reader.completedImage()));
    }
    // All three destinations are held by an unfinished consumer batch, while
    // the critical input queries have already completed. No new image can make
    // the consumer's old lazy hold()-time polling run.
    EXPECT_EQ(reader.observation().gpuReads, 0);
    auto slot = input.reserve(); ASSERT_TRUE(slot != nullptr);
    ProgramFrame frame; frame.frameNumber = frame.deliverySequence = 4; input.stamp(slot, frame);
    std::future<void> release;
    if (independentlyRetire) release = std::async(std::launch::async, [&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      leases.finish(); monitorContext->Flush();
      const auto limit = std::chrono::steady_clock::now()+std::chrono::seconds(1);
      while (leases.retire() && std::chrono::steady_clock::now()<limit) std::this_thread::sleep_for(std::chrono::microseconds(100));
    });
    const auto target = std::chrono::steady_clock::now();
    const auto targetNs = std::chrono::duration_cast<std::chrono::nanoseconds>(target.time_since_epoch()).count();
    MonitorRenderRequest request; request.programDisplayTargetNs = targetNs; request.programDisplayDeadlineNs = targetNs+10000000;
    reader.copy(critical.get(), criticalContext.get(), slot, targetNs, request.programDisplayDeadlineNs, &request);
    reader.completeBeforeAuxiliary(criticalContext.get(), request);
    if (release.valid()) release.get();
    else { leases.finish(); monitorContext->Flush(); while (leases.retire()) std::this_thread::sleep_for(std::chrono::microseconds(100)); }
    const auto observed = reader.observation();
    EXPECT_EQ(observed.capacityPressures, 1u);
    EXPECT_EQ(observed.refused, independentlyRetire ? 0u : 1u);
    EXPECT_EQ(observed.copies, independentlyRetire ? 4u : 3u);
    EXPECT_EQ(observed.admittedAfterPressure, independentlyRetire ? 1u : 0u);
    EXPECT_EQ(observed.gpuReadinessDeadlineMisses, 0u);
  }
}

TEST(D3DProgramBuffer, DisplayReaderUsesSetupImportsForEverySlotWithoutCriticalDriverImport) {
  using namespace corevideo::modules;
  ComPtrLite<ID3D11Device> producer, consumer;
  ComPtrLite<ID3D11DeviceContext> producerContext, consumerContext;
  ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, producer.put(), nullptr, producerContext.put())));
  ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, consumer.put(), nullptr, consumerContext.put())));
  auto registration = D3DVideoConsumers::add(consumer.get(), true);
  D3DProgramDisplaySnapshots pool; ASSERT_TRUE(pool.initialize(producer.get(), 64, 64, 945));
  D3DProgramDisplayReader reader; reader.prewarm(consumer.get(), consumerContext.get(), 64, 64);
  D3DProgramDisplayReaderTestAccess::fault(reader, [](ProgramDisplayReason reason) -> std::optional<HRESULT> {
    return reason == ProgramDisplayReason::Import ? std::optional<HRESULT>(E_FAIL) : std::nullopt;
  });
  D3D11_TEXTURE2D_DESC desc{}; desc.Width = desc.Height = 64; desc.MipLevels = desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtrLite<ID3D11Texture2D> staging;
  ASSERT_TRUE(SUCCEEDED(consumer->CreateTexture2D(&desc, nullptr, staging.put())));
  D3D11_QUERY_DESC queryDesc{D3D11_QUERY_EVENT, 0}; ComPtrLite<ID3D11Query> produced;
  ASSERT_TRUE(SUCCEEDED(producer->CreateQuery(&queryDesc, produced.put())));
  std::vector<std::shared_ptr<D3DProgramDisplaySnapshots::Snapshot>> leases;
  for (size_t index = 0; index < D3DProgramDisplaySnapshots::Capacity; ++index) {
    auto slot = pool.reserve(); ASSERT_TRUE(slot != nullptr); leases.push_back(slot);
    std::vector<uint8_t> pixels(64 * 64 * 4, static_cast<uint8_t>(17 + index));
    producerContext->UpdateSubresource(slot->texture.get(), 0, nullptr, pixels.data(), 64 * 4, 0);
    producerContext->End(produced.get()); producerContext->Flush();
    BOOL ready = FALSE; const auto limit = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (producerContext->GetData(produced.get(), &ready, sizeof(ready), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_FALSE &&
        std::chrono::steady_clock::now() < limit) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_TRUE(ready);
    ProgramFrame frame; frame.deliverySequence = frame.frameNumber = index + 1; pool.stamp(slot, frame);
    auto* view = reader.copy(consumer.get(), consumerContext.get(), slot); ASSERT_TRUE(view != nullptr);
    ComPtrLite<ID3D11Resource> image; view->GetResource(image.put());
    consumerContext->CopyResource(staging.get(), image.get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    ASSERT_TRUE(SUCCEEDED(consumerContext->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)));
    EXPECT_EQ(static_cast<const uint8_t*>(mapped.pData)[0], static_cast<uint8_t>(17 + index));
    consumerContext->Unmap(staging.get(), 0); reader.retire(consumerContext.get(), 0);
  }
  EXPECT_EQ(reader.observation().copies, D3DProgramDisplaySnapshots::Capacity);
  EXPECT_EQ(reader.observation().refused, 0u);
}

TEST(D3DProgramBuffer, DisplayReaderAttributesEachResourceFailureAndExportsExactIncident) {
  using namespace corevideo::modules;
  ComPtrLite<ID3D11Device> device;
  ComPtrLite<ID3D11DeviceContext> context;
  ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, context.put())));
  D3DProgramDisplaySnapshots pool;
  ASSERT_TRUE(pool.initialize(device.get(), 64, 64, 904));
  auto slot = pool.reserve(); ASSERT_TRUE(slot != nullptr);
  ProgramFrame frame; frame.deliverySequence = frame.frameNumber = 91;
  pool.stamp(slot, frame);
  std::shared_ptr<const void> owner = slot;
  for (auto reason : {ProgramDisplayReason::QueryCreate, ProgramDisplayReason::Import,
      ProgramDisplayReason::TextureCreate, ProgramDisplayReason::SrvCreate}) {
    D3DProgramDisplayReader reader;
    D3DProgramDisplayReaderTestAccess::fault(reader, [reason](ProgramDisplayReason actual) -> std::optional<HRESULT> {
      if (actual == reason) return E_OUTOFMEMORY;
      return {};
    });
    EXPECT_EQ(reader.copy(device.get(), context.get(), owner), nullptr);
    auto evidence = reader.observation();
    EXPECT_EQ(evidence.refused, 1u);
    EXPECT_EQ(evidence.refusalReasons[static_cast<size_t>(reason)], 1u);
    EXPECT_EQ(evidence.copies, 0u); EXPECT_EQ(evidence.gpuReads, 0);
    ASSERT_TRUE(evidence.incidentTrace != nullptr);
    const auto& event = evidence.incidentTrace->back();
    EXPECT_EQ(event.reason, reason); EXPECT_EQ(event.hresult, E_OUTOFMEMORY);
    EXPECT_EQ(event.requested, 91u); EXPECT_EQ(event.generation, 904u);
    EXPECT_EQ(event.width, 64u); EXPECT_EQ(event.deviceHresult, S_OK);
    auto result = std::make_shared<MonitorRenderResult>(); result->programDisplayReader = evidence;
    const auto json = programDisplayEvidence(result);
    EXPECT_EQ(json.get("latestReader")->getNumber("readerReadLimit"), 2);
    EXPECT_EQ(json.get("latestReader")->get("incidentTrace")->asArray().back().getString("requestedSequence"), "91");
    EXPECT_FALSE(json.get("observed")->asBool()); // No invented exported-image attribution.
    EXPECT_FALSE(json.get("displayPresentationVerified")->asBool());
    D3DProgramDisplayReaderTestAccess::fault(reader, {});
    if (reason != ProgramDisplayReason::QueryCreate) {
      EXPECT_NE(reader.copy(device.get(), context.get(), owner), nullptr);
      EXPECT_EQ(reader.observation().copies, 1u);
      EXPECT_EQ(reader.observation().refused, 1u);
      EXPECT_EQ(reader.observation().skipped, 0u);
      context->Flush();
    }
  }
}

TEST(D3DProgramBuffer, DisplayReaderCapacityRefusalRecordsPendingIdentitiesWithoutReleasingThem) {
  using namespace corevideo::modules;
  ComPtrLite<ID3D11Device> device;
  ComPtrLite<ID3D11DeviceContext> context;
  ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, context.put())));
  D3DProgramDisplaySnapshots pool;
  ASSERT_TRUE(pool.initialize(device.get(), 64, 64, 905));
  D3DProgramDisplayReader reader;
  D3DProgramDisplayReaderTestAccess::fault(reader, [](ProgramDisplayReason reason) -> std::optional<HRESULT> {
    return reason == ProgramDisplayReason::QueryRead ? std::optional<HRESULT>(S_FALSE) : std::nullopt;
  });
  std::vector<std::shared_ptr<D3DProgramDisplaySnapshots::Snapshot>> slots;
  for (int i = 1; i <= 3; ++i) {
    auto slot = pool.reserve(); ASSERT_TRUE(slot != nullptr);
    ProgramFrame frame; frame.deliverySequence = frame.frameNumber = i;
    pool.stamp(slot, frame); slots.push_back(slot);
    reader.copy(device.get(), context.get(), slot);
  }
  const auto observation = reader.observation();
  EXPECT_EQ(observation.copies, 2u); EXPECT_EQ(observation.completedCopies, 0u);
  EXPECT_EQ(observation.refusalReasons[static_cast<size_t>(ProgramDisplayReason::ReadCapacity)], 1u);
  EXPECT_EQ(observation.notReadyPolls, 3u);
  ASSERT_TRUE(observation.incidentTrace != nullptr);
  const auto& incident = observation.incidentTrace->back();
  EXPECT_EQ(incident.pending, 2u); EXPECT_EQ(incident.requested, 3u);
  EXPECT_EQ(incident.pendingSequences[0], 1u); EXPECT_EQ(incident.pendingSequences[1], 2u);
  EXPECT_GE(incident.pendingAgesNs[0], 0);
  EXPECT_FALSE(slots[0]->quarantined.load()); // Retained, not prematurely recycled.
  // A genuine query failure is separately attributed and cannot release either lease.
  D3DProgramDisplayReaderTestAccess::fault(reader, [](ProgramDisplayReason reason) -> std::optional<HRESULT> {
    return reason == ProgramDisplayReason::QueryRead ? std::optional<HRESULT>(E_FAIL) : std::nullopt;
  });
  reader.retire(context.get());
  EXPECT_EQ(reader.observation().gpuReads, 2);
  EXPECT_EQ(reader.observation().refusalReasons[static_cast<size_t>(ProgramDisplayReason::QueryRead)], 1u);
  EXPECT_EQ(reader.observation().state, "failed");
  EXPECT_TRUE(slots[0]->quarantined.load());
  for (int i = 0; i < 600; ++i) reader.copy(device.get(), context.get(), slots[2]);
  EXPECT_EQ(reader.observation().incidentTrace->size(), D3DProgramDisplayReader::TraceCapacity);
  EXPECT_GT(reader.observation().traceOverwritten, 0u);
  EXPECT_EQ(reader.observation().traceCaptureFailures, 0u);
  EXPECT_EQ(reader.observation().refusalReasons[static_cast<size_t>(ProgramDisplayReason::FailedReader)], 600u);
  EXPECT_EQ(reader.observation().copies, 2u);
  context->Flush();
}

TEST(D3DProgramBuffer, DisplayReaderRetriesSameIdentityWithinOriginalDeadlineAndHonorsExpiryAndCancellation) {
  using namespace corevideo::modules;
  ComPtrLite<ID3D11Device> device;
  ComPtrLite<ID3D11DeviceContext> context;
  ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, context.put())));
  auto pixel = [&](ID3D11ShaderResourceView* view) {
    ComPtrLite<ID3D11Resource> source;
    ComPtrLite<ID3D11Texture2D> texture, staging;
    view->GetResource(source.put());
    EXPECT_TRUE(SUCCEEDED(source->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(texture.put()))));
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = desc.MiscFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    EXPECT_TRUE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, staging.put())));
    context->CopyResource(staging.get(), texture.get()); context->Flush();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    EXPECT_TRUE(SUCCEEDED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)));
    const int value = static_cast<const uint8_t*>(mapped.pData)[32 * mapped.RowPitch + 32 * 4];
    context->Unmap(staging.get(), 0); return value;
  };
  // Functional clock, not a hardware timing qualification: the completion
  // seam controls observation availability; staging pixels independently prove
  // the real submitted copy. No simulated success releases a GPU-owned lease.
  for (int mode = 0; mode < 5; ++mode) { // legacy one-poll, delayed retry, expired, cancelled, immediate recheck
    D3DProgramDisplaySnapshots pool;
    ASSERT_TRUE(pool.initialize(device.get(), 64, 64, 1000 + mode));
    D3DProgramDisplayReader reader;
    int64_t clock = 1000000000;
    const int64_t target = clock, deadline = clock + 16666667;
    D3DProgramDisplayReaderTestAccess::clock(reader, [&] { return clock; });
    D3DProgramDisplayReaderTestAccess::fault(reader, [](ProgramDisplayReason reason) -> std::optional<HRESULT> {
      return reason == ProgramDisplayReason::QueryRead ? std::optional<HRESULT>(S_FALSE) : std::nullopt;
    });
    std::vector<std::shared_ptr<D3DProgramDisplaySnapshots::Snapshot>> slots;
    ID3D11ShaderResourceView* view = nullptr;
    for (int sequence = 1; sequence <= 3; ++sequence) {
      auto slot = pool.reserve(); ASSERT_TRUE(slot != nullptr);
      std::vector<uint8_t> color(64 * 64 * 4, static_cast<uint8_t>(sequence * 60));
      for (size_t i = 3; i < color.size(); i += 4) color[i] = 255;
      context->UpdateSubresource(slot->texture.get(), 0, nullptr, color.data(), 256, 0);
      ProgramFrame frame; frame.frameNumber = frame.deliverySequence = sequence;
      pool.stamp(slot, frame); slots.push_back(slot);
      if (sequence < 3) view = reader.copy(device.get(), context.get(), slot);
    }
    ASSERT_TRUE(view != nullptr);
    EXPECT_EQ(pixel(view), 120); // Genuine completion, while query observation remains held.
    auto cancellation = std::make_shared<MonitorCancellationState>();
    MonitorRenderRequest request; request.cancellation = cancellation;
    unsigned polls = 0;
    D3DProgramDisplayReaderTestAccess::fault(reader, [&](ProgramDisplayReason reason) -> std::optional<HRESULT> {
      if (reason != ProgramDisplayReason::QueryRead) return {};
      ++polls;
      if (polls > 20) clock = deadline; // Broken progress cannot wedge the functional test.
      if (polls > 2 && mode == 2) clock = deadline;
      if (polls > 2 && mode == 3) cancellation->stopped = true;
      if (mode == 1 && polls > 4) return {}; // Real GetData, after two held admission/recheck polls.
      if (mode == 4 && polls > 2) return {}; // The pressure-only flushing recheck can retire real completed reads.
      return S_FALSE;
    });
    view = reader.copy(device.get(), context.get(), slots[2], target, mode == 0 ? 0 : deadline, &request);
    const auto evidence = reader.observation();
    EXPECT_EQ(evidence.capacityPressures, 1u);
    EXPECT_EQ(evidence.lateSubmissions, 0u);
    if (mode == 1 || mode == 4) {
      EXPECT_EQ(reader.copied(), 3u); EXPECT_EQ(pixel(view), 180);
      EXPECT_EQ(evidence.refused, 0u); EXPECT_EQ(evidence.skipped, 0u);
      EXPECT_EQ(evidence.admittedAfterPressure, 1u);
      EXPECT_EQ(evidence.gpuReads, 1);
      if (mode == 4) EXPECT_EQ(evidence.flushingPollRetirements, 2u);
    } else {
      EXPECT_EQ(reader.requested(), 3u); EXPECT_EQ(reader.copied(), 2u);
      EXPECT_EQ(pixel(view), 120); EXPECT_EQ(evidence.gpuReads, 2);
      const auto reason = mode == 0 ? ProgramDisplayReason::ReadCapacity :
          mode == 2 ? ProgramDisplayReason::Deadline : ProgramDisplayReason::Cancelled;
      EXPECT_EQ(evidence.refusalReasons[static_cast<size_t>(reason)], 1u);
      EXPECT_EQ(evidence.refused, mode == 3 ? 0u : 1u);
    }
    D3DProgramDisplayReaderTestAccess::fault(reader, {});
    reader.retire(context.get());
    EXPECT_EQ(reader.observation().gpuReads, 0);
    EXPECT_FALSE(slots[0]->quarantined.load());
  }
}

TEST(D3DProgramBuffer, DisplayReaderRetainsOneReadinessDeadlineFailureAfterLateRecovery) {
  using namespace corevideo::modules;
  ComPtrLite<ID3D11Device> device;
  ComPtrLite<ID3D11DeviceContext> context;
  ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, context.put())));
  D3DProgramDisplaySnapshots pool;
  ASSERT_TRUE(pool.initialize(device.get(), 64, 64, 930));
  auto slot = pool.reserve(); ASSERT_TRUE(slot != nullptr);
  ProgramFrame frame; frame.frameNumber = frame.deliverySequence = 9; pool.stamp(slot, frame);
  D3DProgramDisplayReader reader;
  int64_t clock = 1000000000;
  const auto target = clock, deadline = clock + 16666667;
  D3DProgramDisplayReaderTestAccess::clock(reader, [&] { return clock; });
  ASSERT_TRUE(reader.copy(device.get(), context.get(), slot, target, deadline) != nullptr);
  D3DProgramDisplayReaderTestAccess::fault(reader, [](ProgramDisplayReason reason) -> std::optional<HRESULT> {
    return reason == ProgramDisplayReason::QueryRead ? std::optional<HRESULT>(S_FALSE) : std::nullopt;
  });
  clock = deadline;
  reader.retire(context.get()); reader.retire(context.get());
  auto evidence = reader.observation();
  EXPECT_EQ(evidence.gpuReadinessDeadlineMisses, 1u);
  EXPECT_EQ(evidence.completedCopies, 0u); EXPECT_EQ(evidence.gpuReads, 1);
  EXPECT_FALSE(slot->quarantined.load()); // Still leased; missed readiness cannot permit reuse.
  ASSERT_TRUE(evidence.incidentTrace != nullptr);
  bool attributed = false;
  for (const auto& event : *evidence.incidentTrace) if (event.stage == 9) {
    attributed = true;
    EXPECT_EQ(event.subjectSequence, 9u); EXPECT_EQ(event.subjectGeneration, 930u);
    EXPECT_EQ(event.deadlineNs, deadline); EXPECT_EQ(event.observedNs, deadline);
  }
  EXPECT_TRUE(attributed);
  D3DProgramDisplayReaderTestAccess::fault(reader, {});
  context->Flush(); ++clock;
  const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (reader.needsRetirement() && std::chrono::steady_clock::now() < limit) {
    reader.retire(context.get(), 0); std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  evidence = reader.observation();
  EXPECT_EQ(evidence.gpuReads, 0); EXPECT_EQ(evidence.completedCopies, 1u);
  EXPECT_EQ(evidence.gpuDeadlineCompletedReads, 1u);
  EXPECT_EQ(evidence.gpuReadinessDeadlineMisses, 1u); // Recovery cannot reset away the failed deadline.
}

TEST(D3DProgramBuffer, DisplayReaderCriticalCompletionUsesOriginalDeadlineAndHonorsCancellation) {
  using namespace corevideo::modules;
  ComPtrLite<ID3D11Device> device;
  ComPtrLite<ID3D11DeviceContext> context;
  ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, context.put())));
  D3DProgramDisplaySnapshots pool;
  ASSERT_TRUE(pool.initialize(device.get(), 64, 64, 931));
  auto slot = pool.reserve(); ASSERT_TRUE(slot != nullptr);
  ProgramFrame frame; frame.frameNumber = frame.deliverySequence = 10; pool.stamp(slot, frame);
  D3DProgramDisplayReader reader;
  int64_t clock = 1000000000;
  D3DProgramDisplayReaderTestAccess::clock(reader, [&] { return clock; });
  MonitorRenderRequest request; request.programDisplayDeadlineNs = clock + 16666667;
  auto cancellation = std::make_shared<MonitorCancellationState>(); request.cancellation = cancellation;
  ASSERT_TRUE(reader.copy(device.get(), context.get(), slot, clock, request.programDisplayDeadlineNs, &request) != nullptr);
  int polls = 0;
  D3DProgramDisplayReaderTestAccess::fault(reader, [&](ProgramDisplayReason reason) -> std::optional<HRESULT> {
    if (reason != ProgramDisplayReason::QueryRead) return std::nullopt;
    ++polls; clock += 1000000; return S_FALSE;
  });
  cancellation->stopped.store(true);
  reader.completeBeforeAuxiliary(context.get(), request);
  EXPECT_EQ(polls, 0); EXPECT_EQ(reader.observation().gpuReads, 1);
  cancellation->stopped.store(false);
  reader.completeBeforeAuxiliary(context.get(), request);
  EXPECT_EQ(polls, 17); EXPECT_EQ(reader.observation().gpuReadinessDeadlineMisses, 1u);
  EXPECT_EQ(reader.observation().completedCopies, 0u); EXPECT_EQ(reader.refused(), 0u);
  EXPECT_EQ(request.programDisplayDeadlineNs, 1016666667);
  D3DProgramDisplayReaderTestAccess::fault(reader, {});
  const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (reader.needsRetirement() && std::chrono::steady_clock::now() < limit) {
    reader.retire(context.get(), 0); std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_EQ(reader.observation().gpuReads, 0);
  EXPECT_EQ(reader.observation().gpuReadinessDeadlineMisses, 1u);
}

TEST(D3DProgramBuffer, ExactDisplayPublicationCannotSubstituteAnOlderIdentity) {
  using namespace corevideo::modules;
  ComPtrLite<ID3D11Device> device;
  ComPtrLite<ID3D11DeviceContext> context;
  ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, context.put())));
  D3DProgramDisplaySnapshots pool;
  ASSERT_TRUE(pool.initialize(device.get(), 64, 64, 932));
  auto old = pool.reserve(); ASSERT_TRUE(old != nullptr);
  ProgramFrame frame; frame.frameNumber = frame.deliverySequence = 4; frame.timelineTimestamp100ns = 100;
  pool.stamp(old, frame); pool.publish(old);
  ProgramFrameSharedTexture texture; std::shared_ptr<const void> owner;
  ASSERT_TRUE(pool.select(200, texture, owner)); EXPECT_EQ(texture.frameNumber, 4);
  texture = {}; owner.reset();
  EXPECT_FALSE(pool.select(200, texture, owner, true)); EXPECT_FALSE(owner);
  auto next = pool.reserve(); ASSERT_TRUE(next != nullptr);
  frame.frameNumber = frame.deliverySequence = 5; frame.timelineTimestamp100ns = 200;
  pool.stamp(next, frame); pool.publish(next);
  ASSERT_TRUE(pool.select(200, texture, owner, true)); EXPECT_EQ(texture.frameNumber, 5);
  EXPECT_EQ(owner.get(), next.get());
  D3DProgramDisplayReader reader;
  MonitorRenderRequest request; request.programDisplayDeadlineNs = 1;
  reader.publicationUnavailable(device.get(), request);
  EXPECT_EQ(reader.refused(), 1u); EXPECT_EQ(reader.observation().reused, 0u);
  EXPECT_EQ(reader.observation().refusalReasons[static_cast<size_t>(ProgramDisplayReason::Deadline)], 1u);
  ASSERT_TRUE(reader.observation().incidentTrace != nullptr);
  bool missingPublication = false;
  for (const auto& event : *reader.observation().incidentTrace) if (event.stage == 23) missingPublication = true;
  EXPECT_TRUE(missingPublication);
}

TEST(D3DProgramBuffer, DisplayReaderPrewarmCannotAdvertisePixelsBeforeARealCopy) {
  using namespace corevideo::modules;
  ComPtrLite<ID3D11Device> device;
  ComPtrLite<ID3D11DeviceContext> context;
  ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, context.put())));
  D3DProgramDisplayReader reader; reader.prewarm(device.get(), context.get(), 64, 64);
  EXPECT_FALSE(reader.observation().observed); EXPECT_EQ(reader.observation().copies, 0u);
  EXPECT_EQ(reader.copy(device.get(), context.get(), {}), nullptr);
  D3DProgramDisplaySnapshots pool; ASSERT_TRUE(pool.initialize(device.get(), 64, 64, 933));
  auto slot = pool.reserve(); ASSERT_TRUE(slot != nullptr);
  ProgramFrame frame; frame.frameNumber = frame.deliverySequence = 11; pool.stamp(slot, frame);
  D3DProgramDisplayReaderTestAccess::fault(reader, [](ProgramDisplayReason reason) -> std::optional<HRESULT> {
    return reason == ProgramDisplayReason::Import ? std::optional<HRESULT>(E_FAIL) : std::nullopt;
  });
  EXPECT_EQ(reader.copy(device.get(), context.get(), slot), nullptr);
  EXPECT_FALSE(reader.observation().observed); EXPECT_EQ(reader.observation().copies, 0u);
  D3DProgramDisplayReaderTestAccess::fault(reader, {});
  ASSERT_TRUE(reader.copy(device.get(), context.get(), slot) != nullptr);
  EXPECT_TRUE(reader.observation().observed); EXPECT_EQ(reader.observation().copies, 1u);
  context->Flush();
  const auto limit = std::chrono::steady_clock::now()+std::chrono::seconds(2);
  while (reader.needsRetirement() && std::chrono::steady_clock::now() < limit) {
    reader.retire(context.get(), 0); std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_EQ(reader.observation().completedCopies, 1u);
}

TEST(D3DProgramBuffer, ImmutableDisplayLeaseSurvivesUnreadPublicationAndPoolExhaustion) {
  using namespace corevideo::modules;
  ComPtrLite<ID3D11Device> device;
  ComPtrLite<ID3D11DeviceContext> context;
  ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, context.put())));
  D3DProgramDisplaySnapshots pool;
  ASSERT_TRUE(pool.initialize(device.get(), 64, 64, 19));
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = desc.Height = 64; desc.MipLevels = desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET;
  ComPtrLite<ID3D11Texture2D> input, staging;
  ComPtrLite<ID3D11RenderTargetView> target;
  ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, input.put())));
  ASSERT_TRUE(SUCCEEDED(device->CreateRenderTargetView(input.get(), nullptr, target.put())));
  desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, staging.put())));
  std::shared_ptr<const void> retained;
  ProgramFrameSharedTexture published;
  for (int sequence = 1; sequence <= 100; ++sequence) {
    auto slot = pool.reserve(); ASSERT_TRUE(slot != nullptr);
    const float color[] = {sequence / 255.f, sequence / 255.f, sequence / 255.f, 1};
    context->ClearRenderTargetView(target.get(), color);
    context->CopyResource(slot->texture.get(), input.get());
    context->CopyResource(staging.get(), slot->texture.get()); context->Flush();
    // A blocking staging map proves this test publication's GPU copy completed.
    D3D11_MAPPED_SUBRESOURCE mapped{};
    ASSERT_TRUE(SUCCEEDED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)));
    context->Unmap(staging.get(), 0);
    ProgramFrame frame; frame.frameNumber = frame.deliverySequence = sequence;
    frame.timelineTimestamp100ns = sequence * 166667;
    pool.stamp(slot, frame); pool.publish(slot);
    if (sequence == 1) ASSERT_TRUE(pool.select(frame.timelineTimestamp100ns, published, retained));
  }
  const auto held = std::static_pointer_cast<const D3DProgramDisplaySnapshots::Snapshot>(retained);
  EXPECT_EQ(held->frame.frameNumber, 1); EXPECT_EQ(held->generation, 19u);
  context->CopyResource(staging.get(), held->texture.get()); context->Flush();
  D3D11_MAPPED_SUBRESOURCE mapped{};
  ASSERT_TRUE(SUCCEEDED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)));
  EXPECT_EQ(static_cast<const uint8_t*>(mapped.pData)[32 * mapped.RowPitch + 32 * 4], 1);
  context->Unmap(staging.get(), 0);
  std::vector<std::shared_ptr<D3DProgramDisplaySnapshots::Snapshot>> writers;
  while (auto slot = pool.reserve()) writers.push_back(std::move(slot));
  // Held publication + two history slots + five reserved writers = eight.
  EXPECT_EQ(writers.size(), 5u);
  EXPECT_EQ(pool.reserve(), nullptr);
  writers.clear(); EXPECT_NE(pool.reserve(), nullptr);
}

TEST(D3DProgramBuffer, UnretiredMonitorGpuReadQuarantinesItsPublicationOnShutdown) {
  using namespace corevideo::modules;
  ComPtrLite<ID3D11Device> device;
  ComPtrLite<ID3D11DeviceContext> context;
  ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, context.put())));
  D3DProgramDisplaySnapshots pool;
  ASSERT_TRUE(pool.initialize(device.get(), 64, 64, 41));
  auto slot = pool.reserve(); ASSERT_TRUE(slot != nullptr);
  const auto quarantinedSlot = slot->slot;
  ProgramFrame frame; frame.deliverySequence = frame.frameNumber = 1; frame.timelineTimestamp100ns = 1;
  pool.stamp(slot, frame); pool.publish(slot); // No producer GPU writes in this lifetime-only test.
  ProgramFrameSharedTexture texture;
  std::shared_ptr<const void> owner;
  ASSERT_TRUE(pool.select(1, texture, owner));
  {
    D3DProgramDisplayReader reader;
    ASSERT_TRUE(reader.copy(device.get(), context.get(), owner) != nullptr);
    EXPECT_EQ(reader.observation().gpuReads, 1);
    // Intentionally stop without flushing/polling completion. CPU publication
    // ownership must not be released into a reusable writer slot.
  }
  EXPECT_TRUE(slot->quarantined.load());
  owner.reset(); slot.reset();
  for (int n = 2; n < 30; ++n) {
    auto next = pool.reserve(); ASSERT_TRUE(next != nullptr);
    EXPECT_NE(next->slot, quarantinedSlot);
    frame.deliverySequence = frame.frameNumber = n; frame.timelineTimestamp100ns = n;
    pool.stamp(next, frame); pool.publish(next);
  }
  context->Flush();
}

TEST(D3DProgramBuffer, DisplayReaderRetiresOldGenerationAndReusesItsImageAcrossSlots) {
  using namespace corevideo::modules;
  ComPtrLite<ID3D11Device> producer, consumer;
  ComPtrLite<ID3D11DeviceContext> writer, readerContext;
  ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, producer.put(), nullptr, writer.put())));
  ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, consumer.put(), nullptr, readerContext.put())));
  auto publish = [&](D3DProgramDisplaySnapshots& pool, int width, int sequence, int value) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = 64; desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtrLite<ID3D11Texture2D> source, staging;
    ComPtrLite<ID3D11RenderTargetView> target;
    if (FAILED(producer->CreateTexture2D(&desc, nullptr, source.put())) ||
        FAILED(producer->CreateRenderTargetView(source.get(), nullptr, target.put()))) return std::shared_ptr<const void>{};
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(producer->CreateTexture2D(&desc, nullptr, staging.put()))) return std::shared_ptr<const void>{};
    auto slot = pool.reserve(); if (!slot) return std::shared_ptr<const void>{};
    const float color[] = {value / 255.f, value / 255.f, value / 255.f, 1};
    writer->ClearRenderTargetView(target.get(), color);
    writer->CopyResource(slot->texture.get(), source.get());
    writer->CopyResource(staging.get(), slot->texture.get()); writer->Flush();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(writer->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) return std::shared_ptr<const void>{};
    writer->Unmap(staging.get(), 0); // GPU publication is complete.
    ProgramFrame frame; frame.frameNumber = frame.deliverySequence = sequence;
    frame.timelineTimestamp100ns = sequence;
    pool.stamp(slot, frame); pool.publish(slot);
    ProgramFrameSharedTexture texture; std::shared_ptr<const void> owner;
    pool.select(sequence, texture, owner); return owner;
  };
  auto image = [](ID3D11ShaderResourceView* view) {
    ComPtrLite<ID3D11Resource> resource;
    ComPtrLite<ID3D11Texture2D> texture;
    if (view) { view->GetResource(resource.put()); resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(texture.put())); }
    return texture;
  };
  auto readPixel = [&](ID3D11Texture2D* texture, int width) {
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    if (desc.Width != static_cast<UINT>(width)) return -1;
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = desc.MiscFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtrLite<ID3D11Texture2D> staging;
    if (FAILED(consumer->CreateTexture2D(&desc, nullptr, staging.put()))) return -1;
    readerContext->CopyResource(staging.get(), texture); readerContext->Flush();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(readerContext->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) return -1;
    const int value = static_cast<const uint8_t*>(mapped.pData)[32 * mapped.RowPitch + (width / 2) * 4];
    readerContext->Unmap(staging.get(), 0); return value;
  };
  D3DProgramDisplayReader reader;
  auto oldPool = std::make_unique<D3DProgramDisplaySnapshots>();
  ASSERT_TRUE(oldPool->initialize(producer.get(), 64, 64, 51));
  auto oldOwner = publish(*oldPool, 64, 1, 51); ASSERT_TRUE(oldOwner != nullptr);
  std::weak_ptr<const void> oldLifetime = oldOwner;
  auto oldImage = image(reader.copy(consumer.get(), readerContext.get(), oldOwner));
  ASSERT_TRUE(oldImage.get() != nullptr);
  oldOwner.reset(); oldPool.reset(); // Reader's copy has not been flushed/polled.
  EXPECT_FALSE(oldLifetime.expired());
  D3DProgramDisplaySnapshots newPool;
  ASSERT_TRUE(newPool.initialize(producer.get(), 128, 64, 52));
  auto newOwner = publish(newPool, 128, 1, 204); ASSERT_TRUE(newOwner != nullptr);
  auto* resizedView = reader.copy(consumer.get(), readerContext.get(), newOwner);
  auto newImage = image(resizedView); ASSERT_TRUE(newImage.get() != nullptr);
  EXPECT_EQ(readPixel(oldImage.get(), 64), 51);
  EXPECT_EQ(readPixel(newImage.get(), 128), 204);
  reader.retire(readerContext.get());
  EXPECT_TRUE(oldLifetime.expired());
  EXPECT_EQ(reader.observation().generation, 52u);
  EXPECT_EQ(reader.observation().gpuReads, 0);
  auto nextOwner = publish(newPool, 128, 2, 134); ASSERT_TRUE(nextOwner != nullptr);
  EXPECT_EQ(reader.copy(consumer.get(), readerContext.get(), nextOwner), resizedView);
  EXPECT_EQ(readPixel(newImage.get(), 128), 134);
}

TEST(D3DProgramBuffer, SustainsFullSizeNv12PreparationAtSixtyHertz) {
  ProgramBufferTimerResolution timer;
  using namespace corevideo::modules;
  ComPtrLite<ID3D11Device> device;
  ComPtrLite<ID3D11DeviceContext> context;
  ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
      device.put(), nullptr, context.put())));
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = 1920; desc.Height = 1080; desc.MipLevels = desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_RENDER_TARGET;
  ComPtrLite<ID3D11Texture2D> source;
  ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, source.put())));
  D3DProgramBuffer buffer(device.get(), 1920, 1080, 2, 1, {});
  ASSERT_TRUE(buffer.valid());
  const auto anchor = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
  const auto anchorNs = std::chrono::duration_cast<std::chrono::nanoseconds>(anchor.time_since_epoch()).count();
  for (int slot = 0; slot < 180; ++slot) {
    std::this_thread::sleep_until(anchor + std::chrono::nanoseconds(static_cast<int64_t>(slot) * 1'000'000'000 / 60));
    ProgramFrame frame; frame.frameNumber = slot + 1; frame.productionSlot = slot;
    frame.productionAnchorNs = anchorNs; frame.width = 1920; frame.height = 1080;
    buffer.submit(context.get(), source.get(), std::move(frame), true);
    context->Flush();
    ProgramFrame delivered;
    while (buffer.take(delivered, 0)) {}
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const auto diagnostics = buffer.diagnostics();
  std::cerr << "full-size Nv12 produced=" << diagnostics.produced
            << " delivered=" << diagnostics.delivered
            << " underruns=" << diagnostics.underruns
            << " overflows=" << diagnostics.overflows << '\n';
  EXPECT_GE(diagnostics.delivered, 170u);
  EXPECT_LE(diagnostics.underruns, 5u);
  EXPECT_LE(diagnostics.overflows, 5u);
}

TEST(D3DProgramBuffer, LateReadyHeadDoesNotStealTheFollowingDeadline) {
  ProgramBufferTimerResolution timer;
  using namespace corevideo::modules;
  ComPtrLite<ID3D11Device> device;
  ComPtrLite<ID3D11DeviceContext> context;
  ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
      device.put(), nullptr, context.put())));
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = desc.Height = 64; desc.MipLevels = desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_RENDER_TARGET;
  ComPtrLite<ID3D11Texture2D> source;
  ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, source.put())));
  ComPtrLite<ID3D11RenderTargetView> target;
  ASSERT_TRUE(SUCCEEDED(device->CreateRenderTargetView(source.get(), nullptr, target.put())));
  D3DProgramBuffer buffer(device.get(), 64, 64, 2, 1, {});
  ASSERT_TRUE(buffer.valid());
  const auto anchor = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
  const auto anchorNs = std::chrono::duration_cast<std::chrono::nanoseconds>(anchor.time_since_epoch()).count();
  // The first real GPU preparation completes just AFTER its deadline, but
  // well BEFORE the next. Its loss must remain visible without losing slot 1.
  D3DProgramBufferTestAccess::beforePrepare(buffer, [anchor](int64_t slot) {
    if (slot == 0) std::this_thread::sleep_until(anchor + std::chrono::nanoseconds(2 * 1000000000LL / 60 + 2000000));
  });
  for (int slot = 0; slot < 3; ++slot) {
    std::this_thread::sleep_until(anchor + std::chrono::nanoseconds(slot * 1000000000LL / 60));
    const float gray = static_cast<float>(40 + slot * 60) / 255.f;
    const float color[] = {gray, gray, gray, 1.f};
    context->ClearRenderTargetView(target.get(), color);
    ProgramFrame frame; frame.width = frame.height = 64; frame.frameNumber = 100 + slot;
    frame.productionSlot = slot; frame.productionAnchorNs = anchorNs;
    frame.producedAt100ns = (anchorNs + slot * 1000000000LL / 60) / 100;
    buffer.submit(context.get(), source.get(), frame, true); context->Flush();
  }
  ProgramFrame first, second;
  ASSERT_TRUE(buffer.take(first, 200));
  ASSERT_TRUE(buffer.take(second, 200));
  EXPECT_EQ(first.frameNumber, 101); EXPECT_EQ(second.frameNumber, 102);
  ASSERT_TRUE(first.programNv12Shared != nullptr);
  ASSERT_TRUE(second.programNv12Shared != nullptr);
  // Snapshot pixels must retain the actual independently delivered identities.
  const int firstY = static_cast<int>(std::lround(16.0 + 220.0 * 100 / 256.0));
  const int secondY = static_cast<int>(std::lround(16.0 + 220.0 * 160 / 256.0));
  EXPECT_TRUE(std::abs(static_cast<int>(first.programNv12Shared->front()) - firstY) <= 2);
  EXPECT_TRUE(std::abs(static_cast<int>(second.programNv12Shared->front()) - secondY) <= 2);
  const auto diagnostics = buffer.diagnostics();
  EXPECT_EQ(diagnostics.underruns, 1u); EXPECT_EQ(diagnostics.overflows, 1u);
}

TEST(D3DProgramBuffer, FailedProducerKeepsEmptyOutputReadsPaced) {
  using namespace corevideo::modules;
  ComPtrLite<ID3D11Device> device;
  ComPtrLite<ID3D11DeviceContext> context;
  ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
      device.put(), nullptr, context.put())));
  D3DProgramBuffer buffer(device.get(), 64, 64, 3, 1, {});
  ASSERT_TRUE(buffer.valid());
  D3DProgramBufferTestAccess::fail(buffer);
  EXPECT_EQ(buffer.diagnostics().status, "failed");
  ProgramFrame frame;
  const auto started = std::chrono::steady_clock::now();
  for (int i = 0; i < 4; ++i) EXPECT_FALSE(buffer.take(frame, 20));
  const auto elapsed = std::chrono::steady_clock::now() - started;
  EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 60);
  EXPECT_FALSE(buffer.take(frame, 0));
}

TEST(D3DProgramBuffer, StartupPreparationAllocatesWithoutStartingDeliveryClock) {
  ProgramBufferTimerResolution timer;
  auto compositor = corevideo::modules::createD3D11Compositor();
  ASSERT_TRUE(compositor != nullptr);
  compositor->configureProgramBuffer(3);
  compositor->prepareProgramBuffer(1920, 1080);
  const auto prepared = compositor->programBufferDiagnostics();
  EXPECT_TRUE(prepared.generation > 0);
  EXPECT_EQ(prepared.status, "priming");
  EXPECT_EQ(prepared.produced, 0u);
  EXPECT_EQ(prepared.delivered, 0u);
  // Startup can wait indefinitely for the worker: allocation cannot start a
  // hidden cadence or charge idle startup time as missed Program frames.
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  EXPECT_EQ(compositor->programBufferDiagnostics().underruns, 0u);
  corevideo::modules::ProgramFrame frame;
  EXPECT_FALSE(compositor->takeDeliveredProgramFrame(frame, 0));
  corevideo::modules::CompositorRenderPlan plan;
  plan.width = 1920; plan.height = 1080; plan.skipCpuReadback = true;
  (void)compositor->render(plan, {});
  EXPECT_EQ(compositor->programBufferDiagnostics().generation, prepared.generation);
  ASSERT_TRUE(compositor->takeDeliveredProgramFrame(frame, 1500));
  EXPECT_EQ(frame.width, 1920);
  EXPECT_EQ(frame.height, 1080);
}

TEST(D3DProgramBuffer, RetainsTaggedNv12AndDeliversWithoutFurtherRendering) {
  ProgramBufferTimerResolution timer;
  for (const int depth : {2, 3}) {
    auto compositor = corevideo::modules::createD3D11Compositor();
    ASSERT_TRUE(compositor != nullptr);
    compositor->configureProgramBuffer(depth);
    EXPECT_EQ(compositor->programBufferFrames(), depth);
    corevideo::modules::CompositorRenderPlan plan;
    plan.sceneId = "buffer-test"; plan.renderPlanId = "buffer-test:1";
    plan.width = 1920; plan.height = 1080; plan.fps = 60;
    plan.skipCpuReadback = true; plan.fullProgramReadback = true;
    corevideo::modules::CompositorRenderPlanLayer layer;
    layer.layerId = "route:test"; layer.kind = "participant-video";
    layer.participantId = "test"; layer.sourceId = "zoom:test"; layer.rect = {0, 0, 1, 1};
    plan.layers.push_back(layer);
    // Every producer frame carries a distinct, uniform grayscale BGRA value.
    // The NV12 luma below must match that frame's number, not a later tap cache.
    const auto grayForFrame = [](int64_t number) { return static_cast<uint8_t>(20 + number * 12); };
    const auto sourceFrame = [&](int64_t number) {
      corevideo::modules::VideoFrame source;
      source.participantId = "test";
      source.width = source.pixelWidth = 64; source.height = source.pixelHeight = 64;
      source.pixelStride = 64 * 4; source.frameId = number; source.timestampMs = number * 17;
      auto pixels = std::make_shared<std::vector<uint8_t>>(64u * 64u * 4u);
      const auto gray = grayForFrame(number);
      for (size_t i = 0; i < pixels->size(); i += 4) {
        (*pixels)[i] = (*pixels)[i + 1] = (*pixels)[i + 2] = gray; (*pixels)[i + 3] = 255;
      }
      source.pixels = std::move(pixels);
      return source;
    };
    std::vector<corevideo::modules::ProgramFrame> delivered;
    (void)compositor->render(plan, {sourceFrame(1)}); // Allocate/compile before cadence sampling.
    const auto anchor = std::chrono::steady_clock::now();
    for (int i = 1; i < 14; ++i) {
      std::this_thread::sleep_until(anchor + std::chrono::nanoseconds(i * 1000000000LL / 60));
      (void)compositor->render(plan, {sourceFrame(i + 1)});
      corevideo::modules::ProgramFrame frame;
      while (compositor->takeDeliveredProgramFrame(frame, 0)) delivered.push_back(std::move(frame));
    }
    // Stop producing: retained FIFO packets must still drain on their own
    // scheduled deadlines. This would fail a render-tick delay masquerading
    // as an independent delivery queue.
    corevideo::modules::ProgramFrame tail;
    ASSERT_TRUE(compositor->takeDeliveredProgramFrame(tail, 200));
    delivered.push_back(std::move(tail));
    ASSERT_TRUE(delivered.size() >= 4u);
    int64_t sequence = 0, frameNumber = 0, pts = 0;
    for (const auto& frame : delivered) {
      EXPECT_TRUE(frame.deliverySequence > sequence);
      EXPECT_TRUE(frame.frameNumber > frameNumber);
      EXPECT_TRUE(frame.timelineTimestamp100ns > pts);
      ASSERT_TRUE(frame.programNv12Shared != nullptr);
      EXPECT_EQ(frame.programNv12Shared->size(), 1920u * 1080u * 3u / 2u);
      EXPECT_EQ(frame.programNv12Width, 1920);
      EXPECT_EQ(frame.programNv12Height, 1080);
      const int expectedY = static_cast<int>(std::lround(16.0 + 220.0 * grayForFrame(frame.frameNumber) / 256.0));
      for (const size_t offset : {size_t{540 * 1920 + 960}, size_t{270 * 1920 + 480}, size_t{810 * 1920 + 1440}}) {
        const int actualY = (*frame.programNv12Shared)[offset];
        EXPECT_TRUE(std::abs(actualY - expectedY) <= 2)
            << "frame=" << frame.frameNumber << " expected Y=" << expectedY << " actual Y=" << actualY;
      }
      ASSERT_TRUE(frame.renderPlanEvidence != nullptr);
      EXPECT_EQ(frame.renderPlanEvidence->sceneId, "buffer-test");
      EXPECT_EQ(frame.renderPlanEvidence->layers.front().sourceId, "zoom:test");
      EXPECT_TRUE(frame.gpuOwner != nullptr);
      sequence = frame.deliverySequence; frameNumber = frame.frameNumber; pts = frame.timelineTimestamp100ns;
    }
    EXPECT_TRUE(delivered.front().programNv12Shared != delivered.back().programNv12Shared);
    auto retained = delivered.front().programNv12Shared;
    auto retainedGpu = delivered.front().gpuOwner;
    const auto retainedByte = retained->front();
    const auto previousGeneration = compositor->programBufferDiagnostics().generation;
    // A new resolution creates a fresh GPU resource generation mid-timeline.
    // Its first global production slot is 120, not zero; historical deadlines
    // must not become 120 fictitious underruns in this new queue.
    plan.width = 1280; plan.height = 720; plan.sceneId = "buffer-resized";
    const auto nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const int64_t globalAnchor = nowNs + 1000000000LL - 2000000000LL;
    for (int64_t slot = 120; slot < 123; ++slot) {
      compositor->setProgramProductionTiming(slot, globalAnchor);
      (void)compositor->render(plan, {sourceFrame(slot - 105)});
    }
    EXPECT_EQ(compositor->programBufferDiagnostics().underruns, 0u);
    EXPECT_TRUE(compositor->programBufferDiagnostics().generation > previousGeneration);
    corevideo::modules::ProgramFrame resized;
    ASSERT_TRUE(compositor->takeDeliveredProgramFrame(resized, 1500));
    EXPECT_EQ(resized.width, 1280);
    EXPECT_EQ(resized.height, 720);
    EXPECT_EQ(resized.productionSlot, 120);
    EXPECT_EQ(resized.renderPlanEvidence->sceneId, "buffer-resized");
    EXPECT_TRUE(resized.gpuOwner != retainedGpu);
    EXPECT_EQ(retained->front(), retainedByte);
    compositor.reset(); // All owned packets remain valid after worker/device teardown.
    EXPECT_EQ(retained->front(), retainedByte);
  }
}
// Read real multiview pixels, rather than inferring display progress from the
// independently delivered NV12 packets. Keep two fixed consumer phases.
#include <d3d11.h>
#include "compositor/ComPtrLite.h"
TEST(D3DProgramBuffer, MultiviewProgramPixelsAdvanceAtFixedConsumerPhases) {
  ProgramBufferTimerResolution timer;
  using namespace corevideo::modules;
  ComPtrLite<ID3D11Device> device;
  ComPtrLite<ID3D11DeviceContext> context;
  ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, context.put())));
  for (const int depth : {2, 3}) for (const bool isolated : {false, true}) for (const int phaseMs : {2, 4, 10, 14}) {
    corevideo::test::MonitorTestMode mode(isolated);
    auto compositor = createD3D11Compositor();
    ASSERT_TRUE(compositor != nullptr);
    mode.verify(*compositor, "MultiviewProgramPixelsAdvanceAtFixedConsumerPhases");
    compositor->configureProgramBuffer(depth);
    CompositorRenderPlan program;
    program.sceneId = "phase-test"; program.renderPlanId = "phase-test";
    program.width = 1920; program.height = 1080; program.fps = 60; program.skipCpuReadback = true;
    CompositorRenderPlanLayer layer;
    layer.layerId = "route:test"; layer.kind = "participant-video"; layer.participantId = "test";
    layer.sourceId = "zoom:test"; layer.rect = {0, 0, 1, 1}; layer.borderStyle = "none";
    program.layers.push_back(layer);
    auto multiview = program;
    multiview.layers.front().layerId = "multiview-pgm:buffer-cell";
    multiview.layers.front().hasClipRect = true; multiview.layers.front().clipRect = {0, 0, 1, 1};
    compositor->prepareProgramBuffer(1920, 1080);
    (void)compositor->renderMultiview(multiview, {});
    if (isolated) {
      MonitorRenderRequest warm;
      warm.programPlan = program; warm.multiviewPlan = multiview; warm.multiviewActive = true;
      compositor->submitMonitors(std::move(warm));
      const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (!compositor->latestMonitors() && std::chrono::steady_clock::now() < limit)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      ASSERT_TRUE(compositor->latestMonitors() != nullptr);
    }
    int changes = 0, previous = -1, repeats = 0, gaps = 0;
    const auto anchor = std::chrono::steady_clock::now();
    for (int n = 0; n < 120; ++n) {
      const auto tick = anchor + std::chrono::nanoseconds(n * 1000000000LL / 60);
      timer.waitUntil(tick);
      const auto tickStarted = std::chrono::steady_clock::now();
      compositor->setProgramProductionTiming(n,
          std::chrono::duration_cast<std::chrono::nanoseconds>(anchor.time_since_epoch()).count());
      VideoFrame source;
      source.participantId = "test"; source.width = source.pixelWidth = 64; source.height = source.pixelHeight = 64;
      source.pixelStride = 256; source.frameId = n + 1; source.timestampMs = n * 17;
      auto sourcePixels = std::make_shared<std::vector<uint8_t>>(64u * 64u * 4u, static_cast<uint8_t>(30 + n));
      for (size_t alpha = 3; alpha < sourcePixels->size(); alpha += 4) (*sourcePixels)[alpha] = 255;
      source.pixels = std::move(sourcePixels);
      (void)compositor->render(program, {source});
      const auto productionReturned = std::chrono::steady_clock::now();
      timer.waitUntil(tick + std::chrono::milliseconds(phaseMs));
      auto monitorSubmitted = std::chrono::steady_clock::now();
      ProgramFrameSharedTexture exported;
      if (isolated) {
        ASSERT_TRUE(compositor->hasIsolatedMonitors());
        MonitorRenderRequest request;
        request.sequence = n + 1; request.programPlan = program; request.multiviewPlan = multiview;
        request.multiviewActive = true; request.frames = {source};
        monitorSubmitted = std::chrono::steady_clock::now();
        compositor->submitMonitors(std::move(request));
        const auto waitStarted = std::chrono::steady_clock::now();
        // Use the original absolute frame boundary. A new relative 10 ms
        // timeout can expire before the declared deadline at early phases.
        const auto limit = tick + std::chrono::nanoseconds(1000000000LL / 60);
        std::shared_ptr<const MonitorRenderResult> result;
        do {
          result = compositor->latestMonitors();
          if (result && result->sequence >= n + 1) break;
          std::this_thread::sleep_for(std::chrono::microseconds(100));
        } while (std::chrono::steady_clock::now() < limit);
        // A coarse Windows wake can jump over the final polling interval.
        // Re-read the immutable result and judge its owner publication time,
        // never the test observer's late wake or a freshly extended deadline.
        result = compositor->latestMonitors();
        const auto deadlineNs = std::chrono::duration_cast<std::chrono::nanoseconds>(limit.time_since_epoch()).count();
        if (!result || result->sequence < n + 1 || result->completedAtNs > deadlineNs) {
          const auto now = std::chrono::steady_clock::now();
          const auto diagnostic = compositor->monitorDiagnostics();
          const auto us = [&](auto point) {
            return static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(point - tick).count());
          };
          std::fprintf(stderr,
              "[program-display-timeout] depth=%d isolated=%d phase_ms=%d tick=%d expected_sequence=%d observed_sequence=%lld "
              "tick_start_us=%lld production_return_us=%lld monitor_submit_us=%lld wait_start_us=%lld observed_us=%lld "
              "consumer_deadline_us=16666 wait_budget=absolute_frame_boundary "
              "monitor_submitted=%llu completed=%llu superseded=%llu failed=%llu pending=%d last_sequence=%lld last_work_ms=%.3f "
              "effective_mode=%s readiness=%s failure=%s\n",
              depth, isolated, phaseMs, n, n + 1, static_cast<long long>(result ? result->sequence : -1),
              us(tickStarted), us(productionReturned), us(monitorSubmitted), us(waitStarted), us(now),
              static_cast<unsigned long long>(diagnostic.submitted), static_cast<unsigned long long>(diagnostic.completed),
              static_cast<unsigned long long>(diagnostic.superseded), static_cast<unsigned long long>(diagnostic.failed),
              diagnostic.pending, static_cast<long long>(diagnostic.lastSequence), diagnostic.lastWorkMs,
              diagnostic.effectiveMode.c_str(), diagnostic.readiness.c_str(), diagnostic.failureReason.c_str());
        }
        if (result && result->completedAtNs > deadlineNs) {
          std::fprintf(stderr, "[program-display-publication-late] completed_us=%lld copied=%llu gpu_misses=%llu state=%s\n",
              static_cast<long long>((result->completedAtNs - std::chrono::duration_cast<std::chrono::nanoseconds>(tick.time_since_epoch()).count())/1000),
              static_cast<unsigned long long>(result->programDisplayReader.copiedSequence),
              static_cast<unsigned long long>(result->programDisplayReader.gpuReadinessDeadlineMisses), result->programDisplayReader.state.c_str());
        }
        ASSERT_TRUE(result && result->sequence >= n + 1 && result->completedAtNs > 0 && result->completedAtNs <= deadlineNs)
            << "depth=" << depth << " phase_ms=" << phaseMs << " tick=" << n
            << " expected_sequence=" << n + 1 << " observed_sequence=" << (result ? result->sequence : -1);
        exported = result->multiview;
      } else exported = compositor->renderMultiview(multiview, {source});
      const auto monitorReturned = std::chrono::steady_clock::now();
      // Aim at the same consumer phase. The completion wait above and the
      // keyed-mutex/readback below can pass this deadline: report actual timing
      // so pixel continuity cannot be mistaken for on-time presentation proof.
      timer.waitUntil(tick + std::chrono::milliseconds(16));
      const auto consumerStarted = std::chrono::steady_clock::now();
      if (exported.sharedHandleHex.empty()) continue;
      ComPtrLite<ID3D11Texture2D> texture, staging;
      ComPtrLite<IDXGIKeyedMutex> key;
      const auto handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(std::stoull(exported.sharedHandleHex, nullptr, 0)));
      ASSERT_TRUE(SUCCEEDED(device->OpenSharedResource(handle, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(texture.put()))));
      ASSERT_TRUE(SUCCEEDED(texture->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(key.put()))));
      ASSERT_TRUE(key->AcquireSync(1, 20) == S_OK);
      D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
      desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = desc.MiscFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, staging.put())));
      context->CopyResource(staging.get(), texture.get()); context->Flush(); key->ReleaseSync(0);
      D3D11_MAPPED_SUBRESOURCE mapped{};
      ASSERT_TRUE(SUCCEEDED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)));
      const auto pixel = static_cast<const uint8_t*>(mapped.pData)[(desc.Height / 2) * mapped.RowPitch + (desc.Width / 2) * 4];
      context->Unmap(staging.get(), 0);
      const auto readbackComplete = std::chrono::steady_clock::now();
      const auto us = [&](auto point) {
        return static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(point - tick).count());
      };
      std::fprintf(stderr, "[program-display-pixel] depth=%d isolated=%d phase_ms=%d tick=%d canvas=%lld pixel=%u "
          "tick_start_us=%lld production_return_us=%lld monitor_submit_us=%lld monitor_return_us=%lld "
          "consumer_start_us=%lld readback_complete_us=%lld consumer_deadline_us=16000\n",
          depth, isolated, phaseMs, n, static_cast<long long>(exported.frameNumber), unsigned(pixel),
          us(tickStarted), us(productionReturned), us(monitorSubmitted), us(monitorReturned), us(consumerStarted), us(readbackComplete));
      if (n >= 20 && previous >= 0) {
        EXPECT_EQ(unsigned(pixel), unsigned(30 + n - depth - 1));
        if (pixel != previous) ++changes;
        if (pixel == previous) ++repeats;
        if (pixel != previous + 1) ++gaps;
      }
      previous = pixel;
      ProgramFrame packet; while (compositor->takeDeliveredProgramFrame(packet, 0)) {}
    }
    std::fprintf(stderr, "[program-display-phase] depth=%d isolated=%d phase_ms=%d changes=%d repeats=%d gaps=%d\n",
        depth, isolated, phaseMs, changes, repeats, gaps);
    EXPECT_EQ(changes, 100) << "phase_ms=" << phaseMs << " isolated=" << isolated;
    EXPECT_EQ(repeats, 0) << "phase_ms=" << phaseMs << " isolated=" << isolated;
    EXPECT_EQ(gaps, 0) << "phase_ms=" << phaseMs << " isolated=" << isolated;
  }
}
TEST(D3DProgramBuffer, RetainedGpuOnlyFramesKeepTheirPixelsAfterProducerOverwrite) {
  ProgramBufferTimerResolution timer;
  using namespace corevideo::modules;
  ComPtrLite<ID3D11Device> producer, consumer;
  ComPtrLite<ID3D11DeviceContext> producerContext, consumerContext;
  ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, producer.put(), nullptr, producerContext.put())));
  ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, consumer.put(), nullptr, consumerContext.put())));
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = desc.Height = 64; desc.MipLevels = desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_RENDER_TARGET;
  ComPtrLite<ID3D11Texture2D> source;
  ComPtrLite<ID3D11RenderTargetView> target;
  ASSERT_TRUE(SUCCEEDED(producer->CreateTexture2D(&desc, nullptr, source.put())));
  ASSERT_TRUE(SUCCEEDED(producer->CreateRenderTargetView(source.get(), nullptr, target.put())));
  std::vector<int> pixels;
  int failures = 0;
  {
    auto readExport = [&](const ProgramFrame& frame) {
      // No test assertions on a worker: collect actual receiver pixels and
      // report failures only after the buffer has joined both workers.
      ComPtrLite<ID3D11Texture2D> exported, staging;
      ComPtrLite<IDXGIKeyedMutex> key;
      if (frame.sharedTexture.sharedHandleHex.empty()) { ++failures; return; }
      const auto handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(
          std::stoull(frame.sharedTexture.sharedHandleHex, nullptr, 0)));
      if (FAILED(consumer->OpenSharedResource(handle, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(exported.put()))) ||
          FAILED(exported->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(key.put()))) ||
          key->AcquireSync(1, 20) != S_OK) { ++failures; return; }
      auto stagingDesc = desc;
      stagingDesc.Usage = D3D11_USAGE_STAGING; stagingDesc.BindFlags = 0;
      stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      if (FAILED(consumer->CreateTexture2D(&stagingDesc, nullptr, staging.put()))) {
        key->ReleaseSync(0); ++failures; return;
      }
      consumerContext->CopyResource(staging.get(), exported.get());
      consumerContext->Flush(); key->ReleaseSync(0);
      D3D11_MAPPED_SUBRESOURCE mapped{};
      if (FAILED(consumerContext->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) { ++failures; return; }
      pixels.push_back(static_cast<const uint8_t*>(mapped.pData)[32 * mapped.RowPitch + 32 * 4]);
      consumerContext->Unmap(staging.get(), 0);
    };
    D3DProgramBuffer buffer(producer.get(), 64, 64, 3, 1, {});
    ASSERT_TRUE(buffer.valid());
    const auto anchor = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
    const auto anchorNs = std::chrono::duration_cast<std::chrono::nanoseconds>(anchor.time_since_epoch()).count();
    for (int i = 0; i < 3; ++i) {
      const float value = (40.0f + i * 60.0f) / 255.0f;
      const float color[] = {value, value, value, 1};
      producerContext->ClearRenderTargetView(target.get(), color);
      ProgramFrame frame; frame.frameNumber = i + 1; frame.productionSlot = i;
      frame.productionAnchorNs = anchorNs; frame.width = frame.height = 64;
      buffer.submit(producerContext.get(), source.get(), std::move(frame), false);
      producerContext->Flush();
    }
    const float overwrite[] = {1, 1, 1, 1};
    producerContext->ClearRenderTargetView(target.get(), overwrite); producerContext->Flush();
    for (int i = 0; i < 3; ++i) {
      ProgramFrame frame;
      ASSERT_TRUE(buffer.take(frame, 1500));
      EXPECT_EQ(frame.frameNumber, i + 1);
      EXPECT_TRUE(frame.programNv12Shared == nullptr);
      ProgramFrame exported;
      const auto limit = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
      while ((!buffer.latest(exported) || exported.frameNumber != frame.frameNumber) && std::chrono::steady_clock::now() < limit)
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      ASSERT_EQ(exported.frameNumber, frame.frameNumber);
      readExport(exported);
    }
  }
  EXPECT_EQ(failures, 0);
  ASSERT_EQ(pixels.size(), 3u);
  for (int i = 0; i < 3; ++i) EXPECT_TRUE(std::abs(pixels[i] - (40 + i * 60)) <= 1);
}

TEST(D3DProgramBuffer, BlockedOptionalExportPreservesNativeCadenceAndOtherReaderPixels) {
  ProgramBufferTimerResolution timer;
  using namespace corevideo::modules;
  ComPtrLite<ID3D11Device> device;
  ComPtrLite<ID3D11DeviceContext> context;
  ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, context.put())));
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = desc.Height = 64; desc.MipLevels = desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET;
  ComPtrLite<ID3D11Texture2D> source;
  ComPtrLite<ID3D11RenderTargetView> target;
  ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, source.put())));
  ASSERT_TRUE(SUCCEEDED(device->CreateRenderTargetView(source.get(), nullptr, target.put())));
  for (const bool blockShell : {true, false}) {
    std::mutex gateMutex;
    std::condition_variable gateChanged;
    bool release = false;
    std::atomic<bool> entered{false};
    D3DProgramBuffer buffer(device.get(), 64, 64, 2, 1, {});
    ASSERT_TRUE(buffer.valid());
    // Release before buffer destruction, including a fatal assertion path.
    struct ReleaseGate {
      std::mutex& mutex; std::condition_variable& changed; bool& release;
      ~ReleaseGate() { { std::lock_guard<std::mutex> lock(mutex); release = true; } changed.notify_all(); }
    } releaseGate{gateMutex, gateChanged, release};
    D3DProgramBufferTestAccess::beforeExport(buffer, [&](bool shell) {
      if (shell != blockShell || entered.exchange(true)) return;
      std::unique_lock<std::mutex> lock(gateMutex);
      gateChanged.wait(lock, [&] { return release; });
    });
    const auto anchor = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
    const auto anchorNs = std::chrono::duration_cast<std::chrono::nanoseconds>(anchor.time_since_epoch()).count();
    int received = 0;
    auto receive = [&](ProgramFrame& packet) {
      ++received;
      EXPECT_EQ(packet.frameNumber, received);
      EXPECT_EQ(packet.deliverySequence, received);
      ASSERT_TRUE(packet.programNv12Shared != nullptr);
      const int gray = 40 + static_cast<int>(packet.frameNumber);
      const int expected = static_cast<int>(std::lround(16.0 + 220.0 * gray / 256.0));
      EXPECT_TRUE(std::abs((*packet.programNv12Shared)[540 * 1920 + 960] - expected) <= 2);
    };
    for (int n = 1; n <= 60; ++n) {
      std::this_thread::sleep_until(anchor + std::chrono::nanoseconds((n - 1) * 1000000000LL / 60));
      const float value = (40.0f + n) / 255.0f;
      const float color[] = {value, value, value, 1}; context->ClearRenderTargetView(target.get(), color);
      ProgramFrame frame; frame.frameNumber = n; frame.productionSlot = n - 1;
      frame.productionAnchorNs = anchorNs; frame.width = frame.height = 64;
      buffer.submit(context.get(), source.get(), std::move(frame), true); context->Flush();
      ProgramFrame packet; while (buffer.take(packet, 0)) receive(packet);
      EXPECT_LE(D3DProgramBufferTestAccess::readLeases(buffer), 2u);
    }
    while (received < 60) { ProgramFrame packet; ASSERT_TRUE(buffer.take(packet, 50)); receive(packet); }
    ASSERT_TRUE(entered.load());
    const auto diagnostics = buffer.diagnostics();
    EXPECT_EQ(diagnostics.produced, 60u); EXPECT_EQ(diagnostics.delivered, 60u);
    EXPECT_EQ(diagnostics.underruns, 0u); EXPECT_EQ(diagnostics.overflows, 0u);
    EXPECT_TRUE(diagnostics.displayBusy >= 50u);
    ProgramFrame exported;
    ProgramFrameSharedTexture texture;
    std::shared_ptr<const void> owner;
    const auto limit = std::chrono::steady_clock::now() + std::chrono::milliseconds(10);
    do {
      if (blockShell) { if (buffer.multiview(texture, owner) && texture.frameNumber >= 58) break; }
      else { if (buffer.latest(exported) && exported.frameNumber >= 58) { texture = exported.sharedTexture; break; } }
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    } while (std::chrono::steady_clock::now() < limit);
    ASSERT_TRUE(texture.frameNumber >= 58);
    ComPtrLite<ID3D11Texture2D> shared, staging;
    ComPtrLite<IDXGIKeyedMutex> key;
    const auto handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(std::stoull(texture.sharedHandleHex, nullptr, 0)));
    ASSERT_TRUE(SUCCEEDED(device->OpenSharedResource(handle, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(shared.put()))));
    if (!blockShell) {
      ASSERT_TRUE(SUCCEEDED(shared->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(key.put()))));
      ASSERT_TRUE(key->AcquireSync(1, 20) == S_OK);
    }
    auto stagingDesc = desc; stagingDesc.BindFlags = 0; stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&stagingDesc, nullptr, staging.put())));
    context->CopyResource(staging.get(), shared.get()); context->Flush(); if (key) key->ReleaseSync(0);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    ASSERT_TRUE(SUCCEEDED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)));
    const int pixel = static_cast<const uint8_t*>(mapped.pData)[32 * mapped.RowPitch + 32 * 4];
    context->Unmap(staging.get(), 0);
    EXPECT_TRUE(std::abs(pixel - (40 + texture.frameNumber)) <= 1);
    // The delayed reader must still export frame 1, even though many source
    // slots have been recycled while it was stopped. Its read lease survives.
    { std::lock_guard<std::mutex> lock(gateMutex); release = true; } gateChanged.notify_all();
    ProgramFrame delayed;
    ProgramFrameSharedTexture delayedTexture;
    const auto delayedLimit = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    do {
      if (blockShell) { if (buffer.latest(delayed)) { delayedTexture = delayed.sharedTexture; break; } }
      else { if (buffer.multiview(delayedTexture, owner)) break; }
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    } while (std::chrono::steady_clock::now() < delayedLimit);
    ASSERT_EQ(delayedTexture.frameNumber, 1);
    const auto delayedHandle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(std::stoull(delayedTexture.sharedHandleHex, nullptr, 0)));
    shared = {}; key = {};
    ASSERT_TRUE(SUCCEEDED(device->OpenSharedResource(delayedHandle, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(shared.put()))));
    if (blockShell) {
      ASSERT_TRUE(SUCCEEDED(shared->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(key.put()))));
      ASSERT_TRUE(key->AcquireSync(1, 20) == S_OK);
    }
    context->CopyResource(staging.get(), shared.get()); context->Flush(); if (key) key->ReleaseSync(0);
    ASSERT_TRUE(SUCCEEDED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)));
    const int retainedPixel = static_cast<const uint8_t*>(mapped.pData)[32 * mapped.RowPitch + 32 * 4];
    context->Unmap(staging.get(), 0);
    EXPECT_TRUE(std::abs(retainedPixel - 41) <= 1);
  }
}
#endif
