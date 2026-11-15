# Mac parity M0 — outcomes (2026-11-14)

Plan: `2026-11-14-mac-parity-m0-fixes.md`. All 11 tasks complete, each with an independent
task review (spec + quality), plus a whole-branch review and one fix wave whose scoped
re-review came back clean. Commits `7dd2b4f0..b4d5cc99` on `mac-parity-1.0`. Windows paths
bit-identical (no shared `native/src` file changed). Final state: mac-shell harness 2568/0,
selfcheck PASS, design lint green, `build-metal` ctest 5/5 (main + `realtime` label),
`build-stub` ctest 4/4.

## What the fix rounds caught (why the gates exist)

- **Fix rounds by task:** T3 1, T7 1, T8 1, T9 1, T10 2; final wave 1.
- **Cross-task defects only the whole-branch review saw:** quit could reopen a recording (T9 predicate reused
  across T3/T10 state with the opposite safe default); the editor could edit on-air when
  Preview == Program (T7 guarded only empty Preview). Every per-task test passed while both
  were open. Keep the whole-branch review.
- **Integration ≠ algorithm:** T9's Python driver mirrored the intended stop→poll→EOF and
  passed; the shipped flow was blind because AppKit runs `.terminateLater` in modal-panel run
  loop mode and the sync timer was default-mode only.
- **Mutation runs found test blindness twice:** T10's ±0.1 s tolerance was wider than the
  2-frame threshold it was meant to test; T9's intent assertion could not fail.

## Rulings made on the owner's behalf (in order, with cost if wrong)

1. Execute T1,T2,T4,T5,T3,T7,T6,T8,T9,T10,T11 sequentially, grouped by file — minor rework.
2. T8 (removal) has no red test by nature; review checks grep + payload test — none.
3. A BGRA ISO frame that changes size mid-take is refused with a named warning, not scaled
   (Windows has no BGRA scaler) — a Mac screen-capture ISO that resizes stops until the next
   take; add a scaler later.
4. Departed ISO: skip silence-only ticks, resume the same file on return (gap bridged with
   silence), finalize at Stop (Windows `2dcfef25`) — the departed file stays open until Stop.
5. Deleting the Preview scene leaves Preview empty (Windows moves Preview→Program, which on
   Mac would re-create the live-edit defect) — operator re-cues Preview after deleting it.
6. Port `ApplyBrandOverlayDefaults` rather than leave "Default overlay" inert — superseded by
   ruling 9.
7. The Program tail readiness check on every stop raised a false operator warning under
   ordinary backpressure; promoted to Important — one extra round.
8. ISO tail warning text/scope brought to Windows parity — none.
9. "Default overlay: all-off" must NOT touch the manual lower third (Windows only disables
   Graphics rows; `StudioViewModel.cs:13851`). It is a no-op on Mac until graphics rows exist
   (roadmap M4). The hide/show generation-token race fix stays — if the owner wanted all-off to
   clear the manual key too, it is a one-line restore.

## Deferred minors (not blocking release; triaged by the final review)

- Scene: redundant `var l` reassign in duplicate layer copy; "<name> removed" success message
  styled red like refusals.
- Harness: test runs still touch the media-bin directory via `NSHomeDirectory`.
- Meters: `audioObservation` `@Published` is redundant; comment that the clock-domain age is
  Windows-only (core emits `clockDomain` only under `_WIN32 && _MSC_VER`).
- Brand kit: legacy prefs values `restore-last`/`lower-third-only` show no picker selection
  (normalize to `manual` on decode). `retireAutomationLowerThird` has the same unguarded ack
  race as the old Hide — unreachable while `sceneAutomationAvailable == false`; fix if Auto
  returns.
- Quit: Esc needs focus; `race()` leaves sleeping timer tasks; no-op filter in a test; stop
  responses' snapshots discarded (possible duplicate `stop-recording-session` if not
  idempotent); `processExited`-during-wait branch untested.
- Recording: no adapter-level test for #984 demand wiring (`setVideoConsumerDemand` →
  `takeCpuVideoFrames`); `Tail::path` now holds a warning (rename); per-chunk silence wait.
- Native config: configuring `-DCOREVIDEO_WITH_AVF_ENCODER=ON -DCOREVIDEO_ENABLE_DEV_ADAPTERS=ON`
  WITHOUT `-DCOREVIDEO_STUB=OFF` now fails to link (`probeStoppedWriterForTest` gate mismatch);
  CI unaffected.
- Engine: stale ordering comment in `main-macos.mm` (~166); `engine_command_name` duplicates
  `ipc_command_is`'s cmd scan. **Windows `main.cpp` has the same substring dispatch bug** —
  needs a roadmap row.
- Signing: `sign_nested` skips bare Mach-O helpers inside SDK frameworks (must be fixed before
  notarization — release plan Task 6); redundant SDK re-sign (~25 s).

## Owner manual checks (carried into `docs/qa/macos-1.0-owner-acceptance.md`)

Real Cmd-Q while recording (dialog renders, buttons work in terminate-wait, double Cmd-Q,
main window closed → panel fallback renders unclipped), stream-quit, real camera ISO frame
count, Zoom auth + join under hardened runtime.
