// Zero-copy media frames into the Metal compositor (plan 2026-11-21 N4, Task 5).
// The real Metal compositor, real IOSurface-backed CVPixelBuffers. A machine
// with no Metal device is an explicit skip (the MetalCompositorTest pattern).
#if COREVIDEO_WITH_METAL && !COREVIDEO_STUB

#import <CoreVideo/CoreVideo.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "compositor/MetalPixelBufferTextures.h"
#include "modules/Interfaces.h"
#include "modules/PixelBufferVideoFrame.h"

namespace {

using namespace corevideo::modules;
using corevideo::compositor::MetalPixelBufferTextures;

std::unique_ptr<ICompositor> metalOrSkipReason(std::string& reason) {
  auto compositor = createMetalCompositor();
  if (!compositor) { reason = "createMetalCompositor returned null under COREVIDEO_WITH_METAL"; return nullptr; }
  CompositorRenderPlan probe;
  probe.renderPlanId = "zero-copy-probe";
  const auto frame = compositor->render(probe, {});
  for (const auto& warning : frame.warnings) {
    if (warning.rfind("Metal compositor unavailable", 0) == 0) { reason = warning; return nullptr; }
  }
  if (!frame.gpuComposed) { reason = "Metal compositor did not GPU-compose the probe frame"; return nullptr; }
  return compositor;
}

#define MAKE_COMPOSITOR_OR_SKIP(var, name)                                                   \
  std::string skipReason_;                                                                   \
  auto var = metalOrSkipReason(skipReason_);                                                 \
  if (!var) {                                                                                \
    std::fprintf(stderr, "[  SKIPPED ] MetalZeroCopy.%s: %s\n", name, skipReason_.c_str());  \
    ASSERT_TRUE(skipReason_.find("no Metal device") != std::string::npos);                   \
    return;                                                                                  \
  }

NSDictionary* surfaceAttributes(int width, int height) {
  return @{
    (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
    (id)kCVPixelBufferWidthKey : @(width),
    (id)kCVPixelBufferHeightKey : @(height),
    (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
    (id)kCVPixelBufferMetalCompatibilityKey : @YES,
  };
}

// Fills every pixel with fill(x, y) -> {b, g, r, a}.
template <typename Fill>
void fillBuffer(CVPixelBufferRef buffer, Fill fill) {
  CVPixelBufferLockBaseAddress(buffer, 0);
  auto* base = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(buffer));
  const size_t stride = CVPixelBufferGetBytesPerRow(buffer);
  const int width = static_cast<int>(CVPixelBufferGetWidth(buffer));
  const int height = static_cast<int>(CVPixelBufferGetHeight(buffer));
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const std::array<uint8_t, 4> c = fill(x, y);
      std::memcpy(base + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4u, c.data(), 4);
    }
  }
  CVPixelBufferUnlockBaseAddress(buffer, 0);
}

void fillFlat(CVPixelBufferRef buffer, uint8_t b, uint8_t g, uint8_t r) {
  fillBuffer(buffer, [&](int, int) { return std::array<uint8_t, 4>{b, g, r, 0xff}; });
}

VideoFrame zeroCopyFrame(const std::string& id, CVPixelBufferRef buffer, int64_t frameId) {
  VideoFrame frame;
  frame.participantId = id;
  frame.gpuPixels = makePixelBufferVideoFrame(buffer);
  frame.width = frame.naturalWidth = static_cast<int>(CVPixelBufferGetWidth(buffer));
  frame.height = frame.naturalHeight = static_cast<int>(CVPixelBufferGetHeight(buffer));
  frame.frameId = frameId;
  return frame;
}

VideoFrame cpuTwin(const std::string& id, CVPixelBufferRef buffer, int64_t frameId) {
  VideoFrame frame;
  frame.participantId = id;
  const int width = static_cast<int>(CVPixelBufferGetWidth(buffer));
  const int height = static_cast<int>(CVPixelBufferGetHeight(buffer));
  auto pixels = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(width) * height * 4u);
  CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
  const auto* base = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(buffer));
  const size_t stride = CVPixelBufferGetBytesPerRow(buffer);
  for (int y = 0; y < height; ++y) {
    std::memcpy(pixels->data() + static_cast<size_t>(y) * width * 4u, base + static_cast<size_t>(y) * stride,
                static_cast<size_t>(width) * 4u);
  }
  CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
  frame.pixels = std::move(pixels);
  frame.width = frame.naturalWidth = frame.pixelWidth = width;
  frame.height = frame.naturalHeight = frame.pixelHeight = height;
  frame.pixelStride = width * 4;
  frame.frameId = frameId;
  return frame;
}

