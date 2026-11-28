// P9 true backdrop blur on D3D11: the GPU pass against the shared CPU
// reference (compositor/BackdropBlur.h), the compositor end to end, and the
// frame-budget bench for the outputs gate's "two pack graphics" at 1080p.

#include <gtest/gtest.h>

#if defined(_WIN32) && !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_D3D11

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <dxgi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "BackdropBlurFixture.h"
#include "compositor/BackdropBlur.h"
#include "compositor/D3D11BackdropBlur.h"
#include "modules/Interfaces.h"

namespace {

using corevideo::modules::ComPtrLite;
using corevideo::modules::D3D11BackdropBlur;
namespace compositor = corevideo::compositor;

struct GpuTarget {
  ComPtrLite<ID3D11Device> device;
  ComPtrLite<ID3D11DeviceContext> context;
  ComPtrLite<ID3D11Texture2D> texture, staging;
  ComPtrLite<ID3D11RenderTargetView> view;
  int width = 0, height = 0;
  GpuTarget(int w, int h, const std::vector<uint8_t>& pixels) : width(w), height(h) {
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                 D3D11_SDK_VERSION, device.put(), nullptr, context.put())))
      throw std::runtime_error("D3D11CreateDevice failed");
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = UINT(w); desc.Height = UINT(h); desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA initial{pixels.data(), UINT(w * 4), 0};
    if (FAILED(device->CreateTexture2D(&desc, &initial, texture.put())) ||
        FAILED(device->CreateRenderTargetView(texture.get(), nullptr, view.put())))
      throw std::runtime_error("target creation failed");
    desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, staging.put()))) throw std::runtime_error("staging failed");
    ID3D11RenderTargetView* targets[] = {view.get()};
    context->OMSetRenderTargets(1, targets, nullptr);
  }
  std::vector<uint8_t> read() {
    context->CopyResource(staging.get(), texture.get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) throw std::runtime_error("map failed");
    std::vector<uint8_t> out(size_t(width) * height * 4);
    for (int y = 0; y < height; ++y)
      std::memcpy(out.data() + size_t(y) * width * 4, static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch,
                  size_t(width) * 4);
    context->Unmap(staging.get(), 0);
    return out;
  }
};

struct Difference { int maximum = 0; double mean = 0; size_t over = 0; };
Difference compare(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
  Difference d;
  uint64_t total = 0;
  size_t count = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    if ((i & 3) == 3) continue;  // alpha: Program is opaque on both sides
    const int delta = std::abs(int(a[i]) - int(b[i]));
    d.maximum = std::max(d.maximum, delta);
    if (delta > compositor::kBackdropReferenceTolerance) ++d.over;
    total += uint64_t(delta); ++count;
  }
  d.mean = count ? double(total) / double(count) : 0;
  return d;
}

TEST(D3D11BackdropBlur, MatchesTheSharedCpuReferenceAt1080p) {
  const int width = 1920, height = 1080;
  auto expected = backdrop_fixture::checkerBgra(width, height);
  GpuTarget gpu(width, height, expected);
  D3D11BackdropBlur pass;
  std::string error;
  ASSERT_TRUE(pass.initialize(gpu.device.get(), error)) << error;
  ASSERT_TRUE(pass.reserve(gpu.device.get(), width, height));
  const auto draws = backdrop_fixture::twoPlates1080p();
  ASSERT_EQ(2u, draws.size());
  for (const auto& draw : draws) {
    ASSERT_TRUE(pass.apply(gpu.context.get(), gpu.texture.get(), gpu.view.get(), width, height, draw));
    compositor::backdropReference(expected, width, height, draw);
  }
  const auto actual = gpu.read();
  const auto difference = compare(actual, expected);
  std::printf("[backdrop-parity] d3d11 vs reference: max=%d mean=%.4f over_tolerance=%zu\n", difference.maximum,
              difference.mean, difference.over);
  EXPECT_LE(difference.maximum, compositor::kBackdropReferenceTolerance);
  // The pass did something: the plate interiors no longer equal the checker.
  EXPECT_NE(actual, backdrop_fixture::checkerBgra(width, height));
}

