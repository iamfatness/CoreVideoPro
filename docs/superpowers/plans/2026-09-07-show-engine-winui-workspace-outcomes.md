# Show Engine — Plan 7b Outcomes (the OHG Show WinUI workspace)

Companion to `2026-09-07-show-engine-winui-workspace.md` and the design spec
`../specs/2026-09-07-show-engine-host-bridge-design.md` §9–§11. Written at the close of Plan 7b
execution (2026-09-07), stacked on Plan 7a (`2026-09-07-show-engine-host-bridge-outcomes.md`).

**Shipped:** the operator surface Plan 7a's outcomes said was owed. The OHG show engine is now
**driveable and configurable from the app** — a first-class **OHG Show** tab in the Produce nav
group, and an **OHG show** section in Settings that edits the same `ohg-show-config.json` the
supervisor reads, hot-restarting the engine on Save. Six pieces:

1. **A pure projection** (`Services/OhgSnapshotProjection.cs` → `OhgSnapshotView`) — the engine's
   camelCase `ShowSnapshot` JSON turned into immutable view records by a function that **never
   throws**: every node read is guarded, a malformed node projects neutral and lands in a warnings
   list rather than taking the tab down.
2. **One outbound seam** (`IOhgActionInvoker` + `OhgActionArgs`) — every button on the tab is an
   `ohg.*` invoke through this interface, with typed arg builders covering all 28 action ids and a
   hand-copied contract table in the tests as the drift tripwire. `FakeOhgActionInvoker` is what
   makes every control testable with no child process.
3. **The view model** (`OhgShowViewModel` + four partials: Panelists, Program, Gallery, Gfx) —
   marshaled ingestion, an `(Generation, Revision)` envelope gate, and **diff-updated** rows via
   `ObservableCollectionSync` (never a bound collection replaced at snapshot rate).
4. **The page** (`Views/OhgShowPage.xaml` + `OhgShowPageLogic`) — status strip, setup surface,
   panelist board, and the program / gallery / GFX-and-data panels, with every code-behind
   callback — **DependencyProperty callbacks included** — routed through one `Guarded(...)` shape.
5. **The config editor** (`OhgConfigEditModel`, `OhgSettingsViewModel`, the Settings section) —
   integrations, Mukana polling, looks, the four preset scenes, `driveHost`, default transition and
   tally URL, validated against the app's CURRENT scene ids, with the apply ORDER extracted as the
   pure `OhgConfigApplySteps.Order(engineRunning)` and the adapter hot-swapped through
   `OhgAdapterSlot`.
6. **A one-shot legacy importer** (`IsadoraConfigImporter`) — reads the old Isadora
   `infrastructure.js` / `mukana.js` and reports `Found`/`NotFound` per probe in plain words. It
   never guesses and never changes capacity.

**Counts.** 20 commits (`7147f69c..HEAD`), 56 files, **+10,104 / −71** lines; 41 of those files
are new (21 production, 16 test, plus the page XAML and its code-behind). Tests at HEAD, all
green and all measured on this box:

| Suite | At HEAD | Δ from this branch |
|---|---|---|
| vitest (`show-engine`) | **948** (43 files) | +3 (the seventh conformance case × three host shapes) |
| `Control.Tests` | **65** | 0 (untouched) |
| `ShowEngine.Tests` | **140** | 0 (untouched) |
| `WinUI.Tests` | **1205** | **+262** (16 new test files) |
| Companion module | **12** | 0 (untouched) |

Release WinUI x64 build: **0 errors**. `smoke:show-engine-host`: handshake, 28 actions, PASS.
`AdapterConformanceTests`: **`conformance: 7/7`**. `verify-dist-barrel`: 7/7 × three host shapes
from `dist`.

## Corrections to the plan discovered in execution

Seven places where execution found the plan wrong. Each was corrected in the code and is listed
here so the next plan starts from the truth.

- **Tab statics cannot live on `StudioViewModel`.** The first cut put `ActiveTabDependentProperties`
  there and the test died with `TypeInitializationException → COMException`: `StudioViewModel`'s
  static initializer builds XAML brushes, which no test host can do. The list — and `ParseTab` with
  it — moved to a pure `StudioTabPlumbing` class. The lesson is narrow and sharp: **a static field
  access on `StudioViewModel` is the hazard, not construction**, which is why `ParseTab` passed
  either way before the field was added.