CompositorRenderPlan fullCanvas(const std::string& id, bool light) {
  CompositorRenderPlan plan;
  plan.renderPlanId = "plan-zero-copy";
  CompositorRenderPlanLayer layer;
  layer.layerId = "layer:" + id;
  layer.kind = "participant-video";
  layer.participantId = id;
  layer.sourceId = "zoom:" + id;
  layer.rect = {0.f, 0.f, 1.f, 1.f};
  plan.layers.push_back(layer);
  plan.skipCpuReadback = light;  // a light tick: commit and return, never wait for the GPU
  return plan;
}

std::array<int, 3> previewAt(const ProgramFramePreviewPixels& preview, float fx, float fy) {
  const int x = std::min(preview.width - 1, static_cast<int>(fx * preview.width));
  const int y = std::min(preview.height - 1, static_cast<int>(fy * preview.height));
  const size_t offset = (static_cast<size_t>(y) * preview.width + x) * 4u;
  return {preview.bgra[offset], preview.bgra[offset + 1], preview.bgra[offset + 2]};
}

std::array<int, 3> surfaceCentre(uint32_t iosurfaceId) {
  IOSurfaceRef surface = IOSurfaceLookup(iosurfaceId);
  if (!surface) return {-1, -1, -1};
  IOSurfaceLock(surface, kIOSurfaceLockReadOnly, nullptr);
  const auto* base = static_cast<const uint8_t*>(IOSurfaceGetBaseAddress(surface));
  const size_t stride = IOSurfaceGetBytesPerRow(surface);
  const size_t x = IOSurfaceGetWidth(surface) / 2, y = IOSurfaceGetHeight(surface) / 2;
  const uint8_t* p = base + y * stride + x * 4u;
  std::array<int, 3> out{p[0], p[1], p[2]};
  IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, nullptr);
  CFRelease(surface);
  return out;
}

int64_t steadyMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Disarms the pass gate even when an assertion returns early.
struct GateGuard {
  id<MTLSharedEvent> event;
  ~GateGuard() {
    event.signaledValue = 1'000'000;  // let anything still gated run
    MetalPixelBufferTextures::disarmPassGateForTest();
  }
};

}  // namespace

// The zero-copy frame and a CPU BGRA copy of the SAME buffer render the same
// Program pixels, and the zero-copy render uploads nothing.
// Fixture: a 320x180 gradient, B = x*255/319, G = y*255/179, R = 0x80.
TEST(MetalZeroCopy, AnIOSurfaceFrameRendersTheSamePixelsAsItsBgraTwin) {
  MAKE_COMPOSITOR_OR_SKIP(compositor, "AnIOSurfaceFrameRendersTheSamePixelsAsItsBgraTwin");
  CVPixelBufferRef buffer = nullptr;
  ASSERT_EQ(CVPixelBufferCreate(kCFAllocatorDefault, 320, 180, kCVPixelFormatType_32BGRA,
                                (__bridge CFDictionaryRef)surfaceAttributes(320, 180), &buffer), kCVReturnSuccess);
  fillBuffer(buffer, [](int x, int y) {
    return std::array<uint8_t, 4>{static_cast<uint8_t>(x * 255 / 319), static_cast<uint8_t>(y * 255 / 179), 0x80, 0xff};
  });
  const auto cpu = compositor->render(fullCanvas("cpu", false), {cpuTwin("cpu", buffer, 1)});
  const auto before = compositor->sourceTexStats();
  const auto gpuFrame = zeroCopyFrame("gpu", buffer, 1);
  ASSERT_TRUE(gpuFrame.hasGpuPixels());
  ASSERT_FALSE(gpuFrame.hasPixels());
  const auto gpu = compositor->render(fullCanvas("gpu", false), {gpuFrame});
  const auto after = compositor->sourceTexStats();
  CVPixelBufferRelease(buffer);
  EXPECT_EQ(after.cachedUploads, before.cachedUploads) << "the zero-copy frame was uploaded";
  EXPECT_EQ(after.scratchUploads, before.scratchUploads) << "the zero-copy frame took a scratch upload";
  EXPECT_EQ(after.zeroCopyBinds, before.zeroCopyBinds + 1) << "the zero-copy path was not taken";
  ASSERT_FALSE(cpu.preview.bgra.empty());
  ASSERT_FALSE(gpu.preview.bgra.empty());
  int compared = 0;
  for (const float fx : {0.1f, 0.5f, 0.9f}) {
    for (const float fy : {0.1f, 0.5f, 0.9f}) {
      const auto a = previewAt(cpu.preview, fx, fy), b = previewAt(gpu.preview, fx, fy);
      for (int ch = 0; ch < 3; ++ch) EXPECT_NEAR(a[ch], b[ch], 1) << "fx " << fx << " fy " << fy << " ch " << ch;
      ++compared;
    }
  }
  EXPECT_EQ(compared, 9);
  // The gradient really is on Program (not two identical slates): corners differ.
  EXPECT_GT(std::abs(previewAt(gpu.preview, 0.9f, 0.5f)[0] - previewAt(gpu.preview, 0.1f, 0.5f)[0]), 150);
}

