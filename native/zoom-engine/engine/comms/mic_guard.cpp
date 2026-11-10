#include "mic_guard.h"

namespace edgecomms {

void MicGuard::Reset() {
  installed_ = owns_ = seen_initialised_ = gave_up_ = false;
  logged_install_failure_ = logged_no_licence_ = logged_operator_muted_ = false;
  installed_at_ms_ = retry_at_ms_ = next_unmute_ms_ = 0;
  blocked_.clear();
}

std::vector<std::string> MicGuard::TakeLog() {
  std::vector<std::string> out;
  out.swap(log_);
  return out;
}

void MicGuard::GiveUp(const std::string& reason) {
  owns_ = false;
  gave_up_ = true;
  blocked_ = reason;
  Log(reason);
}

bool MicGuard::Dropped() {
  seen_initialised_ = false;
  if (!owns_) return false;
  GiveUp("Talkback is silent: Zoom dropped the silent virtual mic, so this node's meeting mic was muted again "
         "(the room never hears this PC).");
  return true;
}

void MicGuard::InstallResult(bool ok, const std::string& detail, int64_t now_ms) {
  if (ok) {
    // Installed, NOT owned: the mic stays muted until the virtual mic itself
    // confirms it is initialised.
    installed_ = true;
    installed_at_ms_ = now_ms;
    Log("silent virtual mic installed -- waiting for Zoom to initialise it before opening the meeting mic");
    return;
  }
  retry_at_ms_ = now_ms + kRetryMs;
  if (!logged_install_failure_) {
    logged_install_failure_ = true;
    Log("mic auto-suppress unavailable (" + detail +
        ") -- the meeting mic stays closed, talkback is silent; retrying every 5 s");
  }
}

MicGuard::Act MicGuard::Next(const In& in) {
  if (!in.in_meeting) {
    if (installed_ || owns_ || gave_up_ || !blocked_.empty()) Reset();
    return Act::None;
  }
  if (!in.capture_only) {
    // An operator who speaks in the meeting keeps their own mic. Never
    // installed, never owned; only say when their mute silences talkback.
    if (in.can_talkback && in.self_muted && !logged_operator_muted_) {
      logged_operator_muted_ = true;
      Log("your meeting mic is muted -- talkback is silent until you unmute in Zoom");
    }
    if (!in.self_muted) logged_operator_muted_ = false;
    return Act::None;
  }
  // Owned: watch for the virtual mic going away. Mute first, always.
  if (owns_) {
    if (in.initialised) {
      seen_initialised_ = true;
    } else {
      GiveUp("Talkback is silent: the silent virtual mic stopped, so this node's meeting mic was muted again "
             "(the room never hears this PC).");
      return Act::Revoke;
    }
  }
  if (gave_up_ || !in.can_talkback) return Act::None;
  if (!installed_) {
    if (in.now_ms < retry_at_ms_) return Act::None;
    if (!in.licence) {
      retry_at_ms_ = in.now_ms + kRetryMs;
      blocked_ =
          "Talkback is silent: this sign-in has no Zoom raw-data licence, so the meeting mic stays muted (the room "
          "must never hear this PC).";
      if (!logged_no_licence_) {
        logged_no_licence_ = true;
        Log(blocked_);
      }
      return Act::None;
    }
    blocked_.clear();
    return Act::Install;
  }
  if (!owns_) {
    // Installed, waiting for onMicInitialize. The meeting mic stays muted.
    if (in.initialised) {
      owns_ = true;  // only now does capture-only stand down
      seen_initialised_ = true;
      next_unmute_ms_ = 0;
      Log("silent virtual mic initialised -- opening the meeting mic (silent to the room)");
    } else {
      if (in.now_ms - installed_at_ms_ > kInitTimeoutMs) {
        GiveUp("Talkback is silent: the silent virtual mic never initialised (no onMicInitialize within 5 s), so "
               "this node's meeting mic stays muted. If Zoom only initialises it once audio flows, this is why.");
      }
      return Act::None;
    }
  }
  if (in.self_muted && in.now_ms >= next_unmute_ms_) {
    next_unmute_ms_ = in.now_ms + kUnmuteEveryMs;
    return Act::Unmute;
  }
  return Act::None;
}

}  // namespace edgecomms
