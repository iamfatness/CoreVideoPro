#include "core/CaptureBusRoster.h"
#include "core/CaptureLivenessPolicy.h"
#include "core/SourceVideoIngress.h"

#include <gtest/gtest.h>

namespace {
using namespace corevideo;
constexpr int64_t kSecond = 1'000'000'000;

modules::VideoFrame captureFrame(std::string id, modules::CaptureLiveness liveness) {
  modules::VideoFrame frame;
  frame.participantId = std::move(id);
  frame.pixelWidth = 64;
  frame.pixelHeight = 36;
  frame.frameId = 7;
  frame.captureLiveness = liveness;
  return frame;
}

modules::CaptureLiveness live(int64_t sampleNs) {
  return {modules::CaptureLivenessState::Live, true, sampleNs, 0, modules::CaptureLossReason::None};
}
}

TEST(SourceBusCaptureLoss, OneTickDecisionDrivesOnAirAndTheWire) {
  corevideo::core::SourceBus bus;
  const std::string id = "capture:camera";
  const auto frame = captureFrame(id, live(0));
  corevideo::core::publishHeldCaptureFrame(bus, frame);
  (void)bus.ingest(0, corevideo::core::kCaptureStallNs);
  const auto status = bus.onAirStatusFor(id, corevideo::core::kCaptureStallNs);
  ASSERT_TRUE(status && status->capture);
  EXPECT_EQ(status->capture->decision.health, corevideo::core::SourceHealth::Stalled);

  // A delayed snapshot reports the actual preceding tick's decision, even if
  // the diagnostic clock has crossed the silent-fail threshold since then.
  const auto wire = corevideo::core::sourceHealthState(&bus, 6 * kSecond, [](const auto&) {
    return corevideo::core::SourcePresentationPolicy{"black", "Camera"};
  });
  ASSERT_EQ(wire.asArray().size(), 1u);
  const auto& camera = wire.asArray()[0];
  EXPECT_EQ(camera.getString("onAir"), "stalled");
  EXPECT_EQ(camera.getString("dropoutPolicy"), "black");
  EXPECT_EQ(camera.getNumber("lastSampleAgeMs"), 1500);
  EXPECT_EQ(camera.getNumber("failInMs"), 3500);

  (void)bus.ingest(0, 5 * kSecond);
  const auto failed = bus.onAirStatusFor(id, 5 * kSecond);
  ASSERT_TRUE(failed && failed->capture);
  EXPECT_EQ(failed->capture->decision.health, corevideo::core::SourceHealth::Failed);

  // The capture can stay visually static across the recovery.
  corevideo::core::publishHeldCaptureFrame(bus, captureFrame(id, live(5 * kSecond)));
  (void)bus.ingest(0, 5 * kSecond);
  const auto recovered = bus.onAirStatusFor(id, 5 * kSecond);
  ASSERT_TRUE(recovered && recovered->capture);
  EXPECT_EQ(recovered->capture->decision.health, corevideo::core::SourceHealth::Producing);
}

TEST(SourceBusCaptureLoss, ReportedEndRetainsNoPixelsAndNextFrameClearsFailure) {
  corevideo::core::SourceBus bus;
  const std::string id = "capture:camera";
  corevideo::core::publishHeldCaptureFrame(bus, captureFrame(id, live(kSecond)));
  ASSERT_EQ(bus.ingest(0, kSecond).video.size(), 1u);

  corevideo::core::endHeldCaptureFrame(bus, id);
  EXPECT_TRUE(bus.ingest(0, 2 * kSecond).video.empty());
  const auto ended = bus.onAirStatusFor(id, 2 * kSecond);
  ASSERT_TRUE(ended && ended->capture);
  EXPECT_EQ(ended->capture->decision.health, corevideo::core::SourceHealth::Failed);

  corevideo::core::publishHeldCaptureFrame(bus, captureFrame(id, live(3 * kSecond)));
  ASSERT_EQ(bus.ingest(0, 3 * kSecond).video.size(), 1u);
  const auto reconnected = bus.onAirStatusFor(id, 3 * kSecond);
  ASSERT_TRUE(reconnected && reconnected->capture);
  EXPECT_EQ(reconnected->capture->decision.health, corevideo::core::SourceHealth::Producing);
}

TEST(SourceBusCaptureLoss, TombstonesAreBoundedAndLegacyEndStillRemovesTheSource) {
  corevideo::core::SourceBus bus;
  corevideo::core::publishHeldCaptureFrame(bus, captureFrame("capture:legacy", {}));
  corevideo::core::endHeldCaptureFrame(bus, "capture:legacy");
  EXPECT_FALSE(bus.contains("capture:legacy"));

  for (int i = 0; i < 33; ++i) {
    const std::string id = "capture:" + std::to_string(i);
    corevideo::core::publishHeldCaptureFrame(bus, captureFrame(id, live(kSecond)));
    corevideo::core::endHeldCaptureFrame(bus, id);
  }
  EXPECT_EQ(bus.sourceIds().size(), 32u);
  EXPECT_FALSE(bus.contains("capture:0"));
  EXPECT_TRUE(bus.contains("capture:32"));
}