TEST(D3D11BackdropBlur, MatchesTheReferenceAtSmallBlurAndOddSizes) {
  // A non-multiple-of-d frame and a plate touching the right/bottom edges:
  // exercises the clamped reads at the frame edge.
  const int width = 646, height = 362;
  auto expected = backdrop_fixture::checkerBgra(width, height, 5);
  GpuTarget gpu(width, height, expected);
  D3D11BackdropBlur pass;
  std::string error;
  ASSERT_TRUE(pass.initialize(gpu.device.get(), error)) << error;
  ASSERT_TRUE(pass.reserve(gpu.device.get(), width, height));
  corevideo::graphics::GraphicsBackdropSet set;
  set.width = width; set.height = height;
  corevideo::graphics::GraphicsBackdropPlate plate;
  plate.box = {401.3, 211.7, 260, 170}; plate.clip = {0, 0, double(width), double(height)};
  plate.cornerRadius = 9; plate.blur = 3; plate.tint = 0x30ff8000u;
  compositor::BackdropDraw draw;
  ASSERT_TRUE(compositor::resolveBackdropDraw(plate, set, {}, {0, 0, 1, 1}, {0, 0, 1, 1}, .75f, width, height, draw));
  ASSERT_TRUE(pass.apply(gpu.context.get(), gpu.texture.get(), gpu.view.get(), width, height, draw));
  compositor::backdropReference(expected, width, height, draw);
  const auto difference = compare(gpu.read(), expected);
  std::printf("[backdrop-parity] d3d11 small/odd vs reference: max=%d mean=%.4f\n", difference.maximum, difference.mean);
  EXPECT_LE(difference.maximum, compositor::kBackdropReferenceTolerance);
}

TEST(D3D11BackdropBlur, RefusesWithoutCapacityOrAWrongFormatInsteadOfFaulting) {
  const int width = 320, height = 180;
  GpuTarget gpu(width, height, backdrop_fixture::checkerBgra(width, height));
  D3D11BackdropBlur pass;
  std::string error;
  ASSERT_TRUE(pass.initialize(gpu.device.get(), error)) << error;
  corevideo::graphics::GraphicsBackdropSet set;
  set.width = width; set.height = height;
  corevideo::graphics::GraphicsBackdropPlate plate;
  plate.box = {20, 20, 200, 100}; plate.clip = {0, 0, double(width), double(height)}; plate.blur = 16;
  compositor::BackdropDraw draw;
  ASSERT_TRUE(compositor::resolveBackdropDraw(plate, set, {}, {0, 0, 1, 1}, {0, 0, 1, 1}, 1.f, width, height, draw));
  EXPECT_FALSE(pass.apply(gpu.context.get(), gpu.texture.get(), gpu.view.get(), width, height, draw));  // not reserved
  ASSERT_TRUE(pass.reserve(gpu.device.get(), 160, 90));
  EXPECT_FALSE(pass.apply(gpu.context.get(), gpu.texture.get(), gpu.view.get(), width, height, draw));  // too small
  ASSERT_TRUE(pass.reserve(gpu.device.get(), width, height));
  EXPECT_EQ(width, pass.capacityWidth());
  EXPECT_TRUE(pass.apply(gpu.context.get(), gpu.texture.get(), gpu.view.get(), width, height, draw));
}

