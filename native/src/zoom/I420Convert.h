#pragma once

#include <cstdint>
#include <vector>

namespace corevideo::zoom {

// Convert a planar I420 frame (contiguous Y + U + V, as produced by
// `DecodedI420Frame::i420`) to packed RGBA, downscaling to dstWidth x dstHeight
// with nearest-neighbor sampling. Returns dstWidth*dstHeight*4 bytes (RGBA,
// alpha = 255). When the destination matches the source size this is a straight
// color-space conversion. Returns an empty vector for degenerate inputs.
//
// Uses the standard BT.601 limited-range YCbCr -> RGB coefficients, which is
// what the Zoom raw-data renderer emits.
std::vector<uint8_t> i420ToRgbaThumbnail(const uint8_t* i420, uint32_t srcWidth, uint32_t srcHeight,
                                         uint32_t dstWidth, uint32_t dstHeight);

}  // namespace corevideo::zoom
