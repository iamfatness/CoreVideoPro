#include "modules/Interfaces.h"
#include "modules/ProgramFramePreview.h"
#include "modules/RealZoomCaptureSource.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using corevideo::modules::IZoomCaptureSource;
using corevideo::modules::RealZoomCaptureSource;
using corevideo::modules::VideoFrame;

// Builds a tightly packed solid-color BGRA buffer for width x height.
std::vector<uint8_t> solidBgra(int width, int height, uint8_t b, uint8_t g, uint8_t r, uint8_t a) {
  std::vector<uint8_t> buffer(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
  for (size_t i = 0; i < buffer.size(); i += 4) {
    buffer[i + 0] = b;
    buffer[i + 1] = g;
    buffer[i + 2] = r;
    buffer[i + 3] = a;
  }
  return buffer;
}

const VideoFrame* findFrame(const std::vector<VideoFrame>& frames, const std::string& participantId) {
  for (const auto& frame : frames) {
    if (frame.participantId == participantId) {
      return &frame;
    }
  }
  return nullptr;
}

class CountingSyntheticSource final : public IZoomCaptureSource {
 public:
  std::vector<VideoFrame> pollVideoFrames() override {
    ++videoPolls;
    return {{"synthetic-1", 1280, 720, 0}};
  }
  std::vector<corevideo::modules::AudioFrame> pollAudioFrames() override {
    ++audioPolls;
    return {{"synthetic-1", 48000, 1, 0}};
  }
  std::atomic<int> videoPolls{0};
  std::atomic<int> audioPolls{0};
};

}  // namespace

TEST(RealZoomCaptureSource, StoresAndReturnsRealPixels) {
  RealZoomCaptureSource source;
  const auto pixels = solidBgra(4, 2, 0x11, 0x22, 0x33, 0xff);
  source.ingestFrame("p-1", pixels.data(), 4, 2, /*frameId=*/7, /*timestampMs=*/123);

  EXPECT_EQ(source.participantCount(), 1u);
  const auto frames = source.pollVideoFrames();
  EXPECT_EQ(frames.size(), 1u);
  const auto* frame = findFrame(frames, "p-1");
  ASSERT_NE(frame, nullptr);
  EXPECT_TRUE(frame->hasPixels());
  EXPECT_EQ(frame->pixelWidth, 4);
  EXPECT_EQ(frame->pixelHeight, 2);
  EXPECT_EQ(frame->pixelStride, 16);
  EXPECT_EQ(frame->frameId, 7);
  // The stored bytes match the ingested BGRA buffer verbatim.
  ASSERT_NE(frame->pixels, nullptr);
  EXPECT_TRUE(*frame->pixels == pixels);
}

TEST(RealZoomCaptureSource, OverwritesLatestFramePerParticipant) {
  RealZoomCaptureSource source;
  const auto first = solidBgra(2, 2, 0x10, 0x20, 0x30, 0xff);
  const auto second = solidBgra(2, 2, 0xa0, 0xb0, 0xc0, 0xff);
  source.ingestFrame("p-1", first.data(), 2, 2, 1, 10);
  source.ingestFrame("p-1", second.data(), 2, 2, 2, 20);

  EXPECT_EQ(source.participantCount(), 1u);
  const auto frames = source.pollVideoFrames();
  EXPECT_EQ(frames.size(), 1u);
  const auto* frame = findFrame(frames, "p-1");
  ASSERT_NE(frame, nullptr);
  EXPECT_EQ(frame->frameId, 2);
  ASSERT_NE(frame->pixels, nullptr);
  EXPECT_TRUE(*frame->pixels == second);
}

TEST(RealZoomCaptureSource, KeepsDistinctParticipants) {
  RealZoomCaptureSource source;
  const auto a = solidBgra(2, 2, 0x01, 0x02, 0x03, 0xff);
  const auto b = solidBgra(2, 2, 0xf1, 0xf2, 0xf3, 0xff);
  source.ingestFrame("p-1", a.data(), 2, 2, 1, 0);
  source.ingestFrame("p-2", b.data(), 2, 2, 1, 0);

  EXPECT_EQ(source.participantCount(), 2u);
  const auto frames = source.pollVideoFrames();
  EXPECT_EQ(frames.size(), 2u);
  EXPECT_NE(findFrame(frames, "p-1"), nullptr);
  EXPECT_NE(findFrame(frames, "p-2"), nullptr);
}

