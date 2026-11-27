#include "modules/SharedGpuVideoEncoder.h"

#include <gtest/gtest.h>

using namespace corevideo::modules;

namespace {
struct Counts {
  int builds = 0;
  int starts = 0;
  int stops = 0;
  int submits = 0;
  int keyframeRequests = 0;
};

class FakeEncoder final : public GpuVideoEncoder {
 public:
  explicit FakeEncoder(Counts& counts) : counts_(counts) { ++counts_.builds; }
  bool start(const GpuVideoEncoderConfig&, GpuEncodedChunkSink sink) override {
    ++counts_.starts;
    sink_ = std::move(sink);
    running_ = true;
    return true;
  }
  bool submit(const GpuVideoEncoderFrame& frame) override {
    if (!running_) return false;
    ++counts_.submits;
    const uint8_t bytes[]{0, 0, 0, 1, 0x65, 0x88};
    GpuEncodedChunk chunk;
    chunk.data = bytes;
    chunk.size = sizeof(bytes);
    chunk.keyframe = true;
    chunk.frameNumber = frame.frameNumber;
    sink_(chunk);
    return true;
  }
  bool requestKeyframe() override {
    if (!running_) return false;
    ++counts_.keyframeRequests;
    return true;
  }
  void stop() override {
    if (running_) ++counts_.stops;
    running_ = false;
  }
  bool healthy() const override { return running_; }
  int programTickRate(const GpuVideoEncoderConfig&) const override { return 60; }
 private:
  Counts& counts_;
  GpuEncodedChunkSink sink_;
  bool running_ = false;
};
}

TEST(SharedGpuVideoEncoder, TwoMatchingDestinationsShareOneEncodeAndStopIndependently) {
  Counts counts;
  SharedGpuVideoEncoderPool pool([&] { return std::make_unique<FakeEncoder>(counts); });
  auto rtmp = pool.createClient();
  auto srt = pool.createClient();
  GpuVideoEncoderConfig profile;
  int rtmpFrames = 0, srtFrames = 0;
  ASSERT_TRUE(rtmp->start(profile, [&](const GpuEncodedChunk&) { ++rtmpFrames; }));
  ASSERT_TRUE(srt->start(profile, [&](const GpuEncodedChunk&) { ++srtFrames; }));
  EXPECT_EQ(counts.builds, 1);
  EXPECT_EQ(counts.starts, 1);
  GpuVideoEncoderFrame frame;
  frame.frameNumber = 10;
  ASSERT_TRUE(rtmp->submit(frame));
  ASSERT_TRUE(srt->submit(frame));
  EXPECT_EQ(counts.submits, 1) << "the same Program frame was encoded twice";
  frame.frameNumber = 9;
  ASSERT_TRUE(srt->submit(frame));
  EXPECT_EQ(counts.submits, 1) << "a late destination re-encoded an older Program frame";
  EXPECT_EQ(rtmpFrames, 1);
  EXPECT_EQ(srtFrames, 1);
  ASSERT_TRUE(srt->requestKeyframe());
  EXPECT_EQ(counts.keyframeRequests, 1) << "the destination's resync request never reached the shared encoder";

  rtmp->stop();
  EXPECT_EQ(counts.stops, 0) << "stopping RTMP tore down SRT's encoder";
  ASSERT_TRUE(srt->requestKeyframe());
  EXPECT_EQ(counts.keyframeRequests, 2);
  frame.frameNumber = 11;
  ASSERT_TRUE(srt->submit(frame));
  EXPECT_EQ(rtmpFrames, 1);
  EXPECT_EQ(srtFrames, 2);
  srt->stop();
  EXPECT_EQ(counts.stops, 1);
  EXPECT_FALSE(srt->requestKeyframe());
}

TEST(SharedGpuVideoEncoder, IncompatibleProfilesUseSeparateEncoders) {
  Counts counts;
  SharedGpuVideoEncoderPool pool([&] { return std::make_unique<FakeEncoder>(counts); });
  auto rtmp = pool.createClient();
  auto srt = pool.createClient();
  GpuVideoEncoderConfig profile;
  GpuVideoEncoderConfig other = profile;
  other.bitrateKbps += 1000;
  ASSERT_TRUE(rtmp->start(profile, [](const GpuEncodedChunk&) {}));
  ASSERT_TRUE(srt->start(other, [](const GpuEncodedChunk&) {}));
  EXPECT_EQ(counts.builds, 2);
  rtmp->stop();
  srt->stop();
  EXPECT_EQ(counts.stops, 2);
}

