#include "modules/RtmpFfmpegArgs.h"

#include <gtest/gtest.h>

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
  EXPECT_NE(args.find("-re -thread_queue_size 512 -f rawvideo"), std::string::npos);
  EXPECT_NE(args.find("-re -thread_queue_size 512 -f f32le"), std::string::npos);
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
  EXPECT_NE(args.find("-f h264 -thread_queue_size 512 -i pipe:0"), std::string::npos);
  // A live raw H.264 Annex-B stream on a pipe has no container timestamps, so the
  // input needs wallclock timestamps (realtime-spaced, monotonic) plus a declared
  // frame rate, or -c:v copy muxes an unusable stream the endpoint reads as 0x.
  EXPECT_NE(args.find("-use_wallclock_as_timestamps 1 -r 60 -f h264 -thread_queue_size 512 -i pipe:0"),
            std::string::npos);
  EXPECT_NE(args.find("-c:v copy"), std::string::npos);
  // -stats makes the realtime speed readable from ffmpeg's own stderr (diagnosability).
  EXPECT_NE(args.find("-stats -stats_period 1"), std::string::npos);
  EXPECT_EQ(args.find("-f rawvideo"), std::string::npos);  // no raw video input
  EXPECT_EQ(args.find("-b:v "), std::string::npos);          // no re-encode bitrate
  EXPECT_NE(args.find("-c:a aac"), std::string::npos);       // audio still encoded
  EXPECT_NE(args.find("-map 0:v:0 -map 1:a:0"), std::string::npos);
}

TEST(RtmpFfmpegArgs, BitstreamRtmpDisablesTcpDelayWithoutChangingSrt) {
  auto config = baseConfig();
  config.videoBitstreamInput = true;
  for (const auto* endpoint : {"rtmp://live.example/app/test", "rtmps://live.example/app/test"}) {
    config.endpoint = endpoint;
    const auto args = buildRtmpFfmpegArguments(config);
    EXPECT_NE(args.find(" -tcp_nodelay 1 -f flv "), std::string::npos);
    EXPECT_NE(args.find(" -c:v copy"), std::string::npos);
  }
  config.endpoint = "srt://127.0.0.1:9021?mode=caller";
  config.container = "mpegts";
  const auto args = buildRtmpFfmpegArguments(config);
  EXPECT_EQ(args.find("tcp_nodelay"), std::string::npos);
  EXPECT_NE(args.find(" -f mpegts "), std::string::npos);
}
