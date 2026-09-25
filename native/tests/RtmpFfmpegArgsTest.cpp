#include "modules/RtmpFfmpegArgs.h"
#include "modules/HevcTransportStream.h"

#include <gtest/gtest.h>

#include <utility>

namespace {
using namespace corevideo::modules;

RtmpFfmpegArgsConfig baseConfig() {
  RtmpFfmpegArgsConfig config;
  config.width = 1920;
  config.height = 1080;
  config.fps = 30;
  config.bitrateKbps = 6000;
  config.videoEncoder = "libx264";
  config.videoEncoderExtraArgs = " -preset veryfast -tune zerolatency";
  config.endpoint = "rtmp://ingest.example/live/streamkey";
  return config;
}
}  // namespace

TEST(RtmpFfmpegArgs, NoAudioEmitsAnullsrcSilenceSource) {
  const auto args = buildRtmpFfmpegArguments(baseConfig());
  EXPECT_NE(args.find("anullsrc"), std::string::npos);
  EXPECT_NE(args.find("-c:v libx264"), std::string::npos);
  EXPECT_NE(args.find("-c:a aac"), std::string::npos);
  EXPECT_NE(args.find("-f flv"), std::string::npos);
  EXPECT_NE(args.find("rtmp://ingest.example/live/streamkey"), std::string::npos);
}

TEST(RtmpFfmpegArgs, RealAudioReplacesAnullsrcWithPcmInput) {
  auto config = baseConfig();
  config.hasAudio = true;
  config.audioChannels = 2;
  config.audioSampleRate = 48000;
  config.audioSampleFormat = "f32le";
  config.audioInput = "pipe:3";
  const auto args = buildRtmpFfmpegArguments(config);

  // The silence source must be gone, replaced by a real raw-PCM second input.
  EXPECT_EQ(args.find("anullsrc"), std::string::npos);
  EXPECT_NE(args.find("-f f32le -ar 48000 -ac 2 -i pipe:3"), std::string::npos);
  // A/V are explicitly mapped and the audio is encoded to AAC.
  EXPECT_NE(args.find("-map 0:v:0 -map 1:a:0"), std::string::npos);
  EXPECT_NE(args.find("-c:a aac"), std::string::npos);
  // The VIDEO pipe is read greedily (NO -re) so it drops to live under RTMP
  // backpressure instead of falling progressively behind wallclock (owner
  // incident 2026-09-13: 124s of accumulated lag on a YouTube stream). The AUDIO
  // pipe KEEPS -re to preserve A/V startup ordering.
  EXPECT_NE(args.find("-thread_queue_size 512 -f rawvideo"), std::string::npos);
  EXPECT_EQ(args.find("-re -thread_queue_size 512 -f rawvideo"), std::string::npos);
  EXPECT_NE(args.find("-re -thread_queue_size 512 -f f32le"), std::string::npos);
}

// Regression for the live-lag incident: the video input must never be paced with
// -re (which turned RTMP backpressure into unbounded lag), while both audio paths
// must keep it (which prevents the audio-races-ahead / no-video connection close).
TEST(RtmpFfmpegArgs, VideoPipeIsGreedyWhileAudioIsPaced) {
  corevideo::modules::RtmpFfmpegArgsConfig config;
  config.hasAudio = true;
  config.audioInput = "pipe:3";
  const auto real = corevideo::modules::buildRtmpFfmpegArguments(config);
  // video: no -re immediately before the rawvideo input
  EXPECT_EQ(real.find("-re -thread_queue_size 512 -f rawvideo"), std::string::npos);
  // real audio: -re present
  EXPECT_NE(real.find("-re -thread_queue_size 512 -f f32le"), std::string::npos);

  config.hasAudio = false;
  const auto silent = corevideo::modules::buildRtmpFfmpegArguments(config);
  // the silent fallback still needs -re (lavfi is not realtime)
  EXPECT_NE(silent.find("-re -f lavfi -i anullsrc"), std::string::npos);
  EXPECT_EQ(silent.find("-re -thread_queue_size 512 -f rawvideo"), std::string::npos);
}

TEST(RtmpFfmpegArgs, HonorsAudioChannelsAndSampleRate) {
  auto config = baseConfig();
  config.hasAudio = true;
  config.audioChannels = 1;
  config.audioSampleRate = 44100;
  config.audioBitrateKbps = 192;
  const auto args = buildRtmpFfmpegArguments(config);
  EXPECT_NE(args.find("-ar 44100 -ac 1"), std::string::npos);
  EXPECT_NE(args.find("-c:a aac -b:a 192k -ar 48000"), std::string::npos);
}

