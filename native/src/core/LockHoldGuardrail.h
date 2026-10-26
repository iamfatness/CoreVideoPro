#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace corevideo::core {

// #1056: optional attribution for an over-budget hold (the render tick's
// per-phase breakdown). describe() is called only when the guardrail decides
// to log the hold, never for an in-budget or rate-suppressed one.
class LockHoldDetail {
 public:
  virtual ~LockHoldDetail() = default;
  [[nodiscard]] virtual std::string describe(long long heldUs) const = 0;
};

// Phase 2 increment 6 guardrail (docs/phase2-threading-plan.md §2/§3.6).
//
// The threading contract says `coreMutex` protects only fast in-memory state
// mutation + snapshot publication: sub-millisecond holds. All blocking I/O and
// heavy DSP live on dedicated worker threads (render is video-only, the audio/
// output worker owns the long span under `audioOutputMutex_`, the engine sender
// thread owns pipe writes). This guardrail makes that contract observable:
// every instrumented `coreMutex` hold is timed against a per-site budget.
//
// Semantics (all builds, debug and release):
//   - Telemetry counters per site: total holds, over-budget holds, worst hold.
//   - Over-budget holds emit a rate-capped `[lock-guardrail]` stderr warning:
//     first occurrence and a one-minute production aggregate; detailed
//     diagnostics restores one-second observations for an investigation.
//   - Opt-in strict mode: set COREVIDEO_LOCK_GUARDRAIL_STRICT=1 to turn any
//     over-budget hold into an abort (debugging tool — never on by default;
//     timing asserts are too flaky for CI, especially under TSan's 5-20x
//     slowdown, so the default is warn+count, never fail).
//
// Sanctioned long-hold sites declare their own (larger) budgets below; every
// other instrumented site uses the sub-ms default.
class LockHoldGuardrail {
 public:
  struct SiteStats {
    std::uint64_t holds = 0;
    std::uint64_t overBudget = 0;
    std::uint64_t warningsLogged = 0;
    long long worstHeldUs = 0;
    // #1056: over-budget holds that carried a detail and were attributed in a
    // `[lock-hold-attribution]` line (rate-capped at one per second per site),
    // and the last such line's text (tests and diagnostics).
    std::uint64_t attributionsLogged = 0;
    std::string lastAttribution;
  };

  // The threading contract: un-sanctioned coreMutex holds stay under 1ms.
  static constexpr long long kDefaultBudgetUs = 1000;
  // Sanctioned long-hold sites (each is a deliberate design decision):
  //  - cmd.handle: bounded command mutation/metadata capture only. Projection,
  //    capacity waits, serialization and backend device work are off-lock.
  static constexpr long long kCommandHandleBudgetUs = kDefaultBudgetUs;
  //  - render.display-tick: the video-only GPU tick typically holds ~1-2ms;
  //    half a 60fps frame budget is the alarm line.
  static constexpr long long kRenderTickBudgetUs = 8'000;
  //  - cmd.frame-pump: move the optional thumbnail job; encoding is off-lock.
  static constexpr long long kFramePumpBudgetUs = kDefaultBudgetUs;

  // Records one completed hold of `heldUs` at `site` against `budgetUs`.
  // Returns true when the hold exceeded the budget. With a `detail`, an
  // over-budget hold also logs `[lock-hold-attribution]` with the detail's
  // text, at most once per second per site (the one-minute warning cap would
  // hide most of the ~1/min render-tick overruns a soak sees, #1056), and the
  // rate-capped warning line carries the same text.
  static bool recordHold(const char* site, long long heldUs, long long budgetUs,
                         const LockHoldDetail* detail = nullptr);

  [[nodiscard]] static SiteStats statsForSite(const char* site);
  static void resetForTest();
};

// RAII hold timer: construct immediately AFTER acquiring coreMutex, inside the
// lock scope (so it is destroyed — and the hold recorded — just before the lock
// releases). The registry mutex inside recordHold is a leaf lock: it is taken
// while coreMutex is held and never takes any other lock, so it cannot invert.
class ScopedLockHoldTimer {
 public:
  ScopedLockHoldTimer(const char* site, long long budgetUs, const LockHoldDetail* detail = nullptr)
      : site_(site), budgetUs_(budgetUs), detail_(detail), start_(std::chrono::steady_clock::now()) {}
  ~ScopedLockHoldTimer() {
    const auto heldUs = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - start_)
                            .count();
    LockHoldGuardrail::recordHold(site_, static_cast<long long>(heldUs), budgetUs_, detail_);
  }
  [[nodiscard]] std::chrono::steady_clock::time_point startedAt() const { return start_; }

  ScopedLockHoldTimer(const ScopedLockHoldTimer&) = delete;
  ScopedLockHoldTimer& operator=(const ScopedLockHoldTimer&) = delete;

 private:
  const char* site_;
  long long budgetUs_;
  const LockHoldDetail* detail_;
  std::chrono::steady_clock::time_point start_;
};

}  // namespace corevideo::core
