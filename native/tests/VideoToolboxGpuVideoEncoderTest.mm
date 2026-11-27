// Phase 4 Task 2 (mac-o4a): VideoToolboxGpuVideoEncoder round trips. Each test
// fills a BGRA IOSurface on the CPU, submits frames whose publishedFrameNumber
// already equals frameNumber (the compositor's "blit completed" signal), and
// decodes the Annex B chunks with a VTDecompressionSession to read pixels back.
//
// Fixture invariants (plan-authoring rule 1):
//   - submissions are PACED (wait until fewer than 2 frames are in flight), so
//     no frame is skipped by the in-flight limit unless a test wants it;
//   - stop() completes every frame before chunks are inspected;
//   - test 3: 30 fps with a 2 s GOP is 60 frames, so frames 2..11 cannot be a
//     scheduled keyframe and frame 11's keyframe can only come from the request;
//   - test 5: the picture changes EVERY frame (or the encoder legitimately
//     under-spends and the assertion proves nothing) AND it has structure.
//     PLAN FIXTURE CORRECTED: the plan specified pure LCG noise. Noise is
//     incompressible: measured 31.27 MB for 10 s at 720p30 whatever the rate
//     control (CBR, CBR+average+cap, max QP 51, real-time pacing), so no
//     correct encoder could pass. A moving gradient with light noise hits
//     5.01 MB against the 5.00 MB target with the same settings.

#include "modules/VideoToolboxGpuVideoEncoder.h"

#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <VideoToolbox/VideoToolbox.h>

#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {
using namespace corevideo::modules;

struct Chunk {
  std::vector<uint8_t> bytes;
  bool keyframe = false;
  int64_t frameNumber = 0;
  int64_t pts = 0;
  int64_t dts = 0;
  int64_t duration = 0;
};

struct Collector {
  std::mutex mutex;
  std::vector<Chunk> chunks;
  GpuEncodedChunkSink sink() {
    return [this](const GpuEncodedChunk& c) {
      std::lock_guard<std::mutex> lock(mutex);
      chunks.push_back({std::vector<uint8_t>(c.data, c.data + c.size), c.keyframe, c.frameNumber, c.pts100ns,
                        c.dts100ns, c.duration100ns});
    };
  }
};

IOSurfaceRef makeSurface(int width, int height) {
  NSDictionary* props = @{
    (id)kIOSurfaceWidth: @(width), (id)kIOSurfaceHeight: @(height),
    (id)kIOSurfaceBytesPerElement: @4, (id)kIOSurfacePixelFormat: @((uint32_t)kCVPixelFormatType_32BGRA),
  };
  return IOSurfaceCreate((__bridge CFDictionaryRef)props);
}

void fillSolid(IOSurfaceRef surface, uint8_t b, uint8_t g, uint8_t r) {
  IOSurfaceLock(surface, 0, nullptr);
  auto* base = static_cast<uint8_t*>(IOSurfaceGetBaseAddress(surface));
  const size_t stride = IOSurfaceGetBytesPerRow(surface);
  for (size_t y = 0; y < IOSurfaceGetHeight(surface); ++y) {
    uint8_t* row = base + y * stride;
    for (size_t x = 0; x < IOSurfaceGetWidth(surface); ++x) {
      row[x * 4 + 0] = b;
      row[x * 4 + 1] = g;
      row[x * 4 + 2] = r;
      row[x * 4 + 3] = 255;
    }
  }
  IOSurfaceUnlock(surface, 0, nullptr);
}

void fillNoise(IOSurfaceRef surface, uint32_t& seed) {
  IOSurfaceLock(surface, 0, nullptr);
  auto* base = static_cast<uint8_t*>(IOSurfaceGetBaseAddress(surface));
  const size_t stride = IOSurfaceGetBytesPerRow(surface);
  for (size_t y = 0; y < IOSurfaceGetHeight(surface); ++y) {
    uint8_t* row = base + y * stride;
    for (size_t x = 0; x < IOSurfaceGetWidth(surface) * 4; ++x) {
      seed = seed * 1664525u + 1013904223u;  // fixed-seed LCG
      row[x] = static_cast<uint8_t>(seed >> 24);
    }
  }
  IOSurfaceUnlock(surface, 0, nullptr);
}

