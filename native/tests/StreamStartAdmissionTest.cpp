#include "modules/StreamStartAdmission.h"

#include <gtest/gtest.h>

using corevideo::modules::admitStreamStart;
using corevideo::modules::StreamStartAdmissionInputs;

namespace {
StreamStartAdmissionInputs healthy(const char* codec) {
  StreamStartAdmissionInputs in;
  in.requestedCodec = codec;
  return in;
}
}  // namespace

TEST(StreamStartAdmission, EveryShippedCodecIsAdmittedWhenEverythingAligns) {
  for (const char* codec : {"h264", "h265", "av1"}) {
    const auto a = admitStreamStart(healthy(codec));
    EXPECT_FALSE(a.refused) << codec;
    EXPECT_TRUE(a.resultCode.empty()) << codec;
    EXPECT_TRUE(a.message.empty()) << codec;
  }
}

TEST(StreamStartAdmission, ACompatibilityRefusalWinsAndNamesEnhancedRtmp) {
  auto in = healthy("h265");
  in.compatibilityRefused = true;
  in.compatibilityReason = "enhanced-rtmp-required";
  in.codecHasHardwareEncoder = false;  // would also refuse; compatibility is checked first
  const auto a = admitStreamStart(in);
  EXPECT_TRUE(a.refused);
  EXPECT_EQ(a.resultCode, "enhanced-rtmp-required");
  EXPECT_NE(a.message.find("Enhanced RTMP"), std::string::npos);
  EXPECT_NE(a.message.find("H.265"), std::string::npos);
}

TEST(StreamStartAdmission, NoHardwareEncoderForTheCodecRefusesAndNamesTheCodec) {
  auto in = healthy("av1");
  in.codecHasHardwareEncoder = false;
  in.gpuPathChosen = false;
  in.gpuPathReason = "no-hardware-encoder";
  const auto a = admitStreamStart(in);
  EXPECT_TRUE(a.refused);
  EXPECT_EQ(a.resultCode, "no-hardware-encoder");
  EXPECT_NE(a.message.find("AV1"), std::string::npos);
  EXPECT_NE(a.message.find("hardware encoder"), std::string::npos);
}

// The raw path cannot hold 1080p60 today (spec: fallback throughput is
// sub-project 2), so HEVC/AV1 are GPU-direct or nothing — for ANY fallback reason.
TEST(StreamStartAdmission, HevcOrAv1OnTheCpuFallbackIsRefusedForAnyReason) {
  for (const char* reason : {"forced-off-by-env", "no-encoder-texture", "platform-unsupported",
                             "no-free-encoder-session"}) {
    auto in = healthy("h265");
    in.gpuPathChosen = false;
    in.gpuPathReason = reason;
    const auto a = admitStreamStart(in);
    EXPECT_TRUE(a.refused) << reason;
    EXPECT_EQ(a.resultCode, "no-hardware-encoder") << reason;
    EXPECT_NE(a.message.find(reason), std::string::npos) << reason;  // the path reason is quoted
  }
}

// H.264 keeps every path it has today: a machine on the raw fallback for a
// platform/env/texture reason still streams H.264 exactly as before.
TEST(StreamStartAdmission, H264OnTheCpuFallbackIsStillAdmitted) {
  for (const char* reason : {"forced-off-by-env", "no-encoder-texture", "platform-unsupported",
                             "no-free-encoder-session", "no-hardware-encoder"}) {
    auto in = healthy("h264");
    in.gpuPathChosen = false;
    in.gpuPathReason = reason;
    const auto a = admitStreamStart(in);
    EXPECT_FALSE(a.refused) << reason;
  }
}

TEST(StreamStartAdmission, AGpuEncoderStartFailureRefusesForEveryCodecAndQuotesTheDetail) {
  for (const char* codec : {"h264", "h265", "av1"}) {
    auto in = healthy(codec);
    in.gpuEncoderStartFailed = true;
    in.gpuEncoderFailureDetail = "set-output-type hr=0xC00D36B4";
    const auto a = admitStreamStart(in);
    EXPECT_TRUE(a.refused) << codec;
    EXPECT_EQ(a.resultCode, "gpu-encoder-start-failed") << codec;
    EXPECT_NE(a.message.find("0xC00D36B4"), std::string::npos) << codec;
  }
}
