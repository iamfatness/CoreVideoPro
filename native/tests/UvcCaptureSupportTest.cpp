#include "modules/Interfaces.h"
#include "modules/UvcCaptureSupport.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using corevideo::modules::uvc::UvcFormatCandidate;
using corevideo::modules::uvc::deriveYuvColorHints;
using corevideo::modules::uvc::kUvcNoFirstFrameTimeoutMs;
using corevideo::modules::uvc::nv12ToI420;
using corevideo::modules::uvc::pickBestUvcFormat;
using corevideo::modules::uvc::stableCaptureDeviceIdFromSymbolicLink;
using corevideo::modules::uvc::uvcNoFirstFrameTimedOut;
using corevideo::modules::uvc::uvcNoFirstFrameWarning;
using corevideo::modules::uvc::uvcSubtypeRank;
using corevideo::modules::uvc::yuy2ToI420;

UvcFormatCandidate makeCandidate(int width, int height, int fps, const std::string& fourcc, int index) {
  UvcFormatCandidate candidate;
  candidate.width = width;
  candidate.height = height;
  candidate.fpsNumerator = fps;
  candidate.fpsDenominator = 1;
  candidate.fourcc = fourcc;
  candidate.nativeIndex = index;
  return candidate;
}

}  // namespace

TEST(UvcFormatNegotiation, EmptyListReturnsMinusOne) {
  EXPECT_EQ(pickBestUvcFormat({}), -1);
}

TEST(UvcFormatNegotiation, Prefers1080p60Nv12WhenAvailable) {
  const std::vector<UvcFormatCandidate> candidates = {
      makeCandidate(1280, 720, 60, "NV12", 0),
      makeCandidate(1920, 1080, 60, "NV12", 1),
      makeCandidate(1920, 1080, 30, "NV12", 2),
      makeCandidate(1920, 1080, 60, "MJPG", 3),
  };
  EXPECT_EQ(pickBestUvcFormat(candidates), 1);
}

TEST(UvcFormatNegotiation, Mjpg1080p60BeatsNv12720p30) {
  // The classic USB2 webcam (e.g. C920): 1080p only over MJPG. Resolution and
  // frame rate outrank subtype — the MJPG decode is Media Foundation's job on
  // the capture thread, never the render path.
  const std::vector<UvcFormatCandidate> candidates = {
      makeCandidate(1280, 720, 30, "NV12", 0),
      makeCandidate(1920, 1080, 60, "MJPG", 1),
  };
  EXPECT_EQ(pickBestUvcFormat(candidates), 1);
}

TEST(UvcFormatNegotiation, SixtyBeatsBothThirtyAndOneTwenty) {
  const std::vector<UvcFormatCandidate> candidates = {
      makeCandidate(1920, 1080, 30, "NV12", 0),
      makeCandidate(1920, 1080, 120, "NV12", 1),
      makeCandidate(1920, 1080, 60, "NV12", 2),
  };
  EXPECT_EQ(pickBestUvcFormat(candidates), 2);
}

TEST(UvcFormatNegotiation, FractionalNtscRateCountsAsReachingTarget) {
  // 60000/1001 = 59.94 must count as hitting a 60fps target (and being closer
  // to it than 120).
  std::vector<UvcFormatCandidate> candidates = {
      makeCandidate(1920, 1080, 120, "NV12", 0),
  };
  UvcFormatCandidate ntsc;
  ntsc.width = 1920;
  ntsc.height = 1080;
  ntsc.fpsNumerator = 60000;
  ntsc.fpsDenominator = 1001;
  ntsc.fourcc = "NV12";
  ntsc.nativeIndex = 1;
  candidates.push_back(ntsc);
  EXPECT_EQ(pickBestUvcFormat(candidates), 1);
}

TEST(UvcFormatNegotiation, OversizedOnlyCameraPicksSmallestAboveTarget) {
  const std::vector<UvcFormatCandidate> candidates = {
      makeCandidate(3840, 2160, 30, "NV12", 0),
      makeCandidate(2560, 1440, 30, "NV12", 1),
  };
  EXPECT_EQ(pickBestUvcFormat(candidates), 1);
}

TEST(UvcFormatNegotiation, FitsUnderTargetBeatsOversized) {
  const std::vector<UvcFormatCandidate> candidates = {
      makeCandidate(3840, 2160, 60, "NV12", 0),
      makeCandidate(1280, 720, 30, "YUY2", 1),
  };
  EXPECT_EQ(pickBestUvcFormat(candidates), 1);
}