// A moving diagonal gradient plus light noise: changes every frame, compressible.
void fillMovingGradient(IOSurfaceRef surface, int frame, uint32_t& seed) {
  IOSurfaceLock(surface, 0, nullptr);
  auto* base = static_cast<uint8_t*>(IOSurfaceGetBaseAddress(surface));
  const size_t stride = IOSurfaceGetBytesPerRow(surface);
  for (size_t y = 0; y < IOSurfaceGetHeight(surface); ++y) {
    uint8_t* row = base + y * stride;
    for (size_t x = 0; x < IOSurfaceGetWidth(surface) * 4; ++x) {
      seed = seed * 1664525u + 1013904223u;
      row[x] = static_cast<uint8_t>(((x / 4 + static_cast<size_t>(frame) * 4 + y) & 255) / 2 + ((seed >> 24) >> 3));
    }
  }
  IOSurfaceUnlock(surface, 0, nullptr);
}

GpuVideoEncoderFrame frameFor(IOSurfaceRef surface, int64_t number) {
  GpuVideoEncoderFrame frame;
  frame.iosurfaceId = IOSurfaceGetID(surface);
  frame.width = static_cast<int>(IOSurfaceGetWidth(surface));
  frame.height = static_cast<int>(IOSurfaceGetHeight(surface));
  frame.frameNumber = number;
  frame.publishedFrameNumber = std::make_shared<std::atomic<int64_t>>(number);
  return frame;
}

void submitPaced(VideoToolboxGpuVideoEncoder& encoder, const GpuVideoEncoderFrame& frame) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (encoder.framesInFlight() >= 2 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(encoder.submit(frame));
}

GpuVideoEncoderConfig config(int w, int h, int fps, int kbps, const char* codec) {
  GpuVideoEncoderConfig c;
  c.width = w;
  c.height = h;
  c.fps = fps;
  c.bitrateKbps = kbps;
  c.codec = codec;
  c.keyframeIntervalSeconds = 2.0;
  c.rateControl = "cbr";
  return c;
}

// Annex B -> NAL payloads (without start codes).
std::vector<std::vector<uint8_t>> splitAnnexB(const std::vector<uint8_t>& b) {
  std::vector<size_t> starts;
  for (size_t i = 0; i + 3 < b.size(); ++i) {
    if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 0 && b[i + 3] == 1) {
      starts.push_back(i + 4);
      i += 3;
    }
  }
  std::vector<std::vector<uint8_t>> nals;
  for (size_t k = 0; k < starts.size(); ++k) {
    const size_t end = k + 1 < starts.size() ? starts[k + 1] - 4 : b.size();
    nals.emplace_back(b.begin() + starts[k], b.begin() + end);
  }
  return nals;
}

int nalType(const std::vector<uint8_t>& nal, bool hevc) {
  if (nal.empty()) return -1;
  return hevc ? (nal[0] >> 1) & 0x3F : nal[0] & 0x1F;
}

