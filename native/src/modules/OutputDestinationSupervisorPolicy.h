#pragma once

// PR19 — the DECISION half of the output supervisor, as a pure state machine.
//
// One destination failing must not take a show down on a machine we cannot see.
// Everything about *when* to restart a destination, *when* to stop trying, and
// *whether a destination is actually healthy* lives here: value in, value out,
// injected clock, no threads, no processes, no sleeps. The mechanism half
// (threads, generations, adapter calls) is OutputDestinationSupervisor.h.
//
// ---------------------------------------------------------------------------
// The ladder: the house ladder, not a fifth one
// ---------------------------------------------------------------------------
// BrowserHostRestartPolicy, PluginHostRespawnPolicy, ShowEngineRestartPolicy and
// MediaCoreSupervisor already agree, so this adopts them verbatim rather than
// inventing a variant an operator would have to learn separately:
//
//   * escalating backoff 5 -> 10 -> 20 -> 40 -> 60 s,
//   * GIVE UP after 5 consecutive failures — leave the destination loudly
//     failed rather than churning a restart loop for the rest of the show,
//   * reset the budget after a healthy RUN (not a healthy moment),
//   * an operator action always clears give-up (recover / re-arm).
//
// ---------------------------------------------------------------------------
// What "healthy" means for a DESTINATION
// ---------------------------------------------------------------------------
// Rule 7 is binding: an acknowledgement or a first frame does not establish
// ongoing health. The four existing supervisors each pick the freshest evidence
// their child can produce — a real browser frame, a 30 s run, handshake plus a
// 60 s run, the engine's own status. A destination's equivalent is:
//
//     ACCEPTED UNITS ADVANCING, RE-EVALUATED AT READ TIME.
//
// `acceptedUnits` is the adapter's cumulative accepted video frames plus
// accepted audio frames — the only number in the system that can only move when
// the transport actually took a byte from us. A launched FFmpeg child, an
// NDIlib_send_create that returned non-null, or a status string reading "live"
// are all *launches*, and each of them has already been observed to persist
// after the destination stopped working. The counter cannot.
//
// It is deliberately NOT destination receipt: nothing local can prove an RTMP
// ingest is still accepting. It is local acceptance, which is the strongest
// evidence available in-process, and the adapters already label it honestly
// ("these counters prove local FFmpeg input acceptance, not destination
// receipt"). The supervisor therefore claims exactly that much and no more.
//
// Two different time budgets, because they answer two different questions:
//
//   kProgressStaleMs (1000 ms) — "is it producing RIGHT NOW?" This is the same
//     declared number as core::kProducingProgressStaleMs / the qualification
//     policy's encoderQueueAgeMs. Health and the truthful lifecycle must agree
//     on the instant a destination stops producing, so this is not a new
//     threshold; it is the existing one, reused.
//
//   kStalledRestartMs (5000 ms) — "is it coming back on its own?" Restarting an
//     encoder child on a 1.2 s hiccup is worse than the hiccup: it costs a
//     reconnect and a keyframe. A destination decays out of `producing` at one
//     second (visible immediately, honestly) and is only *acted on* at five.
//
//   kHealthyRunMs (30000 ms) — "has it earned its budget back?" Same number as
//     PluginHostRespawnPolicy. A run resets the failure streak only if it is
//     BOTH producing at this instant AND has been alive this long. A restart
//     that produces one frame and dies must climb the ladder, not reset it.
//
//   kStartGraceMs (15000 ms) — "did it ever come up?" A destination that has
//     never produced a single unit within the grace after its generation began
//     is a failed start, not a slow one. FFmpeg's connect plus the first
//     keyframe fits comfortably inside this.
//
// ---------------------------------------------------------------------------
// Terminal failures bypass the ladder
// ---------------------------------------------------------------------------
// The show engine's exit 78 is the model: a configuration the destination will
// reject again is not worth five retries at escalating cost. An unreachable
// endpoint is retryable — the network may come back. An inadmissible
// configuration (missing endpoint, invalid settings, an illegal NDI source
// name, an absent runtime, a protocol this build does not carry) is not: the
// only thing that can change it is an operator, and an operator action is
// exactly what clears give-up. Terminal failures go straight to gave-up with
// their reason, without burning the retry budget or waiting out a backoff.

