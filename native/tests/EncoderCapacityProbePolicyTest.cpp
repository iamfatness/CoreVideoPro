#include "modules/EncoderCapacityProbe.h"

#include <gtest/gtest.h>

// The probe cache is keyed by codec. One canonical spelling per codec, or the
// same workload is probed twice under two names and the sender's "no free
// session" answer depends on which spelling asked.
TEST(EncoderCapacityProbePolicy, CanonicalProbeCodecCollapsesSpellings) {
  using corevideo::modules::canonicalProbeCodec;
  EXPECT_EQ(canonicalProbeCodec("h264"), "h264");
  EXPECT_EQ(canonicalProbeCodec("H264"), "h264");
  EXPECT_EQ(canonicalProbeCodec("h265"), "hevc");
  EXPECT_EQ(canonicalProbeCodec("hevc"), "hevc");
  EXPECT_EQ(canonicalProbeCodec("hvc1"), "hevc");
  EXPECT_EQ(canonicalProbeCodec("av1"), "av1");
  EXPECT_EQ(canonicalProbeCodec("av01"), "av1");
  EXPECT_EQ(canonicalProbeCodec(""), "h264");
  EXPECT_EQ(canonicalProbeCodec("bogus"), "h264");
}

TEST(EncoderCapacityProbePolicy, ProbeKeyOrdersByCodecFirst) {
  corevideo::modules::EncoderProbeKey a{"av1", 1920, 1080, 60};
  corevideo::modules::EncoderProbeKey b{"h264", 1920, 1080, 60};
  EXPECT_TRUE(a < b);
  EXPECT_FALSE(b < a);
}
