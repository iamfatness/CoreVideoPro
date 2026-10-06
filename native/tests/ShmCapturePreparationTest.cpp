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
using namespace corevideo::modules;

TEST(ShmCapturePreparation, RejectsInvalidAndOverBudgetRequestsBeforeAllocation) {
  ShmCapturePreparation preparation;
  EXPECT_FALSE(preparation.registerBuffer("", "name", 64, 64));
  EXPECT_FALSE(preparation.registerBuffer("id", "name", 0, 64));
  EXPECT_FALSE(preparation.registerBuffer("id", "name", std::numeric_limits<int>::max(), std::numeric_limits<int>::max()));
  EXPECT_EQ(preparation.stats().accepted, 0u);
  EXPECT_EQ(preparation.stats().refused, 3u);
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
  void write(uint32_t seq, uint8_t color) {
    InterlockedExchange(reinterpret_cast<volatile LONG*>(view), seq - 1);
    std::memset(view + 16, color, static_cast<size_t>(width) * height * 4);
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
#endif
