#include "modules/NdiPixelConvert.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

using corevideo::modules::nv12ToUyvy;

namespace {

// Build an NV12 frame whose luma encodes (x,y) and whose chroma encodes the
// chroma-block index, so the packing can be checked exactly rather than by eye.
std::vector<uint8_t> makeNv12(int width, int height) {
  const std::size_t luma = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  std::vector<uint8_t> nv12(luma + luma / 2u, 0);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      nv12[static_cast<std::size_t>(y) * width + x] = static_cast<uint8_t>((y * 16 + x) & 0xFF);
    }
  }
  uint8_t* chroma = nv12.data() + luma;
  for (int cy = 0; cy < height / 2; ++cy) {
    for (int cx = 0; cx < width; cx += 2) {
      chroma[static_cast<std::size_t>(cy) * width + cx] = static_cast<uint8_t>(100 + cy);      // U
      chroma[static_cast<std::size_t>(cy) * width + cx + 1] = static_cast<uint8_t>(200 - cy);  // V
    }
  }
  return nv12;
}

}  // namespace

// The NDI sender publishes UYVY. Getting this packing wrong ships a garbled or
// colour-swapped program to every receiver, so pin the exact byte order.
TEST(NdiPixelConvert, PacksUyvyInOrderAndDuplicatesChromaRows) {
  constexpr int kW = 8;
  constexpr int kH = 4;
  const auto nv12 = makeNv12(kW, kH);
  std::vector<uint8_t> uyvy;
  ASSERT_TRUE(nv12ToUyvy(nv12.data(), kW, kH, nv12.size(), uyvy));
  ASSERT_EQ(uyvy.size(), static_cast<std::size_t>(kW) * kH * 2u);

  for (int y = 0; y < kH; ++y) {
    for (int x = 0; x < kW; x += 2) {
      const std::size_t out = (static_cast<std::size_t>(y) * kW + x) * 2u;
      EXPECT_EQ(uyvy[out + 0], static_cast<uint8_t>(100 + y / 2)) << "U at " << x << "," << y;
      EXPECT_EQ(uyvy[out + 1], static_cast<uint8_t>((y * 16 + x) & 0xFF)) << "Y0";
      EXPECT_EQ(uyvy[out + 2], static_cast<uint8_t>(200 - y / 2)) << "V at " << x << "," << y;
      EXPECT_EQ(uyvy[out + 3], static_cast<uint8_t>((y * 16 + x + 1) & 0xFF)) << "Y1";
    }
  }
}

// 4:2:0 -> 4:2:2 means luma rows 2n and 2n+1 read the SAME NV12 chroma row.
TEST(NdiPixelConvert, AdjacentLumaRowsShareOneChromaRow) {
  constexpr int kW = 4;
  constexpr int kH = 4;
  const auto nv12 = makeNv12(kW, kH);
  std::vector<uint8_t> uyvy;
  ASSERT_TRUE(nv12ToUyvy(nv12.data(), kW, kH, nv12.size(), uyvy));
  const std::size_t row0 = 0;
  const std::size_t row1 = static_cast<std::size_t>(kW) * 2u;
  EXPECT_EQ(uyvy[row0 + 0], uyvy[row1 + 0]) << "rows 0 and 1 must share U";
  EXPECT_EQ(uyvy[row0 + 2], uyvy[row1 + 2]) << "rows 0 and 1 must share V";
  const std::size_t row2 = static_cast<std::size_t>(kW) * 4u;
  EXPECT_NE(uyvy[row0 + 0], uyvy[row2 + 0]) << "row 2 must use the NEXT chroma row";
}

// A short buffer must be refused, not read past. The NV12 tap is asynchronous and
// a partial publish would otherwise walk off the end straight into a receiver.
TEST(NdiPixelConvert, RefusesUndersizedOrDegenerateInput) {
  constexpr int kW = 8;
  constexpr int kH = 4;
  const auto nv12 = makeNv12(kW, kH);
  std::vector<uint8_t> uyvy;
  EXPECT_FALSE(nv12ToUyvy(nv12.data(), kW, kH, nv12.size() - 1, uyvy));
  EXPECT_FALSE(nv12ToUyvy(nullptr, kW, kH, nv12.size(), uyvy));
  EXPECT_FALSE(nv12ToUyvy(nv12.data(), 0, kH, nv12.size(), uyvy));
}

// Is this build instrumented? A sanitizer instruments every memory access, which
// costs roughly an order of magnitude — a wall-clock budget cannot mean anything
// under it. The correctness assertions above still run there; only the timing
// bound is skipped.
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#define COREVIDEO_INSTRUMENTED_BUILD 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer) || \
    __has_feature(memory_sanitizer)
#define COREVIDEO_INSTRUMENTED_BUILD 1
#endif
#endif

// This runs on the audio/output worker, which has a 20ms tick. A per-pixel colour
// conversion here would blow that budget at 1080p; the byte shuffle must not. The
// bound is deliberately loose (CI machines vary) — it exists to catch someone
// reintroducing per-pixel math, not to benchmark the host.
TEST(NdiPixelConvert, FullHdConversionStaysCheapEnoughForTheOutputWorker) {
#ifdef COREVIDEO_INSTRUMENTED_BUILD
  // Announce rather than silently pass — a skipped budget should be visible.
  // (This harness has no GTEST_SKIP; an early return is the whole mechanism.)
  std::printf("[  SKIPPED ] NV12->UYVY budget: meaningless under a sanitizer\n");
  return;
#else
  constexpr int kW = 1920;
  constexpr int kH = 1080;
  const auto nv12 = makeNv12(kW, kH);
  std::vector<uint8_t> uyvy;
  ASSERT_TRUE(nv12ToUyvy(nv12.data(), kW, kH, nv12.size(), uyvy));  // warm the buffer

  const auto start = std::chrono::steady_clock::now();
  constexpr int kIterations = 10;
  for (int i = 0; i < kIterations; ++i) {
    ASSERT_TRUE(nv12ToUyvy(nv12.data(), kW, kH, nv12.size(), uyvy));
  }
  const auto perFrameMs =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() /
      kIterations;
  EXPECT_LT(perFrameMs, 10.0) << "1080p NV12->UYVY took " << perFrameMs
                              << "ms/frame — too slow for the 20ms output tick";
#endif
}
