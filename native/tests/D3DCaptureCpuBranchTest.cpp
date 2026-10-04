#include <gtest/gtest.h>
#if defined(_WIN32) && COREVIDEO_WITH_WGC
#include "modules/D3DCaptureCpuBranch.h"
#include <future>

namespace {
using namespace corevideo::modules;
struct Rig {
  ComPtr<ID3D11Device> producer, reader;
  ComPtr<ID3D11DeviceContext> capture, read;
  bool start() {
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        &producer, nullptr, &capture))) return false;
    ComPtr<IDXGIDevice> dxgi; ComPtr<IDXGIAdapter> adapter;
    if (FAILED(producer.As(&dxgi)) || FAILED(dxgi->GetAdapter(&adapter))) return false;
    return SUCCEEDED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        &reader, nullptr, &read));
  }
  ComPtr<ID3D11Texture2D> image(int width, int height, bool staging = false) {
    D3D11_TEXTURE2D_DESC desc{}; desc.Width = width; desc.Height = height;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.Usage = staging ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT;
    desc.CPUAccessFlags = staging ? D3D11_CPU_ACCESS_READ : 0;
    desc.BindFlags = staging ? 0 : D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> result;
    (staging ? reader : producer)->CreateTexture2D(&desc, nullptr, &result);
    return result;
  }
};
bool submitPrepared(D3DCaptureCpuBranch& cpu, ID3D11DeviceContext* context,
                    ID3D11Texture2D* image, int64_t sequence, uint64_t epoch, int64_t time) {
  const auto before = cpu.stats().copied;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  do {
    cpu.copy(context, image, sequence, epoch, time); context->Flush();
    if (cpu.stats().copied > before) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (std::chrono::steady_clock::now() < deadline);
  return false;
}
}

TEST(D3DCaptureCpuBranch, HeldConversionCannotRetainProductionSlotsOrRelabelPixels) {
  Rig rig; ASSERT_TRUE(rig.start());
  auto consumer = D3DVideoConsumers::add(rig.reader.Get()); ASSERT_NE(consumer, nullptr);
  D3DVideoFramePool production; ASSERT_TRUE(production.initialize(rig.producer.Get(), 32, 32, 1));
  auto source = rig.image(32, 32), readback = rig.image(32, 32, true);
  ASSERT_NE(source, nullptr); ASSERT_NE(readback, nullptr);
  std::promise<void> entered, release; auto started = entered.get_future();
  auto gate = release.get_future().share(); std::atomic<bool> first{true};
  D3DCaptureCpuBranch cpu(rig.producer.Get(), "capture:test", [&] {
    if (first.exchange(false)) { entered.set_value(); gate.wait(); }
  });
  ASSERT_TRUE(cpu.valid());
  std::vector<uint8_t> bytes(32 * 32 * 4, 1);
  rig.capture->UpdateSubresource(source.Get(), 0, nullptr, bytes.data(), 32 * 4, 0);
  const bool submitted = submitPrepared(cpu, rig.capture.Get(), source.Get(), 1, 9, 12345);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (started.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready && std::chrono::steady_clock::now() < deadline) {
    cpu.publishReady(rig.capture.Get()); std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const bool blocked = started.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
  bool complete = true; int delivered = 0;
  for (int sequence = 2; sequence <= 30 && complete; ++sequence) {
    std::fill(bytes.begin(), bytes.end(), static_cast<uint8_t>(sequence));
    rig.capture->UpdateSubresource(source.Get(), 0, nullptr, bytes.data(), 32 * 4, 0);
    const int slot = production.beginCopy(rig.capture.Get(), source.Get());
    cpu.copy(rig.capture.Get(), source.Get(), sequence, 9, sequence * 166667);
    rig.capture->Flush();
    std::shared_ptr<const GpuVideoFrame> ready;
    const auto due = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    do { ready = production.completed(rig.capture.Get(), slot); if (ready) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < due);
    if (!ready) { complete = false; break; }
    const auto image = std::dynamic_pointer_cast<const D3DVideoImage>(ready);
    const auto* view = image->view(consumer->id);
    if (!view) { complete = false; break; }
    rig.read->CopyResource(readback.Get(), view->texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(rig.read->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped))) { complete = false; break; }
    complete = static_cast<const uint8_t*>(mapped.pData)[0] == sequence;
    rig.read->Unmap(readback.Get(), 0); if (complete) ++delivered;
  }
  const auto pressured = cpu.stats();
  release.set_value(); // release before any assertion can unwind a blocked worker
  const auto convertedBy = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (cpu.stats().converted == 0 && std::chrono::steady_clock::now() < convertedBy)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  cpu.stopOnCaptureOwner(rig.capture.Get());
  const auto converted = cpu.take();
  EXPECT_TRUE(submitted); EXPECT_TRUE(blocked); EXPECT_TRUE(complete); EXPECT_EQ(delivered, 29);
  EXPECT_GT(pressured.capacityRefused + pressured.queueRefused, 0u);
  ASSERT_FALSE(converted.empty());
  EXPECT_EQ(converted[0].frameId, 1); EXPECT_EQ(converted[0].sourceEpoch, 9u);
  EXPECT_EQ(converted[0].captureTimestamp100ns, 12345);
  EXPECT_EQ((*converted[0].pixels)[0], 1u);
}