TEST(RtmpFfmpegArgs, PosixNamedPipeOrFdInputIsPlumbedThrough) {
  auto config = baseConfig();
  config.hasAudio = true;
  config.audioInput = "/tmp/corevideo-audio.fifo";
  const auto args = buildRtmpFfmpegArguments(config);
  EXPECT_NE(args.find("-i /tmp/corevideo-audio.fifo"), std::string::npos);
}

TEST(RtmpFfmpegArgs, VideoEncoderAndBitrateAreReflected) {
  auto config = baseConfig();
  config.videoEncoder = "h264_nvenc";
  config.bitrateKbps = 9000;
  const auto args = buildRtmpFfmpegArguments(config);
  EXPECT_NE(args.find("-c:v h264_nvenc"), std::string::npos);
  EXPECT_NE(args.find("-b:v 9000k -maxrate 9000k -bufsize 18000k"), std::string::npos);
}

TEST(RtmpFfmpegArgs, FullProgramNv12InputIsDeclaredWithoutBgraReinterpretation) {
  auto config = baseConfig();
  config.videoInputPixelFormat = "nv12";
  const auto args = buildRtmpFfmpegArguments(config);
  EXPECT_NE(args.find("-f rawvideo -pix_fmt nv12 -s 1920x1080"), std::string::npos);
  EXPECT_EQ(args.find("-pix_fmt bgra"), std::string::npos);
}

TEST(RtmpFfmpegArgs, AdvancedPlatformSettingsReachEncoderArguments) {
  auto config = baseConfig();
  config.fps = 60;
  config.keyframeIntervalSeconds = 1.5;
  config.h264Profile = "baseline";
  config.bFrames = 0;
  const auto args = buildRtmpFfmpegArguments(config);

  EXPECT_NE(args.find("-g 90"), std::string::npos);
  EXPECT_NE(args.find("-profile:v baseline"), std::string::npos);
  EXPECT_NE(args.find("-bf 0"), std::string::npos);
}

TEST(RtmpFfmpegArgs, VbrAllowsHeadroomAboveTargetBitrate) {
  auto config = baseConfig();
  config.bitrateKbps = 6000;
  config.rateControl = "vbr";
  const auto args = buildRtmpFfmpegArguments(config);

  EXPECT_NE(args.find("-b:v 6000k -maxrate 9000k -bufsize 12000k"), std::string::npos);
}

TEST(RtmpVideoFramePacer, FiftyHertzProducerYieldsThirtyVideoWritesPerSecond) {
  RtmpVideoFramePacer pacer;
  int accepted = 0;
  for (int elapsedMs = 0; elapsedMs < 1000; elapsedMs += 20) {
    accepted += pacer.shouldWrite(static_cast<double>(elapsedMs), 30) ? 1 : 0;
  }

  EXPECT_EQ(accepted, 30);
}

TEST(RtmpVideoFramePacer, ResetMakesNextFrameImmediatelyEligible) {
  RtmpVideoFramePacer pacer;
  EXPECT_TRUE(pacer.shouldWrite(1000.0, 30));
  EXPECT_FALSE(pacer.shouldWrite(1010.0, 30));
  pacer.reset();
  EXPECT_TRUE(pacer.shouldWrite(1010.0, 30));
}

// #521 slice 1: GPU-direct encode hands the muxer a compressed H.264 bitstream,
// so the video input is copied (-c:v copy), not re-encoded from raw. The 186 MB/s
// raw-video pipe is gone; audio is still encoded to AAC as before.
TEST(RtmpFfmpegArgs, BitstreamInputModeCopiesVideoAndSkipsRawEncode) {
  corevideo::modules::RtmpFfmpegArgsConfig config;
  config.videoBitstreamInput = true;
  config.fps = 60;
  config.hasAudio = true;
  config.audioInput = "pipe:3";
  const auto args = corevideo::modules::buildRtmpFfmpegArguments(config);
  EXPECT_NE(args.find("-f h264 -probesize 65536 -analyzeduration 1 -thread_queue_size 512 -i pipe:0"), std::string::npos);
  // A live raw H.264 Annex-B stream on a pipe has no container timestamps, so the
  // input needs wallclock timestamps (realtime-spaced, monotonic) plus a declared
  // frame rate, or -c:v copy muxes an unusable stream the endpoint reads as 0x.
  EXPECT_NE(args.find("-use_wallclock_as_timestamps 1 -r 60 -f h264 -probesize 65536 -analyzeduration 1 -thread_queue_size 512 -i pipe:0"),
            std::string::npos);
  EXPECT_NE(args.find("-c:v copy"), std::string::npos);
  // -stats makes the realtime speed readable from ffmpeg's own stderr (diagnosability).
  EXPECT_NE(args.find("-stats -stats_period 1"), std::string::npos);
  EXPECT_EQ(args.find("-f rawvideo"), std::string::npos);  // no raw video input
  EXPECT_EQ(args.find("-b:v "), std::string::npos);          // no re-encode bitrate
  EXPECT_NE(args.find("-c:a aac"), std::string::npos);       // audio still encoded
  EXPECT_NE(args.find("-map 0:v:0 -map 1:a:0"), std::string::npos);
  // Apply the PCM probe limit to the second input, not to the video demuxer.
  EXPECT_NE(args.find("-probesize 32 -analyzeduration 1 -f f32le -ar 48000 -ac 2 -i pipe:3"),
            std::string::npos);
}

