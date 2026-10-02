#if defined(_WIN32) && !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_D3D11
#include "compositor/ComPtrLite.h"
#include "modules/GpuVideoEncoder.h"
#include "modules/Interfaces.h"
#include "modules/MediaFoundationGpuVideoEncoder.h"
#include "modules/RtmpFfmpegArgs.h"
#include "modules/HevcTransportStream.h"

#include <codecapi.h>
#include <d3d11.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

namespace {

corevideo::modules::VideoFrame makeEncoderSourceFrame(int64_t frameNumber, int64_t grayValue) {
  corevideo::modules::VideoFrame frame;
  frame.participantId = "test";
  frame.width = frame.pixelWidth = 64;
  frame.height = frame.pixelHeight = 64;
  frame.naturalWidth = 64;
  frame.naturalHeight = 64;
  frame.pixelStride = 64 * 4;
  frame.timestampMs = frameNumber * 16;
  frame.frameId = frameNumber;
  auto pixels = std::make_shared<std::vector<uint8_t>>(64 * 64 * 4, static_cast<uint8_t>(grayValue));
  for (size_t i = 0; i < pixels->size(); i += 4) {
    const auto v = static_cast<uint8_t>(grayValue & 0xFF);
    (*pixels)[i + 0] = v;
    (*pixels)[i + 1] = v;
    (*pixels)[i + 2] = v;
    (*pixels)[i + 3] = 0xFF;
  }
  frame.pixels = std::move(pixels);
  return frame;
}

int normalizedSystemExitCode(int status) {
  if (status == -1) return -1;
  if ((status & 0xFF00) != 0) return (status >> 8) & 0xFF;
  return status;
}

}  // namespace

#define MAKE_MEDIA_FOUNDATION_GPU_ENCODER_OR_SKIP()                                  \
  auto encoder = corevideo::modules::createMediaFoundationGpuVideoEncoder();          \
  if (!encoder) {                                                                  \
    std::fprintf(stderr,                                                            \
                 "[mf-gpu-encode-test] skipping: createMediaFoundationGpuVideoEncoder " \
                 "returned null\n");                                              \
    return;                                                                        \
  }

