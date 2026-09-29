#include "modules/SharedGpuVideoEncoder.h"

#include <gtest/gtest.h>

using namespace corevideo::modules;

namespace {
struct Counts {
  int builds = 0;
  int starts = 0;
  int stops = 0;
  int submits = 0;
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
  void stop() override {
    if (running_) ++counts_.stops;
    running_ = false;
  }
  bool healthy() const override { return running_; }
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
  EXPECT_EQ(rtmpFrames, 1);
  EXPECT_EQ(srtFrames, 1);

  rtmp->stop();
  EXPECT_EQ(counts.stops, 0) << "stopping RTMP tore down SRT's encoder";
  frame.frameNumber = 11;
  ASSERT_TRUE(srt->submit(frame));
  EXPECT_EQ(rtmpFrames, 1);
  EXPECT_EQ(srtFrames, 2);
  srt->stop();
  EXPECT_EQ(counts.stops, 1);
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
