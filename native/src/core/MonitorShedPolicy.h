#pragma once

#include <cstdint>

namespace corevideo::core {

// MONITOR LOAD-SHEDDING: Program always renders; the monitors give way.
//
// Program `render()`, `renderMultiview()` and `renderPreview()` share ONE render
// thread and ONE D3D immediate context (MediaCore::renderSyntheticTick). Every
// millisecond a monitor pass overruns is a millisecond Program is off the GPU.
// MonitorRenderFaultInjectionTest measured it on the RTX 4090 rig: a sustained
// 25ms Preview stall took Program from 121 produced / 124 delivered per 2s to
// 65 / 65 — a Preview monitor that overran by one and a half frame periods cost
// Program HALF its frames. No buffer depth fixes that: the frames were never
// rendered.
//
// The real fix is a separate monitor compositor (post-beta). This is the beta
// mitigation: when the tick cannot fit the Program frame budget, run the monitor
// passes on every 2nd, then every 3rd tick, so Program keeps its rate and the
// monitor wall drops to 30 / 20 fps — which an operator tolerates far better
// than a stuttering Program.
//
// THE MEASURE is the projected per-tick load at a cadence divisor d, against a
// 90% utilisation ceiling (kShedAboveUtilisationPercent, justified below):
//
//     programCost + monitorCycleCost / d   <=   90% of budget
//     (compared as  (d*programCost + monitorCycleCost)*100 <= d*budget*90
//      — integer ops only, no division)
//
// `programCost` is this tick's render work EXCLUDING the monitor passes (ingest,
// plan, the Program composite); `monitorCycleCost` is the most recent measured
// cost of one full set of monitor passes (multiview + preview), which the caller
// refreshes whenever a pass actually runs. Projecting — rather than reacting to
// whether THIS tick happened to run a monitor pass — is what makes the policy
// stable under its own shedding: a shed tick is cheap, and a naive "this tick was
// fast" signal would recover straight back into the overload.
//
// Deliberately NOT an input: the render worker's CPU-deadline misses. At d=2 a
// 25ms preview pass still makes the tick that runs it finish late — that is
// EXPECTED, and the 3-frame program buffer absorbs it because the next tick is
// cheap. Feeding that lateness back would drive every recoverable overload to
// d=3 and hold it there. A miss caused by coreMutex contention is not monitor
// cost either, and shedding monitors would be the wrong answer to it. The
// projection is the direct evidence; the miss counters stay in the snapshot.
//
// Pure value-in/value-out state machine: no clock, no I/O, no allocation — a few
// integer operations per tick under the lock the caller already holds. Same shape
// as OutputLifecyclePolicy / RenderedSceneAttributionPolicy.

struct MonitorShedObservation {
  // Program frame budget: 1s / program fps. <= 0 means "unknown" and the tick is
  // ignored (no evidence either way).
  std::int64_t budgetNs = 0;
  // This tick's render work, monitor passes excluded.
  std::int64_t programCostNs = 0;
  // Most recent cost of one full set of monitor passes (multiview + preview). 0
  // when no monitor pass is configured.
  std::int64_t monitorCycleCostNs = 0;
};

enum class MonitorShedTransition {
  None,
  Enter,     // 1 -> 2: shedding engaged
  StepUp,    // 2 -> 3
  StepDown,  // 3 -> 2
  Exit,      // 2 -> 1: back to every tick
};

class MonitorShedPolicy {
 public:
  // 3 = monitors at 20fps against a 60fps Program. Past this the monitor wall
  // stops reading as live video, and an overload that 1/3 cadence cannot absorb
  // is not a monitor problem this mitigation can solve (true isolation needs
  // the monitor-compositor split).
  static constexpr int kMaxDivisor = 3;
  // 3 consecutive over-budget ticks (50ms at 60fps) before stepping up. One or
  // two slow ticks are a shader compile, a texture allocation, a one-off stall —
  // the program buffer rides those out, and shedding on them would make the
  // monitor wall flicker between rates for nothing. Three in a row is a
  // sustained overload, and costs Program at most ~2 slots before it is shed.
  static constexpr int kEnterAfterOverBudgetTicks = 3;
  // 60 consecutive healthy ticks (one second at 60fps) before stepping DOWN one
  // level. Recovery is deliberately 20x slower than entry: a wrongly-early
  // recovery costs Program frames; a late one costs only monitor smoothness.
  static constexpr int kRecoverAfterHealthyTicks = 60;
  // A cadence counts as over budget once its projected load exceeds 90% of the
  // budget — NOT 100%. The tick that runs a monitor pass overruns its own slot
  // and finishes late; the cheap ticks after it must absorb that plus every
  // scheduler, timer and GPU jitter, and a jitter bigger than the slack left
  // over loses a Program slot outright. Measured on the RTX 4090 rig with the
  // first cut of this policy at 100%, in a harness running at the DEFAULT
  // Windows timer resolution (~15.6ms sleep granularity — a deliberately
  // jittery render thread; the product runs at 1ms): the stall's pass took
  // 30.4ms against 0.7ms of other work, projected 95% at divisor 2 (<2ms of
  // slack per 33.3ms cycle), the policy held 2, and Program still lost a third
  // of its frames (81 produced against 121). 10% headroom sends that load to
  // divisor 3, where the cycle has a full frame period of slack. At the
  // product's 1ms resolution the same seam stalls ~25.7ms, projects ~80% at
  // divisor 2 and holds there (119 of 121 produced).
  static constexpr std::int64_t kShedAboveUtilisationPercent = 90;
  // A tick counts toward recovery only if the load projected at the NEXT LOWER
  // divisor fits in 75% of the budget. Shedding above 90% and recovering only
  // at <=75% is the hysteresis band: without it a load sitting right at the
  // threshold would step down, overrun for 3 ticks, step up, and flap about
  // once a second.
  static constexpr std::int64_t kRecoveryHeadroomPercent = 75;

