#pragma once
#include "core/MediaCore.h"
#include <chrono>
#include <functional>
#include <thread>
namespace corevideo::testing {
template <typename Decoder, typename... Args>
std::function<std::unique_ptr<modules::IMediaFrameSource>()> mediaFactoryOf(Args... args) {
  return [=] { return std::unique_ptr<modules::IMediaFrameSource>(new Decoder(args...)); };
}
// Renders display ticks until `done(core)` is true or `timeoutMs` elapses; returns whether it became true.
// Media frames are produced by a worker thread (exactly as in production), so a test that wants pixels
// after load-scene-graph pumps here instead of assuming the first tick has them.
inline bool renderUntil(core::MediaCore& core, const std::function<bool(core::MediaCore&)>& done, int timeoutMs = 2000) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    core.renderDisplayTick();
    if (done(core)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}
// Full (audio-bearing) ticks until `done(core)` is true or `timeoutMs` elapses.
// renderUntil drives VIDEO-only display ticks; media AUDIO is gathered only on
// a full tick, which a direct caller gets from applyCommands with no commands.
// `lastState`, when given, receives the snapshot of the tick that satisfied
// `done` - NOT a later one. Media audio is a window that is only due on some
// ticks, so a caller that re-reads the state after the pump can legitimately
// find the tap empty again; the satisfying tick is the honest answer.
inline bool applyUntil(core::MediaCore& core, const std::function<bool(core::MediaCore&)>& done,
                       int timeoutMs = 3000, rpc::Json* lastState = nullptr) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    auto state = core.applyCommands(rpc::Json::Array{});
    if (done(core)) {
      if (lastState != nullptr) *lastState = std::move(state);
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}
// Full ticks, unconditionally: for an assertion that something must NOT appear
// (silence on every bus, say), where waiting for a predicate is impossible and
// asserting on tick zero would pass vacuously.
inline void applyTicks(core::MediaCore& core, int ticks) {
  for (int i = 0; i < ticks; ++i) {
    (void)core.applyCommands(rpc::Json::Array{});
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}
// True once the source bus reports a frame for `sourceId` (health producing).
inline bool busSourceProducing(core::MediaCore& core, const std::string& sourceId) {
  const auto state = core.sessionState(); const auto* sources = state.get("sources");
  if (!sources) return false;
  for (const auto& s : sources->asArray()) if (s.getString("sourceId") == sourceId) return s.getString("health") == "producing";
  return false;
}
}
