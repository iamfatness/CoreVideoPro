#include "modules/EncodedVideoTransportStream.h"
#include "modules/ProgramStreamClock.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <map>
#include <vector>

// #538 Slice 8: one Program stream clock at the mux boundary. Video PTS is the
// compositor frame number; AAC PTS is the shared encoder's sample count from a
// session anchor; both leave the core in ONE TS so FFmpeg never assigns time.

namespace {
using namespace corevideo::modules;

constexpr int kFps = 60;
constexpr int64_t kFrameTicks = 90000 / kFps;  // 1500

std::vector<uint8_t> fakeAdts() {
  // 7-byte ADTS header (AAC-LC, 48 kHz, stereo) + payload. Content is
  // irrelevant here: a muted block is just a different payload at the same time.
  return {0xff, 0xf1, 0x4c, 0x80, 0x02, 0x1f, 0xfc, 0x21, 0x10, 0x04};
}

struct PesTimes {
  std::vector<int64_t> video;
  std::vector<int64_t> audio;
};

int64_t readPts(const uint8_t* p) {
  return (int64_t((p[0] >> 1) & 7) << 30) | (int64_t(p[1]) << 22) |
         (int64_t(p[2] >> 1) << 15) | (int64_t(p[3]) << 7) | (p[4] >> 1);
}

// Walk 188-byte packets and collect the PTS of every PES start per PID.
void collect(const std::vector<uint8_t>& wire, PesTimes& times) {
  ASSERT_EQ(wire.size() % 188, size_t{0});
  for (size_t i = 0; i < wire.size(); i += 188) {
    ASSERT_EQ(wire[i], 0x47);
    const bool start = wire[i + 1] & 0x40;
    const int pid = ((wire[i + 1] & 0x1f) << 8) | wire[i + 2];
    if (!start || (pid != 0x100 && pid != 0x101)) continue;
    const size_t payload = i + 4 + ((wire[i + 3] & 0x20) ? 1 + wire[i + 4] : 0);
    ASSERT_EQ(wire[payload + 2], 1);
    (pid == 0x100 ? times.video : times.audio).push_back(readPts(&wire[payload + 9]));
  }
}

GpuEncodedChunk videoChunk(std::vector<uint8_t>& payload, int64_t frame, bool keyframe) {
  GpuEncodedChunk chunk;
  chunk.data = payload.data();
  chunk.size = payload.size();
  chunk.timingValid = true;
  chunk.keyframe = keyframe;
  chunk.frameNumber = frame;
  // Exactly MediaFoundationGpuVideoEncoder::processInput's stamp.
  chunk.pts100ns = chunk.dts100ns = frame * 10'000'000LL / kFps;
  return chunk;
}
}  // namespace

