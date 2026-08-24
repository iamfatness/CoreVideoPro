#include "i420-range-expand.h"

#include <gtest/gtest.h>

#include <vector>

// The "gamma flash": the Zoom SDK sometimes delivers a limited-range frame even
// though the engine requested BT709_F and everything downstream declares full
// range. These tests pin the expansion that makes the declaration true.

using corevideo::i420_expand_chroma;
using corevideo::i420_expand_limited_to_full;
using corevideo::i420_expand_luma;

TEST(I420RangeExpand, StudioLumaEndpointsOpenToFullRange) {
  EXPECT_EQ(i420_expand_luma(16), 0);
  EXPECT_EQ(i420_expand_luma(235), 255);
}

TEST(I420RangeExpand, SuperblackAndSuperwhiteClampRatherThanWrap) {
  // Studio swing leaves headroom/footroom. Those codes must saturate, never
  // wrap around into the opposite extreme.
  EXPECT_EQ(i420_expand_luma(0), 0);
  EXPECT_EQ(i420_expand_luma(15), 0);
  EXPECT_EQ(i420_expand_luma(236), 255);
  EXPECT_EQ(i420_expand_luma(255), 255);
}

TEST(I420RangeExpand, LumaIsMonotonicAcrossEveryCode) {
  for (int i = 1; i < 256; ++i) {
    EXPECT_LE(i420_expand_luma(static_cast<uint8_t>(i - 1)),
              i420_expand_luma(static_cast<uint8_t>(i)))
        << "luma expansion must never go backwards at code " << i;
  }
}

TEST(I420RangeExpand, NeutralChromaSurvivesExactlySoTheFixCannotTint) {
  // 128 is the neutral point. If it moves at all, every corrected frame picks
  // up a colour cast -- which would be a worse defect than the flash.
  EXPECT_EQ(i420_expand_chroma(128), 128);
}

TEST(I420RangeExpand, StudioChromaEndpointsOpenToFullRange) {
  EXPECT_EQ(i420_expand_chroma(16), 0);
  EXPECT_EQ(i420_expand_chroma(240), 255);
}

TEST(I420RangeExpand, ChromaClampsOutsideStudioSwing) {
  EXPECT_EQ(i420_expand_chroma(0), 0);
  EXPECT_EQ(i420_expand_chroma(255), 255);
}

TEST(I420RangeExpand, ExpandingAFrameLiftsTheShadowsBackOut) {
  // Reproduces the measured live signature: a limited frame reads min=12
  // max=234 where its full-range neighbours read min=0 max=255.
  constexpr size_t kYLen = 16;  // 4x4
  std::vector<uint8_t> src_y(kYLen, 16);
  std::vector<uint8_t> src_u(kYLen / 4, 128);
  std::vector<uint8_t> src_v(kYLen / 4, 128);
  src_y[0] = 16;    // studio black
  src_y[1] = 235;   // studio white
  src_y[2] = 126;   // mid grey

  std::vector<uint8_t> dst_y(kYLen, 0xAA);
  std::vector<uint8_t> dst_u(kYLen / 4, 0xAA);
  std::vector<uint8_t> dst_v(kYLen / 4, 0xAA);

  i420_expand_limited_to_full(src_y.data(), src_u.data(), src_v.data(),
                              dst_y.data(), dst_u.data(), dst_v.data(), kYLen);

  EXPECT_EQ(dst_y[0], 0);
  EXPECT_EQ(dst_y[1], 255);
  EXPECT_NEAR(dst_y[2], 128, 1);
  // Neutral chroma in, neutral chroma out, across the whole plane.
  for (size_t i = 0; i < kYLen / 4; ++i) {
    EXPECT_EQ(dst_u[i], 128) << "u plane tinted at " << i;
    EXPECT_EQ(dst_v[i], 128) << "v plane tinted at " << i;
  }
}

TEST(I420RangeExpand, ExpandingWritesEveryLumaSampleNotJustTheFirst) {
  constexpr size_t kYLen = 64;
  std::vector<uint8_t> src_y(kYLen, 235);
  std::vector<uint8_t> src_u(kYLen / 4, 128);
  std::vector<uint8_t> src_v(kYLen / 4, 128);
  std::vector<uint8_t> dst_y(kYLen, 0);
  std::vector<uint8_t> dst_u(kYLen / 4, 0);
  std::vector<uint8_t> dst_v(kYLen / 4, 0);

  i420_expand_limited_to_full(src_y.data(), src_u.data(), src_v.data(),
                              dst_y.data(), dst_u.data(), dst_v.data(), kYLen);

  for (size_t i = 0; i < kYLen; ++i) {
    EXPECT_EQ(dst_y[i], 255) << "luma sample " << i << " was not expanded";
  }
}

TEST(I420RangeExpand, AFullRangeFrameRoundTripsThroughLimitedWithinOneCode) {
  // Sanity on the inverse relationship: compressing to studio swing and
  // expanding back must land within a code value, so repeated correction of an
  // already-correct picture cannot drift.
  for (int full = 0; full < 256; ++full) {
    const int limited = 16 + (full * 219 + 127) / 255;
    const int back = i420_expand_luma(static_cast<uint8_t>(limited));
    EXPECT_LE(std::abs(back - full), 1)
        << "round trip drifted at full-range code " << full;
  }
}
