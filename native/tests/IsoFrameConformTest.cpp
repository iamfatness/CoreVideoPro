// #482 / T3.8. An ISO writer opens LAZILY at its source's first frame and keeps
// that size for the life of the recording (ISO-1: "sized to that frame's native
// dims (no scaling)"). When the guest's frame size changed later, the current
// frame's dimensions were handed to a writer opened at the ORIGINAL size, and
// the picture cropped or padded.
//
// #478 made this common rather than rare: under stable resolution tiers a guest
// moves 720P <-> 1080P whenever they are cued onto or off a bus, so an ordinary
// show now changes a source's frame size several times.
//
// An MP4 video track has ONE size, so conforming is not a choice - the only
// question is whether it is deliberate or accidental. These pin the deliberate
// version: aspect-preserving, centred, letterboxed with real black.

#include "modules/IsoFrameConform.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using corevideo::modules::conformI420ToNv12;
using corevideo::modules::isoFitRect;

TEST(IsoFrameConform, AnUnchangedSizeFillsTheFrameExactly) {
  const auto fit = isoFitRect(1920, 1080, 1920, 1080);
  EXPECT_EQ(fit.x, 0);
  EXPECT_EQ(fit.y, 0);
  EXPECT_EQ(fit.width, 1920);
  EXPECT_EQ(fit.height, 1080);
}

// The case #478 made routine: the same 16:9 aspect at a different tier. It must
// scale edge to edge with NO letterbox, or every tier change would add bars.
TEST(IsoFrameConform, TheSameAspectAtAnotherTierHasNoBars) {
  for (const auto& size : {std::pair<int, int>{1280, 720}, {640, 360}, {3840, 2160}}) {
    const auto fit = isoFitRect(size.first, size.second, 1920, 1080);
    EXPECT_EQ(fit.x, 0) << size.first << "x" << size.second;
    EXPECT_EQ(fit.y, 0) << size.first << "x" << size.second;
    EXPECT_EQ(fit.width, 1920) << size.first << "x" << size.second;
    EXPECT_EQ(fit.height, 1080) << size.first << "x" << size.second;
  }
}

// A guest whose camera is 4:3 pillarboxes rather than stretching. Stretching
// would be the same defect as cropping, only less obvious in review.
TEST(IsoFrameConform, ADifferentAspectIsLetterboxedNeverStretched) {
  const auto fit = isoFitRect(640, 480, 1920, 1080);
  EXPECT_EQ(fit.height, 1080);
  EXPECT_EQ(fit.width, 1440);  // 4:3 inside 16:9
  EXPECT_EQ(fit.x, 240);       // centred
  EXPECT_EQ(fit.y, 0);

  const auto tall = isoFitRect(1080, 1920, 1920, 1080);  // a phone, portrait
  EXPECT_EQ(tall.height, 1080);
  EXPECT_EQ(tall.width, 608);  // 9:16 inside 16:9 (1080*1080/1920 = 607.5), rounded to even
  EXPECT_EQ(tall.y, 0);
}

// I420 and NV12 carry chroma at half resolution, so an odd origin or extent
// would split a chroma sample across the letterbox edge and fringe it.
TEST(IsoFrameConform, EveryEdgeIsEvenAlignedForHalfResolutionChroma) {
  for (const auto& src : {std::pair<int, int>{641, 481}, {999, 501}, {1001, 999}}) {
    const auto fit = isoFitRect(src.first, src.second, 1920, 1080);
    EXPECT_EQ(fit.x % 2, 0) << src.first << "x" << src.second;
    EXPECT_EQ(fit.y % 2, 0) << src.first << "x" << src.second;
    EXPECT_EQ(fit.width % 2, 0) << src.first << "x" << src.second;
    EXPECT_EQ(fit.height % 2, 0) << src.first << "x" << src.second;
  }
}

TEST(IsoFrameConform, ADegenerateSizeIsRefusedRatherThanDividedByZero) {
  for (const auto& bad : {std::pair<int, int>{0, 1080}, {1920, 0}, {-4, 100}}) {
    const auto fit = isoFitRect(bad.first, bad.second, 1920, 1080);
    EXPECT_EQ(fit.width, 0) << bad.first << "x" << bad.second;
    EXPECT_EQ(fit.height, 0) << bad.first << "x" << bad.second;
  }
}

