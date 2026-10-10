#include "modules/UvcCaptureSupport.h"
#include "modules/Sha256.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace corevideo::modules::uvc {
namespace {

// ---------------------------------------------------------------------------
// Compact portable SHA-256 (FIPS 180-4). Kept local so the stable device-id
// hash is testable in every build (stub included) without a crypto-library
// dependency; the id must match the WinUI shell's SHA256-based
// CreateStableDeviceId byte for byte.
// ---------------------------------------------------------------------------

using hashing::sha256;

}  // namespace

double uvcCandidateFps(const UvcFormatCandidate& candidate) {
  if (candidate.fpsDenominator <= 0 || candidate.fpsNumerator <= 0) {
    return 0.0;
  }
  return static_cast<double>(candidate.fpsNumerator) / static_cast<double>(candidate.fpsDenominator);
}

int uvcSubtypeRank(const std::string& fourcc) {
  if (fourcc == "NV12") {
    return 0;
  }
  if (fourcc == "YUY2") {
    return 1;
  }
  if (fourcc == "MJPG") {
    return 2;
  }
  return 3;
}

int pickBestUvcFormat(
    const std::vector<UvcFormatCandidate>& candidates,
    int targetWidth,
    int targetHeight,
    double targetFps) {
  if (candidates.empty()) {
    return -1;
  }

  // Small tolerance so 59.94 counts as reaching a 60fps target.
  const double fpsFloor = targetFps * 0.98;

  const auto better = [&](const UvcFormatCandidate& lhs, const UvcFormatCandidate& rhs) {
    // 1. Resolution class: fits-under-target beats oversized.
    const bool lhsFits = lhs.width <= targetWidth && lhs.height <= targetHeight;
    const bool rhsFits = rhs.width <= targetWidth && rhs.height <= targetHeight;
    if (lhsFits != rhsFits) {
      return lhsFits;
    }
    const int64_t lhsArea = static_cast<int64_t>(lhs.width) * static_cast<int64_t>(lhs.height);
    const int64_t rhsArea = static_cast<int64_t>(rhs.width) * static_cast<int64_t>(rhs.height);
    if (lhsArea != rhsArea) {
      // Under the target: bigger is better. Over the target: smaller is better.
      return lhsFits ? lhsArea > rhsArea : lhsArea < rhsArea;
    }

    // 2. Frame rate: reaching the target beats missing it; among those that
    // reach it, the closest from above; otherwise the fastest available.
    const double lhsFps = uvcCandidateFps(lhs);
    const double rhsFps = uvcCandidateFps(rhs);
    const bool lhsReaches = lhsFps >= fpsFloor;
    const bool rhsReaches = rhsFps >= fpsFloor;
    if (lhsReaches != rhsReaches) {
      return lhsReaches;
    }
    if (lhsFps != rhsFps) {
      return lhsReaches ? lhsFps < rhsFps : lhsFps > rhsFps;
    }

    // 3. Subtype preference.
    return uvcSubtypeRank(lhs.fourcc) < uvcSubtypeRank(rhs.fourcc);
  };

  int bestIndex = 0;
  for (int index = 1; index < static_cast<int>(candidates.size()); ++index) {
    if (better(candidates[static_cast<size_t>(index)], candidates[static_cast<size_t>(bestIndex)])) {
      bestIndex = index;
    }
  }
  return bestIndex;
}

void nv12ToI420(
    const uint8_t* yPlane,
    int yStride,
    const uint8_t* uvPlane,
    int uvStride,
    int width,
    int height,
    std::vector<uint8_t>& out) {
  if (!yPlane || !uvPlane || width <= 0 || height <= 0 || (width & 1) != 0 || (height & 1) != 0) {
    out.clear();
    return;
  }
  const size_t lumaBytes = static_cast<size_t>(width) * static_cast<size_t>(height);
  const size_t chromaBytes = lumaBytes / 4;
  out.resize(lumaBytes + chromaBytes * 2);

  uint8_t* dstY = out.data();
  for (int row = 0; row < height; ++row) {
    std::memcpy(dstY + static_cast<size_t>(row) * static_cast<size_t>(width),
                yPlane + static_cast<size_t>(row) * static_cast<size_t>(yStride),
                static_cast<size_t>(width));
  }

  uint8_t* dstU = out.data() + lumaBytes;
  uint8_t* dstV = dstU + chromaBytes;
  const int chromaRows = height / 2;
  const int chromaCols = width / 2;
  for (int row = 0; row < chromaRows; ++row) {
    const uint8_t* src = uvPlane + static_cast<size_t>(row) * static_cast<size_t>(uvStride);
    uint8_t* uRow = dstU + static_cast<size_t>(row) * static_cast<size_t>(chromaCols);
    uint8_t* vRow = dstV + static_cast<size_t>(row) * static_cast<size_t>(chromaCols);
    for (int col = 0; col < chromaCols; ++col) {
      uRow[col] = src[col * 2];
      vRow[col] = src[col * 2 + 1];
    }
  }
}

