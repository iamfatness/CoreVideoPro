#pragma once

// Truthful destination lifecycle decisions, as a PURE state machine.
//
// The vocabulary is the one the execution plan names:
//
//   requested -> preparing -> producing -> stopping -> finalizing
//                                       -> completed | failed | interrupted
//
// Two rules are the whole point of this file, and both are Rule 7 ("a request
// acknowledgement, first frame, or nonzero file does not establish ongoing
// health") applied where it was previously ignored:
//
//  1. `requested` is what an accepted Start command earns. Nothing more. The
//     destination only reaches `preparing` when its writer/sender has actually
//     been asked to open, and `producing` only when OBSERVED output progress
//     exists.
//  2. `producing` requires FRESH progress, re-evaluated at READ time. The old
//     `live` was latched on a request and never re-examined, so a wedged writer
//     reported healthy for the rest of the show. Here, a destination whose last
//     observed progress is older than the staleness budget decays to
//     `interrupted` — it stopped producing, and nobody asked it to. It returns
//     to `producing` the moment real progress resumes; it is terminal only if
//     the take ends there.
//
// The threshold is NOT a new number. `scripts/qa/runtime-snapshot-qualification.mjs`
// already declares `DEFAULT_RUNTIME_POLICY.encoderQueueAgeMs = 1000` as the point
// at which pending encoder work is judged stale, and that judge is the thing that
// has to agree with this state. Reusing it keeps one declared definition of "the
// encoder has stopped moving" instead of two that can drift apart.
//
// Everything here is a value-in/value-out decision with an injected clock, so it
// is testable without a writer, a sender, a disk or a sleep — the same shape as
// CaptureReaderStallPolicy / NativeUvcCapturePolicy on the shell side.

#include <cstdint>
#include <string>

namespace corevideo::core {

// See the note above: same declared budget as the qualification policy's
// encoderQueueAgeMs. Do not lower this to make a flaky rig look green.
inline constexpr std::int64_t kProducingProgressStaleMs = 1000;

struct LifecycleDecision {
  std::string state;
  std::string health;
};

// Observation of an ACTIVE destination (desiredActive == true, not stopping).
struct ActiveOutputObservation {
  // The underlying writer/sender has been handed its start (its open was
  // attempted). False = the start is still only an accepted request.
  bool startApplied = false;
  // At least one unit of real output progress has been observed for this
  // generation (a committed video frame / audio packet / accepted send).
  bool everProgressed = false;
  // Monotonic ms of the most recent observed progress. 0 = never.
  std::int64_t lastProgressMs = 0;
  std::int64_t nowMs = 0;
  std::int64_t staleMs = kProducingProgressStaleMs;
  // A non-fatal condition reported by the destination (e.g. the encoder's
  // recording warning). Degrades health; never fakes a state.
  bool degraded = false;
};

class OutputLifecyclePolicy {
 public:
  // A command was accepted. This is an acknowledgement, not evidence.
  static LifecycleDecision requested() { return {"requested", "unknown"}; }

  static LifecycleDecision idle() { return {"idle", "unknown"}; }

  // Progress is fresh iff it happened, and happened within the budget. A
  // destination that has never progressed is never "fresh" no matter the clock.
  [[nodiscard]] static bool progressIsFresh(const ActiveOutputObservation& o) {
    if (!o.everProgressed) return false;
    if (o.staleMs <= 0) return false;
    if (o.nowMs < o.lastProgressMs) return true;  // clock went backwards: do not accuse
    return (o.nowMs - o.lastProgressMs) <= o.staleMs;
  }

  [[nodiscard]] static LifecycleDecision evaluateActive(const ActiveOutputObservation& o) {
    if (!o.startApplied) return requested();
    if (!o.everProgressed) return {"preparing", "unknown"};
    if (progressIsFresh(o)) return {"producing", o.degraded ? "degraded" : "healthy"};
    // It produced, then stopped producing, and no operator asked it to. This is
    // the case the old `live` reported as healthy forever.
    return {"interrupted", "degraded"};
  }

  // Stop was requested; the barrier has not drained yet. NEVER a terminal claim.
  static LifecycleDecision stopping() { return {"stopping", "unknown"}; }

  // The stop barrier is being applied (the container is being finalized).
  static LifecycleDecision finalizing() { return {"finalizing", "unknown"}; }

  // The stop barrier returned. `producedMedia` is the only thing that can make
  // this a completion: a session that finalized without ever writing media has
  // no artifact to have completed.
  [[nodiscard]] static LifecycleDecision finalized(bool producedMedia) {
    return producedMedia ? LifecycleDecision{"completed", "healthy"}
                         : LifecycleDecision{"failed", "failed"};
  }

