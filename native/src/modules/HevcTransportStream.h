#pragma once

#include "modules/GpuVideoEncoder.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace corevideo::modules {

// Single-program, single HEVC stream for the existing encoder -> FFmpeg pipe.
// This is an internal timestamp envelope, not the network muxer. FFmpeg still
// owns AAC encoding and enhanced FLV/RTMP. No decode, re-encode or frame padding.
// PES carries the encoder's PTS/DTS; PAT/PMT and PCR permit ordinary TS demuxers
// to consume it. One session-local epoch is subtracted, preserving all gaps.
class HevcTransportStream {
 public:
  bool packetize(const GpuEncodedChunk& chunk, std::vector<uint8_t>& output) {
    output.clear();
    if (!chunk.data || chunk.size < 6 || !chunk.timingValid || chunk.dts100ns < 0 ||
        chunk.pts100ns < chunk.dts100ns ||
        (started_ && chunk.dts100ns <= lastDts_)) return false;
    if (chunk.data[0] != 0 || chunk.data[1] != 0 ||
        !(chunk.data[2] == 1 || (chunk.data[2] == 0 && chunk.data[3] == 1))) return false;
    if (!started_) epoch_ = chunk.dts100ns;
    const uint64_t pts = ticks(chunk.pts100ns - epoch_);
    const uint64_t dts = ticks(chunk.dts100ns - epoch_);
    if (!started_ || chunk.keyframe || dts - tableDts_ >= 9000) {
      tables(output);
      tableDts_ = dts;
    }
    started_ = true;
    lastDts_ = chunk.dts100ns;

    std::vector<uint8_t> pes{0, 0, 1, 0xe0, 0, 0, 0x80,
                             static_cast<uint8_t>(pts == dts ? 0x80 : 0xc0),
                             static_cast<uint8_t>(pts == dts ? 5 : 10)};
    timestamp(pes, pts == dts ? 2 : 3, pts);
    if (pts != dts) timestamp(pes, 1, dts);
    // An AUD makes the access-unit boundary explicit even for MFTs which omit
    // AUD NALs. Duplicate AUDs are avoided by inspecting the first start code.
    const size_t prefix = chunk.size >= 4 && chunk.data[2] == 0 ? 4 : 3;
    const bool aud = chunk.size > prefix && ((chunk.data[prefix] >> 1) & 63) == 35;
    if (!aud) pes.insert(pes.end(), {0, 0, 0, 1, 0x46, 1, 0x50});
    pes.insert(pes.end(), chunk.data, chunk.data + chunk.size);

    size_t offset = 0;
    while (offset < pes.size()) {
      std::array<uint8_t, 188> packet;
      packet.fill(0xff);
      const bool first = offset == 0;
      packet[0] = 0x47;
      packet[1] = static_cast<uint8_t>(0x01 | (first ? 0x40 : 0)); // PID 0x100
      packet[2] = 0;
      packet[3] = 0x10 | (videoCounter_++ & 15);
      const size_t count = (std::min)(pes.size() - offset, first ? size_t{176} : size_t{184});
      const size_t adaptation = 184 - count;
      if (adaptation) {
        packet[3] |= 0x20;
        packet[4] = static_cast<uint8_t>(adaptation - 1);
        if (adaptation > 1) packet[5] = first ? (0x10 | (chunk.keyframe ? 0x40 : 0)) : 0;
        if (first) {
          const uint64_t pcr = dts & ((uint64_t{1} << 33) - 1);
          packet[6] = static_cast<uint8_t>(pcr >> 25);
          packet[7] = static_cast<uint8_t>(pcr >> 17);
          packet[8] = static_cast<uint8_t>(pcr >> 9);
          packet[9] = static_cast<uint8_t>(pcr >> 1);
          packet[10] = static_cast<uint8_t>((pcr << 7) | 0x7e);
          packet[11] = 0;
        }
      }
      std::copy_n(pes.data() + offset, count, packet.data() + 4 + adaptation);
      output.insert(output.end(), packet.begin(), packet.end());
      offset += count;
    }
    return true;
  }

 private:
  static uint64_t ticks(int64_t hns) {
    // Quotient/remainder avoids overflowing on a long-running program clock.
    return static_cast<uint64_t>(hns / 1000) * 9 +
           static_cast<uint64_t>((hns % 1000) * 9 + 500) / 1000;
  }

  static void timestamp(std::vector<uint8_t>& bytes, uint8_t prefix, uint64_t time) {
    bytes.push_back(static_cast<uint8_t>((prefix << 4) | (((time >> 30) & 7) << 1) | 1));
    bytes.push_back(static_cast<uint8_t>(time >> 22));
    bytes.push_back(static_cast<uint8_t>((time >> 14) | 1));
    bytes.push_back(static_cast<uint8_t>(time >> 7));
    bytes.push_back(static_cast<uint8_t>((time << 1) | 1));
  }

  static void section(std::vector<uint8_t>& output, uint16_t pid, uint8_t& counter,
                      std::vector<uint8_t> bytes) {
    uint32_t crc = 0xffffffff;
    for (uint8_t byte : bytes) {
      crc ^= static_cast<uint32_t>(byte) << 24;
      for (int bit = 0; bit < 8; ++bit)
        crc = (crc & 0x80000000) ? (crc << 1) ^ 0x04c11db7 : crc << 1;
    }
    for (int shift = 24; shift >= 0; shift -= 8) bytes.push_back(static_cast<uint8_t>(crc >> shift));
    std::array<uint8_t, 188> packet;
    packet.fill(0xff);
    packet[0] = 0x47;
    packet[1] = static_cast<uint8_t>(0x40 | (pid >> 8));
    packet[2] = static_cast<uint8_t>(pid);
    packet[3] = 0x10 | (counter++ & 15);
    packet[4] = 0; // section pointer
    std::copy(bytes.begin(), bytes.end(), packet.begin() + 5);
    output.insert(output.end(), packet.begin(), packet.end());
  }

  void tables(std::vector<uint8_t>& output) {
    section(output, 0, patCounter_, {0, 0xb0, 13, 0, 1, 0xc1, 0, 0, 0, 1, 0xf0, 0});
    section(output, 0x1000, pmtCounter_,
            {2, 0xb0, 18, 0, 1, 0xc1, 0, 0, 0xe1, 0, 0xf0, 0, 0x24, 0xe1, 0, 0xf0, 0});
  }

  bool started_ = false;
  int64_t epoch_ = 0, lastDts_ = 0;
  uint64_t tableDts_ = 0;
  uint8_t patCounter_ = 0, pmtCounter_ = 0, videoCounter_ = 0;
};

} // namespace corevideo::modules
