#pragma once

// #440 / T3.2 — what a browser source says when its host process dies.
//
// The adapter reported "browser host exited (code 3)". Exit 3 has exactly one
// common cause on a machine we have never seen: the Evergreen WebView2 Runtime
// is not installed. The operator saw a stalled source and a number.
//
// Same rule as the Zoom join reasons (modules/ZoomJoinFailureMessage.h): an
// exit code is WIRE vocabulary, this is the ONE place it becomes English, and
// an UNKNOWN code passes through with its number rather than being flattened
// into a generic sentence that says nothing.

#include <string>

namespace corevideo::modules {

// The browser host's own exit contract (native/browser-host): 3 is raised both
// when the runtime probe finds nothing AND when creating the WebView2
// environment fails, because on a tester's machine those are the same
// actionable fact.
inline constexpr int kBrowserHostExitWebView2Missing = 3;

[[nodiscard]] inline std::string browserHostExitMessage(int exitCode) {
  if (exitCode == kBrowserHostExitWebView2Missing) {
    return "This browser source needs the Microsoft Edge WebView2 Runtime, which is not "
           "installed (or could not start). Install the Evergreen WebView2 Runtime from "
           "https://developer.microsoft.com/microsoft-edge/webview2/ and reload the source.";
  }
  return "Browser host exited (code " + std::to_string(exitCode) + ").";
}

}  // namespace corevideo::modules
