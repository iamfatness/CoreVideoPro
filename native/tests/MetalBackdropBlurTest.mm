// P9 true backdrop blur on Metal: the twin of D3D11BackdropBlurTest.cpp. The
// GPU pass is judged against the SAME CPU reference and the same tolerance
// (compositor/BackdropBlur.h), which is how Mac and Windows are held to the
// same picture: each matches the reference on its own GPU. A machine without
// a Metal device logs the reason and returns green (the vendored gtest has no
// GTEST_SKIP; same pattern as MetalCompositorTest.cpp).

#include <gtest/gtest.h>

#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_METAL

#import <Metal/Metal.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "BackdropBlurFixture.h"
#include "compositor/BackdropBlur.h"
#include "compositor/MetalBackdropBlur.h"
#include "modules/Interfaces.h"

namespace {

using corevideo::modules::MetalBackdropBlur;
namespace compositor = corevideo::compositor;

id<MTLTexture> makeTarget(id<MTLDevice> device, int width, int height, const std::vector<uint8_t>& pixels) {
  MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:NSUInteger(width)
                                                                                 height:NSUInteger(height)
                                                                              mipmapped:NO];
  desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
  desc.storageMode = MTLStorageModeShared;
  id<MTLTexture> texture = [device newTextureWithDescriptor:desc];
  [texture replaceRegion:MTLRegionMake2D(0, 0, NSUInteger(width), NSUInteger(height))
             mipmapLevel:0
               withBytes:pixels.data()
             bytesPerRow:NSUInteger(width) * 4];
  return texture;
}

std::vector<uint8_t> readTarget(id<MTLTexture> texture) {
  std::vector<uint8_t> out(size_t(texture.width) * texture.height * 4);
  [texture getBytes:out.data()
        bytesPerRow:texture.width * 4
         fromRegion:MTLRegionMake2D(0, 0, texture.width, texture.height)
        mipmapLevel:0];
  return out;
}

int maxDifference(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, double& mean) {
  int maximum = 0;
  uint64_t total = 0;
  size_t count = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    if ((i & 3) == 3) continue;
    const int delta = std::abs(int(a[i]) - int(b[i]));
    maximum = std::max(maximum, delta);
    total += uint64_t(delta); ++count;
  }
  mean = count ? double(total) / double(count) : 0;
  return maximum;
}

#define METAL_DEVICE_OR_SKIP(var)                                           \
  id<MTLDevice> var = MTLCreateSystemDefaultDevice();                       \
  if (!var) {                                                               \
    std::fprintf(stderr, "[metal-test] unavailable: no Metal device\n");    \
    return;                                                                 \
  }

TEST(MetalBackdropBlur, MatchesTheSharedCpuReferenceAt1080p) {
  METAL_DEVICE_OR_SKIP(device);
  const int width = 1920, height = 1080;
  auto expected = backdrop_fixture::checkerBgra(width, height);
  id<MTLTexture> target = makeTarget(device, width, height, expected);
  ASSERT_TRUE(target != nil);
  MetalBackdropBlur pass;
  std::string error;
  ASSERT_TRUE(pass.initialize(device, error)) << error;
  ASSERT_TRUE(pass.reserve(device, width, height));
  const auto draws = backdrop_fixture::twoPlates1080p();
  ASSERT_EQ(2u, draws.size());
  id<MTLCommandQueue> queue = [device newCommandQueue];
  id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
  for (const auto& draw : draws) {
    ASSERT_TRUE(pass.apply(commandBuffer, target, draw));
    compositor::backdropReference(expected, width, height, draw);
  }
  [commandBuffer commit];
  [commandBuffer waitUntilCompleted];
  ASSERT_TRUE(commandBuffer.status == MTLCommandBufferStatusCompleted);
  const auto actual = readTarget(target);
  double mean = 0;
  const int maximum = maxDifference(actual, expected, mean);
  std::printf("[backdrop-parity] metal vs reference: max=%d mean=%.4f\n", maximum, mean);
  EXPECT_LE(maximum, compositor::kBackdropReferenceTolerance);
  EXPECT_NE(actual, backdrop_fixture::checkerBgra(width, height));
}

TEST(MetalBackdropBlur, MatchesTheReferenceAtSmallBlurAndOddSizes) {
  METAL_DEVICE_OR_SKIP(device);
  const int width = 646, height = 362;
  auto expected = backdrop_fixture::checkerBgra(width, height, 5);
  id<MTLTexture> target = makeTarget(device, width, height, expected);
  MetalBackdropBlur pass;
  std::string error;
  ASSERT_TRUE(pass.initialize(device, error)) << error;
  ASSERT_TRUE(pass.reserve(device, width, height));
  corevideo::graphics::GraphicsBackdropSet set;
  set.width = width; set.height = height;
  corevideo::graphics::GraphicsBackdropPlate plate;
  plate.box = {401.3, 211.7, 260, 170}; plate.clip = {0, 0, double(width), double(height)};
  plate.cornerRadius = 9; plate.blur = 3; plate.tint = 0x30ff8000u;
  compositor::BackdropDraw draw;
  ASSERT_TRUE(compositor::resolveBackdropDraw(plate, set, {}, {0, 0, 1, 1}, {0, 0, 1, 1}, .75f, width, height, draw));
  id<MTLCommandQueue> queue = [device newCommandQueue];
  id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
  ASSERT_TRUE(pass.apply(commandBuffer, target, draw));
  [commandBuffer commit];
  [commandBuffer waitUntilCompleted];
  compositor::backdropReference(expected, width, height, draw);
  double mean = 0;
  const int maximum = maxDifference(readTarget(target), expected, mean);
  std::printf("[backdrop-parity] metal small/odd vs reference: max=%d mean=%.4f\n", maximum, mean);
  EXPECT_LE(maximum, compositor::kBackdropReferenceTolerance);
}

