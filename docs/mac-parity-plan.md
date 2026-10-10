# macOS parity — audit of 2026-10-09

Supersedes both the 2026-08-07 version (~940 commits stale) and the unmerged
2026-09-27 re-audit on branch `docs/mac-parity-2026-09-27` (commit
`888c45ca`). The 2026-09-27 text was a real improvement over 2026-08-07, but
two of its own claims are now wrong and are corrected below with evidence:
**§5.1** (the ceiling it found missing on Mac already reaches Mac, core-side,
and the actual gap runs the other direction — under-parity, not
over-budget) and **§5.2** (the recording-folder question it left open is
now checked and closes as a non-gap).

**Scope of this audit:** the 2026-09-27 audit's scope (every non-OHG `fix` on
the Windows shell, 2026-09-05 → 2026-09-27) plus the window since: every
commit on `main` from 2026-09-27 to 2026-10-09 (§8).

---

## 1. The headline

**The Mac is not rotting, and it is not a subsystem behind.** The commit
counts say otherwise and they are misleading.

786 commits in the 2026-09-05 → 2026-09-27 window. Of the 161 on
`native-shell/`: **38 are OHG**, **3 are non-OHG features**, ~20 are non-OHG
fixes, and the remainder are refactors, tests and docs.

So the entire Windows-only *feature* lead is three items (§4), and the real
divergence risk is not features at all — it is **behaviour rulings made once,
on Windows, that nobody checked against the Mac** (§5). Of those two real
divergences, one is now understood to run in the Mac's favor (§5.1) and the
other has closed outright (§5.2) — see §8 for what moved in the following
13 days, and the Task list in §7 for what is left to actually ship.

## 2. What is already healthy — do not re-litigate this

- **The shared core is gated on macOS.** `mac-show-drill` (`ci.yml`, `macos-14`)
  builds Metal + AVF + CoreAudio and runs a headless show rehearsal against the
  real core. The 237 `native/` commits in the 2026-09-05 → 2026-09-27 window
  were proven on Mac continuously, not assumed, and the 90 more in the
  2026-09-27 → 10-09 window (§8) keep that proof current.
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
work but not a re-port of the engine. (The OHG show-engine port referenced
elsewhere in this repo's memory is a separate, TypeScript-side effort; it does
not change this ruling — no host imports it yet.)

## 4. Windows-only features the Mac lacks

Three, all small:

