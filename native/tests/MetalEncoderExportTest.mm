// Phase 4 Task 3 (mac-o4b): the Metal compositor's encoder IOSurface ring.
//
// Fixture invariants (plan-authoring rule 1):
//   - the ring is THREE slots (the plan's D3 value); the in-use test asserts
//     three distinct ids were seen before it pins all three;
//   - the ring is prepared off the render thread, so a test renders (bounded,
//     50 ticks) until the first export instead of assuming tick one exports;
//   - the background colour (B=40, G=120, R=200) is a fixture constant read
//     back from the exported surface, never from the compositor's own output.
#if COREVIDEO_WITH_METAL && !COREVIDEO_STUB

#import <IOSurface/IOSurface.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "modules/Interfaces.h"

namespace {
using namespace corevideo::modules;

std::unique_ptr<ICompositor> metalOrNull(std::string& reason) {
  auto compositor = createMetalCompositor();
  if (!compositor) {
    reason = "createMetalCompositor returned null";
    return nullptr;
  }
  CompositorRenderPlan probe;
  probe.renderPlanId = "encoder-export-probe";
  const auto frame = compositor->render(probe, {});
  if (!frame.gpuComposed) {
    reason = frame.warnings.empty() ? "not GPU composed" : frame.warnings.front();
    return nullptr;
  }
  return compositor;
}

#define MAKE_COMPOSITOR_OR_SKIP(var, name)                                                       \
  std::string skipReason_;                                                                       \
  auto var = metalOrNull(skipReason_);                                                           \
  if (!var) {                                                                                    \
    std::fprintf(stderr, "[  SKIPPED ] MetalEncoderExport.%s: %s - this test did NOT run\n", name, \
                 skipReason_.c_str());                                                           \
    return;                                                                                      \
  }

CompositorRenderPlan solidPlan(bool exportEncoder) {
  CompositorRenderPlan plan;
  plan.renderPlanId = "encoder-export";
  plan.width = 640;
  plan.height = 360;
  plan.skipCpuReadback = true;  // the light 60 Hz tick: export must not need a full tick
  plan.encoderTextureExport = exportEncoder;
  CompositorRenderPlanLayer bg;
  bg.layerId = "tiles-bg:encoder-export";
  bg.kind = "tiles-background";
  bg.rect = {0.f, 0.f, 1.f, 1.f};
  bg.hasFillColor = true;
  bg.fillColor = "#C87828";  // R=200 G=120 B=40
  plan.layers.push_back(bg);
  return plan;
}

ProgramFrame renderUntilExported(ICompositor& compositor, const CompositorRenderPlan& plan) {
  ProgramFrame frame;
  for (int i = 0; i < 50; ++i) {
    frame = compositor.render(plan, {});
    if (frame.encoderSharedTexture.iosurfaceId != 0) return frame;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));  // let the ring's preparation thread run
  }
  return frame;
}

IOSurfaceRef lookup(uint32_t id) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  return IOSurfaceLookup(id);
#pragma clang diagnostic pop
}

TEST(MetalEncoderExport, ExportsTheComposedFrameIntoTheRing) {
  MAKE_COMPOSITOR_OR_SKIP(compositor, "ExportsTheComposedFrameIntoTheRing");
  const auto frame = renderUntilExported(*compositor, solidPlan(true));
  ASSERT_NE(frame.encoderSharedTexture.iosurfaceId, 0u) << "the ring never became ready in 50 ticks";
  EXPECT_TRUE(frame.encoderSharedTexture.sharedHandleHex.empty());
  EXPECT_EQ(frame.encoderSharedTexture.width, 640);
  EXPECT_EQ(frame.encoderSharedTexture.height, 360);
  EXPECT_EQ(frame.encoderSharedTexture.frameNumber, frame.frameNumber);
  EXPECT_TRUE(compositor->encoderExporting());
  ASSERT_TRUE(frame.encoderSharedTexture.publishedFrameNumber);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
  while (frame.encoderSharedTexture.publishedFrameNumber->load() != frame.frameNumber &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_EQ(frame.encoderSharedTexture.publishedFrameNumber->load(), frame.frameNumber)
      << "the blit's completion must publish this frame within 100 ms";
  IOSurfaceRef surface = lookup(frame.encoderSharedTexture.iosurfaceId);
  ASSERT_NE(surface, nullptr) << "in-process lookup of the non-global ring surface (Task 0 row E-1)";
  IOSurfaceLock(surface, kIOSurfaceLockReadOnly, nullptr);
  const auto* base = static_cast<const uint8_t*>(IOSurfaceGetBaseAddress(surface));
  const size_t stride = IOSurfaceGetBytesPerRow(surface);
  const uint8_t* centre = base + 180 * stride + 320 * 4;
  const int b = centre[0], g = centre[1], r = centre[2];
  IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, nullptr);
  CFRelease(surface);
  EXPECT_NEAR(b, 40, 1);
  EXPECT_NEAR(g, 120, 1);
  EXPECT_NEAR(r, 200, 1);
}