TEST(D3DCaptureCpuBranch, AdmissionCountsAllNewSourceBytesAndReleasesReservations) {
  const auto before = D3DVideoImage::residentBytes.load();
  EXPECT_EQ(D3DCaptureCpuReservation::reserve(512u * 1024u * 1024u + 1u), nullptr);
  {
    auto token = D3DCaptureCpuReservation::reserve(4096); ASSERT_NE(token, nullptr);
    EXPECT_EQ(D3DVideoImage::residentBytes.load(), before + 4096);
  }
  EXPECT_EQ(D3DVideoImage::residentBytes.load(), before);
}

TEST(D3DCaptureCpuBranch, BlockedResourcePreparationLeavesProductionPixelsAdvancing) {
  Rig rig; ASSERT_TRUE(rig.start());
  auto consumer = D3DVideoConsumers::add(rig.reader.Get()); ASSERT_NE(consumer, nullptr);
  D3DVideoFramePool production; ASSERT_TRUE(production.initialize(rig.producer.Get(), 32, 32, 1));
  auto source = rig.image(32, 32), readback = rig.image(32, 32, true);
  ASSERT_NE(source, nullptr); ASSERT_NE(readback, nullptr);
  std::promise<void> entered, release; auto started = entered.get_future();
  const auto gate = release.get_future().share(); std::atomic<bool> first{true};
  D3DCaptureCpuBranch cpu(rig.producer.Get(), "capture:prepare", {}, [&] {
    if (first.exchange(false)) { entered.set_value(); gate.wait(); }
  });
  ASSERT_TRUE(cpu.valid());
  cpu.copy(rig.capture.Get(), source.Get(), 1, 1, 100);
  const bool blocked = started.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
  int delivered = 0; bool complete = true;
  for (int sequence = 2; sequence <= 13 && complete; ++sequence) {
    std::vector<uint8_t> pixels(32 * 32 * 4, static_cast<uint8_t>(sequence));
    rig.capture->UpdateSubresource(source.Get(), 0, nullptr, pixels.data(), 128, 0);
    const auto slot = production.beginCopy(rig.capture.Get(), source.Get());
    cpu.copy(rig.capture.Get(), source.Get(), sequence, 1, sequence * 100);
    rig.capture->Flush();
    std::shared_ptr<const GpuVideoFrame> frame;
    const auto due = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    do { frame = production.completed(rig.capture.Get(), slot); if (frame) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < due);
    if (!frame) { complete = false; break; }
    const auto image = std::dynamic_pointer_cast<const D3DVideoImage>(frame);
    const auto* view = image->view(consumer->id);
    if (!view) { complete = false; break; }
    rig.read->CopyResource(readback.Get(), view->texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(rig.read->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped))) { complete = false; break; }
    complete = static_cast<const uint8_t*>(mapped.pData)[0] == sequence;
    rig.read->Unmap(readback.Get(), 0); if (complete) ++delivered;
  }
  const auto pressured = cpu.stats(); release.set_value();
  const bool submitted = submitPrepared(cpu, rig.capture.Get(), source.Get(), 13, 1, 1300);
  const auto due = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (cpu.stats().converted == 0 && std::chrono::steady_clock::now() < due) {
    cpu.publishReady(rig.capture.Get()); std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  cpu.stopOnCaptureOwner(rig.capture.Get()); const auto result = cpu.take();
  EXPECT_TRUE(blocked); EXPECT_TRUE(complete); EXPECT_EQ(delivered, 12);
  EXPECT_EQ(pressured.copied, 0u); EXPECT_GT(pressured.preparationRefused, 0u);
  EXPECT_TRUE(submitted); ASSERT_FALSE(result.empty());
  EXPECT_EQ(result.front().frameId, 13); EXPECT_EQ((*result.front().pixels)[0], 13u);
}

