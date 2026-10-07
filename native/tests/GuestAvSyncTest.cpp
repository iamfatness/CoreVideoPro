#include "core/GuestAvSync.h"
#include <gtest/gtest.h>

using corevideo::core::GuestAvSyncAudio;
using corevideo::core::GuestAvSyncVideo;
using corevideo::modules::AudioFrame;
using corevideo::modules::VideoFrame;

namespace {
AudioFrame tone(const char* id, float first = 1.f) {
  AudioFrame frame;
  frame.participantId = id;
  frame.sampleRate = 1000;
  frame.channels = 1;
  frame.sampleCount = 20;
  frame.pcm.assign(20, 0.f);
  frame.pcm[0] = first;
  return frame;
}
VideoFrame picture(const char* id, int64_t frameId) {
  VideoFrame frame;
  frame.participantId = id;
  frame.frameId = frameId;
  frame.pixelWidth = 2;
  frame.pixelHeight = 2;
  frame.pixelStride = 8;
  frame.pixels = std::make_shared<const std::vector<uint8_t>>(16, 128);
  return frame;
}
}

TEST(GuestAvSync, PositiveOffsetDelaysOnlySelectedGuestAudioByExactSamples) {
  GuestAvSyncAudio sync;
  sync.observeEpoch(1);
  std::vector<AudioFrame> first{tone("101"), tone("202")};
  sync.apply(first, {{"101", 10}});
  ASSERT_EQ(first.size(), 2u);
  EXPECT_EQ(first[0].pcm[0], 0.f);
  EXPECT_EQ(first[0].pcm[10], 1.f);
  EXPECT_EQ(first[1].pcm[0], 1.f);
  std::vector<AudioFrame> second{tone("202", 0.f)};
  sync.apply(second, {{"101", 10}});
  ASSERT_EQ(second.size(), 2u);
  EXPECT_EQ(second.back().participantId, "101");
  EXPECT_EQ(second.back().pcm.size(), 20u);
}

TEST(GuestAvSync, ZeroAndNegativeOffsetsLeaveGuestPcmUntouched) {
  GuestAvSyncAudio sync;
  std::vector<AudioFrame> frames{tone("101"), tone("202")};
  sync.apply(frames, {{"101", 0}, {"202", -200}});
  ASSERT_EQ(frames.size(), 2u);
  EXPECT_EQ(frames[0].pcm[0], 1.f);
  EXPECT_EQ(frames[1].pcm[0], 1.f);
}

TEST(GuestAvSync, DelayedSpeechDrainsWhenZoomStopsSendingPackets) {
  GuestAvSyncAudio sync;
  sync.observeEpoch(1);
  std::vector<AudioFrame> first{tone("101")};
  sync.apply(first, {{"101", 20}});
  EXPECT_EQ(first[0].pcm[0], 0.f);
  std::vector<AudioFrame> second;
  sync.apply(second, {{"101", 20}});
  ASSERT_EQ(second.size(), 1u);
  EXPECT_EQ(second[0].pcm[0], 1.f);
  EXPECT_EQ(second[0].sampleCount, 20);
}

TEST(GuestAvSync, NegativeOffsetDelaysOnlySelectedGuestVideo) {
  GuestAvSyncVideo sync;
  sync.observeEpoch(1);
  std::vector<VideoFrame> first{picture("101", 1), picture("202", 1)};
  sync.apply(first, {{"101", -200}}, 1000);
  std::vector<VideoFrame> next{picture("101", 2), picture("202", 2)};
  sync.apply(next, {{"101", -200}}, 1050);
  EXPECT_EQ(next[0].frameId, 1);
  EXPECT_EQ(next[1].frameId, 2);
  std::vector<VideoFrame> matured{picture("101", 3)};
  sync.apply(matured, {{"101", -200}}, 1250);
  EXPECT_EQ(matured[0].frameId, 2);
}

TEST(GuestAvSync, EpochAndOffsetChangesDiscardOldMedia) {
  GuestAvSyncAudio audio;
  audio.observeEpoch(1);
  std::vector<AudioFrame> old{tone("101")};
  audio.apply(old, {{"101", 200}});
  audio.observeEpoch(2);
  std::vector<AudioFrame> fresh{tone("101", .5f)};
  audio.apply(fresh, {{"101", 10}});
  EXPECT_EQ(fresh[0].pcm[10], .5f);

  GuestAvSyncVideo video;
  video.observeEpoch(1);
  std::vector<VideoFrame> oldPicture{picture("101", 1)};
  video.apply(oldPicture, {{"101", -200}}, 1000);
  video.observeEpoch(2);
  std::vector<VideoFrame> newPicture{picture("101", 99)};
  video.apply(newPicture, {{"101", -200}}, 1001);
  EXPECT_EQ(newPicture[0].frameId, 99);
}

TEST(GuestAvSync, BoundsAndSignsAreExplicit) {
  EXPECT_EQ(corevideo::core::clampGuestAvOffsetMs(-300), -200);
  EXPECT_EQ(corevideo::core::clampGuestAvOffsetMs(300), 200);
  EXPECT_EQ(corevideo::core::clampGuestAvOffsetMs(0), 0);
  EXPECT_EQ(corevideo::core::clampGuestAvOffsetMs(-30), -30);
}

TEST(GuestAvSync, SourceReconnectWithReusedFrameIdDiscardsOldTrimmedPictures) {
  GuestAvSyncVideo sync;
  auto first = picture("101", 1); first.sourceEpoch = 5;
  auto queued = picture("101", 2); queued.sourceEpoch = 5;
  std::vector<VideoFrame> frames{first}; sync.apply(frames, {{"101", -100}}, 0);
  frames = {queued}; sync.apply(frames, {{"101", -100}}, 20);
  EXPECT_EQ(frames.front().frameId, 1);
  auto reconnected = picture("101", 2); reconnected.sourceEpoch = 6;
  frames = {reconnected}; sync.apply(frames, {{"101", -100}}, 40);
  EXPECT_EQ(frames.front().sourceEpoch, 6u);
  EXPECT_EQ(frames.front().frameId, 2);
  reconnected.frameId = 3;
  frames = {reconnected}; sync.apply(frames, {{"101", -100}}, 150);
  EXPECT_EQ(frames.front().sourceEpoch, 6u);
  EXPECT_EQ(frames.front().frameId, 2);
}
