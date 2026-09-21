#if defined(_WIN32) && !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_D3D11
#include "compositor/ComPtrLite.h"
#include "modules/GpuVideoEncoder.h"
#include "modules/Interfaces.h"
#include "modules/MediaFoundationGpuVideoEncoder.h"

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
  int chunksReceived = 0;
  int keyframes = 0;
  std::mutex encodedMutex;
  std::condition_variable encodedCv;
  auto sink = [&](const corevideo::modules::GpuEncodedChunk& chunk) {
    std::lock_guard<std::mutex> lock(encodedMutex);
    encodedBytes.insert(encodedBytes.end(), chunk.data, chunk.data + chunk.size);
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
    const std::string muxInner = "\"" + ffmpegExe.string() + "\" -v error -y -use_wallclock_as_timestamps 1 -r " +
                                 std::to_string(plan.fps) + " -f " + rawDemuxer + " -i \"" + rawPath.string() +
                                 "\" -c:v copy -f flv \"" + flvPath.string() + "\"";
    const int muxStatus = normalizedSystemExitCode(std::system(("\"" + muxInner + "\"").c_str()));
    EXPECT_EQ(muxStatus, 0) << codec << " raw bitstream did not copy-mux into FLV: " << rawPath.string();
    std::error_code fec;
    std::filesystem::remove(flvPath, fec);
  }
}

TEST(MediaFoundationGpuVideoEncoder, DirectSharedTextureH264RoundTrip) { runRoundTrip("h264", "h264", false); }
TEST(MediaFoundationGpuVideoEncoder, DirectSharedTextureHevcRoundTripMuxesWithoutBFrames) { runRoundTrip("hevc", "hevc", true); }
TEST(MediaFoundationGpuVideoEncoder, DirectSharedTextureAv1RoundTrip) { runRoundTrip("av1", "obu", true); }

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