// Frame-budget evidence (spec 6.1, owner ruling 2026-11-27). GPU time of the
// whole pass for the outputs-gate workload's two pack graphics at 1080p,
// measured with D3D11 timestamp queries around the pass only (the region
// copies, both blurs and both composites). Reported, not asserted: CI GPUs
// differ, and a time limit in a unit test would be a flaky gate. The numbers
// go in the PR and docs/reference/backdrop-blur.md with the hardware named.
void benchPlates(const char* label, const std::vector<compositor::BackdropDraw>& draws) {
  const int width = 1920, height = 1080;
  GpuTarget gpu(width, height, backdrop_fixture::checkerBgra(width, height));
  D3D11BackdropBlur pass;
  std::string error;
  ASSERT_TRUE(pass.initialize(gpu.device.get(), error)) << error;
  ASSERT_TRUE(pass.reserve(gpu.device.get(), width, height));
  D3D11_QUERY_DESC disjointDesc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, stampDesc{D3D11_QUERY_TIMESTAMP, 0};
  ComPtrLite<ID3D11Query> disjoint, begin, end;
  ASSERT_TRUE(SUCCEEDED(gpu.device->CreateQuery(&disjointDesc, disjoint.put())));
  ASSERT_TRUE(SUCCEEDED(gpu.device->CreateQuery(&stampDesc, begin.put())));
  ASSERT_TRUE(SUCCEEDED(gpu.device->CreateQuery(&stampDesc, end.put())));
  std::vector<double> gpuUs, cpuUs;
  const int warmup = 30, frames = 600;
  for (int i = 0; i < warmup + frames; ++i) {
    gpu.context->Begin(disjoint.get());
    gpu.context->End(begin.get());
    const auto cpuStart = std::chrono::steady_clock::now();
    for (const auto& draw : draws)
      ASSERT_TRUE(pass.apply(gpu.context.get(), gpu.texture.get(), gpu.view.get(), width, height, draw));
    const auto cpuEnd = std::chrono::steady_clock::now();
    gpu.context->End(end.get());
    gpu.context->End(disjoint.get());
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT timing{};
    while (gpu.context->GetData(disjoint.get(), &timing, sizeof(timing), 0) == S_FALSE) std::this_thread::yield();
    UINT64 t0 = 0, t1 = 0;
    while (gpu.context->GetData(begin.get(), &t0, sizeof(t0), 0) == S_FALSE) std::this_thread::yield();
    while (gpu.context->GetData(end.get(), &t1, sizeof(t1), 0) == S_FALSE) std::this_thread::yield();
    if (i < warmup || timing.Disjoint || timing.Frequency == 0) continue;
    gpuUs.push_back(double(t1 - t0) * 1e6 / double(timing.Frequency));
    cpuUs.push_back(std::chrono::duration<double, std::micro>(cpuEnd - cpuStart).count());
  }
  ASSERT_GT(gpuUs.size(), size_t(frames / 2));
  const auto stats = [](std::vector<double> values) {
    std::sort(values.begin(), values.end());
    double sum = 0;
    for (double v : values) sum += v;
    struct { double mean, p50, p99, max; } s{sum / values.size(), values[values.size() / 2],
                                              values[std::min(values.size() - 1, values.size() * 99 / 100)], values.back()};
    return s;
  };
  const auto g = stats(gpuUs), c = stats(cpuUs);
  ComPtrLite<IDXGIDevice> dxgi;
  ComPtrLite<IDXGIAdapter> adapter;
  DXGI_ADAPTER_DESC adapterDesc{};
  if (SUCCEEDED(gpu.device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(dxgi.put()))) &&
      SUCCEEDED(dxgi->GetAdapter(adapter.put())))
    adapter->GetDesc(&adapterDesc);
  std::printf("[backdrop-bench] %s adapter=%ls frames=%zu plates=%zu\n", label, adapterDesc.Description, gpuUs.size(),
              draws.size());
  std::printf("[backdrop-bench] %s gpu_us mean=%.1f p50=%.1f p99=%.1f max=%.1f (budget 16667 at 60 fps)\n", label, g.mean,
              g.p50, g.p99, g.max);
  std::printf("[backdrop-bench] %s cpu_encode_us mean=%.1f p50=%.1f p99=%.1f max=%.1f\n", label, c.mean, c.p50, c.p99,
              c.max);
}

// The outputs-gate workload's two pack graphics: 1060x168 at blur 32 (1/4
// resolution) and 640x288 at the schema maximum blur 64 (1/8 resolution).
TEST(D3D11BackdropBlurBench, TwoGlassPlatesAt1080p) {
  const auto draws = backdrop_fixture::twoPlates1080p();
  ASSERT_EQ(2u, draws.size());
  benchPlates("two-plates", draws);
}

