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
// session, forcedOffByEnv, platformSupported}.
using corevideo::modules::chooseStreamEncodePath;

TEST(ChooseStreamEncodePath, GpuDirectWhenEverythingAligns) {
  const char* reason = nullptr;
  EXPECT_EQ(chooseStreamEncodePath({true, true, false, true}, /*codecIsH264=*/true,
                                   /*frameHasEncoderTexture=*/true, &reason),
            GpuEncodePath::GpuDirect);
  EXPECT_EQ(std::string(reason), "gpu-direct");
}

TEST(ChooseStreamEncodePath, BaseBlockerKeepsPrecedenceOverSenderGates) {
  // Even with a non-H.264 codec AND no encoder texture, a base blocker (here no
  // hardware encoder) is the reason reported, because it is the more fundamental
  // cause and is checked first.
  const char* reason = nullptr;
  EXPECT_EQ(chooseStreamEncodePath({/*hw=*/false, true, false, true}, /*codecIsH264=*/false,
                                   /*frameHasEncoderTexture=*/false, &reason),
            GpuEncodePath::CpuFallback);
  EXPECT_EQ(std::string(reason), "no-hardware-encoder");
}

TEST(ChooseStreamEncodePath, NonH264CodecFallsBackEvenWhenCapable) {
  const char* reason = nullptr;
  EXPECT_EQ(chooseStreamEncodePath({true, true, false, true}, /*codecIsH264=*/false,
                                   /*frameHasEncoderTexture=*/true, &reason),
            GpuEncodePath::CpuFallback);
  EXPECT_EQ(std::string(reason), "codec-not-h264");
}

TEST(ChooseStreamEncodePath, MissingEncoderTextureFallsBack) {
  const char* reason = nullptr;
  EXPECT_EQ(chooseStreamEncodePath({true, true, false, true}, /*codecIsH264=*/true,
                                   /*frameHasEncoderTexture=*/false, &reason),
            GpuEncodePath::CpuFallback);
  EXPECT_EQ(std::string(reason), "no-encoder-texture");
}

TEST(ChooseStreamEncodePath, EnvToggleFallsBackWithEnvReason) {
  const char* reason = nullptr;
  EXPECT_EQ(chooseStreamEncodePath({true, true, /*forcedOffByEnv=*/true, true}, true, true, &reason),
            GpuEncodePath::CpuFallback);
  EXPECT_EQ(std::string(reason), "forced-off-by-env");
}
}  // namespace