// Reported, not asserted (see the D3D11 bench). A CI virtual machine's GPU
// time says nothing about a customer Mac; the line is for local runs.
TEST(MetalBackdropBlurBench, TwoGlassPlatesAt1080p) {
  METAL_DEVICE_OR_SKIP(device);
  const int width = 1920, height = 1080;
  id<MTLTexture> target = makeTarget(device, width, height, backdrop_fixture::checkerBgra(width, height));
  MetalBackdropBlur pass;
  std::string error;
  ASSERT_TRUE(pass.initialize(device, error)) << error;
  ASSERT_TRUE(pass.reserve(device, width, height));
  const auto draws = backdrop_fixture::twoPlates1080p();
  id<MTLCommandQueue> queue = [device newCommandQueue];
  std::vector<double> gpuUs;
  for (int i = 0; i < 230; ++i) {
    id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
    for (const auto& draw : draws) ASSERT_TRUE(pass.apply(commandBuffer, target, draw));
    [commandBuffer commit];
    [commandBuffer waitUntilCompleted];
    if (i >= 30 && commandBuffer.GPUEndTime > commandBuffer.GPUStartTime)
      gpuUs.push_back((commandBuffer.GPUEndTime - commandBuffer.GPUStartTime) * 1e6);
  }
  if (gpuUs.empty()) return;
  std::sort(gpuUs.begin(), gpuUs.end());
  double sum = 0;
  for (double v : gpuUs) sum += v;
  std::printf("[backdrop-bench] metal device=%s frames=%zu gpu_us mean=%.1f p50=%.1f p99=%.1f max=%.1f\n",
              device.name.UTF8String, gpuUs.size(), sum / gpuUs.size(), gpuUs[gpuUs.size() / 2],
              gpuUs[std::min(gpuUs.size() - 1, gpuUs.size() * 99 / 100)], gpuUs.back());
}

TEST(MetalBackdropBlur, CompositorBlursProgramUnderAGlassPlate) {
  auto probe = corevideo::modules::createMetalCompositor();
  if (!probe) { std::fprintf(stderr, "[metal-test] unavailable: no compositor\n"); return; }
  {
    corevideo::modules::CompositorRenderPlan empty;
    empty.renderPlanId = "probe";
    const auto frame = probe->render(empty, {});
    for (const auto& warning : frame.warnings)
      if (warning.rfind("Metal compositor unavailable", 0) == 0) {
        std::fprintf(stderr, "[metal-test] unavailable: %s\n", warning.c_str());
        return;
      }
  }
  std::string error;
  const auto glass = corevideo::graphics::GraphicsTemplate::load(backdrop_fixture::glassTemplate(true), error);
  ASSERT_TRUE(glass) << error;
  const auto plain = corevideo::graphics::GraphicsTemplate::load(backdrop_fixture::glassTemplate(false), error);
  ASSERT_TRUE(plain) << error;
  const int width = 1280, height = 720;
  const std::vector<corevideo::modules::VideoFrame> frames{backdrop_fixture::checkerFrame(width, height)};
  const auto renderReady = [&](const corevideo::modules::CompositorRenderPlan& plan) {
    auto compositor = corevideo::modules::createMetalCompositor();
    if (!compositor) throw std::runtime_error("no compositor");
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
      auto frame = compositor->render(plan, frames);
      if (std::find(frame.warnings.begin(), frame.warnings.end(), "graphics-preparing") == frame.warnings.end())
        return compositor->render(plan, frames);
      if (std::chrono::steady_clock::now() >= limit) throw std::runtime_error("graphics preparation timed out");
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  };
  const auto blurred = renderReady(backdrop_fixture::glassPlan(width, height, glass));
  const auto sharp = renderReady(backdrop_fixture::glassPlan(width, height, plain));
  EXPECT_TRUE(std::find(blurred.warnings.begin(), blurred.warnings.end(), "graphics-backdrop-unavailable") ==
              blurred.warnings.end());
  const auto inside = [](const auto& frame) { return backdrop_fixture::edgeEnergy(frame.preview, .32f, .32f, .36f, .36f); };
  const auto outside = [](const auto& frame) { return backdrop_fixture::edgeEnergy(frame.preview, .0f, .0f, .2f, .2f); };
  std::printf("[backdrop-e2e] metal inside sharp=%llu glass=%llu; outside sharp=%llu glass=%llu\n",
              (unsigned long long)inside(sharp), (unsigned long long)inside(blurred),
              (unsigned long long)outside(sharp), (unsigned long long)outside(blurred));
  EXPECT_GT(inside(sharp), 0u);
  EXPECT_LT(inside(blurred) * 4, inside(sharp));
  EXPECT_EQ(outside(sharp), outside(blurred));
}

}  // namespace

#endif