// 2026-09-20: GPU-direct HEVC/AV1. The raw elementary stream on pipe:0 needs the
// matching raw demuxer; -c:v copy into FLV is unchanged and this FFmpeg writes
// the enhanced-RTMP fourcc itself (no -tag:v).
TEST(RtmpFfmpegArgs, BitstreamInputModeNamesTheRawDemuxerPerCodec) {
  using corevideo::modules::rawDemuxerForBitstreamCodec;
  EXPECT_EQ(rawDemuxerForBitstreamCodec("h264"), "h264");
  EXPECT_EQ(rawDemuxerForBitstreamCodec("hevc"), "hevc");
  EXPECT_EQ(rawDemuxerForBitstreamCodec("av1"), "obu");
  EXPECT_EQ(rawDemuxerForBitstreamCodec("bogus"), "h264");

  for (const auto& [codec, demuxer] : {std::pair{"hevc", "hevc"}, std::pair{"av1", "obu"}}) {
    corevideo::modules::RtmpFfmpegArgsConfig config;
    config.videoBitstreamInput = true;
    config.videoBitstreamCodec = codec;
    config.fps = 60;
    config.hasAudio = true;
    config.audioInput = "pipe:3";
    const auto args = corevideo::modules::buildRtmpFfmpegArguments(config);
    const std::string clock = "-use_wallclock_as_timestamps 1 -r 60 -f ";
    EXPECT_NE(args.find(clock + demuxer +
                        " -probesize 65536 -analyzeduration 1 -thread_queue_size 512 -i pipe:0"),
              std::string::npos) << codec << " :: " << args;
    EXPECT_NE(args.find("-c:v copy"), std::string::npos) << codec;
    EXPECT_EQ(args.find("-tag:v"), std::string::npos) << codec;
    EXPECT_EQ(args.find("-f h264 "), std::string::npos) << codec;
    if (std::string(codec) == "hevc") {
      EXPECT_NE(args.find("-bsf:v setts=ts=N/(60*TB)"), std::string::npos);
    }
  }
}

TEST(RtmpFfmpegArgs, BitstreamRtmpCoalescesSmallWritesWithoutChangingSrt) {
  auto config = baseConfig();
  config.videoBitstreamInput = true;
  for (const auto* endpoint : {"rtmp://live.example/app/test", "rtmps://live.example/app/test"}) {
    config.endpoint = endpoint;
    const auto args = buildRtmpFfmpegArguments(config);
    EXPECT_NE(args.find(" -tcp_nodelay 0 -f flv "), std::string::npos);
    EXPECT_NE(args.find(" -c:v copy"), std::string::npos);
  }
  config.endpoint = "srt://127.0.0.1:9021?mode=caller";
  config.container = "mpegts";
  const auto args = buildRtmpFfmpegArguments(config);
  EXPECT_EQ(args.find("tcp_nodelay"), std::string::npos);
  EXPECT_NE(args.find(" -f mpegts "), std::string::npos);
}

TEST(RtmpFfmpegArgs, LiveBitstreamAudioDoesNotAddASecondPacingClock) {
  auto config = baseConfig();
  config.videoBitstreamInput = true;
  for (const auto* codec : {"h264", "hevc", "av1"}) {
    config.videoBitstreamCodec = codec;
    config.timestampedHevcInput = std::string(codec) == "hevc";
    config.hasAudio = true;
    config.audioInput = "pipe:3";
    const auto live = buildRtmpFfmpegArguments(config);
    EXPECT_EQ(live.find(" -re "), std::string::npos);
    EXPECT_NE(live.find(" -i pipe:3"), std::string::npos);
    EXPECT_NE(live.find(" -af aresample=async=1:first_pts=0"), std::string::npos);
    config.hasAudio = false;
    const auto silence = buildRtmpFfmpegArguments(config);
    EXPECT_NE(silence.find(" -re -f lavfi -i anullsrc="), std::string::npos);
  }
}

