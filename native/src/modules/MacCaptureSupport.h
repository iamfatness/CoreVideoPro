#pragma once

// Pure, deterministic helpers for the macOS capture adapters (AVFoundation
// cameras + ScreenCaptureKit screens/windows) — the mac sibling of
// UvcCaptureSupport.h, and the same split: everything here is platform-free
// and unit-tested in the stub build; AVFoundation/SCK plumbing lives in the
// gated adapters. The generic pieces (stable-id hashing, the no-first-frame
// watchdog) are NOT forked — the adapters reuse UvcCaptureSupport's directly.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace corevideo::modules::maccapture {

// CVPixelBuffer planes carry row padding; MF handed the UVC path a contiguous
// buffer so uvc::nv12ToI420 has no stride parameters. This is the stride-aware
// equivalent: NV12 (Y plane + interleaved UV) -> tightly packed I420.
// Dimensions must be even; returns false otherwise.
inline bool nv12ToI420Strided(const uint8_t* yBase, size_t yStride, const uint8_t* uvBase,
                              size_t uvStride, int width, int height,
                              std::vector<uint8_t>& outI420) {
  if (!yBase || !uvBase || width <= 0 || height <= 0 || (width & 1) != 0 || (height & 1) != 0) {
    return false;
  }
  const size_t yLen = static_cast<size_t>(width) * static_cast<size_t>(height);
  const size_t chromaLen = yLen / 4;
  outI420.resize(yLen + chromaLen * 2);
  uint8_t* yDst = outI420.data();
  for (int row = 0; row < height; ++row) {
    std::memcpy(yDst + static_cast<size_t>(row) * width, yBase + static_cast<size_t>(row) * yStride,
                static_cast<size_t>(width));
  }
  uint8_t* uDst = outI420.data() + yLen;
  uint8_t* vDst = uDst + chromaLen;
  const int chromaRows = height / 2;
  const int chromaCols = width / 2;
  for (int row = 0; row < chromaRows; ++row) {
    const uint8_t* uvRow = uvBase + static_cast<size_t>(row) * uvStride;
    for (int col = 0; col < chromaCols; ++col) {
      uDst[static_cast<size_t>(row) * chromaCols + col] = uvRow[2 * col];
      vDst[static_cast<size_t>(row) * chromaCols + col] = uvRow[2 * col + 1];
    }
  }
  return true;
}

// BGRA rows with padding -> tightly packed BGRA (the SCK frame publish shape).
inline void copyBgraTight(const uint8_t* base, size_t stride, int width, int height,
                          std::vector<uint8_t>& out) {
  out.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
  for (int row = 0; row < height; ++row) {
    std::memcpy(out.data() + static_cast<size_t>(row) * width * 4u,
                base + static_cast<size_t>(row) * stride, static_cast<size_t>(width) * 4u);
  }
}

// Color-space hints from a CoreVideo pixel-format FourCC + the matrix
// attachment string, mirroring uvc::deriveYuvColorHints' defaults: unknown
// range -> limited, unknown matrix -> BT.709 at >=720p else BT.601.
struct MacYuvColorHints {
  bool fullRange = false;
  bool bt601 = false;
};

inline MacYuvColorHints deriveMacYuvColorHints(uint32_t pixelFormatFourcc,
                                               const std::string& matrixAttachment, int height) {
  MacYuvColorHints hints;
  // '420f' = kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
  // '420v' = ...VideoRange (studio swing).
  hints.fullRange = pixelFormatFourcc == 0x34323066u /* '420f' */;
  if (matrixAttachment.find("601") != std::string::npos) {
    hints.bt601 = true;
  } else if (matrixAttachment.find("709") != std::string::npos) {
    hints.bt601 = false;
  } else {
    hints.bt601 = height < 720;
  }
  return hints;
}

// ScreenCaptureKit source-id minting, shaped exactly like the WGC adapter's
// ("screen:<n>" / "window:<id>") so a shell treats the two platforms alike.
inline std::string sckScreenId(int index) { return "screen:" + std::to_string(index); }
inline std::string sckWindowId(uint32_t windowId) {
  return "window:" + std::to_string(windowId);
}

inline std::string sckScreenName(int index, int width, int height) {
  return "Display " + std::to_string(index + 1) + " (" + std::to_string(width) + "x" +
         std::to_string(height) + ")";
}

}  // namespace corevideo::modules::maccapture
