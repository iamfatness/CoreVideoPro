#pragma once

// Pure FFmpeg command-line builder for the RTMP program sender.
//
// Kept free of FFmpeg/dev-gate dependencies so the argument layout is
// unit-testable in the default stub build. The adapter feeds the program video
// over pipe:0 (rawvideo BGRA) and, when real program audio is available, the F2
// program-audio PCM tap over a second input (pipe:3 / a named pipe) instead of
// the `anullsrc` silence source. The video encoder is resolved upstream from the
// RTMP codec/container compatibility matrix (see RtmpCompatibility.h).

#include <algorithm>
#include <sstream>
#include <string>

namespace corevideo::modules {

struct RtmpFfmpegArgsConfig {
  int width = 0;
  int height = 0;
  int fps = 30;
  int bitrateKbps = 6000;
  std::string videoEncoder = "libx264";        // resolved hardware/cpu encoder
  std::string videoEncoderExtraArgs;            // encoder-specific tuning args
  std::string endpoint;                         // already-quoted by caller? no -> quoted here
  // Real program audio over a second FFmpeg input. When `hasAudio` is false the
  // builder falls back to the silent `anullsrc` source so the FLV mux still
  // carries a valid AAC track.
  bool hasAudio = false;
  int audioChannels = 2;
  int audioSampleRate = 48000;
  std::string audioSampleFormat = "f32le";      // raw PCM format on the audio pipe
  // Path/identifier of the second input. On Windows this is the inherited pipe
  // handle exposed as "pipe:3"; on POSIX it is the read end fd exposed as
  // "pipe:<fd>" or a named-pipe path.
  std::string audioInput = "pipe:3";
};

inline std::string quoteRtmpArgument(const std::string& value) {
  std::string quoted = "\"";
  for (const char ch : value) {
    if (ch == '"') {
      quoted += "\\\"";
    } else {
      quoted += ch;
    }
  }
  quoted += "\"";
  return quoted;
}

inline std::string buildRtmpFfmpegArguments(const RtmpFfmpegArgsConfig& config) {
  const int fps = (std::max)(1, config.fps);
  const int bitrateKbps = (std::max)(1, config.bitrateKbps);
  const int bufferKbps = bitrateKbps * 2;
  std::ostringstream args;
  args << " -hide_banner -loglevel warning"
       << " -f rawvideo -pix_fmt bgra -s " << config.width << "x" << config.height
       << " -r " << fps << " -i pipe:0";
  if (config.hasAudio) {
    const int channels = (std::max)(1, config.audioChannels);
    const int sampleRate = (std::max)(8000, config.audioSampleRate);
    args << " -f " << config.audioSampleFormat << " -ar " << sampleRate
         << " -ac " << channels << " -i " << config.audioInput;
  } else {
    args << " -f lavfi -i anullsrc=channel_layout=stereo:sample_rate=48000";
  }
  args << " -map 0:v:0 -map 1:a:0"
       << " -c:v " << config.videoEncoder << config.videoEncoderExtraArgs
       << " -b:v " << bitrateKbps << "k -maxrate " << bitrateKbps << "k -bufsize " << bufferKbps << "k"
       << " -g " << (fps * 2) << " -pix_fmt yuv420p"
       << " -c:a aac -b:a 160k -ar 48000"
       // Keep the audio clock tied to wallclock-paced video so A/V stays in sync
       // when the PCM pipe briefly under/overruns relative to the frame pipe.
       << " -af aresample=async=1:first_pts=0"
       << " -f flv " << quoteRtmpArgument(config.endpoint);
  return args.str();
}

}  // namespace corevideo::modules