// The bound the schema allows: four full-frame plates at blur 16, the blur at
// which the pass runs at 1/2 resolution with the full 25-tap kernel (the most
// work per output pixel any plate can ask for).
TEST(D3D11BackdropBlurBench, WorstCaseFourFullFramePlatesAt1080p) {
  corevideo::graphics::GraphicsBackdropSet set;
  set.width = 1920; set.height = 1080;
  corevideo::graphics::GraphicsBackdropPlate plate;
  plate.box = {0, 0, 1920, 1080}; plate.clip = plate.box; plate.blur = 16; plate.tint = 0x20000000u;
  std::vector<compositor::BackdropDraw> draws;
  for (size_t i = 0; i < corevideo::graphics::kGraphicsBackdropMaxPlates; ++i) {
    compositor::BackdropDraw draw;
    ASSERT_TRUE(compositor::resolveBackdropDraw(plate, set, {}, {0, 0, 1, 1}, {0, 0, 1, 1}, 1.f, 1920, 1080, draw));
    ASSERT_EQ(2, draw.kernel.downsample);
    ASSERT_EQ(12, draw.kernel.radius);
    draws.push_back(draw);
  }
  benchPlates("worst-case-4x-fullframe", draws);
}

// End to end through the real compositor: a glass template over a hard
// checker blurs the picture inside the plate; the same template without the
// backdrop leaves it sharp. Proves the plan -> raster -> compositor wiring.
TEST(D3D11BackdropBlur, CompositorBlursProgramUnderAGlassPlate) {
  std::string error;
  const auto glass = corevideo::graphics::GraphicsTemplate::load(backdrop_fixture::glassTemplate(true), error);
  ASSERT_TRUE(glass) << error;
  const auto plain = corevideo::graphics::GraphicsTemplate::load(backdrop_fixture::glassTemplate(false), error);
  ASSERT_TRUE(plain) << error;
  const int width = 1280, height = 720;
  const std::vector<corevideo::modules::VideoFrame> frames{backdrop_fixture::checkerFrame(width, height)};
  const auto renderReady = [&](const corevideo::modules::CompositorRenderPlan& plan) {
    auto compositor = corevideo::modules::createD3D11Compositor();
    if (!compositor) throw std::runtime_error("no compositor");
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
      auto frame = compositor->render(plan, frames);
      if (std::find(frame.warnings.begin(), frame.warnings.end(), "graphics-preparing") == frame.warnings.end()) {
        frame = compositor->render(plan, frames);
        return frame;
      }
      if (std::chrono::steady_clock::now() >= limit) throw std::runtime_error("graphics preparation timed out");
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  };
  const auto blurred = renderReady(backdrop_fixture::glassPlan(width, height, glass));
  const auto sharp = renderReady(backdrop_fixture::glassPlan(width, height, plain));
  EXPECT_TRUE(std::find(blurred.warnings.begin(), blurred.warnings.end(), "graphics-backdrop-unavailable") ==
              blurred.warnings.end());
  // Inside the plate (0.25..0.75 of the frame), well clear of its edge.
  const auto inside = [](const auto& frame) { return backdrop_fixture::edgeEnergy(frame.preview, .32f, .32f, .36f, .36f); };
  const auto outside = [](const auto& frame) { return backdrop_fixture::edgeEnergy(frame.preview, .0f, .0f, .2f, .2f); };
  std::printf("[backdrop-e2e] d3d11 inside sharp=%llu glass=%llu; outside sharp=%llu glass=%llu\n",
              (unsigned long long)inside(sharp), (unsigned long long)inside(blurred),
              (unsigned long long)outside(sharp), (unsigned long long)outside(blurred));
  EXPECT_GT(inside(sharp), 0u);
  EXPECT_LT(inside(blurred) * 4, inside(sharp));
  EXPECT_EQ(outside(sharp), outside(blurred));
}

}  // namespace

#endif