// End-to-end proof of the GPU-direct encode path (#521 slice 1, Task 3;
// parametrized per codec 2026-09-20, Task 7): the compositor renders a solid
// mid-gray program into its DEDICATED keyed-mutex encoder texture; the MF
// hardware MFT bound for `codec` opens that shared handle, converts BGRA->NV12
// on the GPU and encodes it; the emitted elementary stream (`rawDemuxer` names
// the ffmpeg demuxer for it) decodes back to the same mid-gray picture. With
// `alsoMuxToFlv` the raw stream is additionally copy-muxed into FLV, which is
// the on-rig proof that the muxer the GPU-direct sender feeds accepts this
// bitstream as-is - for HEVC that means the MFT honoured B-frames OFF.
// Self-skips where the hardware encoder or ffmpeg is absent, so CI stays green
// on machines without either.
static void runRoundTrip(const char* codec, const char* rawDemuxer, bool alsoMuxToFlv) {
  MAKE_MEDIA_FOUNDATION_GPU_ENCODER_OR_SKIP();
  auto compositor = corevideo::modules::createD3D11Compositor();
  ASSERT_TRUE(compositor != nullptr);

  constexpr int64_t kGray = 128;  // mid-gray; the coded Y plane must read back near this.

  corevideo::modules::CompositorRenderPlan plan;
  plan.sceneId = "gpu-direct-smoke";
  plan.renderPlanId = "gpu-direct-smoke";
  plan.width = 320;
  plan.height = 180;
  plan.fps = 60;
  plan.skipCpuReadback = true;
  plan.fullProgramReadback = true;
  corevideo::modules::CompositorRenderPlanLayer layer;
  layer.layerId = "route:test";
  layer.kind = "participant-video";
  layer.participantId = "test";
  layer.sourceId = "zoom:test";
  layer.rect = {0.f, 0.f, 1.f, 1.f};
  layer.borderStyle = "none";
  plan.layers.push_back(layer);

  const auto start = std::chrono::steady_clock::now();
  (void)compositor->render(plan, {makeEncoderSourceFrame(1, kGray)});

  std::vector<uint8_t> encodedBytes;
  std::vector<uint8_t> transportBytes;
  std::vector<double> expectedTimes;
  corevideo::modules::HevcTransportStream transport;
  int64_t firstDts = -1;
  int invalidTimestamps = 0;
  int chunksReceived = 0;
  int keyframes = 0;
  std::mutex encodedMutex;
  std::condition_variable encodedCv;
  auto sink = [&](const corevideo::modules::GpuEncodedChunk& chunk) {
    std::lock_guard<std::mutex> lock(encodedMutex);
    encodedBytes.insert(encodedBytes.end(), chunk.data, chunk.data + chunk.size);
    if (std::string(codec) == "hevc") {
      std::vector<uint8_t> wire;
      if (!transport.packetize(chunk, wire)) {
        ++invalidTimestamps;
        std::fprintf(stderr, "invalid packet pts=%lld dts=%lld valid=%d\n",
            static_cast<long long>(chunk.pts100ns), static_cast<long long>(chunk.dts100ns), chunk.timingValid);
      }
      if (firstDts < 0) firstDts = chunk.dts100ns;
      expectedTimes.push_back(static_cast<double>(chunk.pts100ns - firstDts) / 10000000.0);
      transportBytes.insert(transportBytes.end(), wire.begin(), wire.end());
    }
    ++chunksReceived;
    if (chunk.keyframe) ++keyframes;
    encodedCv.notify_all();
  };

  corevideo::modules::GpuVideoEncoderConfig encoderConfig{
      plan.width, plan.height, plan.fps, 6000, 2.0, "cbr", "high"};
  encoderConfig.codec = codec;
  if (!encoder->start(encoderConfig, sink)) {
    std::fprintf(stderr, "[mf-gpu-encode-test] skipping: encoder start unavailable on this machine\n");
    return;
  }

  constexpr int64_t kFrames = 12;
  // The compositor can start later than the MFT (or temporarily miss its
  // texture deadline). Input requests must survive this gap, rather than being
  // discarded until the hardware encoder has no outstanding credits left.
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  for (int64_t i = 0; i < kFrames; ++i) {
    const auto frame = compositor->render(
        plan, {makeEncoderSourceFrame(start.time_since_epoch().count() + i, kGray)});
    EXPECT_FALSE(frame.encoderSharedTexture.sharedHandleHex.empty())
        << "frameNumber=" << frame.frameNumber << " lacks encoder shared texture";
    corevideo::modules::GpuVideoEncoderFrame encodeFrame;
    encodeFrame.publishedFrameNumber = frame.encoderSharedTexture.publishedFrameNumber;
    encodeFrame.sharedHandleHex = frame.encoderSharedTexture.sharedHandleHex;
    encodeFrame.width = frame.encoderSharedTexture.width;
    encodeFrame.height = frame.encoderSharedTexture.height;
    encodeFrame.frameNumber = frame.frameNumber;
    ASSERT_TRUE(encoder->submit(encodeFrame));
    std::this_thread::sleep_for(std::chrono::milliseconds(8));
  }
  {
    std::unique_lock<std::mutex> lock(encodedMutex);
    encodedCv.wait_for(lock, std::chrono::seconds(8), [&] { return chunksReceived > 0; });
    EXPECT_GE(chunksReceived, 1);
    EXPECT_GE(keyframes, 1) << "no keyframe emitted";
    EXPECT_GE(encodedBytes.size(), static_cast<size_t>(64));
  }
  encoder->stop();
  ASSERT_EQ(invalidTimestamps, 0);
  EXPECT_TRUE(encoder->healthy());

  const std::filesystem::path ffmpegDir = "C:\\ffmpeg\\bin";
  const auto ffmpegExe = ffmpegDir / "ffmpeg.exe";
  std::error_code ec;
  if (!std::filesystem::exists(ffmpegExe, ec) || ec) {
    std::fprintf(stderr,
                 "[  SKIPPED ] MediaFoundationGpuVideoEncoder round-trip codec=%s "
                 "(ffmpeg absent at C:\\ffmpeg\\bin) - bitstream produced, pixels unverified\n",
                 codec);
    return;
  }

  const auto work = std::filesystem::temp_directory_path() /
                    ("corevideo-mf-encode-smoke-" + std::to_string(start.time_since_epoch().count()));
  std::filesystem::create_directories(work, ec);
  const auto rawPath = work / (std::string("program.") + rawDemuxer);
  const auto yuvPath = work / "decoded.yuv";
  {
    std::ofstream raw(rawPath, std::ios::binary);
    raw.write(reinterpret_cast<const char*>(encodedBytes.data()),
              static_cast<std::streamsize>(encodedBytes.size()));
  }

  // Decode to yuv420p and read the CODED Y plane directly: -pix_fmt gray would
  // expand limited->full range and shift a mid-gray by ~1.16x, so the Y plane is
  // the range-independent way to prove the encoded picture is what we rendered.
  // Wrap the whole command in an extra quote pair: cmd.exe strips the outermost
  // quotes, which would otherwise mangle both quoted paths (std::system gotcha).
  const std::string inner = "\"" + ffmpegExe.string() + "\" -v error -f " + rawDemuxer + " -i \"" +
                            rawPath.string() + "\" -f rawvideo -pix_fmt yuv420p \"" +
                            yuvPath.string() + "\"";
  const std::string cmd = "\"" + inner + "\"";
  const int status = normalizedSystemExitCode(std::system(cmd.c_str()));
  ASSERT_EQ(status, 0) << "ffmpeg decode failed status=" << status << " raw=" << rawPath.string();

  std::ifstream yuv(yuvPath, std::ios::binary);
  const std::vector<uint8_t> decoded((std::istreambuf_iterator<char>(yuv)),
                                     std::istreambuf_iterator<char>());
  const size_t lumaBytes = static_cast<size_t>(plan.width) * static_cast<size_t>(plan.height);
  const size_t frameBytes = lumaBytes * 3 / 2;
  ASSERT_GE(decoded.size(), frameBytes) << "decoded less than one full frame";
  const size_t frames = decoded.size() / frameBytes;
  double sum = 0.0;
  size_t count = 0;
  for (size_t f = 0; f < frames; ++f) {
    const size_t base = f * frameBytes;
    for (size_t i = 0; i < lumaBytes; ++i) {
      sum += decoded[base + i];
      ++count;
    }
  }
  const double meanLuma = count ? sum / static_cast<double>(count) : 0.0;
  std::fprintf(stderr,
               "[mf-gpu-encode-test] decoded %zu frame(s), mean coded luma=%.1f (input gray=%lld)\n",
               frames, meanLuma, static_cast<long long>(kGray));
  // A real mid-gray picture. Black would read ~16, white ~235, garbage random.
  EXPECT_TRUE(std::fabs(meanLuma - static_cast<double>(kGray)) <= 16.0)
      << "decoded mean coded luma " << meanLuma << " is not within 16 of the encoded gray " << kGray;

  if (alsoMuxToFlv) {
    // The FLV muxer refuses reordered raw HEVC ("Packet is missing PTS"). This
    // copy-mux is the on-rig proof that the MFT honoured B-frames OFF.
    const auto flvPath = rawPath.parent_path() / (std::string("gpu-encode-") + codec + ".flv");
    corevideo::modules::RtmpFfmpegArgsConfig muxConfig;
    muxConfig.videoBitstreamInput = true;
    muxConfig.videoBitstreamCodec = codec;
    muxConfig.fps = plan.fps;
    muxConfig.timestampedHevcInput = std::string(codec) == "hevc";
    muxConfig.endpoint = flvPath.string();
    auto muxArgs = corevideo::modules::buildRtmpFfmpegArguments(muxConfig);
    auto muxInput = rawPath;
    if (muxConfig.timestampedHevcInput) {
      muxInput = work / "timed-hevc.ts";
      std::ofstream ts(muxInput, std::ios::binary);
      ts.write(reinterpret_cast<const char*>(transportBytes.data()), transportBytes.size());
    }
    muxArgs.replace(muxArgs.find("pipe:0"), 6, "\"" + muxInput.string() + "\"");
    muxArgs.insert(muxArgs.find(" -f flv"), " -shortest");
    const std::string muxInner = "\"" + ffmpegExe.string() + "\" -y" + muxArgs;
    const int muxStatus = normalizedSystemExitCode(std::system(("\"" + muxInner + "\"").c_str()));
    EXPECT_EQ(muxStatus, 0) << codec << " raw bitstream did not copy-mux into FLV: " << rawPath.string();
    if (std::string(codec) == "hevc") {
      const auto probePath = work / "color.txt";
      const auto ffprobeExe = ffmpegDir / "ffprobe.exe";
      const std::string probeInner = "\"" + ffprobeExe.string() +
          "\" -v error -select_streams v:0 -show_entries stream=color_range,color_space,color_transfer,color_primaries"
          " -of default=noprint_wrappers=1 \"" + flvPath.string() + "\" > \"" + probePath.string() + "\"";
      ASSERT_EQ(normalizedSystemExitCode(std::system(("\"" + probeInner + "\"").c_str())), 0);
      std::ifstream probe(probePath);
      const std::string color((std::istreambuf_iterator<char>(probe)), std::istreambuf_iterator<char>());
      EXPECT_TRUE(color.find("color_range=tv") != std::string::npos) << color;
      EXPECT_TRUE(color.find("color_space=bt709") != std::string::npos) << color;
      EXPECT_TRUE(color.find("color_transfer=bt709") != std::string::npos) << color;
      EXPECT_TRUE(color.find("color_primaries=bt709") != std::string::npos) << color;
      const auto timingPath = work / "timing.txt";
      const auto timingCmd = "\"" + ffprobeExe.string() +
          "\" -v error -select_streams v:0 -show_entries packet=pts_time -of csv=p=0 \"" +
          flvPath.string() + "\" > \"" + timingPath.string() + "\"";
      ASSERT_EQ(normalizedSystemExitCode(std::system(("\"" + timingCmd + "\"").c_str())), 0);
      std::ifstream timings(timingPath);
      double previous = -1, current = 0;
      int packetCount = 0;
      while (timings >> current) {
        if (previous >= 0 && packetCount < static_cast<int>(expectedTimes.size()))
          EXPECT_TRUE(std::fabs(current - previous - (expectedTimes[packetCount] - expectedTimes[packetCount - 1])) < 0.002);
        previous = current;
        ++packetCount;
      }
      EXPECT_GE(packetCount, 2); // a burst read from disk must retain video cadence
      EXPECT_EQ(packetCount, static_cast<int>(expectedTimes.size()));
    }
    std::error_code fec;
    std::filesystem::remove(flvPath, fec);
  }
}

