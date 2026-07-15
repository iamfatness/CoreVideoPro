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
#include <cmath>
#include <sstream>
#include <string>

namespace corevideo::modules {

// The media-core output worker runs at the audio block cadence (normally 50 Hz),
// while an RTMP video profile may be 24/30 fps. Feeding every worker tick into
// FFmpeg overproduces large raw BGRA frames until its stdin pipe backpressures.
// This small deadline pacer admits at most the configured video rate while the
// caller can continue feeding audio on every worker tick.
class RtmpVideoFramePacer {
 public:
  bool shouldWrite(double elapsedMs, int fps) {
    const double intervalMs = 1000.0 / static_cast<double>((std::max)(1, fps));
    if (!initialized_ || elapsedMs + intervalMs < nextWriteAtMs_) {
      initialized_ = true;
      nextWriteAtMs_ = elapsedMs + intervalMs;
      return true;
    }

    if (elapsedMs + 0.001 < nextWriteAtMs_) {
      return false;
    }

    // Advance from the prior deadline rather than from the current tick. This
    // avoids a 50 Hz producer quantizing a requested 30 fps stream down to 25.
    do {
      nextWriteAtMs_ += intervalMs;
    } while (nextWriteAtMs_ <= elapsedMs);
    return true;
  }

  void reset() {
    initialized_ = false;
    nextWriteAtMs_ = 0.0;
  }

 private:
  bool initialized_ = false;
  double nextWriteAtMs_ = 0.0;
};

struct RtmpFfmpegArgsConfig {
  int width = 0;
  int height = 0;
  int fps = 30;
  int bitrateKbps = 6000;
  std::string videoInputPixelFormat = "bgra";  // raw pipe:0 pixel layout
  std::string videoEncoder = "libx264";        // resolved hardware/cpu encoder
  std::string videoEncoderExtraArgs;            // encoder-specific tuning args
  double keyframeIntervalSeconds = 2.0;
  std::string rateControl = "cbr";
  std::string h264Profile = "high";
  int bFrames = 2;
  std::string endpoint;                         // already-quoted by caller? no -> quoted here
  // Real program audio over a second FFmpeg input. When `hasAudio` is false the
  // builder falls back to the silent `anullsrc` source so the FLV mux still
  // carries a valid AAC track.
  bool hasAudio = false;
  int audioChannels = 2;
  int audioSampleRate = 48000;
  int audioBitrateKbps = 160;
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
  const int audioBitrateKbps = (std::max)(32, config.audioBitrateKbps);
  const int bufferKbps = bitrateKbps * 2;
  const int maxrateKbps = config.rateControl == "vbr" ? bitrateKbps * 3 / 2 : bitrateKbps;
  const int keyframeFrames = (std::max)(1, static_cast<int>(std::round(
      static_cast<double>(fps) * (std::max)(0.5, (std::min)(10.0, config.keyframeIntervalSeconds)))));
  std::ostringstream args;
  args << " -hide_banner -loglevel warning"
       << " -f rawvideo -pix_fmt " << config.videoInputPixelFormat << " -s " << config.width << "x" << config.height
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
       << " -b:v " << bitrateKbps << "k -maxrate " << maxrateKbps << "k -bufsize " << bufferKbps << "k"
       << " -g " << keyframeFrames;
  if (!config.h264Profile.empty() && config.h264Profile != "auto") {
    args << " -profile:v " << config.h264Profile;
  }
  args << " -bf " << (std::max)(0, (std::min)(4, config.bFrames)) << " -pix_fmt yuv420p"
       << " -c:a aac -b:a " << audioBitrateKbps << "k -ar 48000"
       // Keep the audio clock tied to wallclock-paced video so A/V stays in sync
       // when the PCM pipe briefly under/overruns relative to the frame pipe.
       << " -af aresample=async=1:first_pts=0"
       << " -f flv " << quoteRtmpArgument(config.endpoint);
  return args.str();
}

}  // namespace corevideo::modules