TEST(UvcFormatNegotiation, SubtypeBreaksResolutionAndRateTies) {
  const std::vector<UvcFormatCandidate> candidates = {
      makeCandidate(1920, 1080, 60, "MJPG", 0),
      makeCandidate(1920, 1080, 60, "YUY2", 1),
      makeCandidate(1920, 1080, 60, "NV12", 2),
  };
  EXPECT_EQ(pickBestUvcFormat(candidates), 2);
  EXPECT_TRUE(uvcSubtypeRank("NV12") < uvcSubtypeRank("YUY2"));
  EXPECT_TRUE(uvcSubtypeRank("YUY2") < uvcSubtypeRank("MJPG"));
  EXPECT_TRUE(uvcSubtypeRank("MJPG") < uvcSubtypeRank("H264"));
}

TEST(UvcPixelRepack, Nv12ToI420DeinterleavesChroma) {
  // 4x4 NV12 with distinct strides: Y rows padded to 6 bytes, UV rows to 8.
  const int width = 4;
  const int height = 4;
  const uint8_t y[24] = {
      10, 11, 12, 13, 0xEE, 0xEE,
      20, 21, 22, 23, 0xEE, 0xEE,
      30, 31, 32, 33, 0xEE, 0xEE,
      40, 41, 42, 43, 0xEE, 0xEE,
  };
  const uint8_t uv[16] = {
      101, 201, 102, 202, 0xEE, 0xEE, 0xEE, 0xEE,   // row 0: U0 V0 U1 V1
      111, 211, 112, 212, 0xEE, 0xEE, 0xEE, 0xEE,   // row 1
  };
  std::vector<uint8_t> out;
  nv12ToI420(y, 6, uv, 8, width, height, out);
  ASSERT_TRUE(out.size() == static_cast<size_t>(width * height * 3 / 2));

  // Y plane copied without padding.
  EXPECT_EQ(out[0], 10);
  EXPECT_EQ(out[3], 13);
  EXPECT_EQ(out[4], 20);
  EXPECT_EQ(out[15], 43);
  // U then V planes, deinterleaved.
  const size_t uOffset = static_cast<size_t>(width * height);
  EXPECT_EQ(out[uOffset + 0], 101);
  EXPECT_EQ(out[uOffset + 1], 102);
  EXPECT_EQ(out[uOffset + 2], 111);
  EXPECT_EQ(out[uOffset + 3], 112);
  const size_t vOffset = uOffset + static_cast<size_t>(width * height / 4);
  EXPECT_EQ(out[vOffset + 0], 201);
  EXPECT_EQ(out[vOffset + 1], 202);
  EXPECT_EQ(out[vOffset + 2], 211);
  EXPECT_EQ(out[vOffset + 3], 212);

  // The result is a valid VideoFrame I420 payload.
  corevideo::modules::VideoFrame frame;
  frame.i420 = std::make_shared<std::vector<uint8_t>>(out);
  frame.i420Width = width;
  frame.i420Height = height;
  EXPECT_TRUE(frame.hasI420());
}

TEST(UvcPixelRepack, Nv12RejectsOddDimensions) {
  const uint8_t y[9] = {};
  const uint8_t uv[6] = {};
  std::vector<uint8_t> out{1, 2, 3};
  nv12ToI420(y, 3, uv, 3, 3, 3, out);
  EXPECT_TRUE(out.empty());
}

TEST(UvcPixelRepack, Yuy2ToI420AveragesVerticalChroma) {
  // 2x2 YUY2: each row is Y0 U Y1 V.
  const int width = 2;
  const int height = 2;
  const uint8_t src[8] = {
      50, 100, 51, 200,   // row 0
      52, 110, 53, 210,   // row 1
  };
  std::vector<uint8_t> out;
  yuy2ToI420(src, 4, width, height, out);
  ASSERT_TRUE(out.size() == static_cast<size_t>(width * height * 3 / 2));
  EXPECT_EQ(out[0], 50);
  EXPECT_EQ(out[1], 51);
  EXPECT_EQ(out[2], 52);
  EXPECT_EQ(out[3], 53);
  EXPECT_EQ(out[4], 105);  // U averaged (100+110+1)/2
  EXPECT_EQ(out[5], 205);  // V averaged (200+210+1)/2
}

TEST(UvcStableDeviceId, MatchesWinUiShellSha256Convention) {
  // These vectors are mirrored in the C# CaptureDeviceDiscoveryMapper tests —
  // the shell and the core MUST derive the identical stable id from the same
  // symbolic link so scene routes resolve to the same "capture:<id>" source.
  EXPECT_EQ(stableCaptureDeviceIdFromSymbolicLink("test-device"), "2fe90d9c33ad85a1");
  EXPECT_EQ(
      stableCaptureDeviceIdFromSymbolicLink(
          "\\\\?\\usb#vid_046d&pid_085e&mi_00#6&158f56b0&0&0000#{e5323777-f976-4f5b-9b55-b94699c46e44}\\global"),
      "98a3916224275371");
  EXPECT_EQ(stableCaptureDeviceIdFromSymbolicLink(""), "");
}