// Decodes every chunk. EVERY chunk must decode (review: a helper that returned
// the last good picture let a stream with broken P-frames pass); the result is
// the mean luma (video-range Y) of the final picture, or a negative sentinel.
double decodeMeanLuma(const std::vector<Chunk>& chunks, bool hevc) {
  if (chunks.empty()) return -1;
  __block int decoded = 0;
  std::vector<std::vector<uint8_t>> params;
  for (const auto& nal : splitAnnexB(chunks.front().bytes)) {
    const int t = nalType(nal, hevc);
    if ((!hevc && (t == 7 || t == 8)) || (hevc && (t == 32 || t == 33 || t == 34))) params.push_back(nal);
  }
  std::vector<const uint8_t*> pointers;
  std::vector<size_t> sizes;
  for (const auto& p : params) {
    pointers.push_back(p.data());
    sizes.push_back(p.size());
  }
  CMVideoFormatDescriptionRef format = nullptr;
  const OSStatus fst = hevc
      ? CMVideoFormatDescriptionCreateFromHEVCParameterSets(kCFAllocatorDefault, pointers.size(), pointers.data(),
                                                            sizes.data(), 4, nullptr, &format)
      : CMVideoFormatDescriptionCreateFromH264ParameterSets(kCFAllocatorDefault, pointers.size(), pointers.data(),
                                                            sizes.data(), 4, &format);
  if (fst != noErr || format == nullptr) return -2;
  NSDictionary* attrs = @{(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)};
  __block double lastMean = -3;
  VTDecompressionSessionRef session = nullptr;
  VTDecompressionOutputCallbackRecord record{};
  if (VTDecompressionSessionCreate(kCFAllocatorDefault, format, nullptr, (__bridge CFDictionaryRef)attrs, &record,
                                   &session) != noErr) {
    CFRelease(format);
    return -4;
  }
  for (const auto& chunk : chunks) {
    std::vector<uint8_t> avcc;
    for (const auto& nal : splitAnnexB(chunk.bytes)) {
      const int t = nalType(nal, hevc);
      if ((!hevc && (t == 7 || t == 8)) || (hevc && t >= 32 && t <= 34)) continue;
      const uint32_t n = static_cast<uint32_t>(nal.size());
      const uint8_t len[4] = {uint8_t(n >> 24), uint8_t(n >> 16), uint8_t(n >> 8), uint8_t(n)};
      avcc.insert(avcc.end(), len, len + 4);
      avcc.insert(avcc.end(), nal.begin(), nal.end());
    }
    if (avcc.empty()) continue;
    CMBlockBufferRef block = nullptr;
    CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr, avcc.size(), kCFAllocatorDefault, nullptr, 0,
                                       avcc.size(), 0, &block);
    CMBlockBufferReplaceDataBytes(avcc.data(), block, 0, avcc.size());
    CMSampleBufferRef sample = nullptr;
    const size_t sampleSize = avcc.size();
    CMSampleBufferCreateReady(kCFAllocatorDefault, block, format, 1, 0, nullptr, 1, &sampleSize, &sample);
    VTDecompressionSessionDecodeFrameWithOutputHandler(
        session, sample, 0, nullptr,
        ^(OSStatus status, VTDecodeInfoFlags, CVImageBufferRef image, CMTime, CMTime) {
          if (status != noErr || image == nullptr) return;
          CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
          const auto* y = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(image, 0));
          const size_t stride = CVPixelBufferGetBytesPerRowOfPlane(image, 0);
          const size_t w = CVPixelBufferGetWidthOfPlane(image, 0);
          const size_t h = CVPixelBufferGetHeightOfPlane(image, 0);
          double sum = 0;
          for (size_t r = 0; r < h; ++r)
            for (size_t c = 0; c < w; ++c) sum += y[r * stride + c];
          CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
          lastMean = sum / double(w * h);
          ++decoded;
        });
    VTDecompressionSessionWaitForAsynchronousFrames(session);
    CFRelease(sample);
    CFRelease(block);
  }
  VTDecompressionSessionInvalidate(session);
  CFRelease(session);
  CFRelease(format);
  if (decoded != static_cast<int>(chunks.size())) {
    EXPECT_TRUE(false) << "decoded " << decoded << " of " << chunks.size() << " chunks";
    return -5;
  }
  return lastMean;
}

std::unique_ptr<VideoToolboxGpuVideoEncoder> encoder() { return std::make_unique<VideoToolboxGpuVideoEncoder>(); }

// The repo's skip idiom (the vendored gtest has no GTEST_SKIP): log loudly and
// return green when no hardware encoder session can be created RIGHT NOW. Two
// real cases: a CI virtual machine with no media engine, and (measured
// 2026-11-23) a test process in which the AvfEncoderRecording + AvfCaptureArrival
// + AvfProgramTail suites ran first. Those suites leave VideoToolbox answering
// -12903 (kVTVideoEncoderNotAvailableNowErr) until the process exits. Recorded
// as a candidate issue: whether the shipping recorder holds hardware sessions
// the same way is unverified. Run this suite alone to exercise it.
bool hardwareSessionAvailable(const char* test) {
  NSDictionary* spec = @{(id)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder: @YES};
  VTCompressionSessionRef probe = nullptr;
  const OSStatus st = VTCompressionSessionCreate(kCFAllocatorDefault, 320, 180, kCMVideoCodecType_H264,
                                                 (__bridge CFDictionaryRef)spec, nullptr, nullptr, nullptr,
                                                 nullptr, &probe);
  if (probe) {
    VTCompressionSessionInvalidate(probe);
    CFRelease(probe);
  }
  if (st != noErr) {
    std::fprintf(stderr, "[  SKIPPED ] VideoToolboxGpuVideoEncoder.%s (no hardware encoder session: %d)"
                         " - this test did NOT run\n", test, static_cast<int>(st));
    return false;
  }
  return true;
}
#define REQUIRE_VT_HARDWARE(name) \
  if (!hardwareSessionAvailable(name)) return