// #601: the configured rate control must reach the MFT, and it must mean the
// same thing the CPU (FFmpeg) path means by it. Pure mapping - no hardware, so
// this runs on every Windows machine. The measurement that proves the mapping
// actually BINDS is StreamBackpressureRateProbe.ConfiguredBitrateIsHonoured.
TEST(MediaFoundationGpuVideoEncoder, RateControlVocabularyMapsOntoTheCodecApi) {
  using corevideo::modules::mediaFoundationPeakBitrateBps;
  using corevideo::modules::mediaFoundationRateControlMode;

  EXPECT_EQ(mediaFoundationRateControlMode("cbr"),
            static_cast<unsigned int>(eAVEncCommonRateControlMode_CBR));
  EXPECT_EQ(mediaFoundationRateControlMode("vbr"),
            static_cast<unsigned int>(eAVEncCommonRateControlMode_PeakConstrainedVBR));
  // normalizeRateControl() only ever produces "cbr" or "vbr"; anything else
  // reaching here is a caller that skipped it, and cbr is the safe reading.
  EXPECT_EQ(mediaFoundationRateControlMode("nonsense"),
            static_cast<unsigned int>(eAVEncCommonRateControlMode_CBR));

  // The peak mirrors RtmpFfmpegArgs' maxrate rule exactly.
  EXPECT_EQ(mediaFoundationPeakBitrateBps("cbr", 6000), 6000u * 1000u);
  EXPECT_EQ(mediaFoundationPeakBitrateBps("vbr", 6000), 9000u * 1000u);
  EXPECT_EQ(mediaFoundationPeakBitrateBps("vbr", 2001), 3001500u);
  EXPECT_GT(mediaFoundationPeakBitrateBps("cbr", 0), 0u);
}

