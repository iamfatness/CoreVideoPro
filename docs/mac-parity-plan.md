# macOS parity — audit of 2026-09-27

Replaces the 2026-08-07 version, which predated ~786 commits and described a
product that no longer exists.

**Scope of this audit:** every non-OHG `fix` on the Windows shell between
2026-09-05 and 2026-09-27, sorted by whether macOS already gets it, will never
need it, or genuinely lacks it.

---

## 1. The headline

**The Mac is not rotting, and it is not a subsystem behind.** The commit counts
say otherwise and they are misleading.

786 commits in the window. Of the 161 on `native-shell/`: **38 are OHG**,
**3 are non-OHG features**, ~20 are non-OHG fixes, and the remainder are
refactors, tests and docs.

So the entire Windows-only *feature* lead is three items (§4), and the real
divergence risk is not features at all — it is **behaviour rulings made once,
on Windows, that nobody checked against the Mac** (§5).

## 2. What is already healthy — do not re-litigate this

- **The shared core is gated on macOS.** `mac-show-drill` (`ci.yml`, `macos-14`)
  builds Metal + AVF + CoreAudio and runs a headless show rehearsal against the
  real core. The 237 `native/` commits in this window are proven on Mac
  continuously, not assumed.
- **`mac-shell` builds and is tested.** `mac-shell-tests` does a release build
  plus `COREVIDEO_SHELL_TESTS` and `COREVIDEO_SHELL_SELFCHECK`;
  `mac-shell-design-lint` guards the brand tokens. Verified building clean in
  7.8 s on 2026-09-27.
- **`mac-shell` is moving again.** 16 commits in the week to 2026-09-27, after a
  quiet spell. The Sept 5–9 batch was recording-lifecycle correctness — Stop
  intent surviving delayed snapshots, bridge-generation guards, destinations no
  longer claiming completion before they finished.

## 3. OHG is a deliberate non-port, not a gap

Owner ruling, 2026-09-20: *"a lot of logic but very little value so far."*

The Windows side gained a whole OHG vertical in this window (Plans 7a/7b, 38
commits): a `CoreVideoPro.ShowEngine` project, `ShowEngineBridge`,
`OhgHostAdapter`, a supervisor with restart policy, and an OHG Show tab with
status strip, panelist board, program/gallery/GFX panels, settings with adapter
hot-swap, and a legacy Isadora importer. `mac-shell` has zero references to any
of it.

**That absence is a position, not a backlog item.** If OHG stays low-value, not
porting it is correct and the Mac port is cheaper for it. Record it here so it
stops reading as "behind" in future audits.

Note the engine itself is portable TypeScript (`show-engine/`). What is
Windows-specific is the *host*. If OHG ever earns its keep, a Mac host is real
work but not a re-port of the engine.

## 4. Windows-only features the Mac lacks

Three, all small:

1. **Per-source "On dropout" policy** — hold last frame vs black, persisted,
   shipped as `set-source-policy` (#535 slice 4a). **Verified absent** from
   `mac-shell` (no `dropout` reference anywhere in `mac-shell/Sources/`). The
   core half of this landed in `native/` and so already reaches Mac; what is
   missing is the operator surface and the persistence.
2. **ISOs armed with Zoom capture off are loud** (T3.7 / #470).
3. **Tiles colour pickers** — background, border, glow (T3.5 / #476).

## 5. The real risk: behaviour rulings that live only in WinUI

Of the non-OHG fixes, **8 touched `native/` as well as the shell** — for those,
the core half reaches Mac free and is exercised by the show drill. Named, so
nobody re-does them: the two #535 dropout fixes, both `stream` fixes (warming is
not failing; a failing stream names its own reason), the Zoom join-prompt fix,
#481 (stop muting guests), lower-thirds binding, and live scene routing
startup/shutdown.

The rest are `native-shell`-only. Most of those are WinUI plumbing with no Mac
analogue — discarded scene-canvas element handlers, late-bound page commands as
OneWay, plate tones, D3D device-loss recovery. **Two are not:**

### 5.1 The 8 concurrent Zoom-source ceiling — VERIFIED GAP

`fix(capacity): enforce the 8 concurrent Zoom-source ceiling (owner bandwidth
ruling)` lives entirely in `native-shell`'s `ZoomMediaSpinePayloadBuilder`.

**`mac-shell` has no such ceiling** — verified by search on 2026-09-27. (The
`CEILING` hits in `AudioConsole.swift` are the mastering limiter; the `8` in
`EncoderCapacityProbe.cpp` is an encoder-session cap, a different thing.)

This is an **owner bandwidth ruling**, not a UI nicety: it exists because the
Zoom raw-data downlink budget is finite. A Mac operator can currently exceed it
with no guard, and the failure mode is degraded video rather than a refusal.

### 5.2 Recording folder resolution — LIKELY GAP, unverified

`fix(record): resolve the recording folder to an absolute user path` (T2.8 /
#469) is `native-shell`-only. Whether `mac-shell` resolves its recording path
the same way was **not checked** in this audit. Given the Sept 5–9 mac-shell
work was recording-lifecycle correctness, it may already be handled.

## 6. How much of this is verified, and how much inferred

Honesty about method, because the last two plans in this repo were written
against assumed shapes that turned out wrong:

- **Verified directly:** the CI job scopes; that `mac-shell` builds; the
  OHG absence; the dropout-policy absence; the absence of a Zoom-source ceiling;
  the per-commit tree classification (via `--name-only`, after `--stat`'s path
  truncation produced a wrong first cut).
- **Inferred from commit subjects and touched paths:** which of the
  `native-shell`-only fixes are WinUI plumbing versus operator-visible
  behaviour. §5.2 is explicitly unverified.
- **Not attempted:** any assessment of the 3 features' Mac cost, and any
  audit of the 258 commits from the week of 2026-09-20 (16 of which touched
  `mac-shell`).

## 7. Suggested order, if this gets picked up

1. **The 8-source ceiling** (§5.1). It is a product rule with a real failure
   mode, it is small, and it is the only verified behaviour divergence.
2. **Verify §5.2**, then act or close it.
3. **Per-source dropout policy** (§4.1) — the core half is already there, so
   this is surface plus persistence.
4. The remaining two features, on demand.

OHG stays unported until it earns it.
