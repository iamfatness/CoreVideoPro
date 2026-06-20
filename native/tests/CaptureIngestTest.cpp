#include "compositor/CompositorLayout.h"
#include "modules/Interfaces.h"
#include "modules/ProgramFramePreview.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

namespace {

uint32_t previewPixelRgba(const corevideo::modules::ProgramFramePreviewPixels& preview, int x, int y) {
  if (preview.width <= 0 || preview.height <= 0 || preview.bgra.empty()) {
    return 0;
  }
  const int clampedX = std::max(0, std::min(x, preview.width - 1));
  const int clampedY = std::max(0, std::min(y, preview.height - 1));
  const size_t offset = static_cast<size_t>((clampedY * preview.width + clampedX) * 4);
  return (static_cast<uint32_t>(preview.bgra[offset + 3]) << 24) |
         (static_cast<uint32_t>(preview.bgra[offset + 2]) << 16) |
         (static_cast<uint32_t>(preview.bgra[offset + 1]) << 8) |
         static_cast<uint32_t>(preview.bgra[offset + 0]);
}

}  // namespace

TEST(CaptureIngest, ConnectedDeviceEmitsTestPatternPixels) {
  auto modules = corevideo::modules::createStubModules();
  ASSERT_NE(modules.captureDevice, nullptr);

  // decklink-1 is connected with signal in the stub device set.
  const auto frames = modules.captureDevice->pollVideoFrames(1000);
  const corevideo::modules::VideoFrame* camera = nullptr;
  for (const auto& frame : frames) {
    if (frame.participantId == "capture:decklink-1") {
      camera = &frame;
    }
  }
  ASSERT_NE(camera, nullptr);
  EXPECT_TRUE(camera->hasPixels());
  EXPECT_EQ(camera->pixelWidth, 640);
  EXPECT_EQ(camera->pixelHeight, 360);
  EXPECT_EQ(camera->pixelStride, 640 * 4);

  const auto& pixels = *camera->pixels;
  const size_t row = static_cast<size_t>(camera->pixelHeight / 2) * static_cast<size_t>(camera->pixelStride);
  // Leftmost bar is white.
  EXPECT_EQ(pixels[row + 0], 255);  // B
  EXPECT_EQ(pixels[row + 1], 255);  // G
  EXPECT_EQ(pixels[row + 2], 255);  // R
  // Center bar is green.
  const size_t center = row + static_cast<size_t>(camera->pixelWidth / 2) * 4;
  EXPECT_EQ(pixels[center + 0], 0);    // B
  EXPECT_EQ(pixels[center + 1], 255);  // G
  EXPECT_EQ(pixels[center + 2], 0);    // R
}

TEST(CaptureIngest, DisconnectedDeviceEmitsNoFrame) {
  auto modules = corevideo::modules::createStubModules();
  const auto frames = modules.captureDevice->pollVideoFrames(1000);
  // aja-io-1 is only "detected" (no signal) in the stub set, so it must not emit.
  for (const auto& frame : frames) {
    EXPECT_NE(frame.participantId, "capture:aja-io-1");
  }
}

TEST(CaptureIngest, CaptureFrameCompositesRealPixelsIntoProgramPreview) {
  auto modules = corevideo::modules::createStubModules();
  const auto frames = modules.captureDevice->pollVideoFrames(1000);
  ASSERT_FALSE(frames.empty());

  // Route a connected capture source full-frame into the program.
  corevideo::modules::CompositorRenderPlan plan;
  plan.renderPlanId = "capture-ingest";
  plan.width = 640;
  plan.height = 360;
  plan.layers.push_back({
      "program",
      "participant-video",
      "capture:decklink-1",
      "capture:decklink-1",
      0,
      {0.f, 0.f, 1.f, 1.f},
      1.f,
  });

  corevideo::modules::ProgramFramePreviewPixels preview;
  corevideo::modules::ProgramFrame programFrame;
  programFrame.width = 640;
  programFrame.height = 360;
  corevideo::modules::fillSyntheticProgramFramePreview(preview, plan, frames, programFrame);

  // Non-empty, correctly-sized ProgramFrame pixels (the F1 gate).
  EXPECT_TRUE(preview.width > 0);
  EXPECT_TRUE(preview.height > 0);
  EXPECT_TRUE(preview.bgra.size() == static_cast<size_t>(preview.width) * static_cast<size_t>(preview.height) * 4);

  // The center samples the green test-pattern bar — real capture pixels, not the
  // synthetic participant slate.
  const uint32_t centerPixel = previewPixelRgba(preview, preview.width / 2, preview.height / 2);
  EXPECT_TRUE(((centerPixel >> 8) & 0xff) > 200);   // green channel high
  EXPECT_TRUE(((centerPixel >> 16) & 0xff) < 80);   // red channel low
  EXPECT_TRUE((centerPixel & 0xff) < 80);           // blue channel low
  EXPECT_NE(centerPixel, corevideo::compositor::colorFromParticipantId("capture:decklink-1"));
}