#include <algorithm>
#include <cstdint>
#include <string>

namespace corevideo::modules {

enum class DestinationFailureClass {
  None,
  Retryable,
  Terminal,
};

[[nodiscard]] inline const char* toString(DestinationFailureClass value) {
  switch (value) {
    case DestinationFailureClass::Retryable:
      return "retryable";
    case DestinationFailureClass::Terminal:
      return "terminal";
    case DestinationFailureClass::None:
    default:
      return "none";
  }
}

// The adapters' `lastResultCode` vocabulary, partitioned by "will retrying this
// plausibly produce a different answer?". Anything unrecognised is treated as
// RETRYABLE: guessing "terminal" would silently stop protecting a destination,
// which is the failure this whole item exists to prevent.
[[nodiscard]] inline bool isTerminalResultCode(const std::string& code) {
  return code == "endpoint-missing" ||            // nothing to connect to
         code == "rtmp-settings-missing" ||       // no configuration at all
         code == "rtmp-settings-invalid" ||       // configuration the sender rejects
         code == "source-name-invalid" ||         // NDI name the SDK will reject again
         code == "runtime-missing" ||             // FFmpeg / libNDI is not installed
         code == "ffmpeg-missing" ||              // the executable is not there
         code == "unsupported-kind" ||            // this build does not carry it
         (code.size() > 19 /*strlen("-output-unavailable")*/ &&
          code.compare(code.size() - 19, 19, "-output-unavailable") == 0);
}

struct DestinationObservation {
  // The operator has this destination selected. False means "not running by
  // design": never a fault, and never a restart.
  bool desiredActive = false;
  // The adapter published a record for this destination this cycle. False means
  // no evidence arrived (a wedged writer publishes nothing new) — which is NOT
  // itself a fault, because staleness is judged on acceptedUnits, not on
  // whether a snapshot happened to be produced.
  bool observed = false;
  // Cumulative accepted video + audio units for the CURRENT generation. Only
  // this can establish health.
  std::int64_t acceptedUnits = 0;
  std::string status;             // adapter status: idle/starting/live/warning/failed/stopped
  std::string destinationHealth;  // adapter health: starting/ok/warning/failed/stopped
  std::string lastResultCode;
  std::string lastError;
  std::int64_t nowMs = 0;
};

enum class SupervisorAction {
  None,
  Restart,  // bump the generation and re-open this destination NOW
  GiveUp,   // stop trying; publish it as failed until an operator intervenes
};

struct DestinationSupervisorDecision {
  SupervisorAction action = SupervisorAction::None;
  // Fresh evidence at THIS instant, not a latch.
  bool healthy = false;
  DestinationFailureClass failureClass = DestinationFailureClass::None;
  std::string reason;
};

class OutputDestinationSupervisorPolicy {
 public:
  static constexpr int kMaxConsecutiveFailures = 5;
  static constexpr std::int64_t kBaseBackoffMs = 5000;
  static constexpr std::int64_t kMaxBackoffMs = 60000;
  static constexpr std::int64_t kHealthyRunMs = 30000;
  static constexpr std::int64_t kProgressStaleMs = 1000;
  static constexpr std::int64_t kStalledRestartMs = 5000;
  static constexpr std::int64_t kStartGraceMs = 15000;