- **`OhgBoxViewModel Boxes` belongs to Task 5, not Task 8.** The plan mentioned the collection where
  it was *bound* rather than where it was *introduced*; ruled into Task 5 at pre-flight.
- **A `ValueTuple` cannot be a `CommandParameter`.** `AssignBoxCommand((int, int))` stays unbound;
  the XAML routes box assignment through `AssignSelectedSlotToBoxCommand`.
- **Two settings controls cannot be `Command` bindings.** Remove-look sits inside an
  `ItemsRepeater` template with a null DataContext and no `FrameworkElement` to name for
  `ElementName` (a `Window` is not one); Refresh-scenes must redraw the four preset pickers AFTER
  the command has refreshed the scene list, and `Button.Command` runs after `Click`. Both are
  guarded `Click` handlers that execute the command themselves, and the content test asserts the
  commands in the **code-behind** instead of the XAML.
- **`NumberBox.Value` is a `double` and x:Bind will not narrow it back**, so the intervals and the
  per-look box count are `double` mirrors on the settings view model. `OhgConfigEditModel` is
  deliberately not observable; the section binds `[ObservableProperty]` mirrors that write through.
- **The Import button needs file-picker input before the command**, so it too is a guarded `Click`
  handler; two content tests were adjusted to the Click/Guarded shape (a structural consequence,
  not a weakening — both rules still hold and are still checked against real code).
- **`verify-dist-barrel.mjs` could not run on Windows at all** — Plan 7a's outcomes flagged the
  `pathToFileURL` one-liner as "worth doing in 7b" and it is done (Task 12). Its **type-only** step
  still fails here (`spawnSync npx ENOENT`); the runtime and conformance steps pass.

## Decisions worth not re-litigating

Every ruling from the execution ledger, in the order it was made.

**Pre-flight**

- **Task 5 MUST introduce `ObservableCollection<OhgBoxViewModel> Boxes`** (keyed by box,
  diff-updated); Task 8 only binds it.
- **The `SequentialAsyncQueue`'s links read the adapter through a `Func` at APPLY time**, never
  capturing it at enqueue. Cost if wrong: one late command applied through an adapter the config
  save has already replaced.
- **T7/T8/T10 XAML is verified by build + content tests only** (plan-mandated softness); the first
  live look at the tab is an owner action, exactly as 7a's drill was. Cost if wrong: layout defects
  found on first open — which is why the first-launch checklist below exists.

**Projection and ingestion**

- **`Slots` and `Gallery` are not padded.** The engine always emits capacity slots and 16 cells
  (`LiveSlots` / `GalleryDirector` are fixed-size). Cost if wrong: a short grid on a malformed
  snapshot, which is already warned about.
- **Projection warnings surface in `RecentRefusals`** — loud, in the operator's own status surface,
  rather than a silent neutral render.
- **`ShadowLastCommand`'s producer is Task 10's wiring** (the control surface calls
  `VM.SetShadowLastCommand` on each host command in shadow mode), carried into Task 10 rather than
  faked in Task 3.
- **Task 3's minors #1 and #4 were carried into Task 4 as required changes** on the same view
  model: gate on the envelope's `(Generation, Revision)` **before** projecting, and either delete
  or actually drive the row `IsSelected` flag.

**Commands**

- **The no-selection guard on `AddSelectedToFirstEmpty` is a LOCAL refusal**, not a round trip —
  accepted, and (after fix round 1) pinned by a test.
- **A partial `OnApplied(view)` hook in the core file** is how Task 5/6 add post-apply work without
  reaching into `Apply`'s body from outside, keeping `Apply` single-owner.
- **`AssignBoxCommand((int,int))` stays unbound**; box assignment goes through
  `AssignSelectedSlotToBoxCommand` (see corrections).

**Page and settings**

- **The board's panel titles are "Panelists" and "Seats"**, and Task 8's content test asserts those
  plus "Program", "Gallery" and "GFX & data".