TEST(MediaFoundationGpuVideoEncoder, RejectsUnrepresentableBitrateBeforeHardwareStartup) {
  auto encoder = corevideo::modules::createMediaFoundationGpuVideoEncoder();
  ASSERT_TRUE(encoder != nullptr);
  corevideo::modules::GpuVideoEncoderConfig config{1920, 1080, 60, 0, 2.0, "cbr", "high"};
  for (int rate : {0, -1, (std::numeric_limits<int>::max)()}) {
    config.bitrateKbps = rate;
    EXPECT_FALSE(encoder->start(config, [](const corevideo::modules::GpuEncodedChunk&) {}));
    EXPECT_EQ(encoder->lastFailure(), "invalid-bitrate");
  }
  config.rateControl = "vbr";
  config.bitrateKbps = static_cast<int>((std::numeric_limits<unsigned int>::max)() / 1500u + 1u);
  EXPECT_FALSE(encoder->start(config, [](const corevideo::modules::GpuEncodedChunk&) {}));
  EXPECT_EQ(encoder->lastFailure(), "invalid-bitrate");
}

TEST(MediaFoundationGpuVideoEncoder, DirectSharedTextureH264RoundTrip) { runRoundTrip("h264", "h264", false); }
TEST(MediaFoundationGpuVideoEncoder, DirectSharedTextureHevcRoundTripMuxesWithoutBFrames) { runRoundTrip("hevc", "hevc", true); }
TEST(MediaFoundationGpuVideoEncoder, DirectSharedTextureAv1RoundTrip) { runRoundTrip("av1", "obu", true); }

