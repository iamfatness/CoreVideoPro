#include "modules/Interfaces.h"
#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include <cmath>

#if defined(_WIN32) && !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_D3D11
TEST(D3DProgramBuffer, RetainsTaggedNv12AndDeliversWithoutFurtherRendering) {
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
  using namespace corevideo::modules;
  ComPtrLite<ID3D11Device> device;
  ComPtrLite<ID3D11DeviceContext> context;
  ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, context.put())));
  for (const int phaseMs : {4, 10}) {
    auto compositor = createD3D11Compositor();
    ASSERT_TRUE(compositor != nullptr);
    compositor->configureProgramBuffer(3);
    CompositorRenderPlan program;
    program.sceneId = "phase-test"; program.renderPlanId = "phase-test";
    program.width = 320; program.height = 180; program.fps = 60; program.skipCpuReadback = true;
    CompositorRenderPlanLayer layer;
    layer.layerId = "route:test"; layer.kind = "participant-video"; layer.participantId = "test";
    layer.sourceId = "zoom:test"; layer.rect = {0, 0, 1, 1}; layer.borderStyle = "none";
    program.layers.push_back(layer);
    auto multiview = program;
    multiview.layers.front().layerId = "multiview-pgm:buffer-cell";
    multiview.layers.front().hasClipRect = true; multiview.layers.front().clipRect = {0, 0, 1, 1};
    int changes = 0, previous = -1;
    const auto anchor = std::chrono::steady_clock::now();
    for (int n = 0; n < 24; ++n) {
      const auto tick = anchor + std::chrono::nanoseconds(n * 1000000000LL / 60);
      std::this_thread::sleep_until(tick);
      VideoFrame source;
      source.participantId = "test"; source.width = source.pixelWidth = 64; source.height = source.pixelHeight = 64;
      source.pixelStride = 256; source.frameId = n + 1; source.timestampMs = n * 17;
      source.pixels = std::make_shared<std::vector<uint8_t>>(64u * 64u * 4u, static_cast<uint8_t>(30 + n * 7));
      (void)compositor->render(program, {source});
      std::this_thread::sleep_until(tick + std::chrono::milliseconds(phaseMs));
      const auto exported = compositor->renderMultiview(multiview, {source});
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
      if (n >= 8 && previous >= 0 && pixel != previous) ++changes;
      previous = pixel;
      ProgramFrame packet; while (compositor->takeDeliveredProgramFrame(packet, 0)) {}
    }
    EXPECT_TRUE(changes >= 4) << "phase_ms=" << phaseMs << " pixel_changes=" << changes;
  }
}
#endif