TEST(SharedGpuVideoEncoder, ASubscriberCanRequestIdrFromAnEncoderCallback) {
  Counts counts;
  SharedGpuVideoEncoderPool pool([&] { return std::make_unique<FakeEncoder>(counts); });
  auto client = pool.createClient();
  bool requested = false;
  GpuVideoEncoderConfig profile;
  ASSERT_TRUE(client->start(profile, [&](const GpuEncodedChunk&) {
    requested = client->requestKeyframe();
  }));
  GpuVideoEncoderFrame frame;
  frame.frameNumber = 1;
  ASSERT_TRUE(client->submit(frame));
  EXPECT_TRUE(requested);
  EXPECT_EQ(counts.keyframeRequests, 1);
  client->stop();
}

// The sender converts the shared AAC clock with whatever rate the REAL encoder
// stamps pictures in; the pool client must not answer with the interface default.
TEST(SharedGpuVideoEncoder, ClientReportsTheSharedEncodersProgramTickRate) {
  Counts counts;
  SharedGpuVideoEncoderPool pool([&] { return std::make_unique<FakeEncoder>(counts); });
  auto client = pool.createClient();
  GpuVideoEncoderConfig profile;
  profile.fps = 30;
  ASSERT_TRUE(client->start(profile, [](const GpuEncodedChunk&) {}));
  EXPECT_EQ(client->programTickRate(profile), 60);
  client->stop();
}

// Each destination picks its own pictures on its own writer thread. When one of
// them misses a tick (coalescing, or joining mid-period) it picks the next one;
// the shared 30 fps encoder must still get ONE picture per 30 fps period.
TEST(SharedGpuVideoEncoder, OnePicturePerDestinationPeriodAcrossDestinations) {
  Counts counts;
  SharedGpuVideoEncoderPool pool([&] { return std::make_unique<FakeEncoder>(counts); });
  auto first = pool.createClient();
  auto lagging = pool.createClient();
  GpuVideoEncoderConfig profile;
  profile.fps = 30;
  ASSERT_TRUE(first->start(profile, [](const GpuEncodedChunk&) {}));
  ASSERT_TRUE(lagging->start(profile, [](const GpuEncodedChunk&) {}));
  const auto submit = [](GpuVideoEncoder& client, int64_t tick) {
    GpuVideoEncoderFrame frame;
    frame.frameNumber = tick - 40;  // renders: skipped slots are not counted
    frame.programTick = tick;
    return client.submit(frame);
  };
  EXPECT_TRUE(submit(*first, 100));
  EXPECT_TRUE(submit(*first, 102));
  EXPECT_TRUE(submit(*lagging, 103)) << "a repeat is accepted, not a failure";
  EXPECT_EQ(counts.submits, 2) << "ticks 102 and 103 are the same 30 fps period";
  EXPECT_TRUE(submit(*lagging, 105));
  EXPECT_EQ(counts.submits, 3);
  first->stop();
  lagging->stop();
}

// An encoder timed by the destination fps (Media Foundation) is not thinned by
// the pool: only the frame number dedupes, as before.
TEST(SharedGpuVideoEncoder, ADestinationClockEncoderIsOnlyDedupedByFrameNumber) {
  Counts counts;
  SharedGpuVideoEncoderPool pool([&] { return std::make_unique<FakeEncoder>(counts); });
  auto client = pool.createClient();
  GpuVideoEncoderConfig profile;
  profile.fps = 60;  // equals the fake's tick rate
  ASSERT_TRUE(client->start(profile, [](const GpuEncodedChunk&) {}));
  for (int64_t tick = 1; tick <= 4; ++tick) {
    GpuVideoEncoderFrame frame;
    frame.frameNumber = tick;
    ASSERT_TRUE(client->submit(frame));
  }
  EXPECT_EQ(counts.submits, 4);
  client->stop();
}
