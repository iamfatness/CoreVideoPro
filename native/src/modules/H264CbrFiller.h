#pragma once

#include "modules/GpuVideoEncoder.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <vector>

namespace corevideo::modules {

// H.264 CBR rate control on some hardware encoders does not emit filler for a
// low-complexity scene. Keep one stream-wide byte budget and prepend legal
// user-data SEI padding before the encoded access unit is fanned out. FFmpeg's
// FLV muxer strips filler_data NALs, but retains SEI in the video bitstream. This
// changes neither the picture nor its PTS. Network muxers still own their
// independent copies of the resulting shared bitstream.
class H264CbrFiller {
 public:
  H264CbrFiller(int bitrateKbps, int fps)
      : bitrateBitsPerSecond_(static_cast<uint64_t>((std::max)(0, bitrateKbps)) * 1000),
        frameDuration100ns_(10'000'000LL / (std::max)(1, fps)) {}

  bool pad(const GpuEncodedChunk& chunk, std::vector<uint8_t>& output) {
    output.clear();
    if (!chunk.data || !chunk.size || !chunk.timingValid || chunk.dts100ns < 0 ||
        !hasAnnexBStartCode(chunk) || bitrateBitsPerSecond_ == 0) {
      return false;
    }
    if (!started_) { epoch100ns_ = chunk.dts100ns; started_ = true; }
    if (chunk.dts100ns < epoch100ns_) return false;
    const auto elapsed100ns = static_cast<uint64_t>(chunk.dts100ns - epoch100ns_ +
        (chunk.duration100ns > 0 ? chunk.duration100ns : frameDuration100ns_));
    const uint64_t desiredBits = elapsed100ns / 10'000'000 * bitrateBitsPerSecond_ +
        (elapsed100ns % 10'000'000) * bitrateBitsPerSecond_ / 10'000'000;
    const uint64_t desiredBytes = desiredBits / 8;
    const uint64_t beforePad = emittedBytes_ + chunk.size;
    constexpr uint64_t kMinimumNalBytes = 24;  // start code, SEI header, UUID, trailing bits
    constexpr uint64_t kMaximumFillerBytes = 256 * 1024;
    const uint64_t deficit = desiredBytes > beforePad ? desiredBytes - beforePad : 0;
    const uint64_t fillerBytes = (std::min)(deficit, kMaximumFillerBytes);
    emittedBytes_ = beforePad;
    if (fillerBytes < kMinimumNalBytes) return false;
    size_t dataBytes = static_cast<size_t>(fillerBytes - kMinimumNalBytes);
    auto totalBytes = [](size_t payload) { return size_t{4 + 1 + 1 + 1 + 16 + 1} + payload +
        (16 + payload) / 255; };
    while (dataBytes && totalBytes(dataBytes) > fillerBytes) --dataBytes;
    constexpr uint8_t kUuid[16]{0x47, 0x2a, 0x73, 0x1b, 0x68, 0x4f, 0x4e, 0x28,
                                0x9b, 0x46, 0x25, 0x71, 0x33, 0x64, 0x18, 0x52};
    output.reserve(chunk.size + totalBytes(dataBytes));
    output.insert(output.end(), {0, 0, 0, 1, 0x06, 0x05});  // SEI user_data_unregistered
    size_t payloadBytes = 16 + dataBytes;
    while (payloadBytes >= 255) { output.push_back(0xff); payloadBytes -= 255; }
    output.push_back(static_cast<uint8_t>(payloadBytes));
    output.insert(output.end(), std::begin(kUuid), std::end(kUuid));
    output.insert(output.end(), dataBytes, 0xff);
    output.push_back(0x80);  // rbsp_trailing_bits
    const size_t seiBytes = output.size();
    output.insert(output.end(), chunk.data, chunk.data + chunk.size);
    emittedBytes_ += seiBytes;
    return true;
  }

  [[nodiscard]] uint64_t emittedBytes() const { return emittedBytes_; }

 private:
  static bool hasAnnexBStartCode(const GpuEncodedChunk& chunk) {
    return chunk.size >= 6 && chunk.data[0] == 0 && chunk.data[1] == 0 &&
        (chunk.data[2] == 1 || (chunk.data[2] == 0 && chunk.data[3] == 1));
  }

  uint64_t bitrateBitsPerSecond_ = 0;
  int64_t frameDuration100ns_ = 0;
  bool started_ = false;
  int64_t epoch100ns_ = 0;
  uint64_t emittedBytes_ = 0;
};

}  // namespace corevideo::modules
