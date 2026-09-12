#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>

namespace corevideo::modules {

// The ONE place FFmpeg's own stderr becomes an operator-readable sentence, in the
// `ZoomJoinFailureMessage.h` / `OutputLifecyclePolicy.h` shape: pure, header-only,
// unit-testable without a process.
//
// WHY THIS EXISTS (owner report, 2026-09-12). A stream that would not start was
// reported to the operator as "RTMP output failed. Check the server URL, stream
// key, and network.", so the owner re-entered credentials that were already
// correct. FFmpeg had written the actual reason to its stderr temp file on the
// very first attempt:
//
//   [out#0/flv] Error opening output rtmp://a.rtmp.youtube.com/live2/<key>: I/O error
//   Error opening output files: I/O error
//
// The destination refused the connection. The stream key in that URL is the SAME
// key that worked two minutes later. Nothing read that file, so the one line
// naming the cause was thrown away - the identical gap #473/#494 closed for the
// media decoder, and the rule CLAUDE.md already states twice for this sender:
// read FFmpeg's own stderr before theorising.

namespace detail {

inline bool isPlausibleSecret(const std::string& secret) {
  // A 2-character "secret" matches inside ordinary words ("error", "output") and
  // would shred the very message this exists to preserve. Redaction must never
  // destroy the diagnostic it is protecting.
  return secret.size() >= 6;
}

inline void replaceAll(std::string& text, const std::string& needle, const std::string& replacement) {
  if (needle.empty()) {
    return;
  }
  std::size_t at = 0;
  while ((at = text.find(needle, at)) != std::string::npos) {
    text.replace(at, needle.size(), replacement);
    at += replacement.size();
  }
}

}  // namespace detail

/// Strips every secret that can appear in FFmpeg's diagnostics.
///
/// This runs AT THE SOURCE rather than leaving it to the snapshot redactor,
/// because `lastError` reaches `/snapshot`, the support bundle and the log, and
/// because that redactor's rtmp rule is greedy over non-whitespace (CLAUDE.md) -
/// one URL in a `lastError` eats the properties after it. The HOST deliberately
/// survives: it is the diagnostic, and it is not a secret.
[[nodiscard]] inline std::string redactFfmpegDiagnostics(std::string text,
                                                         const std::string& streamKey,
                                                         const std::string& passphrase) {
  if (detail::isPlausibleSecret(streamKey)) {
    detail::replaceAll(text, streamKey, "<stream-key>");
  }
  if (detail::isPlausibleSecret(passphrase)) {
    detail::replaceAll(text, passphrase, "<passphrase>");
  }
  return text;
}

/// The last `maxBytes` of an FFmpeg stderr log, flattened to a single bounded line.
///
/// The tail and not the head: FFmpeg names the fatal reason last, and an
/// unbounded blob in `lastError` would roll the diagnosis out of a bounded log.
/// One line because `lastError` is rendered as a sentence in operator surfaces.
[[nodiscard]] inline std::string flattenFfmpegStderrTail(const std::string& raw,
                                                         std::size_t maxBytes = 2048) {
  if (raw.empty() || maxBytes == 0) {
    return "";
  }

  const std::size_t start = raw.size() > maxBytes ? raw.size() - maxBytes : 0;
  std::string tail = raw.substr(start);

  // Collapse every run of whitespace (newlines included) into a single separator
  // so multi-line FFmpeg output stays one readable sentence.
  std::string flattened;
  flattened.reserve(tail.size());
  bool pendingBreak = false;
  for (const char raw_ch : tail) {
    const auto ch = static_cast<unsigned char>(raw_ch);
    if (ch == '\r' || ch == '\n') {
      pendingBreak = !flattened.empty();
      continue;
    }
    if (std::isspace(ch) != 0) {
      if (!flattened.empty() && !pendingBreak) {
        flattened.push_back(' ');
      }
      continue;
    }
    if (pendingBreak) {
      flattened += " | ";
      pendingBreak = false;
    }
    flattened.push_back(raw_ch);
  }

  // Trim a trailing separator left by the collapse above.
  while (!flattened.empty() && (flattened.back() == ' ' || flattened.back() == '|')) {
    flattened.pop_back();
  }
  // Collapsing runs can still leave more than the budget (the " | " separators
  // are added, not removed). Trim from the FRONT: FFmpeg names the fatal reason
  // LAST, so cutting the end would drop the one line this exists to carry - the
  // bug the bounded-log test caught on the first green run.
  if (flattened.size() > maxBytes) {
    flattened.erase(0, flattened.size() - maxBytes);
  }
  return flattened;
}

/// The operator sentence: the generic failure, plus FFmpeg's own words.
///
/// An ABSENT tail leaves the generic sentence exactly as it was - inventing
/// detail we do not have would be the same lie pointing the other way.
[[nodiscard]] inline std::string describeFfmpegSenderFailure(const std::string& generic,
                                                             const std::string& redactedTail) {
  const auto firstReal = redactedTail.find_first_not_of(" \t\r\n|");
  if (firstReal == std::string::npos) {
    return generic;
  }
  if (generic.empty()) {
    return "ffmpeg: " + redactedTail;
  }
  return generic + " ffmpeg: " + redactedTail;
}

}  // namespace corevideo::modules
