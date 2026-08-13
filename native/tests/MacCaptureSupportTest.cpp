// Pure-logic tests for the macOS capture support helpers (MacCaptureSupport.h)
// — always compiled, run on every platform — plus gated factory/adapter tests
// that are headless-safe (no camera / no screen-recording permission on CI:
// enumeration may be empty and connects may fail, but rows must stay truthful
// and nothing may crash). Skip idiom: log + early-return (no GTEST_SKIP in the
// vendored gtest).

#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

#include "modules/Interfaces.h"
#include "modules/MacCaptureSupport.h"

namespace {

using namespace corevideo::modules;

TEST(MacCaptureSupport, Nv12StridedConversionHonorsRowPadding) {
  // 4x2 NV12 with padded strides: Y stride 8 (4 used), UV stride 8 (4 used).
  const uint8_t y[16] = {1, 2, 3, 4, 99, 99, 99, 99, 5, 6, 7, 8, 99, 99, 99, 99};
  const uint8_t uv[8] = {10, 20, 30, 40, 99, 99, 99, 99};
  std::vector<uint8_t> i420;
  ASSERT_TRUE(maccapture::nv12ToI420Strided(y, 8, uv, 8, 4, 2, i420));
  ASSERT_EQ(i420.size(), 4u * 2u + 2u * 2u / 2u + 2u);  // 8 Y + 2 U + 2 V
  EXPECT_EQ(i420[0], 1);
  EXPECT_EQ(i420[7], 8);   // last Y, padding skipped
  EXPECT_EQ(i420[8], 10);  // U plane: uv[0], uv[2]
  EXPECT_EQ(i420[9], 30);
  EXPECT_EQ(i420[10], 20);  // V plane: uv[1], uv[3]
  EXPECT_EQ(i420[11], 40);
}

TEST(MacCaptureSupport, Nv12StridedRejectsOddDimensions) {
  const uint8_t data[16] = {};
  std::vector<uint8_t> i420;
  EXPECT_FALSE(maccapture::nv12ToI420Strided(data, 8, data, 8, 3, 2, i420));
  EXPECT_FALSE(maccapture::nv12ToI420Strided(data, 8, data, 8, 4, 1, i420));
  EXPECT_FALSE(maccapture::nv12ToI420Strided(nullptr, 8, data, 8, 4, 2, i420));
}

TEST(MacCaptureSupport, BgraTightCopyDropsRowPadding) {
  const uint8_t src[2 * 12] = {1, 1, 1, 1, 2, 2, 2, 2, 9, 9, 9, 9,
                               3, 3, 3, 3, 4, 4, 4, 4, 9, 9, 9, 9};
  std::vector<uint8_t> out;
  maccapture::copyBgraTight(src, 12, 2, 2, out);
  ASSERT_EQ(out.size(), 16u);
  EXPECT_EQ(out[0], 1);
  EXPECT_EQ(out[4], 2);
  EXPECT_EQ(out[8], 3);
  EXPECT_EQ(out[12], 4);
}

TEST(MacCaptureSupport, ColorHintsMirrorUvcDefaults) {
  // '420f' = full range; explicit 601/709 matrix strings win; unknown matrix
  // falls back on height (>=720 -> BT.709), matching deriveYuvColorHints.
  const uint32_t k420f = 0x34323066u;
  const uint32_t k420v = 0x34323076u;
  auto hints = maccapture::deriveMacYuvColorHints(k420f, "ITU_R_709_2", 1080);
  EXPECT_TRUE(hints.fullRange);
  EXPECT_FALSE(hints.bt601);
  hints = maccapture::deriveMacYuvColorHints(k420v, "ITU_R_601_4", 1080);
  EXPECT_FALSE(hints.fullRange);
  EXPECT_TRUE(hints.bt601);
  hints = maccapture::deriveMacYuvColorHints(k420v, "", 480);
  EXPECT_FALSE(hints.fullRange);
  EXPECT_TRUE(hints.bt601);
  hints = maccapture::deriveMacYuvColorHints(k420v, "", 720);
  EXPECT_FALSE(hints.bt601);
}

TEST(MacCaptureSupport, SckIdsMirrorWgcShapes) {
  EXPECT_EQ(maccapture::sckScreenId(0), "screen:0");
  EXPECT_EQ(maccapture::sckWindowId(4242u), "window:4242");
  EXPECT_EQ(maccapture::sckScreenName(0, 2560, 1440), "Display 1 (2560x1440)");
}

#if COREVIDEO_WITH_AVF_CAPTURE
TEST(MacCaptureAdapters, AvfFactoryEnumeratesTruthfully) {
  auto device = createAvfCaptureDevice();
  ASSERT_NE(device, nullptr);
  // Headless CI has no cameras: an empty list is legal; every listed device
  // must carry the contract fields and nothing is connected yet.
  const auto devices = device->enumerate();
  for (const auto& info : devices) {
    EXPECT_FALSE(info.id.empty());
    EXPECT_EQ(info.vendor, "avfoundation");
    EXPECT_EQ(info.kind, "video");
    EXPECT_FALSE(info.nativeDeviceId.empty());
    EXPECT_EQ(info.connectionState, "detected");
  }
  EXPECT_TRUE(device->pollVideoFrames(0).empty());
  // Connecting an unknown id must not crash and must not fabricate a device.
  const auto after = device->connect("no-such-camera", "capture-key");
  EXPECT_EQ(after.size(), devices.size());
}
#endif

#if COREVIDEO_WITH_SCK
TEST(MacCaptureAdapters, SckFactoryEnumeratesWithoutCrashing) {
  auto device = createSckScreenCaptureDevice();
  ASSERT_NE(device, nullptr);
  // The shareable-content fetch is async + permission-gated: on a headless or
  // unconsented machine the list stays empty — that is truthful, not a
  // failure. Poll twice to exercise the cached-refresh path.
  (void)device->enumerate();
  const auto devices = device->enumerate();
  for (const auto& info : devices) {
    EXPECT_FALSE(info.id.empty());
    EXPECT_EQ(info.vendor, "ScreenCaptureKit");
    EXPECT_TRUE(info.kind == "screen" || info.kind == "window");
  }
  EXPECT_TRUE(device->pollVideoFrames(0).empty());
  const auto after = device->disconnect("screen:99");
  EXPECT_EQ(after.size(), devices.size());
}
#endif

}  // namespace
