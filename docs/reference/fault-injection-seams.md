# Fault-injection seams (beta slice, 2026-09-09) — how to prove stability work

_Moved verbatim from `CLAUDE.md` (#737). Paths are relative to the repo root._

Gates G2 and G3 in `docs/production-realtime-execution-plan.md` (on the PR #419 branch) are written in terms of
injected faults, and until now nothing in this tree could inject anything, so neither
could be attempted. There are now three seams. **All three are test-only, and the guard
is structural, not conditional compilation** — `corevideo-native-tests` links the same
`corevideo_native` library the product does, so a compile-time gate would delete the seam
from the tests too. The guarantee is the same one
`MediaCore::setStillImageDecoderForTest` already relies on: no env var, no command, no
config key, no wire field reaches any of them, and nothing outside `native/tests/` or
`CoreVideoPro.WinUI.Tests` calls them.

- **Forced device loss + blocked present** (shell):
  `native-shell/CoreVideoPro.WinUI/Services/PresentationFaultInjection.cs`. `internal`
  (the WinUI assembly's only `InternalsVisibleTo` is the test project) AND arming refuses
  unless `CoreVideoPro.WinUI.Tests` is loaded in the process — a positive check that fails
  closed. The present hot path reads exactly one static bool. Arming returns an
  `IDisposable` that disarms, and a blocked present ALWAYS carries a hard timeout: a seam
  that can wedge the process it exists to diagnose is not a diagnostic.
- **Blocked monitor render** (core): `native/src/compositor/CompositorFaultInjection.h`,
  consulted at the top of `D3D11Compositor::renderMultiview` / `renderPreview`. One
  relaxed atomic bool per monitor pass; the stall is a plain function pointer, so arming
  allocates nothing and null is the disarmed state.

**What the seams proved, and what they did not:**

- **Device-loss recovery survives a real injected loss.** `PresentationFaultInjectionTests`
  drives the real `Direct3D11InteropService` against a real D3D11 device: the loss is
  classified, the generation is retired, the bounded ladder schedules, a real
  `D3D11CreateDevice` runs at the 250ms rung (measured 266ms), the host adopts the new
  generation, its per-generation handle blacklist is cleared, and the loss report is
  accurate. A second host observing the same loss does NOT spend a second ladder rung.
  **Still unproven:** the swap-chain rebuild and on-screen GPU presentation — a
  `SwapChainPanel` is a XAML object and the test host runs with the Windows App SDK
  bootstrap disabled, so no panel can exist there. That last hop needs the app.
- **A blocked present still blocks UI control** — presents run on the UI thread from
  `CompositionTarget.Rendering`, so the property G2 wants is FALSE today, and no test can
  make it come out otherwise while that is the shape. What is proven is that the
  DIAGNOSIS is independent: `PresentationStageWatchdog` names the stalled stage from its
  own thread while the present is still stuck. `BlockedPresentFaultTests` pins the
  structural gap, so moving presents off the UI thread fails a test that says to come back
  and prove the real property.
- **Program is NOT isolated from monitor rendering.** `MonitorRenderFaultInjectionTest`
  measured it on this rig (RTX 4090, 1080p Program + 720p Preview, 3-frame buffer): with
  no fault, 121 produced / 124 delivered / 4 underruns per 2s; with a sustained 25ms
  Preview stall (unmitigated, at the product's 1ms timer resolution), 77 / 77 / 51 —
  **a Preview compositor overrunning by ~1.5 frame periods costs Program ~36% of its
  frames** (≈ 1 − 16.7/25.7, the slots a 25.7ms pass leaves). The originally published
  "65 / 65 / 64 — HALF its frames" was measured at the default ~15.6ms timer tick, where
  the seam's "25ms" stall actually slept ~31ms. Program `render()`, `renderMultiview()` and
  `renderPreview()` share one render thread and one D3D immediate context. A one-off stall
  costs only the slots it spans and Program recovers on its own. **The program buffer does
  not help here** — it protects delivery timing for frames that were produced, and these
  frames were never rendered. The monitor-compositor split in
  `docs/production-realtime-completion-plan.md` (on the PR #419 branch) is what would give G2 its property; the
  sustained-stall case must be INVERTED when that lands, not deleted.
  **Beta MITIGATION shipped (T1.4 / #431, 2026-09-10): monitor load-shedding.** Program
  always renders; under sustained overload the MONITOR passes give way.
  `core/MonitorShedPolicy.h` (pure, `MonitorShedPolicyTest.cpp`) is fed once per DISPLAY
  tick in `MediaCore::renderSyntheticTick` (videoOnly only — synthetic full ticks never
  feed it, so ordinary unit tests stay timing-free) with the tick's non-monitor render
  cost, the last measured cost of one multiview+preview cycle (each refreshed only when
  that pass runs — and only when that pass is SHEDDABLE: a compositor that exports no
  handle is forced every tick, so its cost is fed as 0), and the budget `1s / outputFps_`. It projects the per-tick load at
  divisor d as `program + monitor/d` and returns a monitor cadence divisor 1/2/3 that
  multiplies `kMultiviewTickDivisor` (still 1 = the healthy cadence) and applies to BOTH
  the multiview pass (phase 0) and the preview pass (phase 1 — staggered so a shed cycle
  never stacks both). Constants: `kEnterAfterOverBudgetTicks = 3` (one or two slow ticks
  are a shader compile; the buffer rides them out), `kShedAboveUtilisationPercent = 90`
  (NOT 100 — jitter headroom: under shedding the tick that runs the monitor pass
  finishes late, and the cheap ticks after it must absorb that plus scheduler / pacer /
  GPU jitter; a lost Program slot costs far more than shedding a level early),
  `kRecoverAfterHealthyTicks = 60` (1s, 20x slower than entry — late
  recovery costs monitor smoothness, early recovery costs Program),
  `kRecoveryHeadroomPercent = 75` (recover only when the next LOWER divisor fits in 75%:
  the 75–90% band is the anti-flap hysteresis), `kMaxDivisor = 3`. It steps one level at
  a time both ways. CPU-deadline misses are deliberately NOT an input (a shed monitor
  tick finishing late is expected and absorbed by the buffer; feeding it back would pin
  every recoverable overload at 3). On a shed tick the cached preview texture/dims are
  REPUBLISHED exactly like the multiview cache (`lastPreviewTexture_`), and the first
  tick, a structural change (`*StructureEmitted_ = false`) or an empty cache still force
  the pass. No locks, no allocation, up to seven clock reads per tick (tick start/end,
  monitor start/end, plus the end of the multiview pass and the start/end of the preview
  pass when they run). **A Program- or GPU-bound overload sheds monitors too, by design:**
  the policy sees only CPU-submission time, and D3D submission is async, so GPU cost
  queued by a monitor pass often surfaces inside Program's NEXT call — shedding monitors
  is the one lever this thread has, and it can only give Program time back.
  Observability: `sessionState().realtimeEvidence.monitorShed {divisor, level,
  enteredCount, shedTicks, lastReason, lastTransitionProgramMs,
  lastTransitionMonitorCycleMs, lastTransitionBudgetMs}` (published unconditionally;
  `lastReason` = none | over-budget | recovered; the `lastTransition*` fields are the
  observation that caused the last divisor change, so a monitor-bound shed is
  distinguishable from a Program/GPU-bound one) and ONE `[monitor-shed] enter|step-up|step-down|exit …` line per state
  change via `nativeLogf`. **Measured (same rig, 1ms timer, one run, 2s windows):**
  raw compositor, no shed — baseline 121/124/4, sustained 25ms 77/77/51; through
  MediaCore with the shed — baseline 121/124/4, sustained 25ms **119/119/9** at divisor 2,
  back to divisor 1 within 3s of the stall ending (182/182/6 per 3s). **What it does NOT
  give:** isolation. Program is protected by cadence: the entry ticks are paid in full,
  and a single monitor pass longer than the slack a 1/3 cadence leaves (~2 frame periods)
  still costs Program slots. `ASustainedMonitorStallIsShedAndProgramKeepsItsRate` asserts
  both halves — the raw leg still delivers <75% of baseline with underruns up by >30,
  a bound tied to the stall (~35% expected loss), and that is the assertion to INVERT
  when the split lands; the shed leg keeps >=90% — and the timing tests now run under
  `timeBeginPeriod(1)` like the product's render thread: at the default ~15.6ms tick the
  seam's "25ms" stall really slept ~31ms (that is what the 65/121 above measured) and
  the harness's own slot sleeps overshot by up to a frame.