TEST(VideoToolboxGpuVideoEncoder, H264RoundTripDecodesTheEncodedGray) {
  REQUIRE_VT_HARDWARE("H264RoundTripDecodesTheEncodedGray");
  auto enc = encoder();
  Collector out;
  ASSERT_TRUE(enc->start(config(320, 180, 30, 2000, "h264"), out.sink())) << enc->lastFailure();
  IOSurfaceRef surface = makeSurface(320, 180);
  fillSolid(surface, 128, 128, 128);
  for (int i = 1; i <= 12; ++i) submitPaced(*enc, frameFor(surface, i));
  enc->stop();
  CFRelease(surface);
  ASSERT_FALSE(out.chunks.empty());
  const auto& first = out.chunks.front();
  EXPECT_TRUE(first.keyframe);
  ASSERT_GE(first.bytes.size(), 4u);
  EXPECT_TRUE(first.bytes[0] == 0 && first.bytes[1] == 0 && first.bytes[2] == 0 && first.bytes[3] == 1)
      << "Annex B start code, not an AVCC length prefix";
  std::vector<int> types;
  for (const auto& nal : splitAnnexB(first.bytes)) types.push_back(nalType(nal, false));
  ASSERT_GE(types.size(), 3u);
  EXPECT_EQ(types[0], 7);  // SPS
  EXPECT_EQ(types[1], 8);  // PPS
  EXPECT_NE(std::find(types.begin(), types.end(), 5), types.end()) << "an IDR slice follows the parameter sets";
  const double luma = decodeMeanLuma(out.chunks, false);
  EXPECT_NEAR(luma, 126.0, 16.0) << "BT.709 limited range of 128: 16 + 219*128/255";
}

TEST(VideoToolboxGpuVideoEncoder, HevcRoundTripHasParameterSetsAndNoReordering) {
  REQUIRE_VT_HARDWARE("HevcRoundTripHasParameterSetsAndNoReordering");
  auto enc = encoder();
  Collector out;
  ASSERT_TRUE(enc->start(config(320, 180, 30, 2000, "hevc"), out.sink())) << enc->lastFailure();
  IOSurfaceRef surface = makeSurface(320, 180);
  fillSolid(surface, 128, 128, 128);
  for (int i = 1; i <= 12; ++i) submitPaced(*enc, frameFor(surface, i));
  enc->stop();
  CFRelease(surface);
  ASSERT_FALSE(out.chunks.empty());
  std::vector<int> types;
  for (const auto& nal : splitAnnexB(out.chunks.front().bytes)) types.push_back(nalType(nal, true));
  ASSERT_GE(types.size(), 4u);
  EXPECT_EQ(types[0], 32);  // VPS
  EXPECT_EQ(types[1], 33);  // SPS
  EXPECT_EQ(types[2], 34);  // PPS
  EXPECT_TRUE(std::find(types.begin(), types.end(), 19) != types.end() ||
              std::find(types.begin(), types.end(), 20) != types.end())
      << "an IDR (19 or 20) follows";
  // CONFIRMATORY (rule 8): emit() falls back to dts = pts when VideoToolbox
  // gives no decode time, so this alone cannot prove reordering is off.
  for (const auto& chunk : out.chunks) {
    EXPECT_EQ(chunk.pts, chunk.dts) << "frame reordering is off (D2): decode order == presentation order";
  }
  // The real evidence: a reordering encoder emits frames out of input order.
  for (size_t i = 1; i < out.chunks.size(); ++i) {
    EXPECT_LT(out.chunks[i - 1].frameNumber, out.chunks[i].frameNumber);
  }
  EXPECT_NEAR(decodeMeanLuma(out.chunks, true), 126.0, 16.0);
}

TEST(VideoToolboxGpuVideoEncoder, RequestKeyframeEmitsAnIdrOnTheNextPicture) {
  REQUIRE_VT_HARDWARE("RequestKeyframeEmitsAnIdrOnTheNextPicture");
  auto enc = encoder();
  Collector out;
  ASSERT_TRUE(enc->start(config(320, 180, 30, 2000, "h264"), out.sink())) << enc->lastFailure();
  IOSurfaceRef surface = makeSurface(320, 180);
  uint32_t seed = 7;
  for (int i = 1; i <= 10; ++i) {
    fillNoise(surface, seed);
    submitPaced(*enc, frameFor(surface, i));
  }
  // Frame 10 must be encoded before the request, or the request could land on it.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (enc->framesInFlight() > 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(enc->requestKeyframe());
  fillNoise(surface, seed);
  submitPaced(*enc, frameFor(surface, 11));
  enc->stop();
  CFRelease(surface);
  ASSERT_EQ(out.chunks.size(), 11u) << "every paced frame is encoded: got " << out.chunks.size() << " skipped " << enc->framesSkipped();
  for (const auto& chunk : out.chunks) {
    if (chunk.frameNumber >= 2 && chunk.frameNumber <= 10) {
      EXPECT_FALSE(chunk.keyframe) << "frame " << chunk.frameNumber << " is inside the 60-frame GOP";
    }
    if (chunk.frameNumber == 11) EXPECT_TRUE(chunk.keyframe) << "the requested IDR";
  }
}