namespace {

// A solid I420 plane set. Y, U and V each uniform so a correct scale of any
// factor returns exactly the same values - which is what makes the pixel
// assertions below exact rather than approximate.
std::vector<std::uint8_t> solidI420(int w, int h, std::uint8_t y, std::uint8_t u, std::uint8_t v) {
  const std::size_t luma = static_cast<std::size_t>(w) * h;
  std::vector<std::uint8_t> frame(luma + luma / 2);
  std::fill(frame.begin(), frame.begin() + luma, y);
  std::fill(frame.begin() + luma, frame.begin() + luma + luma / 4, u);
  std::fill(frame.begin() + luma + luma / 4, frame.end(), v);
  return frame;
}

}  // namespace

// The whole point: whatever arrives, the writer gets EXACTLY the size it opened
// at. A short or long buffer is what cropped and padded the picture.
TEST(IsoFrameConform, TheOutputIsAlwaysExactlyTheOpenedSize) {
  std::vector<std::uint8_t> out;
  for (const auto& src : {std::pair<int, int>{1280, 720}, {3840, 2160}, {640, 480}}) {
    const auto frame = solidI420(src.first, src.second, 120, 90, 200);
    conformI420ToNv12(frame.data(), src.first, src.second, 1920, 1080, out);
    EXPECT_EQ(out.size(), static_cast<std::size_t>(1920) * 1080 * 3 / 2)
        << src.first << "x" << src.second;
  }
}

TEST(IsoFrameConform, AScaledPictureKeepsItsValuesAndTheBarsAreRealBlack) {
  constexpr int kDstW = 1920, kDstH = 1080;
  const auto frame = solidI420(640, 480, 120, 90, 200);  // 4:3 -> pillarboxed
  std::vector<std::uint8_t> out;
  conformI420ToNv12(frame.data(), 640, 480, kDstW, kDstH, out);

  const auto lumaAt = [&](int x, int y) { return out[static_cast<std::size_t>(y) * kDstW + x]; };
  // Centre is the picture.
  EXPECT_EQ(lumaAt(kDstW / 2, kDstH / 2), 120);
  // The pillarbox columns are BT.601 studio-swing black, the same black the
  // BGRA->NV12 convert already writes. Not 0, which would be blacker than legal.
  EXPECT_EQ(lumaAt(4, kDstH / 2), 16);
  EXPECT_EQ(lumaAt(kDstW - 5, kDstH / 2), 16);

  // Chroma is interleaved UV at half resolution, after the luma plane.
  const std::size_t uvBase = static_cast<std::size_t>(kDstW) * kDstH;
  const auto uvAt = [&](int x, int y) {
    const std::size_t i = uvBase + static_cast<std::size_t>(y) * kDstW + x * 2;
    return std::pair<std::uint8_t, std::uint8_t>{out[i], out[i + 1]};
  };
  EXPECT_EQ(uvAt(kDstW / 4, kDstH / 4), (std::pair<std::uint8_t, std::uint8_t>{90, 200}));
  EXPECT_EQ(uvAt(2, kDstH / 4), (std::pair<std::uint8_t, std::uint8_t>{128, 128}));  // neutral bars
}

// The common path must stay cheap and exact: no resampling artefacts when
// nothing changed size.
TEST(IsoFrameConform, AnUnchangedSizeIsCopiedNotResampled) {
  const auto frame = solidI420(64, 36, 77, 55, 201);
  std::vector<std::uint8_t> out;
  conformI420ToNv12(frame.data(), 64, 36, 64, 36, out);
  ASSERT_EQ(out.size(), static_cast<std::size_t>(64) * 36 * 3 / 2);
  for (int i = 0; i < 64 * 36; ++i) {
    ASSERT_EQ(out[i], 77) << "luma " << i;
  }
  for (std::size_t i = 64 * 36; i < out.size(); i += 2) {
    ASSERT_EQ(out[i], 55) << "u " << i;
    ASSERT_EQ(out[i + 1], 201) << "v " << i;
  }
}
