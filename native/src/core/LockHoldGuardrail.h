#pragma once

#include <chrono>
#include <cstdint>

namespace corevideo::core {

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
//   - Over-budget holds emit a rate-capped `[lock-guardrail]` stderr warning
//     (first occurrence, then at most one per second per site, with the
//     suppressed count) so a regression is visible without flooding the log.
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
  };

  // The threading contract: un-sanctioned coreMutex holds stay under 1ms.
  static constexpr long long kDefaultBudgetUs = 1000;
  // Sanctioned long-hold sites (each is a deliberate design decision):
  //  - cmd.handle: a command-carrying media-core-sync legitimately renders a
  //    capped catch-up of synthetic ticks under the lock (plan §6 increment 2);
  //    the existing `[cmd] held core lock` warning fires at 30ms, so the
  //    guardrail budget sits above it as a regression backstop.
  static constexpr long long kCommandHandleBudgetUs = 50'000;
  //  - render.display-tick: the video-only GPU tick typically holds ~1-2ms;
  //    half a 60fps frame budget is the alarm line.
  static constexpr long long kRenderTickBudgetUs = 8'000;
  //  - cmd.frame-pump: base64 preview stringify/enqueue on the throttled pump.
  static constexpr long long kFramePumpBudgetUs = 8'000;

  // Records one completed hold of `heldUs` at `site` against `budgetUs`.
  // Returns true when the hold exceeded the budget.
  static bool recordHold(const char* site, long long heldUs, long long budgetUs);

  [[nodiscard]] static SiteStats statsForSite(const char* site);
  static void resetForTest();
};

// RAII hold timer: construct immediately AFTER acquiring coreMutex, inside the
// lock scope (so it is destroyed — and the hold recorded — just before the lock
// releases). The registry mutex inside recordHold is a leaf lock: it is taken
// while coreMutex is held and never takes any other lock, so it cannot invert.
class ScopedLockHoldTimer {
 public:
  ScopedLockHoldTimer(const char* site, long long budgetUs)
      : site_(site), budgetUs_(budgetUs), start_(std::chrono::steady_clock::now()) {}
  ~ScopedLockHoldTimer() {
    const auto heldUs = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - start_)
                            .count();
    LockHoldGuardrail::recordHold(site_, static_cast<long long>(heldUs), budgetUs_);
  }

  ScopedLockHoldTimer(const ScopedLockHoldTimer&) = delete;
  ScopedLockHoldTimer& operator=(const ScopedLockHoldTimer&) = delete;

 private:
  const char* site_;
  long long budgetUs_;
  std::chrono::steady_clock::time_point start_;
};

}  // namespace corevideo::core