TEST(VideoToolboxGpuVideoEncoder, PtsAdvancesOneFramePeriod) {
  REQUIRE_VT_HARDWARE("PtsAdvancesOneFramePeriod");
  auto enc = encoder();
  Collector out;
  ASSERT_TRUE(enc->start(config(320, 180, 30, 2000, "h264"), out.sink())) << enc->lastFailure();
  IOSurfaceRef surface = makeSurface(320, 180);
  fillSolid(surface, 40, 80, 120);
  // A 30 fps destination gets every other tick of the 60 Hz Program clock.
  for (int i = 1; i <= 8; ++i) submitPaced(*enc, frameFor(surface, 2 * i));
  enc->stop();
  CFRelease(surface);
  ASSERT_GE(out.chunks.size(), 8u);
  for (size_t i = 1; i < out.chunks.size(); ++i) {
    EXPECT_NEAR(static_cast<double>(out.chunks[i].pts - out.chunks[i - 1].pts), 333333.0, 1.0)
        << "pts " << out.chunks[i - 1].pts << " -> " << out.chunks[i].pts;
  }
}

TEST(VideoToolboxGpuVideoEncoder, BitrateIsHonoured) {
  REQUIRE_VT_HARDWARE("BitrateIsHonoured");
  auto enc = encoder();
  Collector out;
  ASSERT_TRUE(enc->start(config(1280, 720, 30, 4000, "h264"), out.sink())) << enc->lastFailure();
  IOSurfaceRef surface = makeSurface(1280, 720);
  uint32_t seed = 12345;
  for (int i = 1; i <= 300; ++i) {
    fillMovingGradient(surface, i, seed);  // a new, structured picture every frame (fixture invariant)
    submitPaced(*enc, frameFor(surface, 2 * i));  // every other 60 Hz Program tick
  }
  enc->stop();
  CFRelease(surface);
  size_t total = 0;
  for (const auto& chunk : out.chunks) total += chunk.bytes.size();
  EXPECT_EQ(enc->framesSkipped(), 0) << "paced: every frame was encoded; skipped " << enc->framesSkipped()
                                     << " chunks " << out.chunks.size() << " bytes " << total;
  // 10 s at 4 Mbps = 5'000'000 bytes, +-20% (pins Windows defect #601).
  EXPECT_GE(total, 4'000'000u) << "bytes " << total;
  EXPECT_LE(total, 6'000'000u) << "bytes " << total;
}

