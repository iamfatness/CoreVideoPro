#pragma once
// macOS engine I420 preparation, shared so it can be unit-tested
// (native/tests/I420EvenFrameTest.cpp).
//
// The macOS SDK exposes no stride accessors on ZoomSDKYUVRawDataI420, so the
// planes are assumed tightly packed at src_w and ceil(src_w/2). The SHM payload
// describes an even-sized frame (y_len + y_len/4 + y_len/4), so an odd frame is
// cropped by one row/column (see valid_i420_frame in main-macos.mm).
//
// I420EvenFrame is the Windows I420RangeNormalizer shape (i420-range-expand.h):
// a limited-range frame is packed and expanded ONCE, into a per-owner scratch
// buffer reallocated only when the frame grows, BEFORE any SHM sequence goes
// odd; every fan-out target then memcpys the prepared frame inside its seqlock
// window. Expanding in place after each target's copy widened the write window
// several-fold and repeated the work per target (mac Tiles N1 review).

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "i420-range-expand.h"

// Copy a src_w-wide I420 frame into `dst`, cropped to the even w x h.
inline void i420_pack_even(uint8_t *dst, const uint8_t *sy, const uint8_t *su, const uint8_t *sv,
                           uint32_t w, uint32_t h, uint32_t src_w)
{
    const size_t y_len = static_cast<size_t>(w) * static_cast<size_t>(h);
    if (w == src_w) {
        std::memcpy(dst, sy, y_len);
    } else {
        for (uint32_t r = 0; r < h; ++r)
            std::memcpy(dst + static_cast<size_t>(r) * w, sy + static_cast<size_t>(r) * src_w, w);
    }
    const uint32_t src_cstride = (src_w + 1) / 2;
    const uint32_t cw = w / 2;
    const uint32_t ch = h / 2;
    uint8_t *du = dst + y_len;
    uint8_t *dv = du + static_cast<size_t>(cw) * ch;
    if (cw == src_cstride) {
        std::memcpy(du, su, static_cast<size_t>(cw) * ch);
        std::memcpy(dv, sv, static_cast<size_t>(cw) * ch);
    } else {
        for (uint32_t r = 0; r < ch; ++r) {
            std::memcpy(du + static_cast<size_t>(r) * cw, su + static_cast<size_t>(r) * src_cstride, cw);
            std::memcpy(dv + static_cast<size_t>(r) * cw, sv + static_cast<size_t>(r) * src_cstride, cw);
        }
    }
}

// Runs `prepare` (the pack + expand) with `lock` RELEASED, so another feed's
// try_lock on the same mutex succeeds for the whole expansion, then re-takes
// the lock with try_lock (an SDK callback thread must never block on it; see
// video_on_frame). Returns prepare's result with the lock owned again, or
// nullptr — lock NOT owned — when prepare refused the frame or the lock could
// not be re-taken; the caller drops the frame either way. After a non-null
// return the caller must re-find any state it referenced under the old hold.
// Lock is std::unique_lock in the engine; a template so the test can observe it.
template <class Lock, class Prepare>
const uint8_t *i420_prepare_with_lock_released(Lock &lock, Prepare &&prepare)
{
    lock.unlock();
    const uint8_t *out = prepare();
    if (!out) return nullptr;
    if (!lock.try_lock()) return nullptr;
    return out;
}

class I420EvenFrame {
public:
    // Packs (and, when `limited`, expands to full range) the frame into the
    // scratch buffer. Returns the packed planes — y_len + 2 * y_len/4 bytes,
    // see size() — or nullptr when y_len exceeds max_y_len (the SHM capacity),
    // checked before any allocation. The source planes are never written.
    const uint8_t *prepare(const uint8_t *sy, const uint8_t *su, const uint8_t *sv,
                           uint32_t w, uint32_t h, uint32_t src_w, bool limited,
                           size_t max_y_len)
    {
        const size_t y_len = static_cast<size_t>(w) * static_cast<size_t>(h);
        if (!sy || !su || !sv || y_len == 0 || y_len > max_y_len) return nullptr;
        size_ = y_len + 2 * (y_len / 4);
        if (scratch_.size() < size_) scratch_.resize(size_);
        uint8_t *y = scratch_.data();
        i420_pack_even(y, sy, su, sv, w, h, src_w);
        if (limited) {
            uint8_t *u = y + y_len;
            uint8_t *v = u + y_len / 4;
            i420_expand_limited_to_full(y, u, v, y, u, v, y_len);
        }
        return y;
    }
    size_t size() const { return size_; }

private:
    std::vector<uint8_t> scratch_;
    size_t size_ = 0;
};
