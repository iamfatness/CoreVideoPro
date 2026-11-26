#include "modules/ProgramAacEncoder.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>
#include <unistd.h>
#endif

namespace {
using corevideo::modules::ProgramAacEncoder;
using corevideo::modules::ProgramAacPacket;

std::vector<ProgramAacPacket> encodeAll(ProgramAacEncoder& encoder, const std::vector<float>& interleaved,
                                        size_t framesPerCall) {
  std::vector<ProgramAacPacket> all;
  for (size_t at = 0; at < interleaved.size() / 2; at += framesPerCall) {
    const size_t frames = std::min(framesPerCall, interleaved.size() / 2 - at);
    std::vector<float> pcm(interleaved.begin() + static_cast<std::ptrdiff_t>(at * 2),
                           interleaved.begin() + static_cast<std::ptrdiff_t>((at + frames) * 2));
    std::vector<ProgramAacPacket> packets;
    EXPECT_TRUE(encoder.encode(pcm, 2, 48000, packets));
    all.insert(all.end(), std::make_move_iterator(packets.begin()), std::make_move_iterator(packets.end()));
  }
  return all;
}
}  // namespace

// Runs on Windows (Media Foundation) and, since Phase 4 O5, Apple (AudioToolbox).
#if defined(_WIN32) || defined(__APPLE__)
TEST(ProgramAacEncoder, EmitsSampleCountedAdtsAtFortyEightKhzStereo) {
  ProgramAacEncoder encoder;
  ASSERT_TRUE(encoder.start());
  std::vector<ProgramAacPacket> all;
  for (int block = 0; block < 50; ++block) {
    std::vector<float> pcm(960 * 2);
    for (int frame = 0; frame < 960; ++frame) {
      const float value = (block == 10 && frame == 0) ? 0.95f : 0.0f;
      pcm[frame * 2] = value;
      pcm[frame * 2 + 1] = value;
    }
    std::vector<ProgramAacPacket> packets;
    ASSERT_TRUE(encoder.encode(pcm, 2, 48000, packets));
    all.insert(all.end(), std::make_move_iterator(packets.begin()), std::make_move_iterator(packets.end()));
  }
  ASSERT_GE(all.size(), 40u);
  for (size_t i = 0; i < all.size(); ++i) {
    ASSERT_GE(all[i].adts.size(), 7u);
    EXPECT_EQ(all[i].adts[0], 0xff);
    EXPECT_EQ(all[i].adts[1] & 0xf0, 0xf0);
    if (i) EXPECT_EQ(all[i].sampleIndex - all[i - 1].sampleIndex, 1024);
  }
}
#endif

#if defined(__APPLE__)
// Phase 4 O5 (D4). Every ADTS header: sync 0xFFF, AAC-LC (profile field 1),
// sampling index 3 (48 kHz), channel configuration 2, and frame_length equal
// to the packet's own size.
TEST(ProgramAacEncoder, AppleAdtsHeadersAreLc48kStereo) {
  ProgramAacEncoder encoder;
  ASSERT_TRUE(encoder.start());
  const auto packets = encodeAll(encoder, std::vector<float>(48000 * 2, 0.0f), 960);  // 1 s of silence
  ASSERT_GE(packets.size(), 40u) << "fixture: 1 s at 48 kHz is ~46 packets";
  for (const auto& p : packets) {
    ASSERT_GE(p.adts.size(), 7u);
    const auto* h = p.adts.data();
    EXPECT_EQ((h[0] << 4) | (h[1] >> 4), 0xFFF) << "sync word";
    EXPECT_EQ(h[1] & 0x01, 1) << "protection absent (no CRC)";
    EXPECT_EQ(h[2] >> 6, 1) << "profile field 1 = AAC-LC";
    EXPECT_EQ((h[2] >> 2) & 0x0F, 3) << "sampling index 3 = 48 kHz";
    EXPECT_EQ(((h[2] & 0x01) << 2) | (h[3] >> 6), 2) << "channel configuration 2";
    const size_t frameLength = ((h[3] & 0x03) << 11) | (h[4] << 3) | (h[5] >> 5);
    EXPECT_EQ(frameLength, p.adts.size()) << "frame_length is the whole ADTS frame";
  }
}

