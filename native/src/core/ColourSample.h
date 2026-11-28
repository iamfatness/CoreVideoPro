#pragma once

// Colour sampling for the eyedropper (graphics styles spec 4.2, slice 3).
// The operator freezes the core's existing Program thumbnail readback (the
// bounded BGRA preview, at most 320x180) and clicks inside that frozen image in
// the colour picker. The pick reads the frozen copy, never a live frame, so the
// colour returned is the colour of the pixel the operator saw. No OS screen
// picker and no input injection are involved. Owned by the core command thread.

#include "rpc/Json.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace corevideo::core {

class ColourSample {
 public:
  static constexpr int kMaxWidth = 320, kMaxHeight = 180;

  // Keeps a BGRA frame and its base64 text (encoded once here, so a snapshot only
  // copies a string). Refuses an empty or oversized frame (`error` names why).
  template <typename Encode>
  bool freeze(const std::string& source, uint64_t frameNumber, int width, int height, std::vector<uint8_t> bgra, Encode encode, std::string& error) {
    if (width <= 0 || height <= 0 || width > kMaxWidth || height > kMaxHeight ||
        bgra.size() != static_cast<size_t>(width) * static_cast<size_t>(height) * 4) {
      error = "colour-sample-no-frame"; return fail(error);
    }
    source_ = source; frameNumber_ = frameNumber; width_ = width; height_ = height; bgra_ = std::move(bgra);
    encoded_ = encode(bgra_.data(), bgra_.size());
    pick_ = rpc::Json{}; error_.clear(); ++revision_;
    return true;
  }

  // Reads the frozen pixel under normalized (x, y) in [0, 1]. `frameNumber` must
  // name the frozen frame, so a pick cannot land on a frame the operator never saw.
  bool pick(double x, double y, uint64_t frameNumber, const std::string& requestId, std::string& error) {
    if (bgra_.empty()) { error = "colour-sample-not-frozen"; return fail(error); }
    if (frameNumber != frameNumber_) { error = "colour-sample-frame-changed"; return fail(error); }
    if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || x > 1 || y < 0 || y > 1) { error = "colour-sample-out-of-frame"; return fail(error); }
    const int px = std::min(width_ - 1, static_cast<int>(std::floor(x * width_)));
    const int py = std::min(height_ - 1, static_cast<int>(std::floor(y * height_)));
    const size_t offset = (static_cast<size_t>(py) * static_cast<size_t>(width_) + static_cast<size_t>(px)) * 4;
    const int b = bgra_[offset], g = bgra_[offset + 1], r = bgra_[offset + 2];
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string value = "#";
    for (const int channel : {r, g, b}) { value += hex[channel >> 4]; value += hex[channel & 15]; }
    pick_ = rpc::Json::Object{{"requestId", requestId}, {"frameNumber", static_cast<double>(frameNumber_)},
        {"x", px}, {"y", py}, {"r", r}, {"g", g}, {"b", b}, {"hex", value}};
    error_.clear(); ++revision_;
    return true;
  }

  void release() {
    if (bgra_.empty() && pick_.isNull()) return;
    bgra_.clear(); bgra_.shrink_to_fit(); encoded_.clear(); encoded_.shrink_to_fit(); source_.clear(); width_ = height_ = 0; frameNumber_ = 0; pick_ = rpc::Json{}; error_.clear(); ++revision_;
  }

  bool frozen() const { return !bgra_.empty(); }
  // Records a refusal decided by the caller (an unsupported source or a malformed command).
  void refuse(const std::string& error) { fail(error); }

  // While frozen the node carries the frozen bytes, so the shell shows exactly the image a pick reads.
  rpc::Json snapshot() const {
    rpc::Json::Object node{{"state", frozen() ? "frozen" : "idle"}, {"revision", static_cast<double>(revision_)},
        {"lastPick", pick_}, {"lastError", error_}};
    if (frozen()) {
      node["source"] = source_; node["frameNumber"] = static_cast<double>(frameNumber_);
      node["width"] = width_; node["height"] = height_; node["pixelFormat"] = "bgra";
      node["bgraBase64"] = encoded_;
    }
    return rpc::Json(std::move(node));
  }

 private:
  bool fail(const std::string& error) { error_ = error; ++revision_; return false; }
  std::string source_, error_, encoded_;
  uint64_t frameNumber_ = 0, revision_ = 0;
  int width_ = 0, height_ = 0;
  std::vector<uint8_t> bgra_;
  rpc::Json pick_;
};

}  // namespace corevideo::core