1. **Per-source "On dropout" policy** — hold last frame vs black, persisted,
   shipped as `set-source-policy` (#535 slice 4a). **Verified absent** from
   `mac-shell` (no `dropout` reference anywhere in `mac-shell/Sources/`). The
   core half of this landed in `native/` and so already reaches Mac
   (`native/src/core/MediaCore.cpp:2252-2296` — command `set-source-policy`,
   `sourceId` must be `"zoom:<pid>"`, values exactly `"hold"`/`"black"`,
   PRESENT-OR-KEEP semantics, must follow `load-scene-graph` in a batch); what
   is missing is the operator surface and the persistence. **Task 4** below
   closes this.
2. **ISOs armed with Zoom capture off are loud** (T3.7 / #470). The core-side
   contract (`IsoCapturePreflight.Describe`, Windows) warns because ISO writers
   open LAZILY at their first frame — a never-fed source leaves no file and no
   downstream warning is possible; an unobserved capture state is not itself a
   warning (round-2 ruling). Mac has the same lazy-open AVF writers and today
   says nothing. **Task 5** below mirrors it.
3. **Tiles colour pickers** — background, border, glow (T3.5 / #476). See §5.3
   for the reclassification: Mac has no Tiles wall surface to put pickers on,
   so this is N/A-until-Tiles, not a gap to close.

## 5. The real risk: behaviour rulings that live only in WinUI

Of the non-OHG fixes, **8 touched `native/` as well as the shell** — for those,
the core half reaches Mac free and is exercised by the show drill. Named, so
nobody re-does them: the two #535 dropout fixes, both `stream` fixes (warming is
not failing; a failing stream names its own reason), the Zoom join-prompt fix,
#481 (stop muting guests), lower-thirds binding, and live scene routing
startup/shutdown.

The rest are `native-shell`-only. Most of those are WinUI plumbing with no Mac
analogue — discarded scene-canvas element handlers, late-bound page commands as
OneWay, plate tones, D3D device-loss recovery. **Two are not — and the first of
those two is corrected below.**

### 5.1 The Zoom-source video ceiling and the 1080P cap — CORRECTED: Mac is fenced BELOW parity, not over budget

The 2026-09-27 audit read this as "`mac-shell` has no such ceiling... A Mac
operator can currently exceed it with no guard" and filed it as a VERIFIED GAP
in the over-budget direction. **That direction is wrong.** Re-verified
2026-10-09:

- The video-subscription ceiling on Windows evolved **6 → 8 → 10**
  (`native-shell/CoreVideoPro.MediaCore/Services/ZoomMediaSpinePayloadBuilder.cs:19`
  — `DefaultMaxVideoSubscriptions = 10`). The header comment records why: at 6
  and 8, the 7th/8th camera-on participants silently got NO raw video
  subscription at all — frozen/placeholder tiles with no error anywhere (live
  meeting, 2026-08-09: seven cameras on, Susan Cho never subscribed). 10 is the
  number that stopped that failure mode.
- Separately, there is now a **core-side 1080P concurrency cap of 8 cameras**
  (`native/src/modules/ZoomSubscriptionResolutionPolicy.h:94` —
  `kMaxConcurrentFullResolutionCameras = 8`, soak-proven 2026-09-13; granted in
  payload order, i.e. budget order, per spine payload). This lives in `native/`
  and already reaches Mac mechanically — the core doesn't care which shell sent
  the payload.
- But the core **deliberately fences the Mac shell out of that 1080P tier
  today.** `wantsFullResolution` returns true only for `kind ==
  "participant-video"` (`ZoomSubscriptionResolutionPolicy.h:71-72`), and the
  header says verbatim: *"the macOS shell sends kind 'video' with purpose
  'program' for every assigned guest and must not be moved to N x 1080P."*
  Mac sends kind `"video"`, not `"participant-video"` — so it never qualifies,
  and every Mac camera subscription sits at 720P regardless of the 8-camera
  cap.

**So the correct framing flips the 2026-09-27 audit's direction entirely: the
gap is under-parity (no 1080P on Mac at all), not over-budget (a Mac operator
exceeding a bandwidth rule).** There is no missing guard to add — there is a
missing capability to grant, deliberately withheld pending the budget
machinery described below.

Two more things the old audit missed because it only looked for a ceiling,
not for how the ceiling is spent:

- **The real engine path has no camera-on check.** In
  `native/src/modules/ZoomEngineRuntime.cpp:435-470`, `Budget::resolve` (here,
  `fullResolution.resolve(kind, purpose)` at line 469) is called for every
  non-audio subscription with no `videoOn`/`hasVideo` check anywhere in that
  loop — a camera-off `participant-video` subscription burns a 1080P grant
  exactly like a camera-on one would. The camera-on filter is therefore the
  **shell's** job; Windows already does it in `ZoomSourceSetPolicy.Resolve`
  (Program routes → Program Tiles → Preview routes → Preview Tiles →
  multiview slots in slot order → ISO-armed → sticky Tiles audio; camera-off
  sources keep audio but never spend video budget — only video is capped).
  Mac's current `pushSpine` (`mac-shell/AppModel.swift:1294-1360`) subscribes
  `assignedIds`, a **Set** — nondeterministic order, no cap, no camera-on
  filter, all purpose `"program"`. The 10-slot patch bay bounds the count at
  10 today by coincidence, not by policy.
- **A `hasVideo` flip does not currently re-push the spine.** The roster-apply
  path (`AppModel.swift:846-874`) calls `recomputeFromSlots()` only when
  auto-assign placed someone. Harmless today, because nothing filters on
  camera-on yet — but it becomes the mirror of the Windows 2026-08-09
  frozen-tile defect the moment a camera-on filter ships without also wiring
  this re-sync trigger.

This is why Task 2 (budget-ordered, camera-on-filtered, capped spine
subscriptions) and Task 3 (the 1080P kind flip, held separately for live
acceptance) are split in §7 — the machinery and the capability grant are
deliberately decoupled so a reviewer can approve the former without
committing to the latter sight-unseen.

### 5.2 Recording folder resolution — CLOSED, non-gap

The 2026-09-27 audit left this open: *"Whether `mac-shell` resolves its
recording path the same way [as the Windows T2.8 bug class] was not checked."*
Checked 2026-10-09:

Mac's recording folder is computed **absolutely, at send time**, from
`moviesDirectory` — not persisted, not relative:

```swift
// mac-shell/Sources/CoreVideoProShell/AppModel.swift:2334
let folder = (NSSearchPathForDirectoriesInDomains(
    .moviesDirectory, .userDomainMask, true).first ?? NSTemporaryDirectory())
    + "/CoreVideoPro"
```

and the same computation is what `SettingsPane` displays to the operator
(`mac-shell/Sources/CoreVideoProShell/SettingsPane.swift:12-14`,
`var recordingFolder: String`). Nothing is stored in prefs and resolved later
against a possibly-different working directory, so the Windows T2.8 bug class
(a persisted *relative* preference resolved against the core's current
working directory, which could have changed) **cannot occur on Mac** — there
is no persisted value and no relative path anywhere in this path.

**§5.2 closes as non-gap.** No task in this plan touches it.

### 5.3 T3.5 (Tiles colour pickers) — reclassified N/A-until-Tiles

§4 lists Tiles colour pickers (background, border, glow; T3.5 / #476) as a
Windows-only feature. It is Windows UI sitting over a Windows-only surface:
Mac has no dedicated Tiles wall surface to put colour pickers on. The
`mac-shell` "tiles" references that do exist (`multiviewTiles`,
`applyMultiviewTiles`, the multiview grid rendering in `AppModel.swift` and
`ShellApp.swift`) are the multiview wall's grid cells — naming only, not a
Tiles bus with its own chrome controls the way Windows has one.

**Reclassified: N/A-until-Tiles, not a gap.** If and when Mac grows a Tiles
wall surface, the colour-picker parity question becomes live again; until
then there is nothing to port it onto.

## 6. How much of this is verified, and how much inferred

Honesty about method, because the last several plans in this repo were
written against assumed shapes that turned out wrong:

- **Verified directly:** the CI job scopes; that `mac-shell` builds; the
  OHG absence; the dropout-policy absence; the three-way ceiling history and
  the 1080P kind fence (§5.1 — file:line read directly, both in
  `native-shell` and `native/`); the recording-folder resolution (§5.2 —
  file:line read directly in both `AppModel.swift` and `SettingsPane.swift`);
  the absence of a Tiles wall surface on Mac (§5.3); the per-commit tree
  classification for the 2026-09-05 → 09-27 window (via `--name-only`, after
  `--stat`'s path truncation produced a wrong first cut); the §8 window
  commit counts and the named PR numbers (verified by the controller,
  2026-10-09, against `origin/main @ 47feb000`).
- **Inferred from commit subjects and touched paths:** which of the
  `native-shell`-only fixes in the 2026-09-05 → 09-27 window are WinUI
  plumbing versus operator-visible behaviour, and the same classification for
  the 15 Windows-shell-only fixes named in §8.
- **Not attempted:** any assessment of the 3 features' Mac cost beyond what
  Tasks 2-5 (§7) now scope directly; a Mac-side slate-name equivalent to
  Windows' `displayName` on source-policy commands (flagged as an open
  follow-up by Task 4, not shipped blind).

## 7. This plan's task list

Supersedes the 2026-09-27 audit's "suggested order" (§7 there). The full plan
lives at `docs/superpowers/plans/2026-10-09-mac-parity-batch.md`; summary:

1. **Task 1 — this document.** Docs-only; corrects §5.1/§5.2 and unblocks the
   scoping below. (This commit.)
2. **Task 2 — `ZoomSourceBudget`: deterministic, capped, camera-on-filtered
   spine subscriptions.** Pure Swift policy type plus `AppModel.pushSpine`
   wiring. Deliberately wire-conservative: kind stays `"video"` and wire
   purpose stays `"program"` for every entry (purpose is part of the engine's
   subscription identity for kind `"video"`; emitting real purposes now would
   tear down warmed subscriptions on every bus move). What changes: subscription
   order becomes deterministic (budget order: Program → Preview → multiview
   slots → ISO-armed), camera-off participants stop holding video
   subscriptions, the count is capped at 10, and a `hasVideo` change re-pushes
   the spine. This lands the machinery the 1080P flip needs without granting
   1080P yet.
3. **Task 3 — the 1080P flip.** One wire change (`kind:
   "participant-video"`, real per-entry purpose) plus a comment-only update to
   `ZoomSubscriptionResolutionPolicy.h`. Deliberately separated from Task 2 so
   a reviewer can approve the budget machinery while holding the capability
   grant itself. **Gated behind a required live-acceptance step before it
   ships in a tagged beta** (owner meeting, ≥3 cameras: 1920-wide delivery
   confirmed, `fullResolutionDemoted` stays 0 at ≤8 cameras, a camera
   off→on toggle resumes under the real engine, and sustained render holds
   frame rate — the 8×1080P Metal decode path has not been soak-proven on
   this rig the way the Windows CPU/GPU path was on 2026-09-13). If the Metal
   path shows distress, the documented fallback is reverting this task's
   one-line kind flip, which restores the 720P fence; Task 2's budget
   machinery stays either way.
4. **Task 4 — per-source dropout policy: surface, persistence, wire.** Closes
   §4 item 1. The core half already reaches Mac (§4); this task adds the
   `SlotRow` menu (Hold last frame / Black), the `ShowInputSlot` field and
   `pushScenes` wire-up (ordered by sourceId ordinal, zoom-only, sent after
   `load-scene-graph` in the batch), and ShellPrefs persistence following the
   existing `chromaKeys` pattern.
5. **Task 5 — ISO-armed-with-capture-off is loud.** Closes §4 item 2 (T3.7
   mirror). A pure `IsoCapturePreflight.warning(...)` helper plus one call
   site in `toggleRecording()`: warns when ISOs are armed and capture is
   either explicitly off or the observed Zoom snapshot says inactive, but
   never warns on an *unobserved* state (no snapshot has ever reported) —
   warning there would fire on every pre-join recording start. Recording
   still starts; this only makes the failure mode loud instead of silent.
6. **Task 6 — close-out.** Marks §5.1/§4.1/T3.7 SHIPPED with PR numbers in this
   document, records Task 3's live-acceptance result (or its pending status),
   and confirms a full green gate run.

(T3.5 — Tiles colour pickers — stays N/A-until-Tiles per §5.3 and has no task
in this plan.)

## 8. The 2026-09-27 → 2026-10-09 window

Verified by the controller, 2026-10-09, against `origin/main`. 150 commits on
`main` in this window.

- **`mac-shell`: 2 commits**, both #823 (delivery diagnostics instrumentation),
  landing 2026-10-07. Nothing operator-visible changed in the Mac shell in
  these 13 days.
- **`native/`: 90 commits.** These reach Mac via the show-drill gate the same
  way the 2026-09-05 → 09-27 window's 237 did (§2) — proven continuously, not
  assumed.
- **`native-shell`: 39 commits, of which 15 touch only the Windows shell**
  (no `native/` or `mac-shell` paths). Most of those 15 are WinUI plumbing
  with no Mac analogue (same pattern as §5's classification of the prior
  window); the operator-visible ones, named so nobody has to re-derive them:
  - **#610** — stream-degradation surfacing to operators.
  - **#507** — transport-control stability as status changes.
  - **#733 / #746** — core-restart recovery and the Zoom rejoin it required
    after a core crash. Mac received its half of this already: the
    **#659 / #661 / #663** slices (versioned roster facts, audio-route
    revision intent, recovery barriers and bounded control pressure) touched
    `mac-shell` directly, landing the recovery-path work on both shells
    together rather than leaving Mac to catch up later.
  - **#538** — RTMP-ingest Sources UI (`6443c81f`, `8d137762`,
    `b4b6844a` — "RTMP ingest Sources editor and saved listener settings").

All **7 open feature PRs** at audit time (#837, #840, #842–#846) **touch zero
`mac-shell` files** — none of the in-flight Windows feature work (advanced
source grading, lower-third editor/casing, OHG-navigation opt-in, sources
guest-role/dropout UI relocation) creates new Mac-parity debt by landing.

**Net effect on this document:** nothing in this window changes §4's item
count, §5.1's correction, §5.2's closure, or §5.3's reclassification. The
window is further evidence that the divergence risk remains concentrated in
the behaviour-ruling class (§5), not in net-new Windows-only surface area.

OHG stays unported until it earns it (§3).
