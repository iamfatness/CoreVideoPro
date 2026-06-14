#include "zoom/ShmFrameReader.h"

#include <cstring>

namespace corevideo::zoom {

namespace {
uint32_t readU32(const uint8_t* p) {
  // The engine writes native-endian uint32; read with the same width. memcpy
  // avoids alignment UB when `base` is not 4-byte aligned.
  uint32_t value = 0;
  std::memcpy(&value, p, sizeof(value));
  return value;
}
}  // namespace

FrameReadStatus tryReadFrame(const uint8_t* base, std::size_t size, DecodedI420Frame& out) {
  if (base == nullptr || size < kShmFrameHeaderBytes) {
    return FrameReadStatus::Malformed;
  }

  const uint32_t sequence = readU32(base);
  if ((sequence & 1u) != 0u) {
    return FrameReadStatus::InProgress;  // writer holds odd while updating
  }

  const uint32_t width = readU32(base + 4);
  const uint32_t height = readU32(base + 8);
  const uint32_t yLen = readU32(base + 12);
  if (width == 0 || height == 0 || yLen == 0) {
    return FrameReadStatus::Empty;
  }

  // I420: Y is width*height, U and V are each a quarter of that.
  if (yLen != width * height || (yLen % 4u) != 0u) {
    return FrameReadStatus::Malformed;
  }
  const std::size_t planeBytes = static_cast<std::size_t>(yLen) + yLen / 2u;
  if (size < kShmFrameHeaderBytes + planeBytes) {
    return FrameReadStatus::Malformed;
  }

  out.i420.resize(planeBytes);
  std::memcpy(out.i420.data(), base + kShmFrameHeaderBytes, planeBytes);

  // Re-read the sequence: if the writer advanced mid-copy the data is torn.
  if (readU32(base) != sequence) {
    return FrameReadStatus::InProgress;
  }

  out.sequence = sequence;
  out.width = width;
  out.height = height;
  return FrameReadStatus::Ok;
}

}  // namespace corevideo::zoom
