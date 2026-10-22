#pragma once
#include "modules/Interfaces.h"
#include <array>
#include <limits>

namespace corevideo::modules {
// Capture owns at most three original arrivals, matching its preparation pool.
// This is history/lookahead for the existing preparation interval, not another
// playout delay: a choice made in slot N resolves before composition in N+1.
// The source clock chooses an identity, never whichever image happens to be
// latest when the render thread reaches its poll. Missing choices remain absent.
class CapturePresentationWindow {
 public:
  static constexpr size_t kCapacity = 3;
  static bool supportedRate(int numerator, int denominator) {
    // This first consumer qualifies 60 and 60000/1001 capture. Other native
    // rates retain their existing selection until their phase model is tested.
    return numerator > 0 && numerator <= 240000 && denominator > 0 && denominator <= 10000 &&
        (static_cast<int64_t>(numerator) == 60LL * denominator ||
         static_cast<int64_t>(numerator) * 1001 == 60000LL * denominator);
  }
  struct Choice { uint64_t generation = 0, epoch = 0; int64_t frameId = 0; };
  void configure(int numerator, int denominator) {
    std::lock_guard<std::mutex> lock(mutex_);
    numerator_ = numerator; denominator_ = denominator;
    reset();
  }
  void publish(VideoFrame frame) {
    frame.capturePresentation.reset(); // no owner -> descriptor -> owner cycle
    frame.monitorGpuPixels.reset();
    VideoFrame retired;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (latestEpoch_ != frame.sourceEpoch || (latestId_ && frame.frameId <= latestId_)) reset();
      latestEpoch_ = frame.sourceEpoch; latestId_ = frame.frameId;
      auto& destination = arrivals_[write_++ % kCapacity];
      retired = std::move(destination); destination = std::move(frame);
    } // retire CPU storage outside the publication lock
  }
  std::optional<Choice> choose(const VideoFrame& observed, int64_t slot, int64_t anchorNs) {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock() || !validRate() || slot < 0 || anchorNs <= 0 ||
        observed.sourceEpoch != latestEpoch_ || observed.frameId <= 0) return {};
    if (!clocked_ || anchorNs != anchor_ || slot < lastSlot_) {
      anchor_ = anchorNs; firstSlot_ = slot; firstId_ = observed.frameId;
      clocked_ = true;
    }
    lastSlot_ = slot;
    const int64_t delta = slot - firstSlot_;
    if (delta > (std::numeric_limits<int64_t>::max)() / numerator_) return {};
    const int64_t scaled = delta * numerator_, divisor = 60LL * denominator_;
    // Select the next capture at the earliest output slot whose existing
    // preparation interval can contain it. Floor would insert an immediate
    // extra source hold for 59.94 Hz and carry that avoidable age forward.
    const int64_t advance = scaled / divisor + (scaled % divisor != 0);
    if (firstId_ > (std::numeric_limits<int64_t>::max)() - advance) return {};
    const auto id = firstId_ + advance;
    // Demand is allowed to name the next arrival before capture publishes it.
    // The same preparation owner then prepares that exact frame on arrival.
    if (observed.preparedGpu && observed.preparedGpu->demand) {
      const auto& demand = observed.preparedGpu->demand;
      auto old = demand->selectedFrameId.load();
      while (old < id && !demand->selectedFrameId.compare_exchange_weak(old, id)) {}
      demand->lastDemand100ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count() / 100);
    }
    return Choice{generation_, latestEpoch_, id};
  }
  std::optional<VideoFrame> resolve(Choice choice) {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock() || choice.generation != generation_ || choice.epoch != latestEpoch_) return {};
    for (const auto& frame : arrivals_)
      if (frame.frameId == choice.frameId && frame.sourceEpoch == choice.epoch) return frame;
    clocked_ = false; // report this missed slot, then recover instead of permanent black
    return {};
  }
 private:
  bool validRate() const {
    return supportedRate(numerator_, denominator_);
  }
  void reset() {
    for (auto& frame : arrivals_) frame = {};
    write_ = 0; latestEpoch_ = 0; latestId_ = 0; clocked_ = false; ++generation_;
  }
  mutable std::mutex mutex_;
  std::array<VideoFrame, kCapacity> arrivals_;
  size_t write_ = 0;
  int numerator_ = 0, denominator_ = 0;
  uint64_t generation_ = 0, latestEpoch_ = 0;
  int64_t latestId_ = 0, firstId_ = 0, firstSlot_ = 0, lastSlot_ = 0, anchor_ = 0;
  bool clocked_ = false;
};
}