void yuy2ToI420(
    const uint8_t* source,
    int sourceStride,
    int width,
    int height,
    std::vector<uint8_t>& out) {
  if (!source || width <= 0 || height <= 0 || (width & 1) != 0 || (height & 1) != 0) {
    out.clear();
    return;
  }
  const size_t lumaBytes = static_cast<size_t>(width) * static_cast<size_t>(height);
  const size_t chromaBytes = lumaBytes / 4;
  out.resize(lumaBytes + chromaBytes * 2);

  uint8_t* dstY = out.data();
  uint8_t* dstU = out.data() + lumaBytes;
  uint8_t* dstV = dstU + chromaBytes;
  const int chromaCols = width / 2;

  for (int row = 0; row < height; ++row) {
    const uint8_t* src = source + static_cast<size_t>(row) * static_cast<size_t>(sourceStride);
    uint8_t* yRow = dstY + static_cast<size_t>(row) * static_cast<size_t>(width);
    for (int col = 0; col < chromaCols; ++col) {
      yRow[col * 2] = src[col * 4];
      yRow[col * 2 + 1] = src[col * 4 + 2];
    }
    if ((row & 1) == 0) {
      // 4:2:2 -> 4:2:0: average this row's chroma with the next row's.
      const uint8_t* next = (row + 1 < height)
                                ? source + static_cast<size_t>(row + 1) * static_cast<size_t>(sourceStride)
                                : src;
      uint8_t* uRow = dstU + static_cast<size_t>(row / 2) * static_cast<size_t>(chromaCols);
      uint8_t* vRow = dstV + static_cast<size_t>(row / 2) * static_cast<size_t>(chromaCols);
      for (int col = 0; col < chromaCols; ++col) {
        const int u = (static_cast<int>(src[col * 4 + 1]) + static_cast<int>(next[col * 4 + 1]) + 1) / 2;
        const int v = (static_cast<int>(src[col * 4 + 3]) + static_cast<int>(next[col * 4 + 3]) + 1) / 2;
        uRow[col] = static_cast<uint8_t>(u);
        vRow[col] = static_cast<uint8_t>(v);
      }
    }
  }
}

std::string stableCaptureDeviceIdFromSymbolicLink(const std::string& symbolicLink) {
  if (symbolicLink.empty()) {
    return "";
  }
  const auto digest = sha256(reinterpret_cast<const uint8_t*>(symbolicLink.data()), symbolicLink.size());
  static const char kHex[] = "0123456789abcdef";
  std::string result;
  result.reserve(16);
  for (size_t i = 0; i < 8; ++i) {
    result.push_back(kHex[(digest[i] >> 4) & 0x0f]);
    result.push_back(kHex[digest[i] & 0x0f]);
  }
  return result;
}

YuvColorHints deriveYuvColorHints(int nominalRange, int transferMatrix, int frameHeight) {
  YuvColorHints hints;
  // MFNominalRange: 1 = MFNominalRange_Normal (0-255), 2 = MFNominalRange_Wide
  // (16-235). Anything else (including unknown) stays limited — the safe
  // broadcast default for capture hardware.
  hints.fullRange = nominalRange == 1;
  // MFVideoTransferMatrix: 1 = BT.709, 2 = BT.601. Unknown falls back by
  // resolution: HD content is BT.709, SD is BT.601.
  if (transferMatrix == 2) {
    hints.bt601 = true;
  } else if (transferMatrix == 1) {
    hints.bt601 = false;
  } else {
    hints.bt601 = frameHeight > 0 && frameHeight < 720;
  }
  return hints;
}

bool uvcNoFirstFrameTimedOut(int64_t frameId, int64_t elapsedMs, int64_t timeoutMs) {
  if (frameId > 0) {
    return false;  // a first frame already arrived — the device is healthy.
  }
  return elapsedMs >= timeoutMs;
}

std::string uvcNoFirstFrameWarning(const std::string& deviceName, int64_t timeoutMs) {
  const std::string name = deviceName.empty() ? "Camera" : deviceName;
  return name + " connected but delivered no frames within " +
         std::to_string(timeoutMs / 1000) +
         "s — the device may be in use by another app (Zoom, Camera Hub, OBS) or "
         "have no input signal.";
}

}  // namespace corevideo::modules::uvc
