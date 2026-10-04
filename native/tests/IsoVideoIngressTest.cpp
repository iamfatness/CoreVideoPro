#include "core/IsoVideoIngress.h"
#include <gtest/gtest.h>

namespace {
corevideo::modules::VideoFrame picture(const std::string& id, int64_t sequence, uint64_t epoch = 1) {
  corevideo::modules::VideoFrame frame;
  frame.participantId = id; frame.frameId = sequence; frame.sourceEpoch = epoch;
  frame.width = frame.pixelWidth = frame.height = frame.pixelHeight = 2;
  frame.pixelStride = 8;
  frame.pixels = std::make_shared<const std::vector<uint8_t>>(16, static_cast<uint8_t>(sequence));
  frame.captureTimestamp100ns = sequence * 166667;
  return frame;
}
}

TEST(IsoVideoIngress, CpuBatchKeepsItsOwnIdentitiesAndCaptureTimes) {
  corevideo::core::IsoVideoIngress ingress;
  auto newer = picture("capture:screen", 100);
  std::vector<corevideo::modules::VideoFrame> converted{picture("capture:screen", 10), picture("capture:screen", 11)};
  ingress.observe({"capture:screen"}, {newer}, converted, 999999999);
  const auto result = ingress.drain(std::chrono::milliseconds(0));
  ASSERT_EQ(result.size(), 2u);
  EXPECT_EQ(result[0].frame.frameId, 10); EXPECT_EQ(result[1].frame.frameId, 11);
  EXPECT_EQ(result[0].timelineTimestamp100ns, converted[0].captureTimestamp100ns);
  EXPECT_EQ(result[1].timelineTimestamp100ns, converted[1].captureTimestamp100ns);
}

TEST(IsoVideoIngress, BoundIsPerSourceAndCountsEveryEviction) {
  corevideo::core::IsoVideoIngress ingress;
  std::vector<corevideo::modules::VideoFrame> converted;
  for (int id = 1; id <= 6; ++id) converted.push_back(picture("capture:fast", id));
  converted.push_back(picture("zoom:slow", 1));
  ingress.observe({"capture:fast", "slow"}, {}, converted, 1);
  const auto result = ingress.drain(std::chrono::milliseconds(0));
  ASSERT_EQ(result.size(), 5u);
  EXPECT_EQ(result.front().frame.frameId, 3);
  EXPECT_EQ(result.back().sourceId, "zoom:slow");
  const auto counters = ingress.counters();
  EXPECT_EQ(counters.at("capture:fast").distinctSubmitted, 6u);
  EXPECT_EQ(counters.at("capture:fast").queueOverflowed, 2u);
}

TEST(IsoVideoIngress, DuplicateIsSuppressedButNewEpochIsDistinct) {
  corevideo::core::IsoVideoIngress ingress;
  auto frame = picture("capture:screen", 1);
  ingress.observe({"capture:screen"}, {frame}, {}, 1);
  ingress.observe({"capture:screen"}, {frame}, {}, 2);
  frame.sourceEpoch = 2;
  ingress.observe({"capture:screen"}, {frame}, {}, 3);
  EXPECT_EQ(ingress.drain(std::chrono::milliseconds(0)).size(), 2u);
  EXPECT_EQ(ingress.counters().at("capture:screen").duplicateRejected, 1u);
}

TEST(IsoVideoIngress, LegacyTimelineAndDirectPathRemainAvailable) {
  corevideo::core::IsoVideoIngress ingress;
  auto frame = picture("7", 1, 0); frame.captureTimestamp100ns = 0;
  ingress.observe({"7"}, {frame, picture("media:logo", 2)}, {}, 3456789);
  const auto result = ingress.drain(std::chrono::milliseconds(0));
  ASSERT_EQ(result.size(), 1u);
  EXPECT_EQ(result[0].sourceId, "zoom:7");
  EXPECT_EQ(result[0].timelineTimestamp100ns, 3456789);
  ASSERT_EQ(ingress.latest({"7"}).size(), 1u);
  ingress.clear();
  EXPECT_TRUE(ingress.latest({"7"}).empty()); EXPECT_TRUE(ingress.counters().empty());
  ingress.observe({"7"}, {frame}, {}, 999);
  EXPECT_EQ(ingress.drain(std::chrono::milliseconds(0)).size(), 1u);
}

TEST(IsoVideoIngress, HoldsCompletedCpuViewBetweenArrivalsAndReleasesDisconnectedSource) {
  corevideo::core::IsoVideoIngress ingress;
  auto cpu = picture("capture:screen", 10);
  auto gpuOnly = picture("capture:screen", 11); gpuOnly.pixels.reset();
  ingress.observe({"capture:screen"}, {gpuOnly}, {cpu}, 1);
  ingress.drain(std::chrono::milliseconds(0));
  ingress.observe({"capture:screen"}, {gpuOnly}, {}, 2);
  const auto held = ingress.latest({"capture:screen"});
  ASSERT_EQ(held.size(), 1u);
  EXPECT_EQ(held[0].frame.frameId, 10);
  EXPECT_TRUE(ingress.drain(std::chrono::milliseconds(0)).empty());
  ingress.observe({"capture:screen"}, {}, {}, 3);
  EXPECT_TRUE(ingress.latest({"capture:screen"}).empty());
}