TEST(ProgramStreamClock, AnchorsOncePerSessionAndStampsBySampleCount) {
  ProgramStreamClock clock;
  ProgramAacPacket packet;
  packet.sampleIndex = 4096;
  clock.beginAudioBlock(0, 0);
  clock.stamp(packet);
  EXPECT_LT(packet.anchorFrameNumber, 0) << "no Program frame yet: no invented time";

  clock.observeProgramFrame(600, 0, true);
  clock.beginAudioBlock(2048, 0);
  clock.stamp(packet);
  ASSERT_EQ(packet.anchorFrameNumber, 600);
  ASSERT_EQ(packet.anchorSampleIndex, 2048);
  int64_t pts = 0;
  ASSERT_TRUE(programAudioPts100ns(packet, kFps, pts));
  EXPECT_EQ(pts, 100'000'000LL + 2048LL * 10'000'000 / 48'000);

  // Later video ticks, bursts, reconnects: the anchor never moves mid-session.
  clock.observeProgramFrame(9000, 0, true);
  clock.beginAudioBlock(999'999, 0);
  clock.stamp(packet);
  EXPECT_EQ(packet.anchorFrameNumber, 600);
  EXPECT_EQ(packet.anchorSampleIndex, 2048);

  // Only the end of the whole stream session re-anchors.
  clock.observeProgramFrame(0, 0, false);
  clock.observeProgramFrame(12000, 0, true);
  clock.beginAudioBlock(1'000'000, 0);
  clock.stamp(packet);
  EXPECT_EQ(packet.anchorFrameNumber, 12000);
  EXPECT_EQ(packet.anchorSampleIndex, 1'000'000);
}

// The anchor is sub-frame: the audio block's steady-clock time minus the
// latest frame's timeline time is carried, so the session relation does not
// depend on which frame happened to be latest when audio first arrived.
TEST(ProgramStreamClock, AnchorCarriesTheSubFrameTimelineOffset) {
  for (const int64_t audioLate100ns : {int64_t{0}, int64_t{41'000}, int64_t{166'000}}) {
    ProgramStreamClock clock;
    // Frame 600 was delivered at timeline 5 s; the first PCM block's timeline
    // is 4.1 ms / 16.6 ms later, still before the next frame is observed.
    clock.observeProgramFrame(600, 50'000'000, true);
    clock.beginAudioBlock(0, 50'000'000 + audioLate100ns);
    ProgramAacPacket packet;
    packet.sampleIndex = 48'000;  // one second of samples later
    clock.stamp(packet);
    int64_t pts = 0;
    ASSERT_TRUE(programAudioPts100ns(packet, kFps, pts));
    EXPECT_EQ(pts, 600LL * 10'000'000 / kFps + audioLate100ns + 10'000'000);
  }
  // Missing timeline evidence falls back to the frame boundary, never garbage.
  ProgramStreamClock clock;
  clock.observeProgramFrame(600, 0, true);
  clock.beginAudioBlock(0, 50'000'000);
  ProgramAacPacket packet;
  clock.stamp(packet);
  EXPECT_EQ(packet.anchorOffset100ns, 0);
}

TEST(EncodedVideoTransportStream, AudioPidSharesTheVideoEpochAndDeclaresAdtsAac) {
  EncodedVideoTransportStream stream(EncodedVideoTransportStream::Codec::H264, true);
  const auto adts = fakeAdts();
  std::vector<uint8_t> wire;
  EXPECT_FALSE(stream.packetizeAudio(adts.data(), adts.size(), 100'000'000, wire))
      << "no epoch yet: audio cannot open the stream";
  stream.begin(100'000'000, wire);
  ASSERT_EQ(wire.size(), size_t{2 * 188});
  EXPECT_EQ(wire[188 + 5 + 2], 23) << "PMT section carries two elementary streams";
  EXPECT_EQ(wire[188 + 5 + 17], 0x0f) << "second stream_type: ADTS AAC";
  EXPECT_FALSE(stream.packetizeAudio(adts.data(), adts.size(), 99'999'999, wire)) << "before epoch";
  ASSERT_TRUE(stream.packetizeAudio(adts.data(), adts.size(), 100'213'333, wire));
  PesTimes times;
  collect(wire, times);
  ASSERT_EQ(times.audio.size(), size_t{1});
  EXPECT_EQ(times.audio[0], 1920);
  EXPECT_FALSE(stream.packetizeAudio(adts.data(), adts.size(), 100'213'333, wire)) << "not monotonic";
}

// Acceptance 7: the single TS FFmpeg reads carries monotonically increasing
// PTS on both PIDs from the core's session clock, and neither a mute gap nor a
// video dropout produces a jump FFmpeg could treat as wall-time catch-up.
TEST(ProgramStreamClock, UnifiedTransportStaysMonotonicThroughMuteAndVideoDropout) {
  ProgramStreamClock clock;
  EncodedVideoTransportStream stream(EncodedVideoTransportStream::Codec::H264, true);
  std::vector<uint8_t> idr{0, 0, 0, 1, 0x65, 0x88, 0x84, 0x21};
  std::vector<uint8_t> inter{0, 0, 0, 1, 0x41, 0x9a, 0x02};
  const auto adts = fakeAdts();
  std::vector<uint8_t> wire;
  PesTimes times;

  int64_t acceptedSamples = 0;
  int64_t nextPacketSample = 0;
  bool begun = false;
  // 20 s of Program at 60 fps; the audio worker delivers a 960-sample block
  // every 20 ms. Frames 400..459 never reach the encoder (a one-second source
  // video dropout / encoder skip). Blocks 300..399 are an explicit mute: the
  // core feeds exact silent samples, so the sample count simply continues.
  for (int64_t frame = 1; frame <= 20 * kFps; ++frame) {
    clock.observeProgramFrame(frame, 0, true);
    const bool dropped = frame >= 400 && frame < 460;
    if (!dropped) {
      auto chunk = videoChunk(frame % 120 == 1 ? idr : inter, frame, frame % 120 == 1);
      if (!begun) {
        stream.begin(chunk.dts100ns, wire);
        collect(wire, times);
        begun = true;
      }
      ASSERT_TRUE(stream.packetize(chunk, wire));
      collect(wire, times);
    }
    // Audio blocks land on a different (50 Hz) cadence from video.
    while (acceptedSamples * kFps < frame * 48'000LL) {
      clock.beginAudioBlock(acceptedSamples, 0);
      acceptedSamples += 960;  // muted or not: 960 exact samples per block
      for (; nextPacketSample + 1024 <= acceptedSamples; nextPacketSample += 1024) {
        ProgramAacPacket packet;
        packet.adts = adts;
        packet.sampleIndex = nextPacketSample;
        clock.stamp(packet);
        int64_t pts = 0;
        ASSERT_TRUE(programAudioPts100ns(packet, kFps, pts));
        if (!stream.packetizeAudio(packet.adts.data(), packet.adts.size(), pts, wire)) continue;
        collect(wire, times);
      }
    }
  }

  ASSERT_GT(times.video.size(), size_t{1000});
  ASSERT_GT(times.audio.size(), size_t{900});
  std::map<int64_t, int> videoSteps;
  for (size_t i = 1; i < times.video.size(); ++i) {
    ASSERT_GT(times.video[i], times.video[i - 1]);
    ++videoSteps[times.video[i] - times.video[i - 1]];
  }
  // Every step is one frame except the dropout, which preserves the exact
  // missing duration (61 frame slots) instead of collapsing or catching up.
  ASSERT_EQ(videoSteps.size(), size_t{2});
  EXPECT_EQ(videoSteps.begin()->first, kFrameTicks);
  EXPECT_EQ(videoSteps.rbegin()->first, 61 * kFrameTicks);
  EXPECT_EQ(videoSteps.rbegin()->second, 1);
  for (size_t i = 1; i < times.audio.size(); ++i) {
    const int64_t step = times.audio[i] - times.audio[i - 1];
    // 1024 samples = 1920 ticks at 90 kHz; ±1 is 100 ns → 90 kHz rounding.
    ASSERT_LE(std::llabs(step - 1920), 1) << "audio packet " << i << " jumped across mute/dropout";
  }
  // One clock: the session anchored sample 0 to frame 1 (TS time 0), so the
  // packet starting at 8 s of samples (index 375 x 1024 = 384000) sits exactly
  // on frame 481's time, across the mute - and 16 s likewise, after the dropout.
  EXPECT_EQ(times.audio.front(), 0);
  EXPECT_LE(std::llabs(times.audio.at(375) - 480 * kFrameTicks), 1);
  EXPECT_LE(std::llabs(times.audio.at(750) - 960 * kFrameTicks), 1);
}

// A destination reconnect is a new FFmpeg process with a new TS epoch; the
// session clock underneath is untouched, so the A/V relation is identical.
TEST(ProgramStreamClock, ReconnectKeepsTheSessionRelationBetweenAudioAndVideo) {
  ProgramStreamClock clock;
  clock.observeProgramFrame(300, 0, true);
  clock.beginAudioBlock(0, 0);
  const auto adts = fakeAdts();
  std::vector<uint8_t> idr{0, 0, 0, 1, 0x65, 0x88, 0x84, 0x21};

  auto relation = [&](int64_t firstFrame) {
    EncodedVideoTransportStream stream(EncodedVideoTransportStream::Codec::H264, true);
    std::vector<uint8_t> wire;
    PesTimes times;
    auto chunk = videoChunk(idr, firstFrame, true);
    stream.begin(chunk.dts100ns, wire);
    // Audio for the same Program instant as the IDR: (firstFrame - 300) frames
    // after the anchor, i.e. that many 800-sample frame slots.
    ProgramAacPacket packet;
    packet.adts = adts;
    packet.sampleIndex = (firstFrame - 300) * 800;
    clock.stamp(packet);
    int64_t pts = 0;
    EXPECT_TRUE(programAudioPts100ns(packet, kFps, pts));
    EXPECT_TRUE(stream.packetizeAudio(packet.adts.data(), packet.adts.size(), pts, wire));
    collect(wire, times);
    EXPECT_TRUE(stream.packetize(chunk, wire));
    collect(wire, times);
    return times.audio.at(0) - times.video.at(0);
  };
  EXPECT_EQ(relation(900), 0);
  EXPECT_EQ(relation(54'000), 0) << "15 minutes later, a fresh process: same relation";
}
