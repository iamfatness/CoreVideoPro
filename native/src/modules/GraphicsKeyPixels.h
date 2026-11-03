#pragma once

#include "modules/Interfaces.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace corevideo::modules {

// NDI graphics key (titler spec Phase 1, "NDI fill+key"): the pure rules the
// GPU pass, the output worker and the sender share, kept out of the D3D adapter
// so they are unit testable without a device or a libNDI runtime.
//
// WHAT GOES ON THE KEY: exactly the native graphics instances the On Air strip
// shows on Program — the plan layers GraphicsInstances::append produced, which
// are the only layers carrying a graphicsInstanceId. Legacy scene overlays
// (lower-third assets, captions) and every video layer are NOT key content:
// the key output is "CoreVideo's graphics over a downstream switcher's own
// picture", so a legacy overlay without an instance stays Program-only until it
// is migrated. The rule is deliberately a single predicate so the compositor,
// the tests and the docs cannot disagree about it.
[[nodiscard]] inline bool graphicsKeyLayerSelected(const CompositorRenderPlanLayer& layer) {
  return !layer.graphicsInstanceId.empty() && layer.hasOverlayContent;
}

// ALPHA CONVENTION. The compositor draws graphics with the "over" blend
// (SRC_ALPHA/INV_SRC_ALPHA on colour, ONE/INV_SRC_ALPHA on alpha, and the
// premultiplied variant for the DirectWrite/D2D rasters), so a transparent
// render target accumulates PREMULTIPLIED colour: each channel already holds
// colour * alpha. NDI's BGRA frame carries STRAIGHT (non-premultiplied) alpha,
// so a receiver keying the stream multiplies by alpha itself; shipping the
// premultiplied buffer unchanged would darken every edge twice. This divides
// the colour back out. A fully transparent pixel is published as 0,0,0,0 —
// its colour is undefined after premultiplication and must not leak noise
// into a receiver that ignores alpha. The division rounds to nearest and
// clamps, so alpha 255 is an exact pass-through.
//
// COST. A lower third covers a small band; most rows of a key frame are
// entirely transparent and the band's interior rows are often entirely opaque.
// Each row is classified by one alpha scan first: all-0 rows become a memset,
// all-255 rows a memcpy, and only mixed rows pay the per-pixel divide.
inline void unpremultiplyBgraRow(const uint8_t* src, uint8_t* dst, int width) {
  if (width <= 0) return;
  bool allTransparent = true;
  bool allOpaque = true;
  for (int x = 0; x < width && (allTransparent || allOpaque); ++x) {
    const uint8_t a = src[static_cast<std::size_t>(x) * 4u + 3u];
    allTransparent = allTransparent && a == 0;
    allOpaque = allOpaque && a == 255;
  }
  const std::size_t bytes = static_cast<std::size_t>(width) * 4u;
  if (allTransparent) {
    std::memset(dst, 0, bytes);
    return;
  }
  if (allOpaque) {
    std::memcpy(dst, src, bytes);
    return;
  }
  for (int x = 0; x < width; ++x) {
    const uint8_t a = src[3];
    if (a == 0) {
      dst[0] = dst[1] = dst[2] = dst[3] = 0;
    } else if (a == 255) {
      dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 255;
    } else {
      const unsigned half = a / 2u;
      for (int c = 0; c < 3; ++c) {
        const unsigned value = (static_cast<unsigned>(src[c]) * 255u + half) / a;
        dst[c] = static_cast<uint8_t>(value > 255u ? 255u : value);
      }
      dst[3] = a;
    }
    src += 4; dst += 4;
  }
}

// Whole-frame form over a mapped staging surface (rows may be padded to
// `srcPitch`); the destination is tightly packed width*4 per row.
inline void unpremultiplyBgra(const uint8_t* src, std::size_t srcPitch, int width, int height, uint8_t* dst) {
  for (int y = 0; y < height; ++y) {
    unpremultiplyBgraRow(src + static_cast<std::size_t>(y) * srcPitch,
                         dst + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4u, width);
  }
}

[[nodiscard]] inline std::size_t graphicsKeyFrameBytes(int width, int height) {
  return static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
}

// The output destination id and the sender's snapshot id. One place, so the
// shell's destination list, ModuleComposition's "which destinations are
// network senders" rule and the supervisor's isolation traits agree.
inline constexpr const char* kNdiGraphicsKeyDestination = "ndi-graphics";
inline constexpr const char* kNdiGraphicsKeySenderId = "ndi:graphics";

// A key with no fresh readback for this long is STALE. The sender then ships
// a fully transparent key and says so, instead of re-sending the last graphic
// forever: a downstream keyer would otherwise hold a lower third on air after
// it was cleared, with health still reading live. 250 ms is 15 frames at 60.
inline constexpr std::int64_t kGraphicsKeyStaleAfterMs = 250;

// Recent straight-alpha key frames, tagged with the compositor frame number
// that produced them. With a Program buffer (2-3 frames) the output worker
// sends a DELAYED Program frame; it asks for the key of that same frame number,
// so graphics never lead Program by the buffer depth. Capacity covers the
// deepest buffer (3) plus reader lag; each entry is a full Program-size frame,
// so it is deliberately small (6 x 8 MB at 1080p, 6 x 33 MB at 4K).
class GraphicsKeyRing {
 public:
  static constexpr std::size_t kCapacity = 6;

