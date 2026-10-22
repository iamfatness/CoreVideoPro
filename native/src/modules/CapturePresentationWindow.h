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
    std::lock_guard<std::mutex> lock(writerMutex_);
    state_.numerator = numerator; state_.denominator = denominator;
    reset();
    expose();
  }
  void publish(VideoFrame frame) {
    frame.capturePresentation.reset(); // no owner -> descriptor -> owner cycle
    frame.monitorGpuPixels.reset();
    std::lock_guard<std::mutex> lock(writerMutex_);
    if (state_.latestEpoch != frame.sourceEpoch || (state_.latestId && frame.frameId <= state_.latestId)) reset();
    state_.latestEpoch = frame.sourceEpoch; state_.latestId = frame.frameId;
    state_.arrivals[state_.write++ % kCapacity] = std::move(frame);
    expose();
  }
  std::optional<Choice> choose(const VideoFrame& observed, int64_t slot, int64_t anchorNs) {
    const auto state = std::atomic_load(&published_);
    if (!state || !supportedRate(state->numerator, state->denominator) || slot < 0 || anchorNs <= 0 ||
        observed.sourceEpoch != state->latestEpoch || observed.frameId <= 0) return {};
    if (!clocked_ || clockGeneration_ != state->generation || anchorNs != anchor_ || slot < lastSlot_) {
      anchor_ = anchorNs; firstSlot_ = slot; firstId_ = observed.frameId;
      clocked_ = true; clockGeneration_ = state->generation;
    }
    lastSlot_ = slot;
    const int64_t delta = slot - firstSlot_;
    if (delta > (std::numeric_limits<int64_t>::max)() / state->numerator) return {};
    const int64_t scaled = delta * state->numerator, divisor = 60LL * state->denominator;
    // Sample the source clock at this production slot. Rounding forward names
    // a future capture instead: near a 59.94/60 phase boundary, ordinary camera
    // arrival jitter plus GPU preparation can then exceed the reserved budget.
    // Floor places the required rate-conversion hold at the beginning of the
    // cycle, keeping preparation lookahead inside the original output deadline.
    const int64_t advance = scaled / divisor;
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
    return Choice{state->generation, state->latestEpoch, id};
  }
  std::optional<VideoFrame> resolve(Choice choice, bool final = true) {
    const auto state = std::atomic_load(&published_);
    if (!state || choice.generation != state->generation || choice.epoch != state->latestEpoch) return {};
    for (const auto& frame : state->arrivals)
      if (frame.frameId == choice.frameId && frame.sourceEpoch == choice.epoch) return frame;
    if (final) clocked_ = false; // temporary readiness probes must not reset cadence
    return {};
  }
 private:
  struct State {
    std::array<VideoFrame, kCapacity> arrivals;
    size_t write = 0;
    int numerator = 0, denominator = 0;
    uint64_t generation = 0, latestEpoch = 0;
    int64_t latestId = 0;
  };
  void expose() { std::atomic_store(&published_, std::make_shared<const State>(state_)); }
  void reset() {
    for (auto& frame : state_.arrivals) frame = {};
    state_.write = 0; state_.latestEpoch = 0; state_.latestId = 0; ++state_.generation;
  }
  // Capture serializes only its own publication/configuration. Program reads an
  // immutable three-arrival snapshot and exclusively owns the cadence clock.
  std::mutex writerMutex_;
  State state_;
  std::shared_ptr<const State> published_;
  uint64_t clockGeneration_ = 0;
  int64_t firstId_ = 0, firstSlot_ = 0, lastSlot_ = 0, anchor_ = 0;
  bool clocked_ = false;
};
}
