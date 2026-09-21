#include "modules/RtmpCompatibility.h"

#include <gtest/gtest.h>

namespace {
using namespace corevideo::modules;
}  // namespace

TEST(RtmpCompatibility, H264IsGuaranteedBaseline) {
  const auto resolved = resolveRtmpCompatibility("h264", false);
  EXPECT_EQ(resolved.requestedVideoCodec, "h264");
  EXPECT_EQ(resolved.videoCodec, "h264");
  EXPECT_EQ(resolved.audioCodec, "aac");
  EXPECT_EQ(resolved.container, "flv");
  EXPECT_FALSE(resolved.fallbackApplied);
  EXPECT_FALSE(resolved.enhancedRtmp);
  EXPECT_TRUE(resolved.warning.empty());
}

// 2026-09-20: a codec the destination cannot take is REFUSED, never downgraded.
// The silent H.265 -> H.264 downgrade delivered the wrong codec on the slow raw
// path and read to the operator as "YouTube is not getting enough data".
TEST(RtmpCompatibility, H265WithoutEnhancedRtmpIsRefusedNotDowngraded) {
  const auto resolved = resolveRtmpCompatibility("h265", false);
  EXPECT_EQ(resolved.requestedVideoCodec, "h265");
  EXPECT_EQ(resolved.videoCodec, "h265");  // the codec asked for, not a substitute
  EXPECT_TRUE(resolved.refused);
  EXPECT_EQ(resolved.reason, "enhanced-rtmp-required");
  EXPECT_FALSE(resolved.fallbackApplied);
  EXPECT_FALSE(resolved.enhancedRtmp);
  EXPECT_NE(resolved.warning.find("Enhanced RTMP"), std::string::npos);
  EXPECT_NE(resolved.warning.find("H.265"), std::string::npos);
}

TEST(RtmpCompatibility, Av1WithoutEnhancedRtmpIsRefusedNotDowngraded) {
  const auto resolved = resolveRtmpCompatibility("av1", false);
  EXPECT_EQ(resolved.videoCodec, "av1");
  EXPECT_TRUE(resolved.refused);
  EXPECT_EQ(resolved.reason, "enhanced-rtmp-required");
  EXPECT_FALSE(resolved.fallbackApplied);
  EXPECT_NE(resolved.warning.find("AV1"), std::string::npos);
}

TEST(RtmpCompatibility, HevcAliasNormalizesToH265ThenIsRefusedWithoutEnhancedRtmp) {
  const auto resolved = resolveRtmpCompatibility("HEVC", false);
  EXPECT_EQ(resolved.requestedVideoCodec, "h265");
  EXPECT_EQ(resolved.videoCodec, "h265");
  EXPECT_TRUE(resolved.refused);
}

TEST(RtmpCompatibility, H264IsNeverRefused) {
  const auto resolved = resolveRtmpCompatibility("h264", false);
  EXPECT_FALSE(resolved.refused);
  EXPECT_TRUE(resolved.reason.empty());
}

TEST(RtmpCompatibility, H265OverEnhancedRtmpKeepsCodecWithAdvisory) {
  const auto resolved = resolveRtmpCompatibility("h265", true);
  EXPECT_EQ(resolved.videoCodec, "h265");
  EXPECT_TRUE(resolved.enhancedRtmp);
  EXPECT_FALSE(resolved.fallbackApplied);
  EXPECT_FALSE(resolved.refused);
  EXPECT_FALSE(resolved.warning.empty());  // advisory: ingest must support E-RTMP
}

TEST(RtmpCompatibility, Av1OverEnhancedRtmpKeepsCodec) {
  const auto resolved = resolveRtmpCompatibility("av1", true);
  EXPECT_EQ(resolved.videoCodec, "av1");
  EXPECT_TRUE(resolved.enhancedRtmp);
  EXPECT_FALSE(resolved.fallbackApplied);
  EXPECT_FALSE(resolved.refused);
}

TEST(RtmpCompatibility, UnknownCodecDefaultsToH264Baseline) {
  const auto resolved = resolveRtmpCompatibility("vp9", false);
  EXPECT_EQ(resolved.requestedVideoCodec, "h264");
  EXPECT_EQ(resolved.videoCodec, "h264");
  EXPECT_FALSE(resolved.fallbackApplied);
  EXPECT_TRUE(resolved.warning.empty());
}