  // 5s, 10s, 20s, 40s, capped at 60s — identical to the four existing ladders.
  [[nodiscard]] static std::int64_t backoffMsForFailureCount(int consecutiveFailures) {
    if (consecutiveFailures <= 0) {
      return kBaseBackoffMs;
    }
    const int shift = (std::min)(consecutiveFailures, 30);
    const std::int64_t scaled = kBaseBackoffMs * (std::int64_t{1} << shift);
    return scaled < kBaseBackoffMs ? kMaxBackoffMs : (std::min)(scaled, kMaxBackoffMs);
  }

  [[nodiscard]] static DestinationFailureClass classify(const DestinationObservation& o) {
    const bool failed = o.status == "failed" || o.destinationHealth == "failed";
    // `warning` + a terminal result code is the shape the adapters use for an
    // inadmissible configuration (runtime-missing, source-name-invalid): they
    // report it as a warning because the show keeps running without that
    // destination. It is still a configuration that will be rejected again.
    if (isTerminalResultCode(o.lastResultCode) && (failed || o.status == "warning")) {
      return DestinationFailureClass::Terminal;
    }
    if (failed) {
      return DestinationFailureClass::Retryable;
    }
    return DestinationFailureClass::None;
  }

  // The destination's generation has just (re)started. Clears per-run evidence
  // but never the failure streak — that is what makes the ladder a ladder.
  void onGenerationStarted(std::int64_t nowMs) {
    generationStartedMs_ = nowMs;
    acceptedUnits_ = 0;
    everProduced_ = false;
    lastProgressMs_ = 0;
    faultPending_ = false;
    gaveUpAnnounced_ = gaveUp_;
  }

  // Operator intervention: recover / re-select / re-arm. Always recoverable
  // without an app restart.
  void reset(std::int64_t nowMs) {
    failures_ = 0;
    gaveUp_ = false;
    gaveUpAnnounced_ = false;
    terminalReason_.clear();
    nextAttemptAtMs_ = 0;
    onGenerationStarted(nowMs);
  }

  [[nodiscard]] DestinationSupervisorDecision observe(const DestinationObservation& o) {
    DestinationSupervisorDecision decision;

    if (!o.desiredActive) {
      // Not running by design. Hold the streak (an operator toggling a broken
      // destination off and on again goes through reset(), not through here),
      // but never fault and never restart.
      faultPending_ = false;
      decision.healthy = false;
      return decision;
    }

    if (o.observed) {
      if (o.acceptedUnits > acceptedUnits_) {
        acceptedUnits_ = o.acceptedUnits;
        lastProgressMs_ = o.nowMs;
        everProduced_ = true;
      } else if (o.acceptedUnits < acceptedUnits_) {
        // Counters went backwards: the adapter restarted itself underneath us.
        // That is a new run, not progress.
        acceptedUnits_ = o.acceptedUnits;
        everProduced_ = o.acceptedUnits > 0;
        lastProgressMs_ = o.acceptedUnits > 0 ? o.nowMs : 0;
      }
    }

    const std::int64_t sinceProgressMs = progressAgeMs(o.nowMs);
    decision.healthy = everProduced_ && sinceProgressMs >= 0 && sinceProgressMs <= kProgressStaleMs;
    decision.failureClass = classify(o);

    // A healthy RUN — producing now, and producing for long enough — returns the
    // budget. A healthy instant does not.
    if (decision.healthy && o.nowMs - generationStartedMs_ >= kHealthyRunMs) {
      failures_ = 0;
      nextAttemptAtMs_ = 0;
    }

    if (gaveUp_) {
      decision.failureClass = terminalReason_.empty() ? decision.failureClass : DestinationFailureClass::Terminal;
      decision.reason = giveUpReason();
      if (!gaveUpAnnounced_) {
        gaveUpAnnounced_ = true;
        decision.action = SupervisorAction::GiveUp;
      }
      return decision;
    }

    if (decision.failureClass == DestinationFailureClass::Terminal) {
      // Bypass the ladder entirely: five retries of a configuration that will be
      // rejected five times is five wasted reconnects and a misleading log.
      gaveUp_ = true;
      gaveUpAnnounced_ = true;
      terminalReason_ = o.lastError.empty() ? o.lastResultCode : o.lastError;
      decision.action = SupervisorAction::GiveUp;
      decision.reason = giveUpReason();
      return decision;
    }

    if (!faultPending_) {
      std::string faultReason;
      if (decision.failureClass == DestinationFailureClass::Retryable) {
        faultReason = o.lastError.empty()
                          ? ("Destination reported a failure (" + o.lastResultCode + ").")
                          : o.lastError;
      } else if (everProduced_ && sinceProgressMs > kStalledRestartMs) {
        faultReason = "Destination stopped accepting output for " +
                      std::to_string(sinceProgressMs / 1000) + "s without being asked to stop.";
      } else if (!everProduced_ && o.nowMs - generationStartedMs_ > kStartGraceMs) {
        faultReason = "Destination never accepted output within " +
                      std::to_string(kStartGraceMs / 1000) + "s of starting.";
      }
      if (!faultReason.empty()) {
        faultPending_ = true;
        pendingReason_ = std::move(faultReason);
        armRetryAfterFailure(o.nowMs);
        if (gaveUp_) {
          decision.action = SupervisorAction::GiveUp;
          decision.reason = giveUpReason();
          return decision;
        }
      }
    }

    if (faultPending_ && o.nowMs >= nextAttemptAtMs_) {
      faultPending_ = false;
      decision.action = SupervisorAction::Restart;
      decision.reason = pendingReason_;
      ++restarts_;
      // The caller opens a new generation and calls onGenerationStarted().
    } else if (faultPending_) {
      decision.reason = pendingReason_;
    }
    return decision;
  }