TEST(MetalEncoderExport, NeverRewritesAnInUseSurface) {
  MAKE_COMPOSITOR_OR_SKIP(compositor, "NeverRewritesAnInUseSurface");
  const auto plan = solidPlan(true);
  const auto first = renderUntilExported(*compositor, plan);
  ASSERT_NE(first.encoderSharedTexture.iosurfaceId, 0u);
  std::vector<IOSurfaceRef> held;
  std::set<uint32_t> heldIds;
  auto hold = [&](uint32_t id) {
    IOSurfaceRef s = lookup(id);
    ASSERT_NE(s, nullptr);
    IOSurfaceIncrementUseCount(s);
    held.push_back(s);
    heldIds.insert(id);
  };
  hold(first.encoderSharedTexture.iosurfaceId);
  std::set<uint32_t> seen{first.encoderSharedTexture.iosurfaceId};
  for (int i = 0; i < 6; ++i) {
    const auto f = compositor->render(plan, {});
    const uint32_t id = f.encoderSharedTexture.iosurfaceId;
    EXPECT_EQ(heldIds.count(id), 0u) << "tick " << i << " rewrote an in-use surface";
    if (id != 0) seen.insert(id);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_EQ(seen.size(), 3u) << "fixture invariant: the ring has exactly three slots";
  for (uint32_t id : seen) {
    if (!heldIds.count(id)) hold(id);
  }
  const auto shedBefore = compositor->encoderExportShedFrames();
  const auto blocked = compositor->render(plan, {});
  EXPECT_EQ(blocked.encoderSharedTexture.iosurfaceId, 0u) << "all three in use: nothing is exported";
  EXPECT_FALSE(compositor->encoderExporting());
  EXPECT_EQ(compositor->encoderExportShedFrames() - shedBefore, 1) << "exactly one shed for that tick";
  for (IOSurfaceRef s : held) {
    IOSurfaceDecrementUseCount(s);
    CFRelease(s);
  }
  // A just-released slot can still read in use while the GPU finishes; bounded retry.
  uint32_t resumed = 0;
  for (int i = 0; i < 10 && resumed == 0; ++i) {
    resumed = compositor->render(plan, {}).encoderSharedTexture.iosurfaceId;
    if (resumed == 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_NE(resumed, 0u) << "export resumes once a slot is free";
}

// Review finding: a slot being rewritten must stop advertising the frame it
// held at COMMIT, not at completion. Cycle the ring back to the first slot and
// read that slot's number immediately after the rewriting render returns.
TEST(MetalEncoderExport, ARewrittenSlotStopsAdvertisingItsOldFrameAtCommit) {
  MAKE_COMPOSITOR_OR_SKIP(compositor, "ARewrittenSlotStopsAdvertisingItsOldFrameAtCommit");
  const auto plan = solidPlan(true);
  const auto first = renderUntilExported(*compositor, plan);
  ASSERT_NE(first.encoderSharedTexture.iosurfaceId, 0u);
  auto published = first.encoderSharedTexture.publishedFrameNumber;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
  while (published->load() != first.frameNumber && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_EQ(published->load(), first.frameNumber) << "precondition: the first blit completed";
  // Three slots: the third render after `first` rewrites first's slot.
  ProgramFrame rewrite;
  for (int i = 0; i < 3; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(3));  // let earlier blits finish, so no slot reads in use
    rewrite = compositor->render(plan, {});
  }
  ASSERT_EQ(rewrite.encoderSharedTexture.iosurfaceId, first.encoderSharedTexture.iosurfaceId)
      << "fixture invariant: the ring cycled back to the first slot";
  EXPECT_NE(published->load(), first.frameNumber)
      << "the slot still advertised its old frame while being rewritten";
}

TEST(MetalEncoderExport, NoExportWhenNoStreamDestination) {
  MAKE_COMPOSITOR_OR_SKIP(compositor, "NoExportWhenNoStreamDestination");
  const auto plan = solidPlan(false);
  for (int i = 0; i < 20; ++i) {
    const auto frame = compositor->render(plan, {});
    EXPECT_EQ(frame.encoderSharedTexture.iosurfaceId, 0u);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_FALSE(compositor->encoderExporting());
}

}  // namespace

#endif
