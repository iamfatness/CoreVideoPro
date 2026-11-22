// Shared helpers for the grading Phase 2 Metal tests (MetalGradeLutPixelTest.cpp,
// MetalGradePreviewTest.cpp). Kept out of MetalCompositorTest.cpp, which another workstream
// (Metal graphics G2) is editing in parallel.
#pragma once
#if COREVIDEO_WITH_METAL
#include <IOSurface/IOSurface.h>
#include <gtest/gtest.h>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include "modules/Interfaces.h"
#include "modules/Sha256.h"

namespace metal_grade_test {
using namespace corevideo::modules;

inline std::unique_ptr<ICompositor> metalOrNull(std::string& why) {
  auto metal = createMetalCompositor();
  if (!metal) { why = "createMetalCompositor returned null under COREVIDEO_WITH_METAL"; return nullptr; }
  CompositorRenderPlan probe; probe.renderPlanId = "metal-probe";
  for (const auto& w : metal->render(probe, {}).warnings)
    if (w.rfind("Metal compositor unavailable", 0) == 0) { why = w; return nullptr; }
  return metal;
}

// The vendored gtest has no GTEST_SKIP: a machine with no Metal device logs and returns green;
// any other failure to build the compositor fails (the MetalCompositorTest rule).
#define METAL_GRADE_OR_SKIP(var)                                                       \
  std::string skip_;                                                                   \
  auto var = ::metal_grade_test::metalOrNull(skip_);                                   \
  if (!var) {                                                                          \
    std::fprintf(stderr, "[metal-grade-test] unavailable: %s\n", skip_.c_str());       \
    ASSERT_TRUE(skip_.find("no Metal device") != std::string::npos) << skip_;          \
    return;                                                                            \
  }

inline VideoFrame bgraFrame(const std::string& id, int w, int h, uint8_t b, uint8_t g, uint8_t r, int64_t frameId = 1) {
  VideoFrame frame;
  frame.participantId = id; frame.frameId = frameId;
  frame.width = frame.pixelWidth = frame.naturalWidth = w;
  frame.height = frame.pixelHeight = frame.naturalHeight = h;
  frame.pixelStride = w * 4;
  auto pixels = std::make_shared<std::vector<uint8_t>>(size_t(w) * h * 4);
  for (size_t i = 0; i < pixels->size(); i += 4) { (*pixels)[i] = b; (*pixels)[i + 1] = g; (*pixels)[i + 2] = r; (*pixels)[i + 3] = 255; }
  frame.pixels = std::move(pixels);
  return frame;
}

inline VideoFrame i420Frame(const std::string& id, int w, int h, uint8_t y, uint8_t u, uint8_t v,
                            bool fullRange, bool bt601, int64_t frameId = 1) {
  VideoFrame frame;
  frame.participantId = id; frame.frameId = frameId;
  frame.width = frame.naturalWidth = w; frame.height = frame.naturalHeight = h;
  const size_t yLen = size_t(w) * h;
  auto planes = std::make_shared<std::vector<uint8_t>>(yLen + yLen / 2);
  std::fill(planes->begin(), planes->begin() + yLen, y);
  std::fill(planes->begin() + yLen, planes->begin() + yLen + yLen / 4, u);
  std::fill(planes->begin() + yLen + yLen / 4, planes->end(), v);
  frame.i420 = std::move(planes); frame.i420Width = w; frame.i420Height = h;
  frame.i420FullRange = fullRange; frame.i420Bt601 = bt601;
  return frame;
}

inline CompositorRenderPlanLayer fullLayer(const std::string& id) {
  CompositorRenderPlanLayer layer;
  layer.layerId = "layer:" + id; layer.kind = "participant-video";
  layer.participantId = id; layer.sourceId = id; layer.rect = {0, 0, 1, 1};
  return layer;
}

// B, G, R of the Program preview at the centre (the CPU readback the compositor already does).
inline std::array<int, 3> programCentre(const ProgramFrame& frame) {
  const auto& p = frame.preview;
  if (p.width <= 0 || p.height <= 0) return {-1, -1, -1};
  const size_t at = (size_t(p.height / 2) * p.width + p.width / 2) * 4;
  if (at + 3 >= p.bgra.size()) return {-1, -1, -1};
  return {p.bgra[at], p.bgra[at + 1], p.bgra[at + 2]};
}

// B, G, R, A read back from a published IOSurface at normalized (fx, fy): the bytes the shell
// will show. Looked up by the global id, exactly as the shell does (IOSurfaceLookup).
inline std::array<int, 4> surfacePixel(uint32_t iosurfaceId, float fx, float fy) {
  IOSurfaceRef surface = iosurfaceId ? IOSurfaceLookup(iosurfaceId) : nullptr;
  if (!surface) return {-1, -1, -1, -1};
  IOSurfaceLock(surface, kIOSurfaceLockReadOnly, nullptr);
  const int w = int(IOSurfaceGetWidth(surface)), h = int(IOSurfaceGetHeight(surface));
  const size_t stride = IOSurfaceGetBytesPerRow(surface);
  const auto* base = static_cast<const uint8_t*>(IOSurfaceGetBaseAddress(surface));
  const int x = std::min(w - 1, std::max(0, int(fx * w))), y = std::min(h - 1, std::max(0, int(fy * h)));
  const auto* px = base + size_t(y) * stride + size_t(x) * 4;
  std::array<int, 4> out{px[0], px[1], px[2], px[3]};
  IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, nullptr);
  CFRelease(surface);
  return out;
}

inline std::string readRepoFile(const std::string& relative) {
  std::ifstream input(std::filesystem::path(COREVIDEO_REPO_ROOT) / relative);
  if (!input) return {};
  std::ostringstream buffer; buffer << input.rdbuf(); return buffer.str();
}

inline std::string sha256Hex(const std::string& text) {
  const auto digest = hashing::sha256(reinterpret_cast<const uint8_t*>(text.data()), text.size());
  std::ostringstream hex; hex << std::hex << std::setfill('0');
  for (auto b : digest) hex << std::setw(2) << int(b);
  return hex.str();
}
}  // namespace metal_grade_test
#endif
