#pragma once

// Test-only fault seams for the MONITOR compositor passes (multiview + preview).
//
// WHY THIS EXISTS (beta slice, 2026-09-09). `docs/production-realtime-execution-plan.md`
// gate G2 is written in terms of injected faults: "monitor rendering and shell
// presentation are fault-injected independently". Nothing in this tree could inject
// either, so the gate could not be attempted and the question "does a stalled Preview
// or Multiview pass delay Program?" had never been answered with a measurement.
//
// It is answerable, because `D3D11Compositor::render()`, `renderMultiview()` and
// `renderPreview()` all run on ONE render thread against ONE immediate context, and
// `D3DProgramBuffer` delivers Program on its OWN thread against a fixed timeline. So a
// monitor stall does not delay Program delivery directly — it delays the next Program
// RENDER, and the program buffer then either absorbs that (its whole purpose) or
// underruns. Where the boundary sits is a number, and this seam is what measures it.
//
// GUARDING — this must be impossible to trip in a shipped build:
//   * There is no environment variable, no command, no config key and no wire field
//     that reaches it. The ONLY way to arm it is an in-process call to
//     `setMonitorRenderStallForTest`, which nothing outside `native/tests/` calls.
//     (Same shape as `MediaCore::setStillImageDecoderForTest`, this repo's existing
//     injection point: a `...ForTest` entry point, default-inert, no external surface.)
//   * The stall is a plain function POINTER, not a std::function: arming allocates
//     nothing, and a null pointer is the default and the disarmed state.
//   * `corevideo-native-tests` links the same `corevideo_native` library the product
//     exe links, so a compile-time gate here would delete the seam from the tests as
//     well. The guarantee is therefore structural (no reachable caller) rather than
//     conditional compilation, and it is exactly the guarantee the existing test seam
//     in MediaCore relies on.
//
// COST WHEN DISABLED — one relaxed atomic bool load per monitor pass (i.e. twice per
// render tick, not per pixel or per layer). No allocation, no lock, no environment
// read, no branch inside any per-layer or per-pixel loop.

#include <atomic>

namespace corevideo::compositor {

enum class MonitorPass {
  Multiview,
  Preview,
};

using MonitorRenderStallFn = void (*)(MonitorPass);

// Definitions live in the header as C++17 inline variables so this compiles into every
// translation unit that needs it without a dedicated .cpp (the compositor sources are
// platform-gated; this header deliberately is not, so tests can include it anywhere).
inline std::atomic<bool>& monitorRenderStallArmedFlag() noexcept {
  static std::atomic<bool> armed{false};
  return armed;
}

inline std::atomic<MonitorRenderStallFn>& monitorRenderStallSlot() noexcept {
  static std::atomic<MonitorRenderStallFn> slot{nullptr};
  return slot;
}

// The hot-path predicate. One relaxed load; nothing else in this header is touched
// when it returns false.
[[nodiscard]] inline bool monitorRenderStallArmed() noexcept {
  return monitorRenderStallArmedFlag().load(std::memory_order_relaxed);
}

// Runs the armed stall for `pass`. Never called unless `monitorRenderStallArmed()`.
inline void invokeMonitorRenderStall(MonitorPass pass) noexcept {
  if (auto* fn = monitorRenderStallSlot().load(std::memory_order_acquire)) {
    fn(pass);
  }
}

// Arm (non-null) or disarm (nullptr) the monitor-pass stall. Test-only; see the
// guarding note above. Ordering matters: publish the pointer before the flag when
// arming, clear the flag before the pointer when disarming, so a render thread that
// observes the flag always observes a valid pointer.
inline void setMonitorRenderStallForTest(MonitorRenderStallFn fn) noexcept {
  if (fn != nullptr) {
    monitorRenderStallSlot().store(fn, std::memory_order_release);
    monitorRenderStallArmedFlag().store(true, std::memory_order_relaxed);
    return;
  }
  monitorRenderStallArmedFlag().store(false, std::memory_order_relaxed);
  monitorRenderStallSlot().store(nullptr, std::memory_order_release);
}

}  // namespace corevideo::compositor