  [[nodiscard]] static bool overBudgetAt(const MonitorShedObservation& o, int divisor) {
    const std::int64_t d = divisor;
    return (d * clamp(o.programCostNs) + clamp(o.monitorCycleCostNs)) * 100 >
           d * o.budgetNs * kShedAboveUtilisationPercent;
  }

  [[nodiscard]] static bool comfortablyWithinAt(const MonitorShedObservation& o, int divisor) {
    const std::int64_t d = divisor;
    return (d * clamp(o.programCostNs) + clamp(o.monitorCycleCostNs)) * 100 <=
           d * o.budgetNs * kRecoveryHeadroomPercent;
  }

  // Feed one render tick. Returns the transition this tick caused (None almost
  // always); the caller logs on anything else and never per tick.
  MonitorShedTransition observe(const MonitorShedObservation& o) {
    if (o.budgetNs <= 0) {
      return MonitorShedTransition::None;
    }
    if (divisor_ > 1 && shedTicks_ < kCounterCeiling) {
      ++shedTicks_;
    }
    if (overBudgetAt(o, divisor_)) {
      healthyStreak_ = 0;
      if (overStreak_ < kCounterCeiling) ++overStreak_;
      if (overStreak_ >= kEnterAfterOverBudgetTicks && divisor_ < kMaxDivisor) {
        overStreak_ = 0;
        ++divisor_;
        lastReason_ = "over-budget";
        if (divisor_ == 2) {
          if (enteredCount_ < kCounterCeiling) ++enteredCount_;
          return MonitorShedTransition::Enter;
        }
        return MonitorShedTransition::StepUp;
      }
      return MonitorShedTransition::None;
    }
    overStreak_ = 0;
    if (divisor_ == 1) {
      return MonitorShedTransition::None;
    }
    if (!comfortablyWithinAt(o, divisor_ - 1)) {
      // Fits at the current cadence but would not fit comfortably one level
      // down: this is the steady state of a shed that is working. Hold.
      healthyStreak_ = 0;
      return MonitorShedTransition::None;
    }
    if (++healthyStreak_ < kRecoverAfterHealthyTicks) {
      return MonitorShedTransition::None;
    }
    healthyStreak_ = 0;
    --divisor_;
    lastReason_ = "recovered";
    return divisor_ == 1 ? MonitorShedTransition::Exit : MonitorShedTransition::StepDown;
  }

  // 1 = every tick; 2; 3.
  [[nodiscard]] int divisor() const { return divisor_; }
  // 0 = not shedding; 1; 2.
  [[nodiscard]] int level() const { return divisor_ - 1; }
  // Times shedding was ENGAGED (1 -> 2). Steps within a shed do not count.
  [[nodiscard]] std::int64_t enteredCount() const { return enteredCount_; }
  // Ticks observed while shedding (divisor > 1), cumulative.
  [[nodiscard]] std::int64_t shedTicks() const { return shedTicks_; }
  // Why the divisor last changed: "none" | "over-budget" | "recovered".
  [[nodiscard]] const char* lastReason() const { return lastReason_; }

  [[nodiscard]] static const char* transitionName(MonitorShedTransition t) {
    switch (t) {
      case MonitorShedTransition::Enter: return "enter";
      case MonitorShedTransition::StepUp: return "step-up";
      case MonitorShedTransition::StepDown: return "step-down";
      case MonitorShedTransition::Exit: return "exit";
      case MonitorShedTransition::None: break;
    }
    return "none";
  }

 private:
  // Counters saturate rather than wrap; a snapshot must never see one go down.
  static constexpr std::int64_t kCounterCeiling = INT64_C(1) << 62;

  static std::int64_t clamp(std::int64_t ns) { return ns < 0 ? 0 : ns; }

  int divisor_ = 1;
  std::int64_t overStreak_ = 0;
  std::int64_t healthyStreak_ = 0;
  std::int64_t enteredCount_ = 0;
  std::int64_t shedTicks_ = 0;
  const char* lastReason_ = "none";
};

}  // namespace corevideo::core