// The 1.0.0 release blocker: a 30 fps destination on the 60 Hz Program clock.
// The sender hands the encoder every OTHER Program frame, so frame numbers
// advance by 2 per encoded picture. Timestamps must follow the Program clock
// (one second of Program = one second of video), and the rate controller must
// hold the target over that same second. Ticks 200..239 never arrive (a Program
// skip of 2/3 s): the video must carry that hole, not close it.
TEST(VideoToolboxGpuVideoEncoder, ThirtyOnSixtyKeepsProgramTimeAndTheTargetBitrate) {
  REQUIRE_VT_HARDWARE("ThirtyOnSixtyKeepsProgramTimeAndTheTargetBitrate");
  auto enc = encoder();
  Collector out;
  ASSERT_TRUE(enc->start(config(1280, 720, 30, 4000, "h264"), out.sink())) << enc->lastFailure();
  EXPECT_EQ(enc->programTickRate(config(1280, 720, 30, 4000, "h264")), 60)
      << "the sender converts the audio clock and picks frames with this rate";
  IOSurfaceRef surface = makeSurface(1280, 720);
  uint32_t seed = 12345;
  std::vector<int64_t> sent;
  for (int64_t tick = 2; tick <= 640; tick += 2) {
    if (tick >= 200 && tick < 240) continue;
    sent.push_back(tick);
    fillMovingGradient(surface, static_cast<int>(sent.size()), seed);
    submitPaced(*enc, frameFor(surface, tick));
  }
  enc->stop();
  CFRelease(surface);
  ASSERT_EQ(sent.size(), 300u) << "fixture: 10 s of encoded pictures";
  ASSERT_EQ(out.chunks.size(), sent.size()) << "skipped " << enc->framesSkipped();
  for (size_t i = 0; i < sent.size(); ++i) {
    EXPECT_NEAR(static_cast<double>(out.chunks[i].pts), static_cast<double>(sent[i] * 10'000'000LL / 60), 1.0)
        << "picture " << i << " is Program tick " << sent[i];
    EXPECT_EQ(out.chunks[i].duration, 333'333) << "one destination frame period";
  }
  // 10 s of pictures at 4 Mbps = 5'000'000 bytes, +-20%. The defect emitted
  // twice that: each picture claimed two destination periods.
  size_t total = 0;
  for (const auto& chunk : out.chunks) total += chunk.bytes.size();
  EXPECT_GE(total, 4'000'000u) << "bytes " << total;
  EXPECT_LE(total, 6'200'000u) << "bytes " << total;
}

// The surface is identified by frameNumber; the picture is TIMED by programTick
// (which keeps the render slots a late Program loop skipped).
TEST(VideoToolboxGpuVideoEncoder, TheProgramTickTimesThePictureNotItsFrameNumber) {
  REQUIRE_VT_HARDWARE("TheProgramTickTimesThePictureNotItsFrameNumber");
  auto enc = encoder();
  Collector out;
  ASSERT_TRUE(enc->start(config(320, 180, 60, 2000, "h264"), out.sink())) << enc->lastFailure();
  IOSurfaceRef surface = makeSurface(320, 180);
  fillSolid(surface, 40, 80, 120);
  for (int i = 1; i <= 8; ++i) {
    GpuVideoEncoderFrame frame = frameFor(surface, i);
    frame.programTick = 600 + i + (i > 4 ? 30 : 0);
    submitPaced(*enc, frame);
  }
  enc->stop();
  CFRelease(surface);
  ASSERT_EQ(out.chunks.size(), 8u);
  for (int i = 1; i <= 8; ++i) {
    EXPECT_EQ(out.chunks[i - 1].frameNumber, i);
    EXPECT_EQ(out.chunks[i - 1].pts, (600 + i + (i > 4 ? 30 : 0)) * 10'000'000LL / 60);
    EXPECT_EQ(out.chunks[i - 1].duration, 166'666);
  }
}

TEST(VideoToolboxGpuVideoEncoder, ColourIsBt709) {
  REQUIRE_VT_HARDWARE("ColourIsBt709");
  auto enc = encoder();
  Collector out;
  ASSERT_TRUE(enc->start(config(320, 180, 30, 2000, "h264"), out.sink())) << enc->lastFailure();
  IOSurfaceRef surface = makeSurface(320, 180);
  fillSolid(surface, 0, 0, 255);  // BGRA red
  for (int i = 1; i <= 6; ++i) submitPaced(*enc, frameFor(surface, i));
  enc->stop();
  CFRelease(surface);
  // 709: 16 + 219 * 0.2126 = 62.6; 601 would be 16 + 219 * 0.299 = 81.5.
  EXPECT_NEAR(decodeMeanLuma(out.chunks, false), 63.0, 6.0);
}

TEST(VideoToolboxGpuVideoEncoder, StartRefusesAv1WithItsReason) {
  auto enc = encoder();
  Collector out;
  EXPECT_FALSE(enc->start(config(320, 180, 30, 2000, "av1"), out.sink()));
  EXPECT_EQ(enc->lastFailure(), "vt-codec-unsupported:av1");
}

TEST(VideoToolboxGpuVideoEncoder, MissingSurfaceIsASkipNotAFailure) {
  REQUIRE_VT_HARDWARE("MissingSurfaceIsASkipNotAFailure");
  auto enc = encoder();
  Collector out;
  ASSERT_TRUE(enc->start(config(320, 180, 30, 2000, "h264"), out.sink())) << enc->lastFailure();
  IOSurfaceRef surface = makeSurface(320, 180);
  auto frame = frameFor(surface, 1);
  CFRelease(surface);  // released: its id no longer resolves
  EXPECT_TRUE(enc->submit(frame));
  enc->stop();
  EXPECT_TRUE(out.chunks.empty());
  EXPECT_EQ(enc->framesSkipped(), 1) << "skipped " << enc->framesSkipped() << " chunks " << out.chunks.size()
                                     << " failure " << enc->lastFailure();
  EXPECT_TRUE(enc->healthy());
}

}  // namespace