TEST(RtmpFfmpegArgs, TimestampedHevcUsesContainerClockWithoutRewritingTimestamps) {
  auto config = baseConfig();
  config.videoBitstreamInput = true;
  config.videoBitstreamCodec = "hevc";
  config.timestampedHevcInput = true;
  const auto args = buildRtmpFfmpegArguments(config);
  EXPECT_NE(args.find("-f mpegts -probesize"), std::string::npos);
  EXPECT_EQ(args.find("use_wallclock_as_timestamps"), std::string::npos);
  EXPECT_EQ(args.find("setts="), std::string::npos);
  EXPECT_NE(args.find("-c:v copy"), std::string::npos);
}

namespace {
std::vector<uint8_t> videoPes(const std::vector<uint8_t>& wire) {
  std::vector<uint8_t> result;
  for (size_t i = 0; i < wire.size(); i += 188) {
    if ((wire[i + 1] & 0x1f) != 1 || wire[i + 2] != 0) continue;
    const size_t start = i + 4 + ((wire[i + 3] & 0x20) ? 1 + wire[i + 4] : 0);
    result.insert(result.end(), wire.begin() + start, wire.begin() + i + 188);
  }
  return result;
}
uint64_t pesPts(const std::vector<uint8_t>& pes, size_t start = 9) {
  return (uint64_t((pes[start] >> 1) & 7) << 30) |
         (uint64_t(pes[start + 1]) << 22) |
         (uint64_t(pes[start + 2] >> 1) << 15) |
         (uint64_t(pes[start + 3]) << 7) | (pes[start + 4] >> 1);
}
}

TEST(HevcTransportStream, PreservesGapsAndReorderingAcrossBurstDelivery) {
  HevcTransportStream stream;
  std::vector<uint8_t> payload{0, 0, 0, 1, 0x46, 1, 0x50};
  GpuEncodedChunk chunk;
  chunk.data = payload.data(); chunk.size = payload.size(); chunk.timingValid = true;
  std::vector<uint8_t> wire;
  chunk.pts100ns = chunk.dts100ns = 100000000;
  ASSERT_TRUE(stream.packetize(chunk, wire));
  EXPECT_EQ(pesPts(videoPes(wire)), uint64_t{0});
  chunk.pts100ns += 1000000; // six frame slots elapsed, not one callback
  chunk.dts100ns += 500000;
  ASSERT_TRUE(stream.packetize(chunk, wire));
  const auto pes = videoPes(wire);
  EXPECT_EQ(pesPts(pes), uint64_t{9000});
  EXPECT_EQ(pesPts(pes, 14), uint64_t{4500});
  EXPECT_FALSE(stream.packetize(chunk, wire)); // duplicate DTS is a defect, not a new frame
  EXPECT_TRUE(wire.empty());
}

TEST(HevcTransportStream, PacketBoundariesPreservePayloadBytes) {
  for (size_t size : {size_t{7}, size_t{161}, size_t{162}, size_t{163}, size_t{184}, size_t{65537}}) {
    HevcTransportStream stream;
    std::vector<uint8_t> payload(size, 0x55);
    const uint8_t aud[] = {0, 0, 0, 1, 0x46, 1, 0x50};
    std::copy(std::begin(aud), std::end(aud), payload.begin());
    GpuEncodedChunk chunk;
    chunk.data = payload.data(); chunk.size = size; chunk.timingValid = true;
    std::vector<uint8_t> wire;
    ASSERT_TRUE(stream.packetize(chunk, wire));
    EXPECT_EQ(wire.size() % 188, size_t{0});
    const auto pes = videoPes(wire);
    ASSERT_EQ(pes.size(), payload.size() + 14);
    EXPECT_TRUE(std::equal(payload.begin(), payload.end(), pes.begin() + 14));
  }
}

TEST(HevcTransportStream, MissingTimingDoesNotInventAClock) {
  HevcTransportStream stream;
  uint8_t payload[] = {0, 0, 1, 0x26, 1, 0};
  GpuEncodedChunk chunk;
  chunk.data = payload; chunk.size = sizeof(payload);
  std::vector<uint8_t> wire;
  EXPECT_FALSE(stream.packetize(chunk, wire));
  chunk.timingValid = true;
  ASSERT_TRUE(stream.packetize(chunk, wire));
  EXPECT_EQ(pesPts(videoPes(wire)), uint64_t{0});
}