TEST(RealZoomCaptureSource, FallsBackToSyntheticWhenNoRealFrames) {
  auto fallback = std::make_unique<CountingSyntheticSource>();
  auto* fallbackPtr = fallback.get();
  RealZoomCaptureSource source(std::move(fallback));

  const auto videoBefore = source.pollVideoFrames();
  EXPECT_EQ(videoBefore.size(), 1u);
  EXPECT_NE(findFrame(videoBefore, "synthetic-1"), nullptr);
  EXPECT_FALSE(findFrame(videoBefore, "synthetic-1")->hasPixels());
  EXPECT_EQ(fallbackPtr->videoPolls.load(), 1);

  // Audio always delegates to the fallback.
  const auto audio = source.pollAudioFrames();
  EXPECT_EQ(audio.size(), 1u);
  EXPECT_EQ(fallbackPtr->audioPolls.load(), 1);

  // Once a real frame is ingested, the fallback is no longer consulted for video.
  const auto pixels = solidBgra(2, 2, 0x12, 0x34, 0x56, 0xff);
  source.ingestFrame("p-1", pixels.data(), 2, 2, 1, 0);
  const auto videoAfter = source.pollVideoFrames();
  EXPECT_EQ(videoAfter.size(), 1u);
  EXPECT_NE(findFrame(videoAfter, "p-1"), nullptr);
  EXPECT_EQ(fallbackPtr->videoPolls.load(), 1);
}

TEST(RealZoomCaptureSource, IngestsBase64FrameEvents) {
  RealZoomCaptureSource source;
  const auto pixels = solidBgra(3, 2, 0x40, 0x50, 0x60, 0xff);
  const auto encoded = corevideo::modules::base64Encode(pixels.data(), pixels.size());

  std::vector<corevideo::rpc::Json> events{
      corevideo::rpc::Json::Object{
          {"type", "zoom-video-frame"},
          {"frame",
           corevideo::rpc::Json::Object{
               {"participantId", "p-9"},
               {"width", 3},
               {"height", 2},
               {"frameId", 5},
               {"observedAtMs", 42.0},
               {"bgraBase64", encoded},
           }},
      },
      // Non-frame events are ignored.
      corevideo::rpc::Json::Object{{"type", "something-else"}},
  };
  source.ingestFrameEvents(events);

  EXPECT_EQ(source.participantCount(), 1u);
  const auto frames = source.pollVideoFrames();
  const auto* frame = findFrame(frames, "p-9");
  ASSERT_NE(frame, nullptr);
  EXPECT_EQ(frame->frameId, 5);
  ASSERT_NE(frame->pixels, nullptr);
  EXPECT_TRUE(*frame->pixels == pixels);
}

TEST(RealZoomCaptureSource, IsThreadSafeUnderConcurrentIngestAndPoll) {
  RealZoomCaptureSource source;
  std::atomic<bool> stop{false};

  std::thread writer([&]() {
    const auto pixels = solidBgra(2, 2, 0x77, 0x88, 0x99, 0xff);
    for (int i = 0; i < 2000 && !stop.load(); ++i) {
      source.ingestFrame("p-1", pixels.data(), 2, 2, i, i);
      source.ingestFrame("p-2", pixels.data(), 2, 2, i, i);
    }
  });

  std::thread reader([&]() {
    for (int i = 0; i < 2000 && !stop.load(); ++i) {
      const auto frames = source.pollVideoFrames();
      for (const auto& frame : frames) {
        // Touch the shared buffer to surface any data race under sanitizers.
        if (frame.hasPixels()) {
          volatile uint8_t first = (*frame.pixels)[0];
          (void)first;
        }
      }
    }
  });

  writer.join();
  reader.join();
  stop.store(true);

  EXPECT_GE(source.participantCount(), 1u);
  const auto frames = source.pollVideoFrames();
  EXPECT_EQ(frames.size(), 2u);
}
