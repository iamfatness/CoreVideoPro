#include "modules/MuxInputRatePolicy.h"
#include "modules/Interfaces.h"

#include <gtest/gtest.h>

using corevideo::modules::MuxInputRateWindow;
using corevideo::modules::currentMuxInputRate;
using corevideo::modules::observeMuxInputWrite;

TEST(MuxInputRatePolicy, ReportsMeasuredCompressedBytesAndGoesIdleOnStall) {
  MuxInputRateWindow window;
  window = observeMuxInputWrite(window, 1000, 125000);
  window = observeMuxInputWrite(window, 1500, 125000);
  window = observeMuxInputWrite(window, 2000, 125000);
  EXPECT_NEAR(window.videoMbps, 3.0, 0.001);
  EXPECT_NEAR(window.videoFps, 3.0, 0.001);
  EXPECT_EQ(window.bytes, 0);
  EXPECT_EQ(window.frames, 0);
  EXPECT_NEAR(currentMuxInputRate(window.videoMbps, 3500, 2000), 3.0, 0.001);
  EXPECT_EQ(currentMuxInputRate(window.videoMbps, 4001, 2000), 0);
}

TEST(MuxInputRatePolicy, IgnoresEmptyWritesAndRestartsAfterClockReset) {
  auto window = observeMuxInputWrite({}, 3000, 125000);
  window = observeMuxInputWrite(window, 3100, 0);
  EXPECT_EQ(window.frames, 1);
  window = observeMuxInputWrite(window, 100, 125000);
  EXPECT_EQ(window.startedMs, 100);
  EXPECT_EQ(window.frames, 2);
}

TEST(MuxInputRatePolicy, ProgressUsesCompletedMuxWritesInsteadOfAcceptedInputFrames) {
  corevideo::modules::OutputSender sender;
  sender.framesSent = 1000;
  sender.audioFramesSent = 48000;
  EXPECT_EQ(corevideo::modules::outputSenderProgressUnits(sender), 49000);
  sender.muxInputVideo = corevideo::modules::OutputSender::MuxInputVideo{};
  sender.muxInputVideo->payloadBytes = 125000;
  EXPECT_EQ(corevideo::modules::outputSenderProgressUnits(sender), 125000);
  sender.framesSent += 1000;
  sender.audioFramesSent += 48000;
  EXPECT_EQ(corevideo::modules::outputSenderProgressUnits(sender), 125000);
}