  static LifecycleDecision failed() { return {"failed", "failed"}; }

  // True while the destination is still working and MUST NOT be reported as
  // finished — the answer to "may Stop claim completion yet?" is always no here.
  [[nodiscard]] static bool isTerminal(const std::string& state) {
    return state == "completed" || state == "failed";
  }

  [[nodiscard]] static bool claimsCompletion(const std::string& state) {
    return state == "completed";
  }
};

// ---------------------------------------------------------------------------
// Senders (RTMP / SRT / NDI). They report rich free-text status strings today
// and no lifecycle at all, so this maps the one onto the other in ONE place
// rather than teaching three adapters a new state machine each.
// ---------------------------------------------------------------------------
struct SenderObservation {
  std::string status;             // adapter status: idle/starting/live/warning/failed/stopped
  std::string destinationHealth;  // adapter health: starting/ok/warning/failed/stopped
  bool desiredActive = false;     // the destination is configured and expected to run
  std::int64_t framesSent = 0;    // cumulative accepted frames
  bool everProduced = false;      // framesSent has been > 0 at some point this run
  std::int64_t lastProgressMs = 0;
  std::int64_t nowMs = 0;
  std::int64_t staleMs = kProducingProgressStaleMs;
  // NOT a failure signal. `lastError` is a STICKY history field the adapters do
  // not clear when they recover — a healthy SRT sender that is genuinely
  // streaming still carries "waiting for composed BGRA program pixels" from its
  // first tick. Treating it as current state would report every live stream as
  // failed. Failure is `status`/`destinationHealth`; this only supplies context.
  bool hasError = false;
};

class SenderLifecyclePolicy {
 public:
  [[nodiscard]] static LifecycleDecision evaluate(const SenderObservation& o) {
    if (o.status == "failed" || o.destinationHealth == "failed") {
      return OutputLifecyclePolicy::failed();
    }
    if (o.status == "stopped") {
      // A sender has no separate finalize barrier: the adapter's stop closes the
      // transport. It completed iff it ever actually sent something.
      return o.everProduced ? LifecycleDecision{"completed", "healthy"}
                            : LifecycleDecision{"completed", "unknown"};
    }
    if (!o.desiredActive || o.status == "idle" || o.status.empty()) {
      return OutputLifecyclePolicy::idle();
    }
    ActiveOutputObservation active;
    active.startApplied = true;  // the adapter exists and has been synced
    active.everProgressed = o.everProduced;
    active.lastProgressMs = o.lastProgressMs;
    active.nowMs = o.nowMs;
    active.staleMs = o.staleMs;
    active.degraded = o.status == "warning" || o.destinationHealth == "warning";
    return OutputLifecyclePolicy::evaluateActive(active);
  }

  // A sender that has completed or failed is finalized: whatever it was going to
  // deliver, it has delivered. `finalized` on a stream means "this destination's
  // run is over and its outcome is known", exactly as it does for a recording.
  [[nodiscard]] static bool finalizedFor(const LifecycleDecision& decision, bool everProduced) {
    if (decision.state == "completed") return everProduced;
    return false;
  }
};

// ---------------------------------------------------------------------------
// Legacy projections. `recording.status` / `recording.writerStatus` predate the
// lifecycle contract and are still read by the shell, support bundles and the
// TS read models. They are PROJECTIONS of the lifecycle now, never an
// independently assigned field — which is what stops the two disagreeing for
// the whole finalize window the way they used to.
// ---------------------------------------------------------------------------
[[nodiscard]] inline std::string publishedRecordingStatus(const std::string& state,
                                                          const std::string& health) {
  if (state == "producing" || state == "live") return health == "degraded" ? "warning" : "recording";
  if (state == "requested" || state == "preparing" || state == "starting") return "starting";
  if (state == "stopping" || state == "finalizing") return "stopping";
  if (state == "completed") return "stopped";
  if (state == "failed") return "failed";
  if (state == "interrupted") return "interrupted";
  return "idle";
}

[[nodiscard]] inline std::string publishedRecordingWriterStatus(const std::string& state) {
  if (state == "producing" || state == "live") return "writing";
  if (state == "requested" || state == "preparing" || state == "starting") return "opening";
  if (state == "stopping" || state == "finalizing") return "finalizing";
  if (state == "completed") return "stopped";
  if (state == "failed") return "failed";
  if (state == "interrupted") return "stalled";
  return "idle";
}

}  // namespace corevideo::core
