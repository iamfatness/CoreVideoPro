#include "i420-range-expand.h"
#include "engine-ipc.h"
#include <gtest/gtest.h>
#include <array>
#include <cmath>
#include <vector>

TEST(I420RangeExpand, EndpointsNeutralAndAllCodeValuesMatchStudioSwingContract) {
  EXPECT_EQ(i420_expand_luma_sample(16), 0);
  EXPECT_EQ(i420_expand_luma_sample(235), 255);
  EXPECT_EQ(i420_expand_chroma_sample(16), 0);
  EXPECT_EQ(i420_expand_chroma_sample(240), 255);
  EXPECT_EQ(i420_expand_chroma_sample(128), 128);
  EXPECT_EQ(i420_expand_luma_sample(12), 0);
  EXPECT_EQ(i420_expand_luma_sample(240), 255);
  for (int i = 0; i < 256; ++i) {
    const auto y = static_cast<uint8_t>(16 + (i * 219 + 127) / 255);
    const auto c = static_cast<uint8_t>(16 + (i * 224 + 127) / 255);
    EXPECT_LE(std::abs(static_cast<int>(i420_expand_luma_sample(y)) - i), 1);
    EXPECT_LE(std::abs(static_cast<int>(i420_expand_chroma_sample(c)) - i), 1);
    if (i != 0) {
      EXPECT_GE(i420_expand_luma_sample(i), i420_expand_luma_sample(i - 1));
      EXPECT_GE(i420_expand_chroma_sample(i), i420_expand_chroma_sample(i - 1));
    }
  }
}

TEST(I420RangeExpand, AlternatingSdkRangeFlagPreservesImageAndBorrowsFullRangePlanes) {
  I420RangeNormalizer normalizer;
  std::array<uint8_t, 256> fullY{}, limitedY{};
  std::array<uint8_t, 64> fullU{}, fullV{}, limitedU{}, limitedV{};
  for (int i = 0; i < 256; ++i) {
    fullY[i] = static_cast<uint8_t>(i);
    limitedY[i] = static_cast<uint8_t>(16 + (i * 219 + 127) / 255);
  }
  for (int i = 0; i < 64; ++i) {
    fullU[i] = static_cast<uint8_t>(i * 4);
    fullV[i] = static_cast<uint8_t>(255 - i * 4);
    limitedU[i] = static_cast<uint8_t>(16 + (fullU[i] * 224 + 127) / 255);
    limitedV[i] = static_cast<uint8_t>(16 + (fullV[i] * 224 + 127) / 255);
  }
  const uint8_t* retainedScratch = nullptr;
  for (int frame = 0; frame < 30; ++frame) {
    const bool limited = frame % 3 == 1;
    const auto pixels = normalizer.normalize(limited ? limitedY.data() : fullY.data(),
        limited ? limitedU.data() : fullU.data(), limited ? limitedV.data() : fullV.data(), fullY.size(), limited, kMaxVideoShmYLen);
    if (!limited) {
      EXPECT_EQ(pixels.y, fullY.data());
      EXPECT_EQ(pixels.u, fullU.data());
      EXPECT_EQ(pixels.v, fullV.data());
    } else {
      if (retainedScratch) EXPECT_EQ(pixels.y, retainedScratch);
      retainedScratch = pixels.y;
    }
    for (int i = 0; i < 256; ++i) EXPECT_LE(std::abs(int(pixels.y[i]) - fullY[i]), limited ? 1 : 0);
    for (int i = 0; i < 64; ++i) {
      EXPECT_LE(std::abs(int(pixels.u[i]) - fullU[i]), limited ? 1 : 0);
      EXPECT_LE(std::abs(int(pixels.v[i]) - fullV[i]), limited ? 1 : 0);
    }
  }
  // The SDK-owned buffers must never be modified by the correction.
  EXPECT_EQ(limitedY.front(), 16);
  EXPECT_EQ(limitedY.back(), 235);
}

TEST(I420RangeExpand, InPlaceConversionAndInvalidInputsNeverPartiallyConvert) {
  std::array<uint8_t, 8> y{235, 16, 16, 16, 16, 16, 16, 16};
  std::array<uint8_t, 2> u{128, 128}, v{240, 240};
  i420_expand_limited_to_full(y.data(), u.data(), v.data(), y.data(), u.data(), v.data(), 6);
  EXPECT_EQ(y[0], 235);
  i420_expand_limited_to_full(nullptr, u.data(), v.data(), y.data(), u.data(), v.data(), 8);
  EXPECT_EQ(y[0], 235);
  i420_expand_limited_to_full(y.data(), u.data(), v.data(), y.data(), u.data(), v.data(), 8);
  EXPECT_EQ(y[0], 255);
  EXPECT_EQ(y[1], 0);
  EXPECT_EQ(u[0], 128);
  EXPECT_EQ(v[0], 255);
}

TEST(I420RangeExpand, CameraAndShareCapsRejectBeforePlaneReadAndScratchGrowsSafely) {
  I420RangeNormalizer normalizer;
  // Deliberately tiny input: an oversize request must reject before reading it.
  uint8_t sample = 16;
  for (const auto cap : {kMaxVideoShmYLen, kMaxShareShmYLen}) {
    for (const bool limited : {false, true}) {
      const auto rejected = normalizer.normalize(&sample, &sample, &sample, cap + 4, limited, cap);
      EXPECT_TRUE(rejected.y == nullptr);
    }
  }
  for (const size_t yLength : {size_t{16}, size_t{256}, size_t{16}}) {
    std::vector<uint8_t> y(yLength, 235), u(yLength / 4, 128), v(yLength / 4, 16);
    const auto expanded = normalizer.normalize(y.data(), u.data(), v.data(), yLength, true, kMaxVideoShmYLen);
    EXPECT_EQ(expanded.y[yLength - 1], 255);
    EXPECT_EQ(expanded.u[yLength / 4 - 1], 128);
    EXPECT_EQ(expanded.v[yLength / 4 - 1], 0);
  }
}