  [[nodiscard]] bool gaveUp() const { return gaveUp_; }
  [[nodiscard]] int consecutiveFailures() const { return failures_; }
  [[nodiscard]] int restarts() const { return restarts_; }
  [[nodiscard]] bool faultPending() const { return faultPending_; }
  [[nodiscard]] std::int64_t nextAttemptAtMs() const { return nextAttemptAtMs_; }
  [[nodiscard]] bool everProduced() const { return everProduced_; }
  [[nodiscard]] std::string giveUpReason() const {
    if (!gaveUp_) return {};
    if (!terminalReason_.empty()) {
      return "Destination configuration was rejected and will be rejected again: " + terminalReason_ +
             ". Fix it and re-arm the destination.";
    }
    return "Destination failed " + std::to_string(kMaxConsecutiveFailures) +
           " times in a row and will not be restarted again: " + pendingReason_ +
           " Re-arm the destination to try again.";
  }

  // -1 when nothing has ever been accepted (never "fresh", whatever the clock).
  [[nodiscard]] std::int64_t progressAgeMs(std::int64_t nowMs) const {
    if (!everProduced_) return -1;
    if (nowMs < lastProgressMs_) return 0;  // clock went backwards: do not accuse
    return nowMs - lastProgressMs_;
  }

 private:
  void armRetryAfterFailure(std::int64_t nowMs) {
    if (failures_ >= kMaxConsecutiveFailures) {
      gaveUp_ = true;
      gaveUpAnnounced_ = true;
      return;
    }
    nextAttemptAtMs_ = nowMs + backoffMsForFailureCount(failures_);
    failures_ += 1;
  }

  int failures_ = 0;
  int restarts_ = 0;
  bool gaveUp_ = false;
  bool gaveUpAnnounced_ = false;
  bool faultPending_ = false;
  bool everProduced_ = false;
  std::int64_t acceptedUnits_ = 0;
  std::int64_t lastProgressMs_ = 0;
  std::int64_t generationStartedMs_ = 0;
  std::int64_t nextAttemptAtMs_ = 0;
  std::string pendingReason_;
  std::string terminalReason_;
};

}  // namespace corevideo::modules
