#include "modules/UvcIsoArrivalQueue.h"
#include "modules/I420CaptureArrival.h"
#include "core/IsoVideoIngress.h"
#include <gtest/gtest.h>
using namespace corevideo::modules;

TEST(UvcIsoArrivalQueue, DelayedRenderDrainsActualArrivalsInsteadOfSamplingLatest) {
  I420CaptureArrival capture("capture:camera", {});
  UvcIsoArrivalQueue queue;
  queue.require(true);
  auto cpu = std::make_shared<const std::vector<uint8_t>>(64 * 64 * 3 / 2, 128);
  std::vector<VideoFrame> originals;
  for (int i = 0; i < 4; ++i) { originals.push_back(capture.publish(cpu, 64, 64, true, true)); queue.offer(originals.back()); }
  auto batch = queue.take();
  corevideo::core::IsoVideoIngress ingress;
  ingress.observe({"capture:camera"}, {originals.back()}, batch, 9999);
  const auto recorded = ingress.drain(std::chrono::milliseconds(0));
  ASSERT_EQ(recorded.size(), 4u);
  for (size_t i = 0; i < recorded.size(); ++i) {
    EXPECT_EQ(recorded[i].frame.frameId, originals[i].frameId);
    EXPECT_EQ(recorded[i].frame.sourceEpoch, originals[i].sourceEpoch);
    EXPECT_EQ(recorded[i].timelineTimestamp100ns, originals[i].captureTimestamp100ns);
    EXPECT_EQ(recorded[i].frame.i420, cpu);
    EXPECT_TRUE(recorded[i].frame.i420FullRange); EXPECT_TRUE(recorded[i].frame.i420Bt601);
  }
  EXPECT_EQ(queue.stats().accepted, 4u); EXPECT_EQ(queue.stats().drained, 4u);
  EXPECT_EQ(queue.stats().overflowed, 0u); EXPECT_EQ(queue.stats().bytes, 0u);
}

TEST(UvcIsoArrivalQueue, DisabledDemandReleasesReferencesAndRearmStartsWithNewArrivals) {
  I420CaptureArrival capture("capture:camera", {});
  UvcIsoArrivalQueue queue;
  auto cpu = std::make_shared<const std::vector<uint8_t>>(64 * 64 * 3 / 2, 128);
  queue.offer(capture.publish(cpu, 64, 64, false, false));
  EXPECT_TRUE(queue.take().empty()); EXPECT_EQ(queue.stats().accepted, 0u);
  queue.require(true); queue.offer(capture.publish(cpu, 64, 64, false, false));
  EXPECT_EQ(cpu.use_count(), 2); // caller and FIFO: descriptors share existing pixels
  queue.require(false);
  EXPECT_EQ(cpu.use_count(), 1); EXPECT_EQ(queue.stats().released, 1u);
  queue.require(true); EXPECT_TRUE(queue.take().empty());
  queue.offer(capture.publish(cpu, 64, 64, false, false));
  auto batch = queue.take(); ASSERT_EQ(batch.size(), 1u); EXPECT_EQ(batch[0].frameId, 3);
}

TEST(UvcIsoArrivalQueue, OverflowIsBoundedAndEveryEvictionIsVisible) {
  I420CaptureArrival capture("capture:camera", {});
  UvcIsoArrivalQueue queue; queue.require(true);
  auto cpu = std::make_shared<const std::vector<uint8_t>>(64 * 64 * 3 / 2, 128);
  for (int i = 0; i < 9; ++i) queue.offer(capture.publish(cpu, 64, 64, false, false));
  const auto stats = queue.stats();
  EXPECT_EQ(stats.pending, 4u); EXPECT_EQ(stats.overflowed, 5u);
  EXPECT_LE(stats.bytes, UvcIsoArrivalQueue::kMaxBytes);
  auto batch = queue.take(); ASSERT_EQ(batch.size(), 4u);
  EXPECT_EQ(batch.front().frameId, 6); EXPECT_EQ(batch.back().frameId, 9);
}

TEST(UvcIsoArrivalQueue, ResizeAndReconnectKeepOriginalEpochsAndNeverRelabelFrames) {
  UvcIsoArrivalQueue queue; queue.require(true);
  auto cpu = std::make_shared<const std::vector<uint8_t>>(128 * 64 * 3 / 2, 128);
  I420CaptureArrival first("capture:camera", {});
  queue.offer(first.publish(cpu, 64, 64, false, false));
  queue.offer(first.publish(cpu, 128, 64, false, false));
  I420CaptureArrival reconnected("capture:camera", {});
  queue.offer(reconnected.publish(cpu, 64, 64, false, false));
  auto batch = queue.take(); ASSERT_EQ(batch.size(), 3u);
  EXPECT_LT(batch[0].sourceEpoch, batch[1].sourceEpoch);
  EXPECT_LT(batch[1].sourceEpoch, batch[2].sourceEpoch);
  EXPECT_EQ(batch[0].frameId, 1); EXPECT_EQ(batch[1].frameId, 2); EXPECT_EQ(batch[2].frameId, 1);
}

TEST(UvcIsoArrivalQueue, ByteBoundRefusesOversizeAndEvictsBeforeFrameLimit) {
  UvcIsoArrivalQueue queue; queue.require(true);
  VideoFrame frame; frame.participantId = "capture:camera";
  frame.width = frame.i420Width = 4096; frame.height = frame.i420Height = 4096;
  frame.i420 = std::make_shared<const std::vector<uint8_t>>(4096u * 4096u * 3 / 2, 128);
  queue.offer(frame); queue.offer(frame);
  EXPECT_EQ(queue.stats().pending, 1u); EXPECT_EQ(queue.stats().overflowed, 1u);
  frame.i420 = std::make_shared<const std::vector<uint8_t>>(UvcIsoArrivalQueue::kMaxBytes + 1, 128);
  queue.offer(frame);
  EXPECT_EQ(queue.stats().pending, 1u); EXPECT_EQ(queue.stats().overflowed, 2u);
  EXPECT_LE(queue.stats().bytes, UvcIsoArrivalQueue::kMaxBytes);
}