- **The `ActiveTabNotifications` list test stands in for the brief's `TabChrome` selector test** —
  it has strictly better mutation coverage (dropping the tab from the derived list reds it).
- **Remove-look and Refresh-scenes are guarded `Click` handlers**; **observable mirrors** on the
  settings view model; **double-typed `NumberBox` mirrors** (all three, see corrections).
- **Per-look scene combos refill only on `Loaded`** — accepted as a deferred minor, because the
  settings window builds a fresh view model every time it is opened, so the gap lives only inside
  one open session.

## Carried

**Carried from Plan 7a, still open** (each was out of 7b's scope for the reason given):

- **The core's positional fallback means an empty box is NOT guaranteed blank.**
  `native/src/core/RouteSourcePolicy.h` makes a route with no participant/capture/media id inherit
  `videoFrames[routeIndex]`, so a cleared OHG box can composite an arbitrary decoded guest. Fixing
  it needs a route-contract sentinel in the C++ core. **This is the one carry with an on-air
  consequence**: "cleared" still means *not the previous guest*, not *blank*.
- **The engine cues each look twice** — `setPreview(look:<id>)` cues the preset with empty routes
  one sequence before `applyLook` cues the same preset with routes. Visible in the goldens as
  `LookPreviewCue`. Harmless now that the adapter seeds its preset map, but the fix belongs in the
  engine's emission order, not the adapter.
- **No per-input lower-third title.** `SetInputLowerThirdTitle` is still a logged no-op — the shell
  has no such surface, and 7b did not add one.
- **Tiles cell ordering is recorded, not driven.** `setGallery` is captured and surfaced; CoreVideo
  Tiles has no explicit cell-order API. A Tiles ordering API is a separate design.
- **Raise-hand is not on the core protocol**, so `handRaised` is always `false`; the hands queue
  remains Mukana's.
- **`tallyUrl` posting.** 7b makes the field **editable** in Settings; nothing still posts to it.

**New carries from this plan:**

- **Per-look unknown fields are dropped.** `OhgConfigEditModel` preserves unknown *engine-level*
  fields in an `Extra` bag (round-trip tested) but has no per-look equivalent, so a hand-written
  look carrying a field the editor does not know loses it on the next Save. Fix = a per-look
  `Extra`, same shape as the engine-level one.
- **Overrides are not exposed on the snapshot (D5).** The override editor is a form, not a grid:
  it can set and delete by PIN but **cannot list** existing overrides, because the snapshot carries
  the effective role and not the override table. The right fix is engine-side (publish `overrides`
  on the snapshot), never deriving it in the shell by diffing roles.
- **Per-look scene combos are not refilled on `ItemsRepeater` recycling** (and "Refresh scene list"
  redraws only the four preset pickers). Closing and reopening Settings picks up new scenes
  everywhere; the gap is within one open session.
- **`SceneIdForName` matches by NAME**, so two scenes with the same name are ambiguous.
- **`ApplyShowConfigAsync` has no unit test of its own** — `MainWindow` is not constructible in a
  test. The apply ORDER is the extracted, tested part; the switch bodies are covered by the build
  and the checklist below.
- **Restart-engine is fire-and-await, not verified healthy.** `SaveStatus` "Saved and applied"
  means *applied, and the restart was issued*; the page's own status strip is the operator-facing
  health signal.

## Final-review findings, fixed pre-merge

A whole-branch review after the last task found ten defects. All are fixed in one commit
(`fix(winui): final review fix wave …`), each with a test; the gates below were re-run green.

| # | Finding | Fix |
|---|---|---|
| **C1** | **The plate-tone picker offered values no engine accepts.** `PlateTones = ["neutral","warm","cool"]`; the engine's enum (`contracts.ts` `PLATE_TONES`) is `neutral \| accent \| guest \| breaking`, and `optionalPlateTone` (`config.ts`) THROWS on anything else — so saving a look with "warm" wrote a config that made the host exit **78**: config rejected, TERMINAL, no respawn. A settings picker could kill the show engine. | One copy of the three enums in `OhgLookChoices`, read by both the pickers and `Validate()`; `OhgSettingsChoicesTests` pins them against literal copies of `PLATE_TONES`/`TALLY_SOURCES`/`BOX_FILLS` *and* against the `contracts.ts` text. `Validate()` now also refuses an out-of-enum `plateTone`/`tallySource`/`boxFill` by name, so a hand-edited config cannot slip through either. |
| **I2** | **A look's `Label` was never validated and `Id` was checked with `IsNullOrEmpty`.** `AddLook()` mints `{Id="", Label=""}` and `ToConfig()` emits `label:""`, which the engine's `requireString` refuses — exit 78 again. | `IsNullOrWhiteSpace` for both; `"Look '<id>' needs a label"`. Tests assert each refusal writes nothing and applies nothing. |
| **I3** | **A refused HOST command reached only `_vm.CommandStatus`.** Spec §10 puts adapter refusals on the OHG tab's status strip — the one the operator is actually looking at. | `OhgShowViewModel.NoteAdapterRefusal(...)` (marshaled, newest-first, capped at 10) called beside the existing `SetShadowLastCommand` push. |
| **I4** | **The seat tap could replace a guest on air by accident.** The seat button both selects the seat and runs `AssignSelectedToSlotCommand`, and `SelectedParticipantId` was never cleared — so "tap seat 4 to seat someone, then tap seat 2 to look at it" fired `ohg.panelist.replace` on seat 2. | Controller ruling: keep the tap-tap gesture. A successful assign (seat or first-empty) CLEARS the selection; a seat tap with no selection only SELECTS the seat, silently ("Select a panelist first" is now guidance for the explicit assign affordances only). |
| **I5** | **Two `Mode=OneTime` bindings on a late-assigned root.** The page's `ViewModel` DP is set after construction, so the "Set up OHG" button was inert and the Gallery honesty note rendered empty. | Both `Mode=OneWay`, plus a content test that fails on ANY `OneTime` binding over a `ViewModel.` path (commands included). |
| **M6** | `_appliedRevision` was set from the PROJECTED view, not the envelope the gate compares — a body without `revision` projects 0, so the gate would re-run the whole diff on every republish (bound-collection churn at snapshot rate). | `Apply(view, snapshot.Revision)`. |
| **M7** | `Attach(bridge)` only subscribed, so a snapshot published between construction and attach was lost until the engine's next publish. | Attach re-reads `Latest`/`Health` AFTER subscribing (worst case: one revision applied twice, which the gate swallows). Tested over a real supervisor + bridge with a scripted child. |
| **M8** | `Page_GuardsEveryUiCallback` only checked that each handler EXISTS. `OnRoleComboLoaded` was not guarded (it delegated to a method with its own try/catch). | The per-handler `MethodBody(...).Contains("Guarded(")` assertion ported from the settings-window test; `OnRoleComboLoaded` wrapped, so there is **no documented exception**. |
| **M9** | `FillChoiceCombo` silently substituted `choices[0]` for a stored value it did not know — the UI then showed something the model did not hold. | The stored value is ADDED as an extra item; the new enum rules refuse it, out loud, at Save. |
| **M11** | The seat `ItemsRepeater`'s `UniformGridLayout` sat in an `Auto` column (infinite available width → ten seats in one row). | That column is `Width="*"`. |

### FIRST-LAUNCH CHECKLIST (owner, on first open of the OHG Show tab)

Consolidated from the Task 7, 8 and 10 reports. Nothing on this page has ever been rendered — the
whole surface is verified by build plus content tests, which is the plan's stated softness — so
this list is the manual gate. Run it in order; items marked ⚠ are the ones most likely to be
wrong.

1. **With no `ohg-show-config.json`,** the OHG tab shows the **setup surface** (not a blank page),
   and its button opens Settings → OHG show.
2. **With a config,** the status strip shows engine state and detail, and the **shadow-mode banner**
   when `driveHost` is false.
3. **⚠ Exactly one surface is visible** — setup *or* workspace, never both. x:Bind function bindings
   are not evaluated while an argument path is unresolved, so a transient double-render would show
   as both.
4. **⚠ Seat grid wrapping.** The seats `ItemsRepeater` uses a `UniformGridLayout` inside an `Auto`
   width column, which offers infinite available width — it may lay all 10 seats in ONE row instead
   of wrapping. Fix by giving the column a `*` width or pinning the repeater's width.
5. **The panelist board** lists panelists, seats and unseated guests; clicking a panelist then a
   seat assigns it.
6. **Right-click flyout targets the seat under the cursor.** *(Final review: NOT a defect — the
   house `ElementName` pattern.* The flyout's `Command` resolves through
   `ElementName=OhgShowPageRoot` and its `CommandParameter` is the row's own `Slot`, which is the
   same shape every other templated command on this page uses.) Still worth one confirming click:
   remove seat 3 and confirm **seat 3** empties.
7. **⚠ Role combo after container recycling.** Scroll the roster far enough to recycle rows: each
   visible row's dropdown must still show ITS role, and changing one must not fire a change for
   another. The `ohg: role …` lines in `launch.log` are the tell.
8. **⚠ Guidance colour reads as guidance, not tally.** Trigger a LOCAL refusal (press "Add to
   first empty" with no panelist selected, or a gallery cell with no seat selected — a bare seat
   tap is now silent by design, see final-review I4) and confirm the amber status line does not
   read as an on-air indication.
9. **Program panel:** PGM/PVW labels, current speaker and queue readouts update; Cut / Auto /
   Preview / Direct cut work; the look picker cues and the box strip pages with Next/Prev guest.
10. **Direct cut before the first snapshot** must be **disabled** until PVW is known.
11. **⚠ Look combo after container recycling / tab switching.** Leave the OHG tab, come back, and
    cue a look from Companion or OSC: the picker must show the engine's current look, never a stale
    or empty selection. (This is the `PropertyChanged` + DP-callback path that Task 10's review
    found broken once already.)
12. **Toggle re-entrancy — CONFIRM ONLY (the latch is already implemented).** Flip AS-follow and
    Smart **from the engine side** and watch `show-engine.log`: the switch must move without the
    page re-sending `asFollow.set` / `gallery.smart.set`. The page applies the value under a
    re-entrancy flag and compares against the last APPLIED value, so this is a confirmation, not an
    open risk; a per-snapshot echo would mean the latch is not holding.
13. **Gallery:** cells render; replace / remove / reset / empty work; Smart gallery toggles without
    echoing. **⚠** Confirm the wall stays **4 columns** at the panel's real width — a narrow window
    could reflow it to 3 and renumber the operator's mental grid mid-show.
14. **Box and gallery-cell flyouts:** right-click a box (Preview this box / Clear box) and a gallery
    cell (Remove from gallery), both with nothing selected and with a seat selected. The local
    refusals ("Select a slot first", "That box is empty") must appear in the status strip, never a
    crash.
15. **GFX & data:** question in/out, headline in/out/change, the three Mukana lamps colour
    correctly, and registry override set/delete work.
16. **Headline / override text fields:** typing must not be clobbered by an incoming snapshot (the
    view model seeds the headline fields only while both are empty).
17. **⚠ Combo popups before the first snapshot.** Open the look picker and the override-role picker
    while the engine is still starting: they must show the configured looks / the five roles with
    no selection — never an empty popup, never a flicker as the first snapshot lands.
18. **Restart Engine works from the status strip** while the engine is wedged.
19. **Save → the engine restarts and the page rebuilds.** Edit a look's label and Save:
    `show-engine.log` shows the child exiting and respawning, `launch.log` carries `ohg: show config
    applied (engine restarted, …)`, and the tab's look picker shows the NEW label **without an app
    restart**.
20. **First-time setup shows the restart-app message.** On a machine with no config, fill the
    section in and Save: the file is written, `SaveStatus` reads "Saved. Restart CoreVideo Pro to
    start the show engine.", and the restart line is visible. After an app restart the engine starts
    and the tab leaves the setup surface.
21. **The scene combos list the app's scenes.** The four preset pickers and each look's scene picker
    list "(none)" plus every scene name from the Scenes tab; picking one and saving stores its **id**
    (check `ohg-show-config.json`), and a stored id whose scene was deleted shows the raw id rather
    than silently blank.
22. **Validation refuses loudly.** Save with a look that has no scene and `driveHost` on: the
    message names the look, and nothing is written or restarted.
23. **Shadow-mode round trip.** With `driveHost` off, drive the engine and confirm the page's "last
    shadowed command" line updates after each host command, and that nothing reaches program.
24. **The seat tap consumes the selection (final-review fix I4).** Tap a panelist, tap a seat: the
    guest is seated AND the panelist selection clears. Then tap a DIFFERENT, occupied seat — it must
    only select that seat; nobody may be replaced.
25. **The "Set up OHG" button opens Settings (final-review fix I5).** On a machine with no config,
    the setup surface's button must actually navigate — it was an inert `OneTime` binding.
26. **The Gallery honesty note is visible (final-review fix I5).** The line under the gallery wall
    must render its text, not an empty block.


## Deferred minors

Every `minor (deferred)` from the ledger, kept so they are findable rather than rediscovered.

| Task | Deferred minor |
|---|---|
| 1 | The garbage test covers 3 of ~15 nodes with wrong kinds; eight near-duplicate guarded readers; per-entry malformed skips are silent |
| 2 | Fully-qualified `Array.Empty`; no null-guard comment in the bridge wrapper |
| 3 | `Dispose` does not fence in-flight marshaled callbacks; `Attach` does not re-read `latest()`; create+update double-updates a new row; duplicate-key dedupe untested; warning dedupe alternates on malformed/clean streams |
| 4 | Fully-qualified `ControlInvokeResult`; redundant selection-flag reassertion at the end of `Apply` (intentional, uncommented) |
| 5 | A `"(page 1/0)"` label when `PageCount` is 0; the empty-`Looks` label fallback is untested |
| 6 | Seeding guard `IsNullOrEmpty` vs refusal `IsNullOrWhiteSpace` asymmetry (safe, uncommented); Hands/Question lamp details unasserted; no whitespace-only refusal variants |
| 7 | `OnActiveTabChanged`'s `foreach` over the derived list is not itself tested (the view model is unconstructible) |
| 8 | `SyncOverrideRoleCombo` reassigns `ItemsSource` on each sync; SmartGallery freshness is tied to `OnAppliedGfx` by comment only |
| 9 | Per-look unknown fields are dropped (no per-look `Extra` bag); `Default()`'s per-look values are unverifiable against a written spec |
| 10 | Per-look combos not refilled on `ItemsRepeater` recycling; `SceneIdForName` matches by name (ambiguous on duplicate names); a partial-apply window on a restart exception (acceptable — `RestartAsync` stops first); a failed restart after Save leaves `ValidationMessage` cleared (the edit was saved; only `SaveStatus` says the engine is down); a transient `OhgShow == null` leaves the previous `Looks` on the combo until the next notification |
| 11 | (none recorded — the two Importants and one minor were all fixed in round 1) |
| 12 | (none) |

## Fix rounds per task

**Six fix rounds across twelve tasks**, one each on Tasks 4, 7, 8, 9, 10 and 11; none on Tasks 1,
2, 3, 5, 6 or 12. No task needed a second round.

| Task | Rounds | What the round fixed |
|---|---|---|
| 1 | 0 | — |
| 2 | 0 | — |
| 3 | 0 | Review clean; two minors carried into Task 4 as required changes |
| 4 | 1 | No test for the accepted no-selection guard on `AddSelectedToFirstEmpty` |
| 5 | 0 | — |
| 6 | 0 | — |
| 7 | 1 | Three unguarded UI callbacks (the process fail-fast class) + four minors folded in |
| 8 | 1 | A DP-changed callback calling `AttachShowEvents` unguarded — **and the guard test was blind to DP callbacks**, so it had to be taught to police them |
| 9 | 1 | An enabled integration with an empty Mukana URL/event emitted a config the engine rejects with exit 78; a corrupt existing config was silently replaced by defaults |
| 10 | 1 | The page never re-attached to a rebuilt `OhgShow` view model (+ 5 minors and 1 self-found) |
| 11 | 1 | `ImportLooks` reported defaults it never wrote when `existing.Looks` was empty; the import-surface content test passed on a XAML comment |
| 12 | 0 | — |

## Mutation results

Rule 7 again: every task brief carried a named "Mutations to run" block, and every implementer ran
them and reported signatures. **33 mutations across 11 tasks, every one red** — and one of them
found a defect in the *harness* rather than the code.

| Task | Mutations | All red? | Note |
|---|---|---|---|
| 1 | 3 | yes | `OnAir` from panelist presence instead of tally; `slot:N` formatting; throw-instead-of-warn on a malformed gallery node (correctly red across three tests) |
| 2 | 3 | yes | unconditional two-arg `panelist.add`; `slot` typed as string; a builder dropped from the 28-id coverage set |
| 3 | 3 | yes | `ObservableCollectionSync.Apply` → `Clear()`+`Add` (reds 6 tests — the intended blast radius for the 0xc000027b rule); revision short-circuit dropped; `marshal` bypassed on `OnLog` |
| 4 | 3 | yes | always-`add` on an occupied slot; the Fail text swallowed; **the carried gate** compared `Revision` only, dropping `Generation` |
| 5 | 3 | yes | `"slot N"` instead of `"slot:N"`; the page suffix dropped from the look label; `Boxes` replaced instead of diff-updated |
| 6 | 3 | yes | the blank-location local refusal dropped; `RegistryLamp` read health instead of capabilities; headline seeding made unconditional (clobbers an operator's in-progress edit) |
| 7 | 3 | yes | a deleted `AutomationProperties.Name`; `SelectedValue="{x:Bind}"` added to a repeater ComboBox; `StudioTab.OhgShow` skipped when deriving the notification list |
| 8 | 3 | yes | a deleted automation name; x:Bind selection on the look ComboBox; the `Guarded` wrapper removed from a toggle handler |
| 9 | 3 | yes | `Extra` preservation dropped; preset validation inverted on `DriveHost`; `apply` called before `Save` |
| 10 | 3 | yes | the apply ORDER scrambled (restart before materialize); the adapter captured at ENQUEUE instead of apply; a deleted automation name |
| 11 | 3 | yes | the Mukana regex anchored to double quotes only; the importer overwriting existing looks; the importer mutating the caller's `existing` model |
| 12 | none named | — | the conformance case IS the assertion, and its golden was read against the case's intent before being pinned |

**The harness finding.** Task 10's red-first pass caught its own content-test helper lying:
`MethodBody`, copied from Task 7, brace-matched from the declaration, so for an
**expression-bodied** handler (`=> Guarded("…", () => { … })`) it returned the LAMBDA body and
missed the `Guarded(` wrapping it — reporting a guarded handler as unguarded. The helper now
detects `=>` before `{` and reads to the terminating `;` at depth 0. This is the same class as Plan
7a's Task 3 mutation that was run against the fixture rather than the emit site: *a mutation only
proves something if you know what it mutated.*

## Process note

**Which authoring rules fired.**

- **Rule 7 (put the mutations in the brief)** — load-bearing again: 33 mutations, every one
  reported unprompted, and one of them (Task 10) exposed a broken assertion helper rather than
  broken production code. That is precisely what rule 7 exists for.
- **Rule 10 (a structural test must exercise the structure)** — Task 8's review is the textbook
  instance and the sharpest finding of the plan: the "every callback is guarded" test **did not
  police DependencyProperty callbacks**, so a DP callback could call `AttachShowEvents` bare and
  the guard test stayed green. The fix was to widen the test until deleting a `Guarded` wrapper on
  a DP callback reds it. A structural rule that cannot see one whole category of the structure is
  not enforcing anything.
- **Rule 9 (a new contract needs a conforming fixture)** — Task 2's hand-copied 28-id contract
  table, and Task 12's seventh conformance case, which pins the ONE thing a single-look suite is
  structurally incapable of asserting: what a host is told about a chair the incoming look does not
  seat.
- **Rule 5 (read the real signature, not the plan's claim about it)** — fired three times, each
  time about a WinUI constraint the plan had guessed at: `ValueTuple` as a `CommandParameter`,
  `NumberBox.Value` as a `double`, and `ElementName` inside an `ItemsRepeater` template.
- **Rule 6 (walk the carried obligations line by line)** — the two Task 3 minors were not filed as
  minors and forgotten; they were promoted into Task 4's brief as required changes on the same view
  model, and `ShadowLastCommand`'s producer was carried explicitly into Task 10's dispatch.
- **Rule 1** fired quietly in the pre-flight conflict scan, which caught the `OhgBoxViewModel`
  ownership ambiguity and the enqueue-vs-apply adapter question before either was written.

**What reviewers found that mutations did not.** Every Important in this plan came from a reviewer.
Two are worth naming because they are different species:

- **Task 10 — the page never followed a rebuilt view model.** `ApplyShowConfigAsync` replaces
  `StudioViewModel.OhgShow` on every Save, but `OhgShowPage.AttachShowEvents` ran only from `Loaded`
  and the `ViewModel` DP callback, and `StudioWorkspace` hosts the page by VISIBILITY — so neither
  fired. The page kept `_subscribedShow` pointing at a **disposed** view model and its look
  `ItemsSource` at that model's `Looks`, so the operator's next look pick sent an **old-config look
  id to the new engine**. Not one of the 1,181 tests green at that moment could see it: it is a
  defect in the *lifetime relationship between two objects*, and no mutation of either object's own
  code exhibits it. The fix extracted the decision as a pure
  `OhgShowPageLogic.ShowSubscriptionChange(previous, current)`.
- **Task 11 — a content test that passed on a comment.** `Section_BindsTheSaveAndImportSurface`
  matched the string `OhgSettings.ImportLegacyCommand`, which appeared in a **XAML comment** above
  the button, not in any binding. A test whose evidence is a substring will happily be satisfied by
  prose. It now asserts the exact `Click="OnOhgImportLegacyClicked"` attribute, `DoesNotContain` for
  the command name in the XAML (so it cannot regress to passing on prose), and that the named
  handler's body actually reaches `ImportLegacyCommand.ExecuteAsync(`. Every content test in this
  plan is a substring test over source text, and this is the failure mode of the entire genre —
  worth re-reading the others with it in mind.

**On 7a's candidate rule 11 — recommend PROMOTING it, with one word changed.** 7a proposed: *a plan
that introduces a cross-process or cross-language seam must drive it end-to-end inside the plan, not
after it.* 7b is corroborating evidence of the general principle but not of that exact wording: this
plan crossed no new process boundary, and yet its two costliest defects (Task 10's stale
subscription, Task 8's blind guard test) are the same *shape* — a contract that holds inside each
component and fails in the **relationship between** them, invisible to any test written from one
side. The generalization the two plans jointly support is: **a plan that introduces a new seam —
process, language, or object-lifetime — must exercise it from both sides in one run, inside the
plan.** 7a's evidence was `AdapterConformanceTests` finding what 1,023 unit tests could not; 7b's is
a page and a view model each perfectly tested and wired to the wrong instance of each other. Promote
it as rule 11 with "or object-lifetime" added; the WinUI-side instrument it implies is the one this
plan did not have — a test that constructs the page's subscription logic against a REPLACED view
model, which is exactly what `OhgShowPageLogic` now makes possible.

**A note on honesty of scope, continued from 7a.** Two of 7a's deliberate no-ops
(`SetInputLowerThirdTitle`, `setGallery` ordering) are still no-ops and still say so at the code
site; 7b added a third of the same shape — the D5 override editor that can set and delete but
cannot list, with the reason (the snapshot does not carry the override table) written where a reader
will meet it. The alternative — deriving the override list in the shell by diffing effective roles
against Mukana's — would have been a fabricated behavior dressed as a feature, and it is exactly
what "never invent a source" forbids one level up.

## Merge preparation corrections

A settings save now catches persistence/apply exceptions, preserves the editable
model, and reports whether the document was saved before apply failed. Two
filesystem/apply regressions verify failure status and successful retry.

Queued host commands capture an immutable adapter binding before UI dispatch.
Execution rejects a retired binding or engine generation. A settings swap raises
the minimum generation until restart, so an old engine callback cannot attach to
the new configuration in that interval. Post-await UI feedback is discarded when
its binding retired. A Take already issued is not undone. This supersedes the
previous read-current-adapter-at-apply rule: old command payloads must not be
reinterpreted against a new show config.

Independent review passed; 40 focused settings/adapter/queue tests passed. Full
CI is rerunning. The known native macOS recording-rate failure remains unchanged.