// A held frame drawn on several ticks is wrapped once, not once per draw.
TEST(MetalZeroCopy, AHeldIOSurfaceFrameIsBoundOnceAndReused) {
  MAKE_COMPOSITOR_OR_SKIP(compositor, "AHeldIOSurfaceFrameIsBoundOnceAndReused");
  CVPixelBufferRef buffer = nullptr;
  ASSERT_EQ(CVPixelBufferCreate(kCFAllocatorDefault, 64, 36, kCVPixelFormatType_32BGRA,
                                (__bridge CFDictionaryRef)surfaceAttributes(64, 36), &buffer), kCVReturnSuccess);
  fillFlat(buffer, 0xE0, 0x20, 0x20);
  const auto frame = zeroCopyFrame("held", buffer, 7);
  const auto before = compositor->sourceTexStats();
  for (int i = 0; i < 3; ++i) (void)compositor->render(fullCanvas("held", false), {frame});
  const auto after = compositor->sourceTexStats();
  CVPixelBufferRelease(buffer);
  EXPECT_EQ(after.zeroCopyBinds, before.zeroCopyBinds + 1);
  EXPECT_EQ(after.cachedUploads, before.cachedUploads);
}

// THE LIFETIME RULE. A surface a pass sampled must not go back to the decoder's
// pool - where the decoder would write the next picture into it - until that
// pass has completed on the GPU. The GPU is held deterministically by the pass
// gate (plan default P7): pass 1 waits for event value 1, pass 2 for value 2.
//
// Fixture invariants: a CVPixelBufferPool whose allocation threshold is 2, so
// the pool can hand out a third buffer ONLY by recycling one it handed out
// before; frame A (RED) then frame B (BLUE) on ONE source, so B evicts A from
// the compositor's per-source cache and pass 1's lease is A's only holder.
TEST(MetalZeroCopy, ASampledSurfaceIsNeverRecycledBeforeItsPassCompletes) {
  MAKE_COMPOSITOR_OR_SKIP(compositor, "ASampledSurfaceIsNeverRecycledBeforeItsPassCompletes");
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  ASSERT_NE(device, nil);
  id<MTLSharedEvent> event = [device newSharedEvent];
  GateGuard guard{event};
  CVPixelBufferPoolRef pool = nullptr;
  ASSERT_EQ(CVPixelBufferPoolCreate(kCFAllocatorDefault, nullptr, (__bridge CFDictionaryRef)surfaceAttributes(64, 36),
                                    &pool), kCVReturnSuccess);
  NSDictionary* threshold = @{ (id)kCVPixelBufferPoolAllocationThresholdKey : @2 };
  const auto take = [&](CVPixelBufferRef& out) {
    out = nullptr;
    return CVPixelBufferPoolCreatePixelBufferWithAuxAttributes(kCFAllocatorDefault, pool,
                                                               (__bridge CFDictionaryRef)threshold, &out);
  };
  CVPixelBufferRef a = nullptr, b = nullptr;
  ASSERT_EQ(take(a), kCVReturnSuccess);
  ASSERT_EQ(take(b), kCVReturnSuccess);
  const uint32_t idA = IOSurfaceGetID(CVPixelBufferGetIOSurface(a));
  const uint32_t idB = IOSurfaceGetID(CVPixelBufferGetIOSurface(b));
  fillFlat(a, 0x20, 0x20, 0xE0);  // RED
  fillFlat(b, 0xE0, 0x20, 0x20);  // BLUE

  // Warm the target so its IOSurface id is known, then gate the GPU.
  const auto warm = compositor->render(fullCanvas("other", false), {});
  const uint32_t programSurface = warm.sharedTexture.iosurfaceId;
  ASSERT_NE(programSurface, 0u);
  const auto statsBefore = compositor->sourceTexStats();
  MetalPixelBufferTextures::armPassGateForTest(event, 1);
  {
    const auto frameA = zeroCopyFrame("clip", a, 1);
    (void)compositor->render(fullCanvas("clip", true), {frameA});
  }
  MetalPixelBufferTextures::armPassGateForTest(event, 2);
  {
    const auto frameB = zeroCopyFrame("clip", b, 2);
    (void)compositor->render(fullCanvas("clip", true), {frameB});
  }
  EXPECT_EQ(compositor->sourceTexStats().zeroCopyBinds, statsBefore.zeroCopyBinds + 2);
  // Every reference the test held is gone: only the compositor can keep A and B out of the pool.
  CVPixelBufferRelease(a);
  CVPixelBufferRelease(b);
  a = b = nullptr;
  std::vector<CVPixelBufferRef> recycled;
  for (int i = 0; i < 2; ++i) {
    CVPixelBufferRef next = nullptr;
    if (take(next) == kCVReturnSuccess && next) {
      const uint32_t id = IOSurfaceGetID(CVPixelBufferGetIOSurface(next));
      EXPECT_NE(id, idA) << "surface A went back to the pool while pass 1 could still sample it";
      EXPECT_NE(id, idB) << "surface B went back to the pool while pass 2 could still sample it";
      fillFlat(next, 0x20, 0xE0, 0x20);  // the decoder writing its NEXT picture: GREEN
      recycled.push_back(next);
    }
  }
  // Let pass 1 run. It must sample A as it was: RED, never the GREEN a recycle wrote.
  event.signaledValue = 1;
  std::array<int, 3> afterPass1{};
  for (const auto deadline = steadyMs() + 3000; steadyMs() < deadline;) {
    afterPass1 = surfaceCentre(programSurface);
    if (std::abs(afterPass1[2] - 0xE0) <= 6 && std::abs(afterPass1[0] - 0x20) <= 6) break;
    if (std::abs(afterPass1[1] - 0xE0) <= 6 && std::abs(afterPass1[0] - 0x20) <= 6) break;  // GREEN: torn
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_NEAR(afterPass1[2], 0xE0, 6) << "pass 1 drew a recycled surface (centre BGR " << afterPass1[0] << ","
                                      << afterPass1[1] << "," << afterPass1[2] << ")";
  EXPECT_NEAR(afterPass1[1], 0x20, 6);
  event.signaledValue = 2;
  std::array<int, 3> afterPass2{};
  for (const auto deadline = steadyMs() + 3000; steadyMs() < deadline;) {
    afterPass2 = surfaceCentre(programSurface);
    if (std::abs(afterPass2[0] - 0xE0) <= 6) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_NEAR(afterPass2[0], 0xE0, 6) << "pass 2 drew B (BLUE)";
  // No leak: once pass 1 has completed, A is free again. B stays held by the
  // per-source cache (it is the source's current frame).
  for (auto* buffer : recycled) CVPixelBufferRelease(buffer);
  recycled.clear();
  MetalPixelBufferTextures::disarmPassGateForTest();
  (void)compositor->render(fullCanvas("other", false), {});  // a completed, waited pass
  bool aCameBack = false;
  for (int i = 0; i < 2 && !aCameBack; ++i) {
    CVPixelBufferRef next = nullptr;
    if (take(next) == kCVReturnSuccess && next) {
      aCameBack = IOSurfaceGetID(CVPixelBufferGetIOSurface(next)) == idA;
      recycled.push_back(next);
    }
  }
  for (auto* buffer : recycled) CVPixelBufferRelease(buffer);
  EXPECT_TRUE(aCameBack) << "surface A was never released after its pass completed (a leaked lease)";
  CVPixelBufferPoolRelease(pool);
}

#endif  // COREVIDEO_WITH_METAL && !COREVIDEO_STUB
