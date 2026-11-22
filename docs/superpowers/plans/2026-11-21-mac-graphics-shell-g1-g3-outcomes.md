# Graphics in the Mac shell G1-G3 — outcomes

Plan: `2026-11-21-mac-graphics-shell-g1-g3.md`. Branch `mac-graphics-shell` (from `mac-metal-graphics-g1`
afb05c26). Shell-only lane: `git diff afb05c26..HEAD -- native/` is empty, so the Metal native suites are
unaffected by construction. Not pushed. Independent review: pending (main runs it after this commit).

## Commits

| Commit | Task | What |
|---|---|---|
| da9feb1c | 2 | harness timer seam (cherry-pick of def55f05) |
| 929458b4 | 1 | the plan |
| 804bafd2 | 3 | `KeychainStore`; stream key and Zoom tokens moved onto it, item names unchanged |
| 572bfad5 | 4 | `GraphicsState` decode + `GraphicsGateway` (thin D1 pre-check, one in flight, correlation); gateway/state vectors |
| dcc98d81 | 5 | `GraphicsModel`, Graphics tab Operate/Design, On Air strip, legacy drawer L5/L7/L8/L18a |
| 7f5648fc | 6 | `GraphicsDocumentStore` (schema 3, D3 verbatim preservation); document vectors + a Windows-written show |
| 3342421e | 7-8 | `GraphicsPlayout` (Ensure/Cue/Show/auto-out/pin reinstall) + Operate playlist, inspector, Guest rows, default rows; playout vectors |
| e53739e9 | 9 | F-keys (`GraphicsHotkeys`, monitor on Studio + Graphics) |
| bdd6887d | 10-11 | cue-rides-Take proof; `LowerThirdAuthority` (the one lower-third entry) |
| (this) | 12 | opt-in live check + this doc |

Tasks 7 and 8 share one commit (the playlist model wires the playout's rows and pins), as do 10 and 11
(Task 10 is test-only and lives in the same test file).

## Verification

- Shell suite (`COREVIDEO_SHELL_TESTS=1`): **6561 checks, 0 failed**, including the live check below.
  Without `COREVIDEO_SHELL_LIVE_CORE` the live case skips loudly (6552 checks). Design lint passes.
- MediaCore exporter (`GraphicsShellVectorTests`, `~/.dotnet`): 4/4 green; committed vectors equal what the
  real MediaCore types compute. Vectors: gateway 40 rows, state 5 (two real Metal-core nodes), document 68
  (42 row validity + 26 load outcomes), playout 13, plus `documents/windows-show.json` written by the real
  Windows store.
- **Live check** (D11; `native/build-metal/corevideo-native`, Release, rebuilt incrementally with
  `DEVELOPER_DIR=/Library/Developer/CommandLineTools`: no work, already current with this branch's native
  tree) through `MediaCoreBridge` + `CoreLaunchEnvironment.extras`, real `GraphicsModel`, real `AppModel.take()`,
  Program pixels read from the core's exported 1920x1080 BGRA IOSurface:
  - Show a Title row (interview default rows built from the core's own catalog): accepted, first draw,
    **0.55 %** of Program pixels changed, one ON AIR chip from core truth;
  - Hide: Program **0.000 %** different from the baseline;
  - Cue alone: Program untouched (< 0.1 %); then an ordinary Mac Take: the title is on Program,
    **0.55 %** changed. Cue rides Take is proven on pixels, not assumed.
- Mutations, each built and run against the full suite, all **red**:
  - T4: skip epoch check; accept unknown fields; correlate on lastResult only; drop the in-flight gate; drop the duplicate-key check.
  - T5: strip from accepted results; Clear all skipping show-pending; brand kit only on edit; timing not re-sent on air; old preset kept.
  - T6: drop unknown keys; save when read-only; skip the fingerprint check; skip duplicate-key check; skip the .v2.bak.
  - T7-8: hash not lower-cased; trigger without waiting for the cue revision; auto-out surviving an epoch change;
    update sent when nothing changed; Show before commit; Guest title filled.
  - T9/11: route while typing; Shift+F12 cueing a layout; adopt authority before the Show is accepted;
    Hide to the playlist while authority is legacy (the #1166 defect).

## Deviations from the plan, and why

1. **Gateway reads keys in document order with duplicates** (`GraphicsJSON.topLevelKeys`). JSONSerialization
   keeps only the last duplicate and forgets order, so the first cut let a duplicate field through and could
   name a different first fault than Windows on a multi-fault request. Vector rows pin both.
2. **Managed lower-third branch ported** (D8 named only playlist vs legacy). With authority `instances` and
   no Guest row, Windows hides the managed instances (`ApplyManagedLowerThirdIntentAsync`); routing that case
   to the legacy key would be the #1166 class of bug. Ported without the rebuild path (no Mac UI for it).
3. **Legacy Hide always sends the retire.** The Mac has no shell-side "enabled" flag (Windows
   `ProgramLowerThirdEnabled`), so a Hide right after a Show, before the core echoes, must still go out (the
   existing RC2 control test pins it). Show is skipped only when the key is already up, so it never re-animates.
4. **The stepper `onChange` that set Custom was removed** (drawer and footer): it also fired when a preset moved
   the numbers, so every preset pick read Custom. Edits go through `setBuildInMs/OutMs` instead.
5. **A held F-key fires once** (repeats swallowed). Windows does not say; a hold re-Showing a graphic is unsafe.
6. **Cue fields omit Sources-column mapping** (`GraphicsMappedValues`, a later slice) and **no clock configure**
   (G4). A Windows row with `sourceMappings` plays on Mac with its own field values.
7. **Template install has no library-full eviction** (designs arrive with G8); the refusal text is surfaced.
8. **Unported document sections** (cues, layouts, rules, feeds, data, columns, designs) are counted and
   id-checked in the C#'s check order, not deep-validated (D3); a row's `dataBinding` contents likewise.

## Open items

1. **First preparation is ~8 s on this Metal core, past the 5 s cue budget** (live log: `first Cue after
   8.06s ... cuePreparation=preparing`, finished 8.08 s; the next cue of the same template lands at once).
   So an operator's first Show of a template after a core start is refused "The graphic is still preparing.
   Retry when ready." Measured on a loaded machine (other lanes building, load ~7): **unverified on a quiet
   box**. Native work (prepare cost, or warm the playlist's templates per core generation), outside this lane.
   Windows uses the same 5 s budget; widening it on Mac only was not done.
2. **Spec 5.3 "templates the playlist pins are reinstalled per core generation"**: pins reinstall lazily on
   the next Ensure, not eagerly on epoch change. Eager reinstall would also address item 1.
3. **Suppression flag** is recorded on `GraphicsModel` but not published to the control state or diagnostics.
4. **Windows-show import spike (D5)**: covered by the exported fixture only; asset folders (`PackageRoot`)
   unverified on a real Windows show.
5. **macOS F-key system settings**: the hint line is shown; real hardware with media keys not exercised.
6. **Pre-existing, unrelated**: `GraphicsDocumentStoreTests.DesignsRoundTripAndAreValidated` fails when the
   MediaCore tests run on macOS (`Path.GetInvalidFileNameChars()` is NUL and `/` only there). Not touched.
7. **Pixel proof by Program-surface diff, not a template-region check.** The changed fraction is small
   (0.55 %) but the Hide and Cue-alone legs show the baseline is stable to < 0.1 %.
