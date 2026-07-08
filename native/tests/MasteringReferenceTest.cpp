#include "modules/MasteringReference.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <cmath>
#include <vector>

using corevideo::modules::analyzeMasteringReference;
using corevideo::modules::deriveMasteringParamsFromReference;
using corevideo::modules::MasteringParams;
using corevideo::modules::MasteringReferenceProfile;

namespace {

// Interleaved-stereo sine at freq/amp.
std::vector<float> stereoSine(double freq, double amp, size_t frames, double rate) {
  std::vector<float> pcm(frames * 2);
  for (size_t i = 0; i < frames; ++i) {
    const float v = static_cast<float>(amp * std::sin(2.0 * 3.14159265358979 * freq * i / rate));
    pcm[i * 2] = v;
    pcm[i * 2 + 1] = v;
  }
  return pcm;
}

}  // namespace

TEST(MasteringReference, InvalidInputReturnsInvalidProfile) {
  auto p = analyzeMasteringReference(nullptr, 0, 48000.0);
  EXPECT_FALSE(p.valid);
}

TEST(MasteringReference, BassToneHasDominantLowBand) {
  auto pcm = stereoSine(80.0, 0.3, 48000, 48000.0);  // 1s of 80 Hz
  auto p = analyzeMasteringReference(pcm.data(), 48000, 48000.0);
  ASSERT_TRUE(p.valid);
  EXPECT_GT(p.lowDb, p.midDb);
  EXPECT_GT(p.lowDb, p.highDb);
}

TEST(MasteringReference, BrightToneHasDominantHighBand) {
  auto pcm = stereoSine(9000.0, 0.3, 48000, 48000.0);  // 1s of 9 kHz
  auto p = analyzeMasteringReference(pcm.data(), 48000, 48000.0);
  ASSERT_TRUE(p.valid);
  EXPECT_GT(p.highDb, p.lowDb);
  EXPECT_GT(p.highDb, p.midDb);
}

TEST(MasteringReference, MidToneHasDominantMidBand) {
  auto pcm = stereoSine(1000.0, 0.3, 48000, 48000.0);
  auto p = analyzeMasteringReference(pcm.data(), 48000, 48000.0);
  ASSERT_TRUE(p.valid);
  EXPECT_GT(p.midDb, p.lowDb);
  EXPECT_GT(p.midDb, p.highDb);
}

TEST(MasteringReference, DeriveMatchesTiltAndLoudnessWithinCaps) {
  // A bright reference: high band well above mid -> positive high shelf, capped.
  MasteringReferenceProfile ref;
  ref.valid = true;
  ref.lowDb = -30.0;
  ref.midDb = -20.0;
  ref.highDb = -10.0;      // +10 vs mid, should cap at +6
  ref.integratedLufs = -16.0;

  MasteringParams base;
  auto out = deriveMasteringParamsFromReference(ref, base);
  EXPECT_TRUE(out.enabled);
  EXPECT_LT(std::abs(out.highShelfDb - 6.0), 1e-9);   // capped
  EXPECT_LT(std::abs(out.lowShelfDb - (-6.0)), 1e-9);   // -30 - -20 = -10 -> cap -6
  EXPECT_LT(std::abs(out.targetLufs - (-16.0)), 1e-9);
}

TEST(MasteringReference, DeriveIgnoresSilentReferenceLoudness) {
  MasteringReferenceProfile ref;
  ref.valid = true;
  ref.lowDb = ref.midDb = ref.highDb = -60.0;
  ref.integratedLufs = corevideo::modules::kAudioDbfsFloor;  // silence
  MasteringParams base;
  base.targetLufs = -14.0;
  auto out = deriveMasteringParamsFromReference(ref, base);
  EXPECT_LT(std::abs(out.targetLufs - (-14.0)), 1e-9);  // kept base; silent ref ignored
}
