#pragma once

#include "core/LockHoldGuardrail.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <string>

namespace corevideo::core {

// #1056 / #1049: attribution for an over-budget `render.display-tick` hold.
//
// The 2026-10-25 soak lost a Program frame to a 15,127 us display-tick hold
// and nothing said where the time went: the stage averages only exist with
// verbose logging and average away a single slow tick. This records, for
// EVERY production display tick, how long each phase of the tick took and how
// long the three sanctioned frame-group engine calls (R1/R2/R5) waited for
// ZoomEngineRuntime::mutex_. It is cheap (one steady_clock read per phase, a
// fixed array, no allocation) and is only formatted when the guardrail decides
// to log an over-budget hold.
//
// Ownership: the render loop owns one instance on its stack and installs it
// on the render thread with a Scope for the duration of the tick. MediaCore
// marks phases through current(), so command-lane/test callers of
// renderSyntheticTick (no Scope installed) record nothing and never race the
// render thread's instance.
class RenderTickPhases final : public LockHoldDetail {
 public:
  enum class Phase : std::uint8_t {
    Setup,         // production timing + beginMonitorTick, before the tick body
    R1Poll,        // pollCompositorVideoFrames (engine mutex, frame group)
    R2Decoded,     // latestDecodedVideoFrames (engine mutex, frame group)
    BusTap,        // per-frame collection + syncZoomParticipantSources
    CapturePoll,   // slate, capture delivery and demand, browser poll
    Gather,        // gatherSourceVideo + guest A/V trim
    StillsFloor,   // still media, cold media, floor observation
    R5CpuArrival,  // appendCpuVideoFrames (engine mutex, frame group)
    IsoIngress,    // capture CPU take + ISO video ingress
    Plan,          // overlay, wall ages, plans, continuity, monitor submit
    Program,       // compositor render + delivery attribution
    Monitors,      // multiview + preview passes (or isolated monitor results)
    Emit,          // shared-texture events, shed policy and the tick tail
    kCount
  };
  static constexpr std::size_t kPhaseCount = static_cast<std::size_t>(Phase::kCount);

  static const char* name(Phase phase) {
    static constexpr std::array<const char*, kPhaseCount> kNames{
        "setup", "r1-poll", "r2-decoded", "bus-tap", "capture-poll", "gather", "stills-floor",
        "r5-cpu-arrival", "iso-ingress", "plan", "program", "monitors", "emit"};
    return kNames[static_cast<std::size_t>(phase)];
  }

  // Starts a tick: clears the phases and stamps the first mark at `now`.
  void begin(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
    phaseNs_.fill(0);
    engineWaitNs_.fill(0);
    visited_ = 0;
    last_ = now;
    started_ = true;
  }
  // Charges the time since the previous mark to `phase`.
  void mark(Phase phase, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
    if (!started_) return;
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - last_).count();
    if (ns > 0) phaseNs_[static_cast<std::size_t>(phase)] += ns;
    visited_ |= 1u << static_cast<unsigned>(phase);
    last_ = now;
  }
  // The part of `phase` spent waiting for ZoomEngineRuntime::mutex_.
  void addEngineWait(Phase phase, std::int64_t ns) {
    if (ns > 0) engineWaitNs_[static_cast<std::size_t>(phase)] += ns;
  }
  // Work the render thread did BEFORE the hold (not part of it): the engine
  // pre-read and the coreMutex wait. Reported beside the hold, never in it.
  void setOutsideHold(std::int64_t preReadNs, std::int64_t lockWaitNs) {
    preReadNs_ = preReadNs;
    lockWaitNs_ = lockWaitNs;
  }
  void setSlot(std::int64_t slot) { slot_ = slot; }

  [[nodiscard]] std::int64_t phaseNs(Phase phase) const { return phaseNs_[static_cast<std::size_t>(phase)]; }
  [[nodiscard]] std::int64_t engineWaitNs(Phase phase) const {
    return engineWaitNs_[static_cast<std::size_t>(phase)];
  }
  // True once mark(phase) ran this tick (even for a sub-tick-resolution phase).
  [[nodiscard]] bool visited(Phase phase) const { return (visited_ >> static_cast<unsigned>(phase)) & 1u; }
  [[nodiscard]] std::int64_t attributedNs() const {
    std::int64_t total = 0;
    for (const auto ns : phaseNs_) total += ns;
    return total;
  }
  [[nodiscard]] std::int64_t frameGroupEngineWaitNs() const {
    return engineWaitNs(Phase::R1Poll) + engineWaitNs(Phase::R2Decoded) + engineWaitNs(Phase::R5CpuArrival);
  }

  // "slot=N phases_us=[setup=12 r1-poll=3/wait=0 ...] other_us=N frame_group_engine_wait_us=N
  //  outside_hold_us=[pre-read=N lock-wait=N]". Phases under 1 us are omitted.
  [[nodiscard]] std::string describe(long long heldUs) const override {
    std::string text = "slot=" + std::to_string(slot_) + " phases_us=[";
    bool first = true;
    for (std::size_t i = 0; i < kPhaseCount; ++i) {
      const auto us = phaseNs_[i] / 1000;
      const auto waitUs = engineWaitNs_[i] / 1000;
      if (us <= 0 && waitUs <= 0) continue;
      if (!first) text += ' ';
      first = false;
      text += name(static_cast<Phase>(i));
      text += '=';
      text += std::to_string(us);
      const auto phase = static_cast<Phase>(i);
      if (phase == Phase::R1Poll || phase == Phase::R2Decoded || phase == Phase::R5CpuArrival) {
        text += "/wait=";
        text += std::to_string(waitUs);
      }
    }
    const long long otherUs = heldUs - attributedNs() / 1000;
    text += "] other_us=" + std::to_string(otherUs > 0 ? otherUs : 0) +
            " frame_group_engine_wait_us=" + std::to_string(frameGroupEngineWaitNs() / 1000) +
            " outside_hold_us=[pre-read=" + std::to_string(preReadNs_ / 1000) +
            " lock-wait=" + std::to_string(lockWaitNs_ / 1000) + "]";
    return text;
  }

  // The instance installed on this thread, or nullptr.
  static RenderTickPhases*& current() {
    thread_local RenderTickPhases* installed = nullptr;
    return installed;
  }
  static void markCurrent(Phase phase) {
    if (auto* phases = current()) phases->mark(phase);
  }

  class Scope {
   public:
    explicit Scope(RenderTickPhases& phases) : previous_(current()) { current() = &phases; }
    ~Scope() { current() = previous_; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

   private:
    RenderTickPhases* previous_;
  };

 private:
  std::array<std::int64_t, kPhaseCount> phaseNs_{};
  std::array<std::int64_t, kPhaseCount> engineWaitNs_{};
  std::chrono::steady_clock::time_point last_{};
  std::int64_t preReadNs_ = 0;
  std::int64_t lockWaitNs_ = 0;
  std::int64_t slot_ = -1;
  std::uint32_t visited_ = 0;
  bool started_ = false;
};

}  // namespace corevideo::core
