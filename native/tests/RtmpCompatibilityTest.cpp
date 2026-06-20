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

TEST(RtmpCompatibility, H265FallsBackToH264ByDefault) {
  const auto resolved = resolveRtmpCompatibility("h265", false);
  EXPECT_EQ(resolved.requestedVideoCodec, "h265");
  EXPECT_EQ(resolved.videoCodec, "h264");  // downgraded for FLV compatibility
  EXPECT_TRUE(resolved.fallbackApplied);
  EXPECT_FALSE(resolved.enhancedRtmp);
  EXPECT_FALSE(resolved.warning.empty());
}

TEST(RtmpCompatibility, Av1FallsBackToH264ByDefault) {
  const auto resolved = resolveRtmpCompatibility("av1", false);
  EXPECT_EQ(resolved.videoCodec, "h264");
  EXPECT_TRUE(resolved.fallbackApplied);
  EXPECT_FALSE(resolved.warning.empty());
}

TEST(RtmpCompatibility, HevcAliasNormalizesToH265ThenFallsBack) {
  const auto resolved = resolveRtmpCompatibility("HEVC", false);
  EXPECT_EQ(resolved.requestedVideoCodec, "h265");
  EXPECT_EQ(resolved.videoCodec, "h264");
  EXPECT_TRUE(resolved.fallbackApplied);
}

TEST(RtmpCompatibility, H265OverEnhancedRtmpKeepsCodecWithAdvisory) {
  const auto resolved = resolveRtmpCompatibility("h265", true);
  EXPECT_EQ(resolved.videoCodec, "h265");
  EXPECT_TRUE(resolved.enhancedRtmp);
  EXPECT_FALSE(resolved.fallbackApplied);
  EXPECT_FALSE(resolved.warning.empty());  // advisory: ingest must support E-RTMP
}

TEST(RtmpCompatibility, Av1OverEnhancedRtmpKeepsCodec) {
  const auto resolved = resolveRtmpCompatibility("av1", true);
  EXPECT_EQ(resolved.videoCodec, "av1");
  EXPECT_TRUE(resolved.enhancedRtmp);
  EXPECT_FALSE(resolved.fallbackApplied);
}

TEST(RtmpCompatibility, UnknownCodecDefaultsToH264Baseline) {
  const auto resolved = resolveRtmpCompatibility("vp9", false);
  EXPECT_EQ(resolved.requestedVideoCodec, "h264");
  EXPECT_EQ(resolved.videoCodec, "h264");
  EXPECT_FALSE(resolved.fallbackApplied);
  EXPECT_TRUE(resolved.warning.empty());
}
