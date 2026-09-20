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

// 2026-09-20: AV1 binds the NVIDIA AV1 MFT and runs at the correct cadence, but
// emits near-empty access units (~54 bytes vs H.264's ~12,483 at 1080p60), so
// the muxed stream is ~18 kbit/s against a configured 6 Mbps. A codec that
// streams at 0.3% of its bitrate ships REFUSED, never silently broken.
TEST(StreamStartAdmission, ANotDeliverableCodecRefusesAndNamesTheCodecAndTheDetail) {
  auto in = healthy("av1");
  in.codecKnownNotDeliverable = true;
  in.notDeliverableDetail = "near-empty access units, ~18 kbit/s against the configured bitrate";
  const auto a = admitStreamStart(in);
  EXPECT_TRUE(a.refused);
  EXPECT_EQ(a.resultCode, "codec-not-deliverable");
  EXPECT_NE(a.message.find("AV1"), std::string::npos);
  EXPECT_NE(a.message.find("near-empty access units"), std::string::npos);
  EXPECT_NE(a.message.find("H.264 or H.265"), std::string::npos);
}

// A destination that cannot carry the codec at all is the more actionable
// message, so the compatibility refusal still wins.
TEST(StreamStartAdmission, ACompatibilityRefusalStillWinsOverNotDeliverable) {
  auto in = healthy("av1");
  in.compatibilityRefused = true;
  in.compatibilityReason = "enhanced-rtmp-required";
  in.codecKnownNotDeliverable = true;
  in.notDeliverableDetail = "near-empty access units";
  const auto a = admitStreamStart(in);
  EXPECT_TRUE(a.refused);
  EXPECT_EQ(a.resultCode, "enhanced-rtmp-required");
}

// The flag is per-codec policy, not a global kill switch: H.264 is untouched.
TEST(StreamStartAdmission, H264WithTheNotDeliverableFlagClearIsStillAdmitted) {
  auto in = healthy("h264");
  in.codecKnownNotDeliverable = false;
  const auto a = admitStreamStart(in);
  EXPECT_FALSE(a.refused);
  EXPECT_TRUE(a.resultCode.empty());
}

// An empty detail must never render empty parentheses at the operator.
TEST(StreamStartAdmission, ANotDeliverableRefusalWithNoDetailStillReadsAsASentence) {
  auto in = healthy("av1");
  in.codecKnownNotDeliverable = true;
  const auto a = admitStreamStart(in);
  EXPECT_TRUE(a.refused);
  EXPECT_EQ(a.resultCode, "codec-not-deliverable");
  EXPECT_NE(a.message.find("known defect"), std::string::npos);
  EXPECT_EQ(a.message.find("()"), std::string::npos);
}