TEST(ProgramAacEncoder, AppleSampleIndexAdvancesBy1024) {
  ProgramAacEncoder encoder;
  ASSERT_TRUE(encoder.start());
  const auto packets = encodeAll(encoder, std::vector<float>(48000 * 2, 0.1f), 480);
  ASSERT_GE(packets.size(), 40u);
  // AudioToolbox's AAC-LC encoder primes with 2112 frames (kAudioConverterPrimeInfo
  // leadingFrames, logged at start). Packet n carries input samples from
  // n*1024 - 2112, so packets 0 and 1 are pure priming (dropped) and the first
  // kept packet starts at 3*1024 - 2112 = 960. A sampleIndex that ignored the
  // priming would start at 0 and put Program audio ~44 ms late in the TS.
  EXPECT_EQ(packets.front().sampleIndex, 960) << "the first kept packet starts at input sample 960";
  EXPECT_EQ(packets.front().pts100ns, 200000) << "960 samples at 48 kHz = 20 ms";
  for (size_t i = 1; i < packets.size(); ++i) {
    EXPECT_EQ(packets[i].sampleIndex - packets[i - 1].sampleIndex, 1024);
    EXPECT_NEAR(static_cast<double>(packets[i].pts100ns - packets[i - 1].pts100ns), 213333.0, 1.0)
        << "1024 samples at 48 kHz = 21.333 ms";
  }
  EXPECT_EQ(encoder.acceptedSamples(), 48000) << "every handed sample is counted, whole packets or not";
}

// 2 s of a 1 kHz sine at amplitude 0.5, encoded, written as an .aac (ADTS) file
// and decoded back with ExtAudioFile: RMS within 10% of 0.5/sqrt(2) = 0.3536,
// and the strongest 10 Hz DFT bin is 1000 Hz.
TEST(ProgramAacEncoder, AppleDecodesToTheInputTone) {
  ProgramAacEncoder encoder;
  ASSERT_TRUE(encoder.start());
  std::vector<float> tone(96000 * 2);
  for (int i = 0; i < 96000; ++i) {
    const float v = 0.5f * static_cast<float>(std::sin(2.0 * M_PI * 1000.0 * i / 48000.0));
    tone[i * 2] = v;
    tone[i * 2 + 1] = v;
  }
  const auto packets = encodeAll(encoder, tone, 960);
  ASSERT_GE(packets.size(), 90u);
  const auto path = std::filesystem::temp_directory_path() / ("cvp-aac-" + std::to_string(::getpid()) + ".aac");
  {
    std::ofstream out(path, std::ios::binary);
    for (const auto& p : packets) out.write(reinterpret_cast<const char*>(p.adts.data()), p.adts.size());
  }
  CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(path.c_str()),
                                                         static_cast<CFIndex>(path.string().size()), false);
  ExtAudioFileRef file = nullptr;
  ASSERT_EQ(ExtAudioFileOpenURL(url, &file), noErr);
  CFRelease(url);
  AudioStreamBasicDescription pcm{};
  pcm.mSampleRate = 48000;
  pcm.mFormatID = kAudioFormatLinearPCM;
  pcm.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
  pcm.mChannelsPerFrame = 2;
  pcm.mBitsPerChannel = 32;
  pcm.mFramesPerPacket = 1;
  pcm.mBytesPerFrame = 8;
  pcm.mBytesPerPacket = 8;
  ASSERT_EQ(ExtAudioFileSetProperty(file, kExtAudioFileProperty_ClientDataFormat, sizeof(pcm), &pcm), noErr);
  std::vector<float> decoded;
  std::vector<float> chunk(4096 * 2);
  for (;;) {
    AudioBufferList list{};
    list.mNumberBuffers = 1;
    list.mBuffers[0].mNumberChannels = 2;
    list.mBuffers[0].mDataByteSize = static_cast<UInt32>(chunk.size() * sizeof(float));
    list.mBuffers[0].mData = chunk.data();
    UInt32 frames = 4096;
    ASSERT_EQ(ExtAudioFileRead(file, &frames, &list), noErr);
    if (frames == 0) break;
    decoded.insert(decoded.end(), chunk.begin(), chunk.begin() + frames * 2);
  }
  ExtAudioFileDispose(file);
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  ASSERT_GE(decoded.size() / 2, 80000u) << "most of the 2 s came back";
  // Skip the first and last 0.1 s (codec edges).
  const size_t from = 4800, to = decoded.size() / 2 - 4800;
  double sum = 0;
  std::vector<double> left;
  for (size_t i = from; i < to; ++i) {
    sum += decoded[i * 2] * decoded[i * 2];
    left.push_back(decoded[i * 2]);
  }
  const double rms = std::sqrt(sum / static_cast<double>(to - from));
  EXPECT_NEAR(rms, 0.3536, 0.3536 * 0.10) << "decoded RMS of a 0.5 sine";
  // DFT at 10 Hz spacing over exactly 48000 samples (1 s): bins 100..3000 Hz.
  const size_t n = 48000;
  ASSERT_GE(left.size(), n);
  int bestHz = 0;
  double best = -1;
  for (int hz = 100; hz <= 3000; hz += 10) {
    double re = 0, im = 0;
    for (size_t i = 0; i < n; ++i) {
      const double phase = 2.0 * M_PI * hz * static_cast<double>(i) / 48000.0;
      re += left[i] * std::cos(phase);
      im -= left[i] * std::sin(phase);
    }
    const double mag = re * re + im * im;
    if (mag > best) { best = mag; bestHz = hz; }
  }
  EXPECT_EQ(bestHz, 1000) << "the strongest 10 Hz bin is the input tone";
}
#endif
