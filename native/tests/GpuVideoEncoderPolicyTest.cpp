#include "modules/GpuVideoEncoder.h"

#include <gtest/gtest.h>

namespace {
using corevideo::modules::GpuEncodePath;
using corevideo::modules::GpuEncodePathInputs;
using corevideo::modules::GpuEncodePathPolicy;

// Inputs order: {hardwareEncoderAvailable, sessionAvailable, forcedOffByEnv, platformSupported}

TEST(GpuEncodePathPolicy, GpuWhenEverythingIsAvailable) {
  EXPECT_EQ(GpuEncodePathPolicy::choose({true, true, false, true}), GpuEncodePath::GpuDirect);
}

TEST(GpuEncodePathPolicy, EnvToggleForcesCpuEvenWhenCapable) {
  EXPECT_EQ(GpuEncodePathPolicy::choose({true, true, /*forcedOffByEnv=*/true, true}),
            GpuEncodePath::CpuFallback);
}

TEST(GpuEncodePathPolicy, NoHardwareEncoderFallsBack) {
  EXPECT_EQ(GpuEncodePathPolicy::choose({false, true, false, true}), GpuEncodePath::CpuFallback);
}

TEST(GpuEncodePathPolicy, NoFreeSessionFallsBack) {
  EXPECT_EQ(GpuEncodePathPolicy::choose({true, false, false, true}), GpuEncodePath::CpuFallback);
}

TEST(GpuEncodePathPolicy, UnsupportedPlatformFallsBack) {
  EXPECT_EQ(GpuEncodePathPolicy::choose({true, true, false, /*platformSupported=*/false}),
            GpuEncodePath::CpuFallback);
}

// reason() names the FIRST blocker, checked platform -> env -> hardware -> session,
// so a support bundle says exactly why a machine is on the CPU fallback.
TEST(GpuEncodePathPolicy, ReasonNamesTheFirstBlocker) {
  EXPECT_EQ(std::string(GpuEncodePathPolicy::reason({true, true, false, false})), "platform-unsupported");
  EXPECT_EQ(std::string(GpuEncodePathPolicy::reason({true, true, true, true})), "forced-off-by-env");
  EXPECT_EQ(std::string(GpuEncodePathPolicy::reason({false, true, false, true})), "no-hardware-encoder");
  EXPECT_EQ(std::string(GpuEncodePathPolicy::reason({true, false, false, true})), "no-free-encoder-session");
  EXPECT_EQ(std::string(GpuEncodePathPolicy::reason({true, true, false, true})), "gpu-direct");
}

// chooseStreamEncodePath: the sender's start-time decision. base = {hardware,
// session, forcedOffByEnv, platformSupported}. The codec is the one ACTUALLY
// SENT (resolved through RtmpCompatibility), and codecHasGpuEncoder is what the
// capacity probe says about THAT codec on this machine.
using corevideo::modules::chooseStreamEncodePath;

TEST(ChooseStreamEncodePath, GpuDirectForEveryShippedCodecWhenEverythingAligns) {
  for (const char* codec : {"h264", "hevc", "av1"}) {
    const char* reason = nullptr;
    EXPECT_EQ(chooseStreamEncodePath({true, true, false, true}, codec,
                                     /*codecHasGpuEncoder=*/true,
                                     /*frameHasEncoderTexture=*/true, &reason),
              GpuEncodePath::GpuDirect) << codec;
    EXPECT_EQ(std::string(reason), "gpu-direct") << codec;
  }
}

TEST(ChooseStreamEncodePath, BaseBlockerKeepsPrecedenceOverSenderGates) {
  const char* reason = nullptr;
  EXPECT_EQ(chooseStreamEncodePath({/*hw=*/false, true, false, true}, "av1",
                                   /*codecHasGpuEncoder=*/false,
                                   /*frameHasEncoderTexture=*/false, &reason),
            GpuEncodePath::CpuFallback);
  EXPECT_EQ(std::string(reason), "no-hardware-encoder");
}

// 2026-09-20: "codec-not-h264" is retired. A codec this machine has no hardware
// encoder for reports the SAME reason as no hardware at all, so support bundles
// keep one vocabulary.
TEST(ChooseStreamEncodePath, ACodecWithoutAGpuEncoderReportsNoHardwareEncoder) {
  const char* reason = nullptr;
  EXPECT_EQ(chooseStreamEncodePath({true, true, false, true}, "hevc",
                                   /*codecHasGpuEncoder=*/false,
                                   /*frameHasEncoderTexture=*/true, &reason),
            GpuEncodePath::CpuFallback);
  EXPECT_EQ(std::string(reason), "no-hardware-encoder");
}

TEST(ChooseStreamEncodePath, MissingEncoderTextureFallsBack) {
  const char* reason = nullptr;
  EXPECT_EQ(chooseStreamEncodePath({true, true, false, true}, "h264", true,
                                   /*frameHasEncoderTexture=*/false, &reason),
            GpuEncodePath::CpuFallback);
  EXPECT_EQ(std::string(reason), "no-encoder-texture");
}

TEST(ChooseStreamEncodePath, EnvToggleFallsBackWithEnvReason) {
  const char* reason = nullptr;
  EXPECT_EQ(chooseStreamEncodePath({true, true, /*forcedOffByEnv=*/true, true}, "h264", true, true,
                                   &reason),
            GpuEncodePath::CpuFallback);
  EXPECT_EQ(std::string(reason), "forced-off-by-env");
}

TEST(GpuVideoEncoderConfig, CodecDefaultsToH264) {
  corevideo::modules::GpuVideoEncoderConfig cfg;
  EXPECT_EQ(cfg.codec, "h264");
}
}  // namespace
