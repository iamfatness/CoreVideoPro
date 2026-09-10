#pragma once

#include <cstdint>

namespace corevideo::core {

// MONITOR LOAD-SHEDDING: Program always renders; the monitors give way.
//
// Program `render()`, `renderMultiview()` and `renderPreview()` share ONE render
// thread and ONE D3D immediate context (MediaCore::renderSyntheticTick). Every
// millisecond a monitor pass overruns is a millisecond Program is off the GPU.
// MonitorRenderFaultInjectionTest measured it on the RTX 4090 rig. At the
// product's 1ms timer resolution a sustained 25ms Preview stall (measured
// ~25.7ms per pass) took Program from 121 to 77 produced frames per 2s — ~36%
// lost, close to the 1 - 16.7/25.7 a stall that long predicts. (The originally
// documented "25ms stall halves Program", 121 -> 65, was taken at the default
// ~15.6ms timer tick, where that seam actually slept ~31ms.) No buffer depth
// fixes it: the frames were never rendered.
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
// A PROGRAM-BOUND OR GPU-BOUND OVERLOAD SHEDS MONITORS TOO, and that is
// intended. The policy only sees CPU-submission time; it cannot tell "Program is
// expensive" from "the monitors are expensive", and it does not try. D3D
// submission is asynchronous: GPU work queued by a monitor pass surfaces LATER,
// inside whichever call next blocks on the device — often Program's own
// render() or its readback on the following tick — so a GPU-bound monitor wall
// frequently shows up as Program cost, not monitor cost. Shedding the monitors
// is the one lever this thread has either way, and it can only give Program
// time back. If the overload really is Program alone, shedding buys little and
// the divisor sits at 3 while it lasts: the monitors pay in smoothness, which is
// the stated priority. The snapshot publishes the programMs / monitorCycleMs
// that triggered the last transition so the two cases are distinguishable
// after the fact.
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
  // budget — NOT 100%. This is JITTER HEADROOM. Under shedding the tick that
  // runs a monitor pass overruns its own slot and finishes late, and the cheap
  // ticks after it have to absorb that lateness plus every scheduler wake-up,
  // pacer, driver and GPU-completion jitter the cycle meets; any jitter bigger
  // than the slack left over loses a Program slot outright, and the cost of a
  // lost slot (a Program frame) is far higher than the cost of shedding one
  // level early (monitor smoothness). 10% of a divisor-2 cycle is ~3.3ms and of
  // a divisor-3 cycle ~5ms — a few times the measured pacer wake-up error.
  // (First measured while the fault harness still ran at the default ~15.6ms
  // timer tick, i.e. with far worse jitter than the product: a cycle projected
  // at 95% held divisor 2 and Program still lost a third of its frames. That
  // run exaggerates the jitter, so it is the illustration, not the evidence;
  // the argument above stands at any resolution.)
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
        lastTransition_ = o;
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
    lastTransition_ = o;
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
  // The observation that caused the last divisor change (all zero before the
  // first). Its programCostNs vs monitorCycleCostNs is what tells a
  // monitor-bound shed from a Program/GPU-bound one.
  [[nodiscard]] const MonitorShedObservation& lastTransitionObservation() const { return lastTransition_; }

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
  MonitorShedObservation lastTransition_{};
};

}  // namespace corevideo::core
