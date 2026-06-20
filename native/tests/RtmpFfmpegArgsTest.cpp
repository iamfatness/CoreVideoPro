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
}

TEST(RtmpFfmpegArgs, HonorsAudioChannelsAndSampleRate) {
  auto config = baseConfig();
  config.hasAudio = true;
  config.audioChannels = 1;
  config.audioSampleRate = 44100;
  const auto args = buildRtmpFfmpegArguments(config);
  EXPECT_NE(args.find("-ar 44100 -ac 1"), std::string::npos);
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
