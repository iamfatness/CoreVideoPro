#pragma once

// #482 / T3.8 — conform an ISO source's frame to the size its writer opened at.
//
// An ISO writer opens LAZILY at its source's first frame and keeps that size for
// the life of the recording (ISO-1: "sized to that frame's native dims (no
// scaling)"). When the guest's frame size changed later, the CURRENT frame's
// dimensions were passed to a writer opened at the ORIGINAL size, and the
// picture cropped or padded. #478 made that routine rather than rare: under
// stable resolution tiers a guest moves 720P <-> 1080P every time they are cued
// onto or off a bus.
//
// An MP4 video track has ONE size, so conforming is not a choice — the only
// question is whether it is deliberate. This makes it deliberate:
// aspect-preserving, centred, letterboxed with legal black, in the same shape
// the FFmpeg media path already uses
// (scale=...force_original_aspect_ratio=decrease, pad=...).
//
// Pure and header-only so the geometry and the pixels are both unit-testable
// without a writer. Runs on the AsyncEncoderSink WRITER thread, never under
// coreMutex and never on the audio worker — the convert law is unchanged.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace corevideo::modules {

struct IsoFitRect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

// Largest centred rect of the source's aspect that fits the destination, with
// every edge EVEN: I420 and NV12 carry chroma at half resolution, so an odd
// origin or extent splits a chroma sample across the letterbox edge and fringes
// it. A degenerate input returns an empty rect rather than dividing by zero.
[[nodiscard]] inline IsoFitRect isoFitRect(int srcW, int srcH, int dstW, int dstH) {
  if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) {
    return {};
  }
  const auto even = [](int value) { return value & ~1; };
  // Compare aspects in integer cross-products: no float, no rounding drift.
  const long long srcAspect = static_cast<long long>(srcW) * dstH;
  const long long dstAspect = static_cast<long long>(dstW) * srcH;
  int width = dstW;
  int height = dstH;
  if (srcAspect > dstAspect) {
    // Source is wider: full width, bars top and bottom.
    height = static_cast<int>((static_cast<long long>(dstW) * srcH + srcW / 2) / srcW);
  } else if (srcAspect < dstAspect) {
    // Source is taller: full height, bars left and right.
    width = static_cast<int>((static_cast<long long>(dstH) * srcW + srcH / 2) / srcH);
  }
  width = std::max(2, std::min(even(width), even(dstW)));
  height = std::max(2, std::min(even(height), even(dstH)));
  return {even((dstW - width) / 2), even((dstH - height) / 2), width, height};
}

namespace detail {

// Box-filtered sample of one 8-bit plane. Averaging the source footprint rather
// than point-sampling it is what keeps a 4K guest downscaled to 1080p from
// aliasing into shimmer — a still frame that crawls is worse than a soft one.
[[nodiscard]] inline std::uint8_t samplePlaneBox(const std::uint8_t* plane, int planeW, int planeH,
                                                 int dstX, int dstY, int rectW, int rectH) {
  const int x0 = static_cast<int>(static_cast<long long>(dstX) * planeW / rectW);
  const int x1 = std::max(x0 + 1, static_cast<int>(static_cast<long long>(dstX + 1) * planeW / rectW));
  const int y0 = static_cast<int>(static_cast<long long>(dstY) * planeH / rectH);
  const int y1 = std::max(y0 + 1, static_cast<int>(static_cast<long long>(dstY + 1) * planeH / rectH));
  unsigned total = 0;
  unsigned count = 0;
  for (int y = y0; y < std::min(y1, planeH); ++y) {
    const std::uint8_t* row = plane + static_cast<std::size_t>(y) * planeW;
    for (int x = x0; x < std::min(x1, planeW); ++x) {
      total += row[x];
      ++count;
    }
  }
  return count ? static_cast<std::uint8_t>((total + count / 2) / count) : std::uint8_t{0};
}

}  // namespace detail

// I420 (planar Y, U, V) -> NV12 (Y plane + interleaved UV) at EXACTLY dstW x
// dstH. Bars are BT.601 studio-swing black (Y=16, UV=128) — the same black
// convertBgraToNv12 already writes, not 0, which is blacker than legal.
//
// An unchanged size takes a straight copy/interleave: the common path must not
// pay for a resample it does not need, and must be bit-exact.
inline void conformI420ToNv12(const std::uint8_t* i420, int srcW, int srcH, int dstW, int dstH,
                              std::vector<std::uint8_t>& out) {
  if (dstW <= 0 || dstH <= 0) {
    out.clear();
    return;
  }
  const std::size_t dstLuma = static_cast<std::size_t>(dstW) * dstH;
  out.assign(dstLuma + dstLuma / 2, 0);
  std::uint8_t* dstY = out.data();
  std::uint8_t* dstUV = out.data() + dstLuma;
  if (!i420 || srcW <= 0 || srcH <= 0) {
    std::memset(dstY, 16, dstLuma);
    std::memset(dstUV, 128, dstLuma / 2);
    return;
  }

  const std::size_t srcLuma = static_cast<std::size_t>(srcW) * srcH;
  const std::uint8_t* srcY = i420;
  const std::uint8_t* srcU = i420 + srcLuma;
  const std::uint8_t* srcV = srcU + srcLuma / 4;
  const int srcChromaW = srcW / 2;
  const int srcChromaH = srcH / 2;

  if (srcW == dstW && srcH == dstH) {
    std::memcpy(dstY, srcY, srcLuma);
    for (std::size_t i = 0; i < srcLuma / 4; ++i) {
      dstUV[i * 2] = srcU[i];
      dstUV[i * 2 + 1] = srcV[i];
    }
    return;
  }

  const auto fit = isoFitRect(srcW, srcH, dstW, dstH);
  if (fit.width <= 0 || fit.height <= 0) {
    std::memset(dstY, 16, dstLuma);
    std::memset(dstUV, 128, dstLuma / 2);
    return;
  }

  std::memset(dstY, 16, dstLuma);
  std::memset(dstUV, 128, dstLuma / 2);

  for (int y = 0; y < fit.height; ++y) {
    std::uint8_t* row = dstY + static_cast<std::size_t>(fit.y + y) * dstW + fit.x;
    for (int x = 0; x < fit.width; ++x) {
      row[x] = detail::samplePlaneBox(srcY, srcW, srcH, x, y, fit.width, fit.height);
    }
  }

  // Chroma is half resolution on BOTH sides, and the fit rect is even-aligned,
  // so the chroma rect is exactly half of it with no rounding to reconcile.
  const int chromaFitW = fit.width / 2;
  const int chromaFitH = fit.height / 2;
  for (int y = 0; y < chromaFitH; ++y) {
    std::uint8_t* row = dstUV + static_cast<std::size_t>(fit.y / 2 + y) * dstW + fit.x;
    for (int x = 0; x < chromaFitW; ++x) {
      row[x * 2] = detail::samplePlaneBox(srcU, srcChromaW, srcChromaH, x, y, chromaFitW, chromaFitH);
      row[x * 2 + 1] = detail::samplePlaneBox(srcV, srcChromaW, srcChromaH, x, y, chromaFitW, chromaFitH);
    }
  }
}

}  // namespace corevideo::modules