TEST(D3DCaptureCpuBranch, ResizeBoundsRetirementAndPreservesAdmittedEpochs) {
  Rig rig; ASSERT_TRUE(rig.start());
  auto firstImage = rig.image(32, 32), secondImage = rig.image(64, 64), thirdImage = rig.image(96, 96);
  ASSERT_NE(firstImage, nullptr); ASSERT_NE(secondImage, nullptr); ASSERT_NE(thirdImage, nullptr);
  const auto fill = [&](ID3D11Texture2D* image, uint8_t value) {
    D3D11_TEXTURE2D_DESC desc{}; image->GetDesc(&desc);
    std::vector<uint8_t> pixels(static_cast<size_t>(desc.Width) * desc.Height * 4, value);
    rig.capture->UpdateSubresource(image, 0, nullptr, pixels.data(), desc.Width * 4, 0);
  };
  fill(firstImage.Get(), 1); fill(secondImage.Get(), 2); fill(thirdImage.Get(), 4);
  std::promise<void> entered, release; auto started = entered.get_future();
  auto gate = release.get_future().share(); std::atomic<bool> first{true};
  D3DCaptureCpuBranch cpu(rig.producer.Get(), "capture:resize", [&] {
    if (first.exchange(false)) { entered.set_value(); gate.wait(); }
  });
  ASSERT_TRUE(cpu.valid());
  const bool firstSubmitted = submitPrepared(cpu, rig.capture.Get(), firstImage.Get(), 1, 1, 100);
  const auto firstDue = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (started.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready && std::chrono::steady_clock::now() < firstDue) {
    cpu.publishReady(rig.capture.Get()); std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const bool secondSubmitted = submitPrepared(cpu, rig.capture.Get(), secondImage.Get(), 2, 2, 200);
  const auto secondDue = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (cpu.stats().admitted < 2 && std::chrono::steady_clock::now() < secondDue) {
    cpu.publishReady(rig.capture.Get()); std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const auto boundedBytes = D3DVideoImage::residentBytes.load();
  cpu.copy(rig.capture.Get(), thirdImage.Get(), 3, 3, 300);
  const auto stillBoundedBytes = D3DVideoImage::residentBytes.load();
  release.set_value();
  const auto thirdDue = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (cpu.stats().converted < 2 && std::chrono::steady_clock::now() < thirdDue) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  while (cpu.stats().copied < 3 && std::chrono::steady_clock::now() < thirdDue) {
    cpu.copy(rig.capture.Get(), thirdImage.Get(), 4, 3, 400); rig.capture->Flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  while (cpu.stats().converted < 3 && std::chrono::steady_clock::now() < thirdDue) {
    cpu.publishReady(rig.capture.Get()); std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  cpu.stopOnCaptureOwner(rig.capture.Get());
  const auto result = cpu.take();
  EXPECT_TRUE(firstSubmitted); EXPECT_TRUE(secondSubmitted);
  EXPECT_EQ(stillBoundedBytes, boundedBytes);
  EXPECT_GT(cpu.stats().capacityRefused, 0u);
  ASSERT_EQ(result.size(), 3u);
  EXPECT_EQ(result[0].width, 32); EXPECT_EQ(result[0].sourceEpoch, 1u);
  EXPECT_EQ(result[1].width, 64); EXPECT_EQ(result[1].sourceEpoch, 2u);
  EXPECT_EQ(result[2].width, 96); EXPECT_EQ(result[2].sourceEpoch, 3u);
  EXPECT_EQ(result[2].frameId, 4); EXPECT_EQ(result[2].captureTimestamp100ns, 400);
  EXPECT_EQ((*result[2].pixels)[0], 4u);
}
#endif