TEST(UvcColorHints, DefaultsToLimitedRangeAndResolutionMatrix) {
  // Unknown attributes: limited range; matrix by frame height.
  const auto hd = deriveYuvColorHints(0, 0, 1080);
  EXPECT_FALSE(hd.fullRange);
  EXPECT_FALSE(hd.bt601);
  const auto sd = deriveYuvColorHints(0, 0, 480);
  EXPECT_FALSE(sd.fullRange);
  EXPECT_TRUE(sd.bt601);
}

TEST(UvcColorHints, HonorsExplicitMediaTypeAttributes) {
  // MFNominalRange_Normal (1) = full range; MFVideoTransferMatrix_BT601 (2).
  const auto full601 = deriveYuvColorHints(1, 2, 1080);
  EXPECT_TRUE(full601.fullRange);
  EXPECT_TRUE(full601.bt601);
  // MFNominalRange_Wide (2) = 16-235; MFVideoTransferMatrix_BT709 (1) wins
  // over the SD-height fallback.
  const auto limited709 = deriveYuvColorHints(2, 1, 480);
  EXPECT_FALSE(limited709.fullRange);
  EXPECT_FALSE(limited709.bt601);
}

TEST(UvcCaptureDeviceFactory, GatedByBuildFlag) {
#if COREVIDEO_WITH_UVC
  // Dev build: the factory constructs (enumeration is lazy and lists devices
  // only — it never opens a camera, so this is safe on a busy rig).
  auto device = corevideo::modules::createUvcCaptureDevice();
  ASSERT_NE(device, nullptr);
  const auto devices = device->enumerate();
  for (const auto& info : devices) {
    EXPECT_FALSE(info.id.empty());
    EXPECT_EQ(info.vendor, "uvc");
    EXPECT_EQ(info.kind, "video");
    EXPECT_FALSE(info.nativeDeviceId.empty());
    EXPECT_EQ(info.connectionState, "detected");
  }
  // No device was connected, so no frames may flow.
  EXPECT_TRUE(device->pollVideoFrames(1000).empty());
#else
  EXPECT_EQ(corevideo::modules::createUvcCaptureDevice(), nullptr);
#endif
}

TEST(UvcNoFirstFrameWatchdog, NeverStallsWhenAFrameHasArrived) {
  // frameId > 0 means the first frame landed — the device is healthy no matter
  // how much wall time has elapsed (a slow camera is still a working camera).
  EXPECT_FALSE(uvcNoFirstFrameTimedOut(/*frameId=*/1, /*elapsedMs=*/1'000'000, 4000));
  EXPECT_FALSE(uvcNoFirstFrameTimedOut(/*frameId=*/42, /*elapsedMs=*/4001, 4000));
}

TEST(UvcNoFirstFrameWatchdog, StallsOnlyPastTheTimeoutWithNoFrame) {
  EXPECT_FALSE(uvcNoFirstFrameTimedOut(/*frameId=*/0, /*elapsedMs=*/0, 4000));
  EXPECT_FALSE(uvcNoFirstFrameTimedOut(/*frameId=*/0, /*elapsedMs=*/3999, 4000));
  EXPECT_TRUE(uvcNoFirstFrameTimedOut(/*frameId=*/0, /*elapsedMs=*/4000, 4000));
  EXPECT_TRUE(uvcNoFirstFrameTimedOut(/*frameId=*/0, /*elapsedMs=*/9000, 4000));
}

TEST(UvcNoFirstFrameWatchdog, DefaultTimeoutIsFourSeconds) {
  EXPECT_EQ(kUvcNoFirstFrameTimeoutMs, 4000);
  // The default arg matches the constant.
  EXPECT_TRUE(uvcNoFirstFrameTimedOut(0, 4000));
  EXPECT_FALSE(uvcNoFirstFrameTimedOut(0, 3999));
}

TEST(UvcNoFirstFrameWatchdog, WarningNamesTheDeviceAndTheLikelyCause) {
  const std::string warning = uvcNoFirstFrameWarning("Game Capture HD60 S+", 4000);
  EXPECT_NE(warning.find("Game Capture HD60 S+"), std::string::npos);
  EXPECT_NE(warning.find("no frames"), std::string::npos);
  EXPECT_NE(warning.find("in use by another app"), std::string::npos);
  EXPECT_NE(warning.find("4s"), std::string::npos);
}

TEST(UvcNoFirstFrameWatchdog, WarningFallsBackToGenericNameWhenEmpty) {
  const std::string warning = uvcNoFirstFrameWarning("", 4000);
  EXPECT_NE(warning.find("Camera connected but delivered no frames"), std::string::npos);
}