  // The key output was requested at `nowMs`. Staleness for "no frame ever
  // arrived" counts from here, so a rig that can never build still turns the
  // key transparent instead of leaving the receiver on its last frame.
  void arm(std::int64_t nowMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!armed_) { armed_ = true; armedAtMs_ = nowMs; }
  }

  void publish(GraphicsKeyFrame frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    frames_.push_back(std::move(frame));
    while (frames_.size() > kCapacity) frames_.pop_front();
  }

  // The key for Program frame `programFrameNumber`: the newest entry produced
  // at or before it (the readback can lag the render by a frame). False while
  // the key is still warming (armed < 250 ms ago, nothing published). A
  // selection with `stale` set carries no pixels: the caller sends transparent.
  bool select(std::int64_t programFrameNumber, std::int64_t nowMs, GraphicsKeySelection& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    out = {};
    if (!armed_) return false;
    if (frames_.empty()) {
      if (nowMs - armedAtMs_ < kGraphicsKeyStaleAfterMs) return false;
      out.stale = true;
      return true;
    }
    if (nowMs - frames_.back().publishedMs >= kGraphicsKeyStaleAfterMs) {
      out.stale = true;
      return true;
    }
    const GraphicsKeyFrame* chosen = nullptr;
    for (auto it = frames_.rbegin(); it != frames_.rend(); ++it) {
      if (it->frameNumber <= programFrameNumber) { chosen = &*it; break; }
    }
    // Every entry is NEWER than the requested frame: the ring is shorter than
    // the buffer delay (should not happen at capacity 6 vs depth 3). Use the
    // oldest — the closest available — and report it as not exact.
    if (!chosen) chosen = &frames_.front();
    out.frame = *chosen;
    out.exact = chosen->frameNumber == programFrameNumber;
    return true;
  }

  [[nodiscard]] std::size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return frames_.size();
  }

 private:
  mutable std::mutex mutex_;
  std::deque<GraphicsKeyFrame> frames_;
  bool armed_ = false;
  std::int64_t armedAtMs_ = 0;
};

// Retry latch for building the key's GPU resources. A persistent failure
// (driver refusal, TDR, out of memory) must not rebuild a D3D device on the
// render thread every 60 Hz tick or log on every attempt: a failed size is
// retried only after kRetryAfterMs, a NEW size is tried at once, and each
// failing size is logged once until a build succeeds again.
class GraphicsKeyRetryLatch {
 public:
  static constexpr std::int64_t kRetryAfterMs = 5000;

  [[nodiscard]] bool shouldAttempt(int width, int height, std::int64_t nowMs) const {
    return !(failed_ && width == failedW_ && height == failedH_ && nowMs - failedAtMs_ < kRetryAfterMs);
  }
  // Returns true when this failure should be logged.
  bool noteFailure(int width, int height, std::int64_t nowMs) {
    failed_ = true; failedW_ = width; failedH_ = height; failedAtMs_ = nowMs;
    const bool log = !(loggedW_ == width && loggedH_ == height);
    loggedW_ = width; loggedH_ = height;
    return log;
  }
  void noteSuccess() {
    failed_ = false;
    loggedW_ = loggedH_ = -1;
  }

 private:
  bool failed_ = false;
  int failedW_ = 0, failedH_ = 0;
  std::int64_t failedAtMs_ = 0;
  int loggedW_ = -1, loggedH_ = -1;
};

// Output worker: put the selected key (or a transparent one) on the frame the
// "ndi-graphics" sender will see. `transparentCache` is reused across ticks so
// a stale key costs no allocation after the first.
inline void stampGraphicsKey(ProgramFrame& frame, const GraphicsKeySelection& selection,
                             std::shared_ptr<const std::vector<uint8_t>>& transparentCache) {
  if (!selection.stale && selection.frame.bgra && selection.frame.width > 0 && selection.frame.height > 0) {
    frame.graphicsKeyBgraShared = selection.frame.bgra;
    frame.graphicsKeyWidth = selection.frame.width;
    frame.graphicsKeyHeight = selection.frame.height;
    frame.graphicsKeyFrameNumber = selection.frame.frameNumber;
    frame.graphicsKeyStale = false;
    return;
  }
  const int width = selection.frame.width > 0 ? selection.frame.width : frame.width;
  const int height = selection.frame.height > 0 ? selection.frame.height : frame.height;
  const auto bytes = graphicsKeyFrameBytes(width, height);
  if (!transparentCache || transparentCache->size() != bytes) {
    try {
      transparentCache = std::make_shared<const std::vector<uint8_t>>(bytes, uint8_t{0});
    } catch (...) {
      transparentCache.reset();
    }
  }
  frame.graphicsKeyBgraShared = transparentCache;
  frame.graphicsKeyWidth = transparentCache ? width : 0;
  frame.graphicsKeyHeight = transparentCache ? height : 0;
  frame.graphicsKeyFrameNumber = -1;
  frame.graphicsKeyStale = true;
}

// What the graphics-key sender does with a frame, as data, so the decision is
// testable without a libNDI runtime.
struct GraphicsKeySendVerdict {
  bool send = false;
  std::string status;      // starting | live | warning
  std::string resultCode;  // key-pixels-missing | key-stale | ok
  std::string warning;
};

[[nodiscard]] inline GraphicsKeySendVerdict graphicsKeySendVerdict(const ProgramFrame& frame) {
  const bool hasPixels = frame.graphicsKeyBgraShared && frame.graphicsKeyWidth > 0 && frame.graphicsKeyHeight > 0 &&
                         frame.graphicsKeyBgraShared->size() >=
                             graphicsKeyFrameBytes(frame.graphicsKeyWidth, frame.graphicsKeyHeight);
  if (!hasPixels) {
    return {false, "starting", "key-pixels-missing", "NDI graphics key is waiting for the graphics layer readback."};
  }
  if (frame.graphicsKeyStale) {
    return {true, "warning", "key-stale",
            "NDI graphics key has no fresh graphics frame (over 250 ms); sending a transparent key."};
  }
  return {true, "live", "ok", ""};
}

}  // namespace corevideo::modules