TEST(MediaFoundationGpuVideoEncoder, HevcRepeatedStartStopWithoutProducerIsBounded) {
  MAKE_MEDIA_FOUNDATION_GPU_ENCODER_OR_SKIP();
  corevideo::modules::GpuVideoEncoderConfig config{320, 180, 60, 6000, 2.0, "cbr", "high"};
  config.codec = "hevc";
  for (int cycle = 0; cycle < 20; ++cycle) {
    if (!encoder->start(config, [](const corevideo::modules::GpuEncodedChunk&) {})) {
      if (cycle == 0) return;  // hardware unavailable
      ASSERT_TRUE(false) << "HEVC restart failed at cycle " << cycle;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    const auto before = std::chrono::steady_clock::now();
    encoder->stop();
    EXPECT_TRUE(std::chrono::steady_clock::now() - before < std::chrono::seconds(2))
        << "HEVC stop blocked at cycle " << cycle;
    encoder->stop();  // idempotent, including the activation shutdown
  }
}

TEST(MediaFoundationGpuVideoEncoder, HevcRepeatedActiveEncodeShutdown) {
  // Exercise driver callbacks and outstanding encoded samples, not only an
  // idle event queue. Each cycle verifies a decodable HEVC payload before stop.
  for (int cycle = 0; cycle < 10; ++cycle) runRoundTrip("hevc", "hevc", true);
}

// #521 slice 1, Task 5: an unhealthy or not-running encoder fails submit(). This
// is the contract the OutputDestinationSupervisor rides — a device loss sets
// healthy_=false in the encode loop (deviceRemovedReason()), after which submit()
// returns false, the sender reports a video-write failure, and the supervisor
// restarts it (re-deciding the encode path). A real TDR is not injected here; the
// device-loss classification is verified by inspection + the round-trip test above
// proving healthy() stays true on the happy path.
TEST(MediaFoundationGpuVideoEncoder, SubmitFailsWhenNotRunningSoTheSupervisorRestarts) {
  MAKE_MEDIA_FOUNDATION_GPU_ENCODER_OR_SKIP();
  corevideo::modules::GpuVideoEncoderFrame frame;
  frame.sharedHandleHex = "0x1234";
  frame.width = 320;
  frame.height = 180;
  frame.frameNumber = 1;
  // Before start(): not running and not healthy, so submit must fail.
  EXPECT_FALSE(encoder->healthy());
  EXPECT_FALSE(encoder->submit(frame));
  // After stop() (idempotent from the never-started state) the contract holds.
  encoder->stop();
  EXPECT_FALSE(encoder->submit(frame));
}
#endif
