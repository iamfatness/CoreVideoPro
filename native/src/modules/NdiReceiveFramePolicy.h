#pragma once

#include "core/FrameAllocation.h"
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace corevideo::modules {

// NDI receive buffers belong to libNDI and are freed immediately after capture.
// The source bus must own a packed BGRA copy before that happens.
inline std::shared_ptr<const std::vector<uint8_t>> copyNdiBgra(
    const uint8_t* data, int width, int height, int stride, bool opaque) {
  if (!data || width <= 0 || height <= 0 || width > 4096 || height > 2160 ||
      stride < width * 4 || stride > 65536) {
    return {};
  }
  static core::FrameAllocationFailures allocationFailures("ndi-receive");
  const size_t bgraBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
  auto pixels = core::tryMakeFrameBuffer(bgraBytes);
  if (!pixels) {
    allocationFailures.note(bgraBytes);  // #728: dropped, same as an invalid frame
    return {};
  }
  for (int y = 0; y < height; ++y) {
    auto* out = pixels->data() + static_cast<size_t>(y) * width * 4;
    std::memcpy(out, data + static_cast<size_t>(y) * stride,
                static_cast<size_t>(width) * 4);
    if (opaque) {
      for (int x = 0; x < width; ++x) out[x * 4 + 3] = 255;
    }
  }
  return pixels;
}

inline std::vector<float> interleaveNdiAudio(const float* data, int channels,
                                             int samples, int channelStrideBytes) {
  if (!data || channels <= 0 || channels > 8 || samples <= 0 || samples > 8192 ||
      channelStrideBytes < samples * static_cast<int>(sizeof(float)) ||
      channelStrideBytes > 1'048'576) {
    return {};
  }
  std::vector<float> result(static_cast<size_t>(samples) * 2);
  const auto* left = reinterpret_cast<const float*>(
      reinterpret_cast<const uint8_t*>(data));
  const auto* right = reinterpret_cast<const float*>(
      reinterpret_cast<const uint8_t*>(data) +
      (channels > 1 ? channelStrideBytes : 0));
  for (int i = 0; i < samples; ++i) {
    result[static_cast<size_t>(i) * 2] = left[i];
    result[static_cast<size_t>(i) * 2 + 1] = right[i];
  }
  return result;
}

}  // namespace corevideo::modules
