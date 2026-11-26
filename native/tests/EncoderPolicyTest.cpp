#include "modules/EncoderPolicy.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

using corevideo::modules::chooseEncoderCandidate;
using corevideo::modules::codecHasSupportedHardwareEncoder;
using corevideo::modules::encoderCandidatesFor;
using corevideo::modules::isSupportedEncoder;
using corevideo::modules::preferredEncoderFor;

namespace {

bool contains(const std::vector<std::string>& list, const std::string& value) {
  return std::find(list.begin(), list.end(), value) != list.end();
}

}  // namespace

// These are LICENSING decisions expressed as code, not performance tuning. Each
// exclusion below has a cost attached to reversing it, so pin them: a future
// "just add x264 for better quality" would silently move an LGPL product to GPL,
// and re-adding HEVC would reacquire the content-distribution patent exposure
// that dropping it removed.
TEST(EncoderPolicy, NeverOffersGplOrRoyaltyEncumberedSoftwareEncoders) {
  for (const char* forbidden : {"libx264", "libx265", "libsvtav1", "libaom-av1", "libopenh264",
                                "libkvazaar"}) {
    EXPECT_FALSE(isSupportedEncoder(forbidden)) << forbidden << " must not be shippable";
  }
  for (const auto& codec : {"h264", "h265", "av1"}) {
    for (const auto& mode : {"auto", "nvenc", "videotoolbox", "cpu", "software"}) {
      for (const auto& candidate : encoderCandidatesFor(codec, mode)) {
        EXPECT_TRUE(isSupportedEncoder(candidate))
            << candidate << " offered for codec=" << codec << " mode=" << mode;
      }
      EXPECT_TRUE(isSupportedEncoder(preferredEncoderFor(codec, mode)));
    }
  }
}

// Intel Quick Sync and AMD AMF are not supported tiers.
TEST(EncoderPolicy, NeverOffersUnsupportedGpuVendors) {
  for (const auto& codec : {"h264", "h265", "av1"}) {
    for (const auto& mode : {"auto", "nvenc", "qsv", "amf", "videotoolbox"}) {
      for (const auto& candidate : encoderCandidatesFor(codec, mode)) {
        EXPECT_EQ(candidate.find("_qsv"), std::string::npos) << candidate;
        EXPECT_EQ(candidate.find("_amf"), std::string::npos) << candidate;
      }
    }
  }
}

// OWNER REVERSAL 2026-09-20: HEVC encode SHIPS on Windows (NVENC). The
// 2026-08-06 exclusion (patent exposure) was reversed by the owner with that
// exposure accepted. macOS gained HEVC through VideoToolbox in Phase 4 (O3);
// see AppleOffersHevcThroughVideoToolbox.
TEST(EncoderPolicy, HevcShipsOnWindowsNvencOnly) {
#if defined(__APPLE__)
  // Windows policy only; the Apple HEVC policy is AppleOffersHevcThroughVideoToolbox.
#else
  EXPECT_TRUE(codecHasSupportedHardwareEncoder("h265"));
  EXPECT_TRUE(isSupportedEncoder("hevc_nvenc"));
  EXPECT_EQ(preferredEncoderFor("h265", "nvenc"), "hevc_nvenc");
  EXPECT_EQ(preferredEncoderFor("h265", "auto"), "hevc_nvenc");
  const auto candidates = encoderCandidatesFor("h265", "auto");
  ASSERT_EQ(candidates.size(), 1u);  // NVENC or nothing: no software HEVC, no h264 substitute
  EXPECT_EQ(candidates[0], "hevc_nvenc");
#endif
}

// H.264 must ALWAYS have a path — it is the delivery default and the fallback
// every other decision leans on.
TEST(EncoderPolicy, H264IsAlwaysAvailable) {
  EXPECT_TRUE(codecHasSupportedHardwareEncoder("h264"));
  EXPECT_FALSE(encoderCandidatesFor("h264", "auto").empty());
}

#if !defined(__APPLE__)
// Windows: NVENC first, with Media Foundation as the ONLY fallback so a machine
// without an NVIDIA card degrades instead of being unable to stream at all.
TEST(EncoderPolicy, WindowsFallsBackToMediaFoundationButNothingElse) {
  const auto candidates = encoderCandidatesFor("h264", "auto");
  ASSERT_EQ(candidates.size(), 2u);
  EXPECT_EQ(candidates[0], "h264_nvenc");
  EXPECT_EQ(candidates[1], "h264_mf");
}

