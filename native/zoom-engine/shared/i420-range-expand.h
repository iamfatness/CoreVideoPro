#pragma once

// Expand limited-range ("studio swing") I420 to full range.
//
// The engine asks the Zoom SDK for VideoRawdataColorspace_BT709_F (main.cpp)
// and EVERYTHING downstream declares those frames full-range: the CPU path in
// ZoomEngineClient.cpp converts with unity luma scale and no -16 offset, and
// VideoFrame::i420FullRange (Interfaces.h) defaults to true so the GPU shader
// skips its expansion branch. The SDK does not always honour the request --
// YUVRawDataI420::IsLimitedI420() reports, per frame, that it came back
// limited. A limited frame rendered as if it were full lifts blacks and
// crushes whites for exactly one frame: the "gamma flash".
//
// So the fix normalises the PIXELS here, once per frame, keeping the
// downstream full-range declaration true for every frame rather than trying to
// thread a per-frame range flag through the SHM header, the core, and both
// compositors.

#include <cstddef>
#include <cstdint>

namespace corevideo {

inline uint8_t i420_clamp_byte(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return static_cast<uint8_t>(v);
}

// Round half away from zero without floating point: the correction runs on
// every sample of a 1080p frame, and a per-sample double conversion is not
// worth a fraction of a code value.
inline int i420_div_round(int numerator, int denominator)
{
    const int half = denominator / 2;
    return numerator >= 0 ? (numerator + half) / denominator
                          : (numerator - half) / denominator;
}

// Luma: studio swing 16..235 opens to 0..255. Codes outside the swing are
// footroom/headroom and SATURATE -- an unclamped (y-16)*255/219 would wrap
// superblack into bright values, which is a worse artefact than the flash.
inline uint8_t i420_expand_luma(uint8_t y)
{
    return i420_clamp_byte(i420_div_round((static_cast<int>(y) - 16) * 255, 219));
}

// Chroma: studio swing 16..240 opens to 0..255 about the neutral point 128.
// The offset-then-scale-then-re-offset shape is what keeps 128 EXACTLY at 128,
// so correcting a frame can never introduce a colour cast.
inline uint8_t i420_expand_chroma(uint8_t c)
{
    const int centred = static_cast<int>(c) - 128;
    return i420_clamp_byte(i420_div_round(centred * 255, 224) + 128);
}

// Expand a whole I420 frame. Planes are contiguous and may not overlap.
// `y_len` is width*height; each chroma plane is y_len/4.
inline void i420_expand_limited_to_full(const uint8_t* src_y,
                                        const uint8_t* src_u,
                                        const uint8_t* src_v,
                                        uint8_t* dst_y,
                                        uint8_t* dst_u,
                                        uint8_t* dst_v,
                                        size_t y_len)
{
    for (size_t i = 0; i < y_len; ++i) {
        dst_y[i] = i420_expand_luma(src_y[i]);
    }
    const size_t c_len = y_len / 4;
    for (size_t i = 0; i < c_len; ++i) {
        dst_u[i] = i420_expand_chroma(src_u[i]);
        dst_v[i] = i420_expand_chroma(src_v[i]);
    }
}

}  // namespace corevideo