// AV1 is Ada-or-nothing: there is no OS or software fallback to hide behind.
TEST(EncoderPolicy, WindowsAv1IsNvencOnlyWithNoFallback) {
  EXPECT_TRUE(codecHasSupportedHardwareEncoder("av1"));
  const auto candidates = encoderCandidatesFor("av1", "auto");
  ASSERT_EQ(candidates.size(), 1u);
  EXPECT_EQ(candidates[0], "av1_nvenc");
  EXPECT_FALSE(contains(candidates, "h264_mf"));
}
// Phase 4 O3 changed only the Apple branch: Windows HEVC and H.264 candidates
// are exactly what they were.
TEST(EncoderPolicy, WindowsHevcPolicyIsUnchanged) {
  EXPECT_EQ(encoderCandidatesFor("h265", "auto"), std::vector<std::string>{"hevc_nvenc"});
  EXPECT_EQ(encoderCandidatesFor("h264", "auto"), (std::vector<std::string>{"h264_nvenc", "h264_mf"}));
}
#else
// Apple Silicon's media engine DECODES AV1 (from M3) but cannot encode it, so an
// AV1 request has no hardware to land on and must be reported, not substituted.
TEST(EncoderPolicy, AppleSiliconHasNoAv1Encoder) {
  EXPECT_FALSE(codecHasSupportedHardwareEncoder("av1"));
  EXPECT_EQ(preferredEncoderFor("av1", "auto"), "h264_videotoolbox");
}

// Phase 4 O3: HEVC encode on Apple through VideoToolbox. Admission still keeps
// HEVC GPU-direct-only (D1, StreamStartAdmission.h), so this is the encoder
// POLICY, not permission to stream HEVC off the GPU path.
TEST(EncoderPolicy, AppleOffersHevcThroughVideoToolbox) {
  EXPECT_TRUE(isSupportedEncoder("hevc_videotoolbox"));
  EXPECT_TRUE(isSupportedEncoder("h264_videotoolbox"));
  EXPECT_TRUE(codecHasSupportedHardwareEncoder("h264"));
  EXPECT_TRUE(codecHasSupportedHardwareEncoder("h265"));
  EXPECT_FALSE(codecHasSupportedHardwareEncoder("av1"));
  EXPECT_EQ(preferredEncoderFor("h265", "videotoolbox"), "hevc_videotoolbox");
  EXPECT_EQ(preferredEncoderFor("h265", "auto"), "hevc_videotoolbox");
  EXPECT_EQ(preferredEncoderFor("h264", "videotoolbox"), "h264_videotoolbox");
  EXPECT_EQ(preferredEncoderFor("h264", "auto"), "h264_videotoolbox");
  EXPECT_EQ(encoderCandidatesFor("h265", "auto"), std::vector<std::string>{"hevc_videotoolbox"});
  EXPECT_EQ(encoderCandidatesFor("h265", "videotoolbox"), std::vector<std::string>{"hevc_videotoolbox"});
  EXPECT_EQ(encoderCandidatesFor("h264", "auto"), std::vector<std::string>{"h264_videotoolbox"});
  EXPECT_EQ(encoderCandidatesFor("h264", "videotoolbox"), std::vector<std::string>{"h264_videotoolbox"});
}

TEST(EncoderPolicy, MacUsesVideoToolboxOnly) {
  const auto candidates = encoderCandidatesFor("h264", "auto");
  ASSERT_EQ(candidates.size(), 1u);
  EXPECT_EQ(candidates[0], "h264_videotoolbox");
}
#endif

// An explicit hardware choice is operator intent: return it alone so FFmpeg
// reports the real device/driver failure rather than quietly substituting.
TEST(EncoderPolicy, ExplicitHardwareChoiceIsNotSubstituted) {
  const auto nvenc = encoderCandidatesFor("h264", "nvenc");
  ASSERT_EQ(nvenc.size(), 1u);
  EXPECT_EQ(nvenc[0], "h264_nvenc");
  EXPECT_FALSE(contains(nvenc, "h264_mf"));
}

// The trial encode spawns FFmpeg inside the stream start. It runs only when it
// can change the choice: never for a single candidate (every Mac start, every
// explicit hardware choice), in order otherwise, stopping at the first success.
TEST(EncoderPolicy, ProbesOnlyWhenTheAnswerCanChangeTheChoice) {
  std::vector<std::string> probed;
  const auto probe = [&](bool answer) {
    return [&probed, answer](const std::string& candidate) { probed.push_back(candidate); return answer; };
  };

  EXPECT_EQ(chooseEncoderCandidate({"h264_videotoolbox"}, "x", probe(false)), "h264_videotoolbox");
  EXPECT_TRUE(probed.empty()) << "a single candidate must never be probed; probed " << probed.size();

  EXPECT_EQ(chooseEncoderCandidate({}, "fallback", probe(true)), "fallback");
  EXPECT_TRUE(probed.empty());

  EXPECT_EQ(chooseEncoderCandidate({"h264_nvenc", "h264_mf"}, "x", probe(true)), "h264_nvenc");
  EXPECT_EQ(probed, (std::vector<std::string>{"h264_nvenc"})) << "stops at the first available candidate";

  probed.clear();
  EXPECT_EQ(chooseEncoderCandidate({"h264_nvenc", "h264_mf"}, "x", probe(false)), "h264_nvenc")
      << "nothing available: the first candidate, so FFmpeg reports the real failure";
  EXPECT_EQ(probed, (std::vector<std::string>{"h264_nvenc", "h264_mf"}));
}
