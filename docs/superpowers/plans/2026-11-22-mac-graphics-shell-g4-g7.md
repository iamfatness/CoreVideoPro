# Graphics in the Mac shell, G4-G7: names-list editor, countdown transport, local data, crawls, Shot Layouts

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
> `docs/superpowers/plan-authoring-rules.md` is binding. Every task names its mutations; the task report lists
> which went red. **A supplied test that contradicts a reasonable implementation is the first suspect, not the
> code** (rule 4): say so in the report before changing behaviour to fit it.

**Status:** plan, 2026-11-22, DRAFT (owner has not reviewed). Written from `origin/main` e117a8fa.
Branch for implementation: `mac-graphics-shell-g4-g7` off current `main`.

**Goal:** A Mac operator can edit a row's names list, run a countdown, load a CSV/JSON file into a table or crawl
design, publish data revisions to Program, run and control crawls, and put a scene plus its graphics on air with
one Shot Layout Take (guarded when the layout carries values or data pins), all in a Graphics tab laid out like
Windows.

**Architecture:** Ports of the Windows MediaCore types into Swift files under `M/`, owned by the existing
`GraphicsModel` sub-model (no graphics behaviour as `AppModel` methods, except the one guard argument on
`AppModel.take`). Every Windows rule is pinned by vectors the C# exporter computes from the real MediaCore types;
sequenced behaviour is pinned by **transcripts the C# exporter records from the real `GraphicsPlayoutService`**
running against a scripted core, which the Swift scripted core replays. No native code changes.

**Tech Stack:** Swift 5.9 / SwiftUI (mac-shell), xUnit exporter in `native-shell/CoreVideoPro.MediaCore.Tests`
(run under `~/.dotnet`), the shared C++ core (unchanged) for live checks.

**Spec:** `docs/superpowers/specs/2026-11-15-mac-graphics-shell-design.md` (slices G4-G7, sections 2.3, 2.6, 2.7,
5.2-5.7, 7). Windows behaviour specs: `docs/superpowers/specs/2026-11-03-graphics-phase3-data-production.md`,
`...-slice2-crawl.md`, `2026-11-04-graphics-phase3-slice4-shot-layouts.md`. Prior Mac work:
`2026-11-21-mac-graphics-shell-g1-g3.md` + `-outcomes.md`, `2026-11-22-mac-graphics-warmup-outcomes.md`.

Path conventions: `W/` = `native-shell/CoreVideoPro.WinUI/`, `MC/` = `native-shell/CoreVideoPro.MediaCore/`,
`MCT/` = `native-shell/CoreVideoPro.MediaCore.Tests/`, `M/` = `mac-shell/Sources/CoreVideoProShell/`,
`CG/` = `contracts/graphics-shell/`.

---

## What this lane found before planning (verified in code)

1. **Names-list playback and clock configure already landed** (G1-G3 review round: `GraphicsPlayout.variantEntries`,
   `nextVariant`, `convertCell`, `configureClock`). What remains of G4 is the **names-list editor** (Windows: Operate
   inspector "Names list…" flyout, `W/Controls/GraphicsPlayoutControl.NamesList.cs`; parser
   `MC/Services/GraphicsBuilder.cs:166-221` + `GraphicsBuilder.NamesList.cs`) and the **countdown transport** on the
   row card (Windows: `GraphicsPlayoutControl.cs:454-465`, `GraphicsPlayoutService.ClockAsync` `:420-427`). The Mac
   has neither (`grep '"start"\|ClockAsync' M/Graphics*.swift` finds nothing).
2. **Clock configuration is not edited in Windows Operate.** Mode/duration/format/finish are set only in the Builder
   (`W/Controls/GraphicsBuilderControl.cs:117-125,584-591`) and the Cues & automation workspace
   (`GraphicsWorkspaceControl.cs:344-358`), both Design surfaces (Mac slice G8). G4 therefore ports the transport only.
3. **The native prerequisites for G5/G6 already exist.** Metal G2 made tables and crawls render: tables expand to
   rect + text primitives in the shared `native/src/graphics/GraphicsTemplate.cpp`; crawls draw from the prepared
   atlas with `crawlItemX` in `native/src/compositor/MetalCompositorAdapter.mm`;
   `GraphicsCompositionPixel.DataTablePublishesCompleteRowsAndKeepsAcceptedPixelsOnFailure` and
   `GraphicsCompositionPixel.CrawlUsesPreparedTextAndClipsBothDirections` (`native/tests/GraphicsRpcPixelTest.cpp:699,1303`)
   sit inside `#if COREVIDEO_WITH_D3D11 || COREVIDEO_WITH_METAL` and are green on Metal
   (`2026-11-21-mac-metal-graphics-g2-outcomes.md`). Crawl timing, loop-boundary adoption, pause/resume and the
   guarded layout Take (`native/src/core/GuardedLayoutTake.cpp`, in the shared `corevideo_native` library,
   `native/CMakeLists.txt:138`) are platform-neutral.
4. **The Mac Operate layout does not match Windows.** Windows `GraphicsPlayoutControl` (`:46-93`) is a two-column
   grid: left column = header row (PLAYLIST + actions), SHOT LAYOUTS band, a scrolling list, a status line; right
   column = a fixed **300 px** bordered, independently scrolling INSPECTOR. The page above it
   (`W/Views/OverlaysPage.xaml:22-48`) is row 0 = Operate|Design switch with the status text beside it, row 1 = On Air
   strip, row 2 = the playout grid. The Mac (`M/GraphicsOperatePane.swift:430-451`, `M/GraphicsPlaylistPane.swift`)
   puts status, strip, rows and an **inline** inspector into one `ScrollView`. This is the grade-editor defect class
   the owner just rejected, and every G4-G7 inspector addition would deepen it. Task 1 fixes it first.
5. **The Mac inspector would flood with data fields.** `GraphicsModel.editableFields` offers every string/number
   template field; a data-table row has `table-row-1..12` and a crawl row `crawl-items`/`crawl-revision`. Windows
   offers only the Title line plus `GraphicsRowFields.Typeable` (string, no choices, not `name/title/time/label`, not a
   data field; `MC/Services/GraphicsRowFields.cs:35-39`). Ported in Task 1.
6. **Windows' default Shot Layouts reference scene ids the Mac does not have.** `DefaultShotLayouts()` uses `intro`,
   `interview`, `panel` (`MC/Services/GraphicsWorkspaceLibrary.Playlist.cs:40-43`); the Mac scene catalog is
   `fullscreen`, `two-up`, `pip` (`M/AppModel.swift:499-503`). Owner decision 2.
7. **Creating a data table on Windows goes through the design lifecycle and the Builder** (`GraphicsBuilderControl.Data.cs`
   → `GraphicsDesignWorkflow.CreateDataTableAsync` → `GraphicsDesignService.CreateDataTableAsync`/`Create` (installs,
   stores the design and the dataset) → the Builder's "Add to playlist" (`SaveToRowAsync`/`PlaylistRow`)). The Mac has
   no Builder until G8, so G5 carries the needed subset of the design lifecycle (create, `designs` + `dataSources`
   written by the Mac, library-full eviction). Owner decision 1.
8. **Raw JSON number text matters.** `GraphicsLocalData.ReadJson` keeps a number cell's raw text
   (`property.Value.GetRawText()`, so `1.50` stays `1.50`) and refuses case-insensitive duplicate keys.
   `JSONSerialization` loses both. CSV uses `Microsoft.VisualBasic.FileIO.TextFieldParser`, whose defaults trim
   whitespace around unquoted fields and skip blank lines. Both are pinned by vectors (Task 5).
9. **A warming guarded Take blocks ordinary scene syncs in the core.** While `pendingLayoutTake_` exists, any batch
   with a `load-scene-graph` or a transition and no guard is recorded as protocol failure `guarded-layout-take-pending`
   (`GuardedLayoutTake.cpp:44-47`). The Mac's debounced `syncScenes()` (`M/AppModel.swift:3871-3879`) must hold while a
   guarded Take is pending (Task 15). How Windows avoids it is UNVERIFIED; Task 15 step 1 reads
   `StudioViewModel.SyncActiveSceneAsync` and records the answer.

## Native work this lane depends on

**None is blocking.** No native file is changed by this plan. Soft dependencies on the Metal G3-G5 lane:

| Need | Metal slice | If it has not landed |
|---|---|---|
| A graphics-cue step in `scripts/mac-show-drill.py` (master plan rule: a drill with a crawl on air, judged per frame) | Metal G4 (`--graphics-cue`) | Task 12 adds `--graphics-crawl` to the drill script itself (Python only), reusing whatever Metal G4 lands if it is there first. Ask the Metal lane to make its step accept an arbitrary template manifest + fields so one flag serves both. |
| Cross-rasterizer parity for table cells (ellipsis) and crawl item widths vs DirectWrite | Metal G4 (parity reference) | Not a blocker: a Windows-authored table may truncate differently; recorded, not fixed here. |
| Crawl/table behaviour under capacity and cache pressure | Metal G3 | Group D tests already pass on Metal (G2 outcomes); nothing to wait for. |
| Template thumbnails | Metal G5 | Not used by G4-G7 (the look picker is G8). |

## Global Constraints

- Shell only; `git diff main..HEAD -- native/` stays empty. No new wire command (spec 5.3).
- Windows is the oracle. Every rule ported here has an exporter-computed vector or transcript; a Windows rule change
  fails `GraphicsShellVectorTests` until regenerated (`COREVIDEO_WRITE_GRAPHICS_SHELL_VECTORS=1`).
- **Fixtures are written by real Windows code.** Show documents under `CG/documents/` come from the real
  `GraphicsDocumentStore`/`GraphicsShowDocument` serializer in the exporter; never hand-edited. Raw data inputs
  (`CG/data/*.csv|json`) are operator-style files and may be hand-written, but every expected output is computed by C#.
- **Timers are injected.** Playout waits use `GraphicsPlayout.sleep`/`now`; the guarded Take loop uses
  `AppModel.timerSleepForTesting`; tests drive them with `ShellTests.ManualTimers` or an immediate scripted core.
  No wall-clock waits in unit tests.
- **Every feature that sends to the core is re-established after a core restart** (Task 17 audits each one).
- Limits, verbatim from MC: dataset ≤ 1 MiB, ≤ 1,000 rows, ≤ 32 columns, ≤ 12 visible rows, ≤ 8 visible columns
  (`GraphicsLocalData.cs:13-17`); crawl ≤ 64 items, speed 1-1200, gap 0-512, direction left|right, mode loop|single,
  defaults left / 120 / 48 / loop (`GraphicsCrawlData.cs:9-17`); names list ≤ 64 lines, name 1-128, title ≤ 256,
  ≤ 30 cells of ≤ 4096 chars (`GraphicsPlaylist.cs:96-100`); Shot Layout ≤ 16 rows, ≤ 32 layouts, hotkey
  `Shift+F1`..`Shift+F11`, ≤ 32 scalar overrides, string override ≤ 4096 UTF-8 bytes (`GraphicsPlaylist.cs:119-144`,
  `ShotLayoutRowSettings.cs`); guarded Take 1-8 cues, warming expires at 5 s (`GuardedLayoutTake.cpp:83,143`);
  8 instances per core; 16 templates in the native library; 32 designs (`GraphicsDesignIds.MaxDesigns`); gateway
  request cap 32 KiB; document 2 MiB, schema 3.
- Operator copy is Windows' copy, verbatim (quoted in each task by file:line). Vocabulary Cue / Show / Hide.
- New screens follow the Windows **layout structure** named in each task, not just its controls.

## Review Focus

1. **A Windows-authored CSV with spaces, quotes and a blank line** must load to exactly the rows/types Windows
   computes (TextFieldParser trims and skips blanks). Pinned in Task 5 (`csv-trim-and-blank-lines` vector).
2. **A JSON data file with `1.50`, `1e3` and two keys differing only in case** must keep raw number text and refuse
   the duplicate, as Windows. Pinned in Task 5.
3. **A guarded Take while the Mac's debounced scene sync fires** must not let an ordinary batch reach the core during
   warming. Pinned in Task 15 (`guarded-take-holds-scene-sync`).
4. **A Windows show whose Shot Layouts name Windows scene ids** must refuse at Cue with the Windows message and show
   the editor's missing-scene line, never skip the scene or silently retarget. Pinned in Task 16.
5. **A data row on a full native library** must evict unused design versions and retry once, not report "library
   full" while unused designs occupy it. Pinned in Task 8 (`create-evicts-unused-design-when-full`).

---

## Defaults taken (owner may override)

| # | Default | Why | Tasks |
|---|---|---|---|
| D1 | The Operate inspector is a fixed 300 pt right column; the left column holds header, SHOT LAYOUTS band, playlist scroll and status, as `GraphicsPlayoutControl.cs:46-93`. | Layout parity (finding 4) | 1 |
| D2 | Clocks: transport only (Start/Pause/Resume/Reset + readout on countdown rows). No Mac-only clock editor; configuration editing arrives with the Builder (G8). | Windows edits clocks only in Design (finding 2) | 4 |
| D3 | The countdown readout is ported byte for byte, including C#'s `m\:ss` TimeSpan format, which drops hours (3600 s reads `0:00`). | Parity; the quirk is reported for a Windows fix, not diverged | 4 |
| D4 | Data designs: "Create design" in the Mac sheets creates the design **and** adds its playlist row in one step (Windows needs the Builder's "Add to playlist", absent on Mac until G8). | Finding 7; owner decision 1 | 8, 9 |
| D5 | The Mac writes the `designs` and `dataSources` sections (only entries it creates or edits), validating them with the ported `GraphicsDataSource.Valid` and design-id rules; all other sections stay verbatim (G1-G3 D3). | A Mac-created table must persist and reinstall | 7 |
| D6 | The Mac ports `GraphicsDesignEviction` for both the data-design install and the playout pin install, with no editor draft or unsaved-edit terms (the Mac has neither until G8). | Review Focus 5 | 8 |
| D7 | Remote/feed-bound rows (`dataBinding.remote`) keep refusing with the existing G1-G3 text until feeds (G9). Local crawls, including live follow, play from G6. | Feeds are G9 | 11 |
| D8 | Design pane "YOUR DESIGNS" bar mirrors Windows' bar (`GraphicsBuilderControl.cs:646-690`): label, chips, then Open design / Duplicate / Delete / New design / Data table… / Data crawl… / Remote feeds…. Only Data table… and Data crawl… are enabled; the rest are disabled with help "Arrives with the Builder" / "Arrives with remote feeds". | Same layout, honest affordances; owner decision 4 | 9 |
| D9 | "Add default rows" also installs the four preset Shot Layouts when the show has none (Windows `GraphicsPlayoutActions.cs:82-83`), with scene ids per owner decision 2 (default: Windows ids verbatim). | Parity | 13 |
| D10 | While a guarded Take is pending, `AppModel` holds `pushScenes()` and re-arms one sync when the guard resolves. | Finding 9 | 15 |
| D11 | Live checks run the opt-in harness (`COREVIDEO_SHELL_LIVE_CORE=native/build-metal/corevideo-native`), never the owner's app; pixel judgments read the core's exported Program IOSurface as in G1-G3 Task 12. | G1-G3 D11 | 10, 12, 17 |
| D12 | Names-list text uses `\n` line endings on the Mac. The exporter normalizes C#'s `Environment.NewLine` with `.Replace("\r\n", "\n")` before writing a `names-editor` expected value (the area file's own CRLF normalization does not reach escaped `\r\n` inside JSON strings). | Vectors must be platform-stable | 2 |

## Owner decisions needed (recommendation first)

1. **Data designs before the Builder:** create + add to playlist in one step (D4). Alternative: hold G5 table creation
   until G8 and only play/publish Windows-authored data rows.
2. **Default Shot Layout scene ids on a Mac show:** recommend keeping Windows' ids (`intro`/`interview`/`panel`) so a
   show file is identical on both platforms, and showing the editor's existing "Scene '<id>' is no longer in this
   show" line; alternative: map through the Mac director map (`M/AppModel.swift:3964`: intro→fullscreen,
   interview→two-up, panel→two-up) for presets installed on a Mac. Either way, Windows-authored layouts keep their ids.
3. **Clock editing:** confirm it waits for the Builder (D2). The master plan row "G4 names list, clocks" can be read as
   asking for an editor; Windows has none in Operate.
4. **Disabled-but-visible Design bar buttons** (D8) vs hiding them.
5. **Remote/feed rows** refuse until G9 (D7), or play their last published values statically as Windows' playout would.
6. **Sources-column mapping** (`sourceMappings`, Windows Designer slice 5) is still unported on the Mac (G1-G3 deviation
   6). It needs the Sources page's graphics columns; recommend it moves to the Sources workstream, not this lane.
7. **The `m\:ss` readout quirk (D3):** file a `backlog` issue for Windows and keep parity until it is fixed there.

## Carried obligations (rule 6), discharged where

| From | Obligation | Discharged in |
|---|---|---|
| G1-G3 outcomes, review round | Crawl and remote/live-follow rows refuse; names-list editor is G4 | Tasks 2-3, 11 (remote stays, D7) |
| G1-G3 deviation 6 | Cue fields omit Sources-column mapping | Not here (owner decision 6) |
| G1-G3 deviation 7 / warmup open item 3 | No library-full eviction on the Mac | Task 8 (D6) |
| G1-G3 Task 9 | Shift+F1-F11 route to nothing until G7 | Task 16 |
| Warmup outcomes 5.3 | Designs reinstalled per core generation | Task 8 test: a Mac-created data design is reinstalled after restart |
| G1-G3 open item 3 | Suppression flag unpublished | Not here (control lane, G11) |
| Master plan §7 | Drill with a graphics cue judged per frame | Task 12 |

---

## Conformance machinery (shared by Tasks 2-16)

- **Area files** (new, each added with its consumer): `CG/rowfields.vectors.json` (T1), `CG/names-editor.vectors.json`
  (T2), `CG/data.vectors.json` (T5-T7), `CG/layouts.vectors.json` (T13-T14), and transcript areas
  `CG/clock-transcripts.vectors.json` (T4), `CG/data-transcripts.vectors.json` (T10-T11),
  `CG/layout-transcripts.vectors.json` (T14). Row shape stays `{id, operation, input, expected}`.
- **Transcripts.** Extract the scripted core in `MCT/GraphicsPlayoutServiceTests.cs:9-120` (its `Invoke`/`Observe`/
  `Delay` and the instance state machine) into `MCT/GraphicsScriptedCore.cs` (behaviour unchanged; the existing tests
  use it). The exporter runs the **real** `GraphicsPlayoutService` against it and records a transcript row:
  `input = {document: "documents/<file>.json", initialNode: <graphics node>, intent: {op, args...}, script: <named reaction,
  e.g. "accept-all" | "refuse-update" | "cue-fails-second">}`; `expected = {steps: [{command: {type, graphics: <body
  minus requestId/clientId/schemaVersion/coreEpoch>}, nodeAfter: <graphics node>}], reason, documentAfter?: <the
  sections the intent changes>}`. Attempt ids (`layout-<guid>`) and request ids are replaced by `ATTEMPT`/omitted.
- **Swift replay.** `ScriptedGraphicsCore` (`M/ShellTestsGraphicsOperate.swift:11`) gains
  `init(replaying steps: [JSONObject], initialNode: JSONObject)`: command *k* is compared to `steps[k].command`
  (normalized the same way) and answered with `steps[k].nodeAfter`. A transcript passes when the Swift intent sends
  exactly the recorded commands in order and ends with the recorded `reason`.
- **Rule 9 (contracts honored by a fixture):** the C# scripted core honours `cueOwner` (a cue's owner is stored and
  `uncue` with a different owner is refused), `rideTake`, `target: program` updates, crawl `pause/resume/restart`, clock
  actions and `layoutTakeReceipts`; at least one transcript per area exercises each.

Commands used throughout:

```bash
# shell suite (from repo root)
(cd mac-shell && swift build -c release) && COREVIDEO_SHELL_TESTS=1 mac-shell/.build/release/CoreVideoProShell
# exporter (check mode; write mode adds COREVIDEO_WRITE_GRAPHICS_SHELL_VECTORS=1)
~/.dotnet/dotnet test native-shell/CoreVideoPro.MediaCore.Tests --filter "FullyQualifiedName~GraphicsShell"
# opt-in live core
COREVIDEO_SHELL_LIVE_CORE=$PWD/native/build-metal/corevideo-native COREVIDEO_SHELL_TESTS=1 mac-shell/.build/release/CoreVideoProShell
```

Expected for the suites: `0 failed`. The pre-existing macOS failure
`GraphicsDocumentStoreTests.DesignsRoundTripAndAreValidated` (G1-G3 open item 6) is outside the `GraphicsShell` filter.

---

## Slice 0: layout parity (prerequisite for every slice)

### Task 1: Operate laid out like Windows; inspector fields per `GraphicsRowFields`

**Files:**
- Create: `M/GraphicsInspector.swift` (the right column), `M/GraphicsRowFields.swift`, `M/ShellTestsGraphicsLayoutParity.swift`
- Modify: `M/GraphicsOperatePane.swift` (tab header + Operate grid), `M/GraphicsPlaylistPane.swift` (left column only;
  inline inspector removed), `M/GraphicsModel+Playlist.swift` (`selectedKey`, `editableFields` → `GraphicsRowFields`)
- Modify: `MCT/GraphicsShellVectorTests.cs` (+ `RowFieldsVectors`), new `CG/rowfields.vectors.json`

**Interfaces:**
- Produces: `enum GraphicsRowFields { static let handled = ["name","title","time","label"]; static func titleFieldKey(_ row: GraphicsRow) -> String?; static func templateFields(_ row: GraphicsRow, state: GraphicsState?, pins: [GraphicsTemplatePin]) -> [GraphicsTemplateField]; static func typeable(_ row: GraphicsRow, state: GraphicsState?, pins: [GraphicsTemplatePin], titleKey: String?) -> [GraphicsTypeableField]; static func eligibleForMapping(_ f: GraphicsTemplateField) -> Bool; static func isDataField(_ id: String) -> Bool }`
  with `struct GraphicsTypeableField: Equatable { let id: String; let label: String; let maxLength: Int? }`.
  Ports `GraphicsRowFields.cs:20-39,78-81`, `GraphicsMappedValues.Eligible`, `GraphicsDataTable.IsDataField`
  (`table-row-*`, `crawl-items`, `crawl-revision`, `data-revision`), `TitleFieldKey` (`GraphicsPlayoutControl.cs:165-166`).
- Produces: `GraphicsModel.selectedKey: String` (`""`, `rowId`, or `rowId|sourceId`, as Windows `_selectedKey`) and
  `GraphicsModel.inspectorSections(for key: String) -> [GraphicsInspectorSection]` where
  `enum GraphicsInspectorSection: Equatable { case header(title: String, caption: String), name, titleLine(key: String, readOnly: Bool), typeable(GraphicsTypeableField), fromSources, namesList, data, hidesAfter, hotkey, hotkeyHint, openInDesign, noGraphicWarning }`.
  Later tasks fill `namesList` (T3) and `data` (T10/T11); this task emits them only when Windows would
  (`row.kind == guest || hasVariants`; `row.dataBinding != nil`) and renders a placeholder row for each.

**Layout (from `W/Views/OverlaysPage.xaml:22-48` and `GraphicsPlayoutControl.cs:46-93`):**
- Tab, top to bottom: row 0 = HStack[Operate|Design segmented control, then `graphics.status` single line, truncating,
  secondary colour]; row 1 = On Air strip (unchanged view); row 2 = the Operate grid. Status leaves the Operate pane.
- Operate grid = HStack(spacing 12): **left** (flexible) VStack(spacing 8) of [header HStack: "PLAYLIST" mono label,
  Spacer, "Add default rows"], [SHOT LAYOUTS band: empty `EmptyView` until Task 16], [ScrollView of row cards, takes
  the remaining height], [document status line, wraps]; **right** = 300 pt wide bordered rounded panel (Studio.panel,
  Studio.border, corner 8, padding 12) containing its own vertical ScrollView with "INSPECTOR" mono label first.
  With no selection the inspector shows "Select a row or a guest to edit its name, title line and timing."
  (`GraphicsPlayoutControl.cs:599`).
- Row cards stay full-width in the left column; tapping a card selects `rowId`, tapping a Guest/names-list line selects
  `rowId|sourceId` (Windows `Select`, `:470,:489`). The selected card gets the accent border.
- Inspector section order = Windows `RefreshInspector` order (`:598-692`): header (row name or entry label; caption =
  Windows `Caption(row)` `:181-184` or "Guest · <sourceId>"), Name, Title line (read-only + "Edit names in Sources →"
  when Guest+followSources), typeable fields, From Sources toggle (Guest), Names list… (T3), data actions (T10/T11),
  Hides after (labels **Manual / 8 s / 15 s**, `:42`), Hotkey, the hint "Hotkeys work on the Studio and Graphics tabs
  when no text box has focus. Shift+F12 plays everything out." (`:685`), Open in Design (disabled, existing help), the
  no-graphic warning (`:691-692`).

- [ ] **Step 1: Exporter rows.** Add `RowFieldsVectors` to `MCT/GraphicsShellVectorTests.cs`: for each playlist row in
  `documents/windows-show.json`, `documents/windows-names-clock.json` and (from Task 7, add then) `windows-data.json`,
  compute `TitleFieldKey`, `TemplateFieldsOf` ids, `Typeable` (id, label, maxLength) against the real catalog node used
  by `NamesListAndClockVectors`. Include a synthetic data-table row built with the real `GraphicsDataTable.Manifest`
  (fixture invariant: its template fields include `table-row-1`..`table-row-<limit>`; `typeable` must contain none).
  Run write mode, then check mode: PASS.
- [ ] **Step 2: Failing Swift tests** in `ShellTestsGraphicsLayoutParity.swift`:
  `testRowFieldsVectorsReplay` (every `rowfields` row equal), `testInspectorSectionsFollowWindowsOrder` (a Guest row
  following Sources → `[.header, .name, .titleLine(key:"title", readOnly:true), .fromSources, .namesList, .hidesAfter,
  .hotkey, .hotkeyHint, .openInDesign]`; the windows-show `title` row → no `.fromSources`, no `.namesList`),
  `testDataRowOffersNoDataFieldBoxes` (synthetic table row: no `.typeable` whose id has prefix `table-row-`),
  `testHidesAfterLabels` (`GraphicsModel.hidesAfterChoices.map(\.label) == ["Manual","8 s","15 s"]`).
  Run the suite: these FAIL (missing symbols).
- [ ] **Step 3: Implement** `GraphicsRowFields`, `selectedKey`, `inspectorSections`, the views above. `editableFields`
  is deleted; the number-field boxes go (Windows types no numbers in Operate).
- [ ] **Step 4: Layout proof.** Snapshot the Graphics tab at Windows' probe sizes with the harness:
  `COREVIDEO_SHELL_SNAPSHOT=/tmp/g4-1360.png COREVIDEO_SHELL_TAB=graphics COREVIDEO_SHELL_WINDOW=1360x768` and
  `...1600x900`, with a row selected (stage via the test document path). Attach both PNGs to the task report; the
  inspector must be beside the list, not below it, at both sizes.
- [ ] **Step 5: Run** the shell suite: PASS (0 failed). **Mutations** (each must red): inspector rendered inside the
  row card again (`testInspectorIsBesideTheList`: a pure `GraphicsOperateLayout.columns(width:)` returns
  `[.list, .inspector(width: 300)]` for widths ≥ 960 — write this test in step 2 too); `typeable` keeps data fields;
  section order swaps Hides after/Hotkey.
- [ ] **Step 6: Commit** `feat(mac): graphics Operate in Windows' two-column layout; inspector fields per GraphicsRowFields`.

---

## Slice G4: names-list editor and countdown transport

### Task 2: names-list parse and text (port + vectors)

**Files:** Create `M/GraphicsNamesList.swift`, `M/ShellTestsGraphicsNames.swift`; modify `MCT/GraphicsShellVectorTests.cs`
(+ `NamesEditorVectors`); new `CG/names-editor.vectors.json`.

**Interfaces:**
- Produces: `enum GraphicsNamesList { static func parse(_ text: String, existing: [JSONObject], variableIds: [String]?) -> [JSONObject]; static func text(_ variants: [JSONObject]) -> String; static func newRowId(_ name: String, taken: [String]) -> String }`.
  Ports `GraphicsBuilder.ParseNamesList`, `NameListColumns`, `ReadNameListCells`, `ClosingQuote`, `Unquote`,
  `WriteNameListCells`, `NewRowId` exactly (`GraphicsBuilder.cs:58-68,166-221`, `GraphicsBuilder.NamesList.cs`). Variant
  objects use the document's JSON shape `{id, name, title, cells}`. C# `string.Length` is UTF-16: truncations at 128,
  256, 4096 count UTF-16 units; `ToLowerInvariant` for ids keeps only `a-z0-9`.

- [ ] **Step 1: Exporter rows** (`names-editor`), each `{operation:"parse", input:{text, existing, variableIds}, expected:{variants}}`
  or `{operation:"text", ...}`. Inputs must include (rule 3, quantify): plain `name, title` lines; comma, tab and
  semicolon separators; a quoted first cell containing a comma; `""` escapes; a header `name,title,location,Text-Size,bad id,location`
  with `variableIds = ["location","text-size"]` (expected: uppercase id lowered, invalid and duplicate columns dropped);
  `variableIds = null`; header-only input (empty result); a blank line and CRLF input; 70 lines (truncated at 64);
  a 200-character name; a 5,000-character cell; duplicate names (ids `-2`); `existing` with a reordered and an edited
  line (ids kept for unchanged names, as `GraphicsBuilderTests.cs:171-176`); a non-ASCII name (`Zoë Ç`) for `newRowId`;
  a name of digits only (`newRowId` → `graphic`). `text` rows: variants with and without cells (expected text
  normalized per D12). Write, then check: PASS.
- [ ] **Step 2: Failing test** `testNamesEditorVectorsReplay` (every row equal; variants compared as JSON objects).
  FAIL.
- [ ] **Step 3: Implement** `GraphicsNamesList`.
- [ ] **Step 4: Run** suite: PASS. **Mutations:** trim cells before unquoting in header mode (red on the quoted-cell
  row); drop the 64 cap (red); `newRowId` keeps leading digits (red); ids not reused from `existing` (red on the
  edit-stability row).
- [ ] **Step 5: Commit** `feat(mac): names-list parser and writer (port of GraphicsBuilder.ParseNamesList)`.

### Task 3: Names list… editor in the inspector

**Files:** Modify `M/GraphicsInspector.swift`, `M/GraphicsModel+Playlist.swift`; tests in `M/ShellTestsGraphicsNames.swift`.

**Interfaces:**
- Consumes: `GraphicsNamesList.parse/text` (T2), `GraphicsRowFields.templateFields` + `eligibleForMapping` (T1).
- Produces: `GraphicsModel.saveNamesList(rowId: String, text: String, templateId: String, templateVersion: Int) -> String?`
  (nil = saved; else the message to show). Rules from `GraphicsPlayoutControl.NamesList.cs:40-63`: refuse silently
  (return `""`) when the row is gone or its template id/version changed since the popover opened; empty parse of
  non-blank text → `"No names found. Use Name, Title and optional variable IDs in the header."`; otherwise
  `variants = parse(text, existing: row.variants, variableIds: eligible template field ids)` and
  `kind = variants.isEmpty ? (row.followSources ? "guest" : row.kind) : "title"`; save through `setRows` (a failed
  save returns `documentStatus`).

**Layout (from `GraphicsPlayoutControl.NamesList.cs`):** a full-width button "Names list…" in the inspector that opens
a popover (Windows Flyout) 320 pt wide, VStack(spacing 8): header label "Names list CSV" over a multi-line text editor
(min height 130, max 260, wraps, placeholder "name,title,location"), button "Import CSV…" (`NSOpenPanel`, `.csv` only,
replaces the text with the file's contents), button "Save names list", then a wrapping status text. Opening the popover
refills the text from the row (`flyout.Opening`).

- [ ] **Step 1: Failing tests:** `testSaveNamesListTurnsAGuestRowIntoATitleNamesList` (windows-show `guest` row, text
  `"Ada Lovelace, Mathematician\nBo Diddley,"` → kind `title`, two variants with names from the text, `followSources`
  unchanged on disk); `testClearingTheListReturnsAFollowingRowToGuest` (empty text on a names-list row whose
  `followSources` is true → kind `guest`, variants `[]`); `testSaveRefusedWhenTheTemplateChanged` (pass a stale
  version → returns `""`, document unchanged); `testNoNamesMessage` (`"name,title\n"` → exact message above);
  `testCellsOnlyForEligibleFields` (windows-names-clock `panel-names`, header `name,title,text-size,time` → cells
  contain `text-size`, not `time`). FAIL.
- [ ] **Step 2: Implement** `saveNamesList` and the popover.
- [ ] **Step 3: Run** suite: PASS. **Mutations:** kind left unchanged when variants appear (red); variableIds = all
  template fields (red on `time`); template-change guard removed (red).
- [ ] **Step 4: Commit** `feat(mac): names-list editor in the Operate inspector`.

### Task 4: countdown transport on the row card

**Files:** Modify `M/GraphicsState.swift` (decode `instances[].clock {text, state, finishAction, remainingSeconds}`),
`M/GraphicsPlayout.swift` (+ `clock`), `M/GraphicsPlaylistPane.swift` (card row), `MCT/GraphicsShellVectorTests.cs`
(+ `ClockTranscripts`), new `CG/clock-transcripts.vectors.json`; tests `M/ShellTestsGraphicsNames.swift` (or a new
`ShellTestsGraphicsClock.swift`).

**Interfaces:**
- Produces: `GraphicsPlayout.clock(_ entry: GraphicsPlaylistEntry, action: String) async -> GraphicsOperationResult`
  (port of `ClockAsync`, `GraphicsPlayoutService.cs:420-427`: action ∉ start|pause|resume|reset → refuse
  "Clock action must be start, pause, resume or reset."; no instance → "That row is not on Preview or Program yet. Cue
  or Show it first."; else `graphics.clock.control {instanceId, expectedRevision: max(1, revision), action}`; accepted
  reason "Clock <action>."). `GraphicsModel.clock(rowId:action:)` wraps it with `perform`.
- Produces: `GraphicsClockReadout.text(row: GraphicsRow, instance: GraphicsInstance?) -> String`:
  `"<clock.text> · <clock.state>"` when the instance has a clock, else `"<m:ss of durationSeconds> · not cued"` with
  C#'s `m\:ss` semantics (minutes component 0-59, D3).

**Layout (`GraphicsPlayoutControl.cs:454-465`):** only for rows whose `clock.mode == "countdown"`: a horizontal row under
the card header, leading inset 46 pt, spacing 6: the readout (mono 11, secondary), then chip buttons Start, Pause,
Resume, Reset, each disabled while busy or when the row has no instance.

- [ ] **Step 1: Exporter transcripts** (`clock-transcripts`): using `GraphicsScriptedCore` and
  `documents/windows-names-clock.json` (`countdown` row, fixture invariant: `clock.mode == "countdown"`,
  `durationSeconds == 600`), record: `start` with no instance (refusal, zero commands); Show then `start`, `pause`,
  `resume`, `reset` (each one `graphics-clock-control` with the instance's revision); an invalid action `"stop"`
  (refusal). Plus pure rows for `readout`: durations 600, 59, 3600, 5400 (expected `10:00`, `0:59`, `0:00`, `30:00`),
  and an instance clock `{text:"09:41", state:"running"}`. Write, check: PASS.
- [ ] **Step 2: Failing tests:** `testClockTranscriptsReplay`, `testClockReadoutVectors`,
  `testTransportShownOnlyForCountdownRows` (pure `GraphicsModel.showsClockTransport(row)`: windows-names-clock
  `countdown` true, `panel-names` false). FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** suite: PASS. **Mutations:** `expectedRevision` = raw revision 0 for a fresh instance (red on
  the transcript); readout using total minutes (red on 3600/5400); transport shown for wall clocks (red).
- [ ] **Step 5: Commit** `feat(mac): countdown Start/Pause/Resume/Reset on clock rows`.

### Review G4 (independent)

- [ ] Dispatch a fresh reviewer (not the implementer) with: this plan's G4 tasks, `GraphicsPlayoutControl.cs`
  (inspector + clock row + NamesList partial), `GraphicsBuilder.cs:58-221`, and the diff. Ask specifically: does the
  Mac Operate match Windows' **layout structure** at 1360×768 and 1600×900 (two PNGs from Task 1); can a vector be
  deleted with tests still green; does any names-list edit change an on-air line's instance id (ids must be stable).
  Fix every real finding test-first; record verdicts in the outcomes doc.

---

## Slice G5: local datasets

### Task 5: local data parsing (port of `GraphicsLocalData`)

**Files:** Create `M/GraphicsLocalData.swift`, `M/ShellTestsGraphicsData.swift`; modify `MCT/GraphicsShellVectorTests.cs`
(+ `DataVectors`, a partial file `GraphicsShellVectorTests.Data.cs` is fine); new `CG/data.vectors.json`, inputs under
`CG/data/`.

**Interfaces:**
- Produces: `struct GraphicsDataColumn: Equatable { let id: String; let kind: String }`,
  `struct GraphicsDataRow: Equatable { let key: String; let cells: [String] }`,
  `struct GraphicsLocalData: Equatable { let columns: [GraphicsDataColumn]; let rows: [GraphicsDataRow];
  static func read(url: URL, key: String?) throws -> GraphicsLocalData; static func parse(_ text: String, format: String, key: String?) throws -> GraphicsLocalData;
  func withKey(_ column: String?) throws -> GraphicsLocalData; var isValid: Bool; func project(columns: [String], orderBy: String?, limit: Int) throws -> [[String]];
  var json: JSONObject; init?(json: JSONObject) }` with `struct GraphicsDataError: Error, Equatable { let message: String }`
  carrying Windows' exact messages (`GraphicsLocalData.cs`).
- Parsing rules to reproduce (all pinned by vectors, none by prose alone): UTF-8 strict with BOM stripped; 1 MiB on
  bytes; CSV per `TextFieldParser` with `HasFieldsEnclosedInQuotes = true`, delimiter `,`, its default
  `TrimWhiteSpace = true`, blank lines skipped, quoted fields may span lines; kind inference per `ReadCsv` (number =
  `double.TryParse(Float, Invariant)` finite, which accepts surrounding whitespace and exponents and rejects `NaN`/
  `Infinity`; boolean = `bool.TryParse`, case-insensitive, whitespace-trimmed); generated keys `0001`.. (`D4`); JSON via
  a **purpose-built scanner** (depth ≤ 4, raw number text kept, property order kept, duplicate names detected
  case-insensitively); `Project` sort descending by number with `-inf` for blanks and ordinal key tie-break.

- [ ] **Step 1: Inputs + exporter rows.** Commit operator-style inputs under `CG/data/` (fixture invariants stated in a
  `CG/data/README.md` table: row counts, which column is numeric, which keys tie): `scores.csv` (keyed `team`, numeric
  `points` with two ties, a quoted cell with a comma and `""`, spaces around unquoted cells, one blank line, a cell
  `1e3`, a cell ` 7 `, a boolean column `True`/`false`/blank, a non-ASCII team name), `crawl.csv` (one text column, an
  empty cell, 70 rows), `scores.json` (same data as objects, a number written `1.50`, a `null` cell), plus refusals
  generated in the exporter (not committed as files): > 1 MiB, 1,001 rows, 33 columns, a CSV row with a missing cell,
  JSON depth 5, JSON keys `Team`/`team`, JSON mixed kinds in one column, a missing key column, duplicate keys, invalid
  UTF-8 bytes, a `.txt` extension. Rows: `parse` (expected `{columns, rows}` or `{error}`), `withKey`, `project`
  (columns × orderBy null|`points` × limit 1, 3, 12; invariant: the tie rows appear in ordinal key order).
- [ ] **Step 2: Failing test** `testDataParseVectorsReplay` reads each input via `GraphicsLocalData.read` (so the byte
  path is exercised) and compares. FAIL.
- [ ] **Step 3: Implement.** No `JSONSerialization` on the data path.
- [ ] **Step 4: Run** suite: PASS. **Mutations:** CSV without trimming (red on the spaces row); JSON numbers via
  `JSONSerialization` (red on `1.50`); case-sensitive duplicate detection (red); tie-break by original order (red).
- [ ] **Step 5: Commit** `feat(mac): local CSV/JSON datasets parsed exactly as Windows (GraphicsLocalData port)`.

### Task 6: table and crawl fields, manifests and bindings

**Files:** Create `M/GraphicsDataTable.swift`; tests in `M/ShellTestsGraphicsData.swift`; exporter rows in `DataVectors`.

**Interfaces:**
- Produces: `struct GraphicsCrawlOptions: Equatable { var speed = 120.0, gap = 48.0, direction = "left", mode = "loop"; var isValid: Bool }`;
  `struct GraphicsDataBinding: Equatable { var sourceId: String; var keyColumn: String?; var columns: [String]; var sortColumn: String?; var limit: Int; var revision: Int64; var crawl: GraphicsCrawlOptions?; var liveFollow = false; var remote = false; var isValid: Bool; var json: JSONObject; init?(json: JSONObject) }`
  (`GraphicsDataTable.cs:9-23`, Web JSON names `sourceId,keyColumn,columns,sortColumn,limit,revision,crawl,liveFollow,remote`);
  `enum GraphicsDataTable { static func fields(_ data: GraphicsLocalData, _ binding: GraphicsDataBinding) throws -> JSONObject; static func manifest(templateId: String, name: String, binding: GraphicsDataBinding, data: GraphicsLocalData) throws -> JSONObject; static func binding(fromDefinition: JSONObject) -> GraphicsDataBinding?; static func isDataField(_ id: String) -> Bool }`;
  `enum GraphicsCrawlData { static let maxItems = 64; static func items(_ data: GraphicsLocalData, column: String, limit: Int) throws -> [String] }`.
  Cell JSON uses C#'s `UnsafeRelaxedJsonEscaping` (non-ASCII and `"` handling must match byte for byte: vectors carry
  the strings) and the 4096-byte checks.
- `GraphicsDesignSeeds.Manifest("title", id, 1, name)` is the base of both manifests: port it into the same file
  (`MC/Services/GraphicsDesignSeeds.cs`, 23 lines).

- [ ] **Step 1: Exporter rows:** `fields` and `manifest` for table bindings (columns 1..8, sort null|numeric, limit 1
  and 12, remote false|true) and crawl bindings (left|right, loop|single, limit 1 and 64), against `scores.csv`/
  `crawl.csv`; refusals (sort on a text column, 9 columns, a row whose encoded cells exceed 4096 bytes, an empty crawl
  result, a crawl item with a tab); `bindingValid` rows (every clause of `Valid()` false once, rule 3); `binding` from a
  manifest's metadata round trip. Manifests are compared **as parsed JSON objects** (the core hashes its own sorted
  canonical form, `GraphicsTemplate.cpp:697`, so key order is irrelevant; values are not).
- [ ] **Step 2: Failing test** `testDataTableVectorsReplay`. FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** suite: PASS. **Mutations:** rank column not prepended when sorted (red); escaping with
  `JSONSerialization` defaults (red on the non-ASCII cell); crawl limit not applied before the empty filter (red per
  `Items`, which filters then takes).
- [ ] **Step 5: Commit** `feat(mac): data table and crawl fields and manifests (GraphicsDataTable/CrawlData port)`.

### Task 7: document sections the Mac now writes

**Files:** Modify `M/GraphicsDocument.swift`; tests `M/ShellTestsGraphicsData.swift`; exporter `DataVectors` +
fixture `CG/documents/windows-data.json`; new `MCT/GraphicsShellMacDocumentTests.cs`; new fixture
`CG/documents/mac-data-roundtrip.json` (written by the Swift harness, read by C#).

**Interfaces:**
- Produces on `GraphicsDocument`: `var dataSources: [GraphicsDataSourceRecord]` (get/set; set replaces only the
  section), `var designs: [JSONObject]` (get/set, verbatim objects), `mutating func upsertDataSource(_:)`,
  `mutating func appendDesign(_ design: JSONObject)`; `struct GraphicsDataSourceRecord: Equatable { id, name, revision: Int64, receivedAt: Date, data: GraphicsLocalData, feedSequence: Int64?; var isValid: Bool }`
  (`GraphicsWorkspaceLibrary.Data.cs:5-12`). `receivedAt` written as ISO 8601 with offset (System.Text.Json reads it).
- Validation grows exactly where the Mac writes: every `dataSources` entry passes `isValid`; a row's `dataBinding`
  contents pass `GraphicsDataBinding.isValid` with `kind == title && variants.isEmpty`
  (`GraphicsPlaylist.cs:65`); design entries keep G1-G3's id/count checks. Windows check order and messages are kept
  (`GraphicsDocumentStore.cs` Validate; "Invalid graphics datasets." etc.).

- [ ] **Step 1: Exporter fixture** `windows-data.json`, written by the real store from: a data source from
  `scores.csv` keyed `team` (revision 2), one from `crawl.csv` (revision 1); two designs made with the real
  `GraphicsDataTable.Manifest` (a leaderboard sorted by `points`, limit 5; a loop crawl) with ids from
  `GraphicsDesignIds.NewTemplateId(..., hex6: () => "a1b2c3")`/`"d4e5f6"`; rows bound to each (table published at
  revision 1 while the source is at 2, so publish has work to do; crawl `liveFollow: true`); one `shotLayouts` entry
  with `rowSettings` (filled in Task 13). Invariants (stated in the exporter comment): binding `sourceId` equals a
  `dataSources` id; row template ids equal design template ids; table row `revision` < its source `revision`. Document
  vectors: the fixture loads and re-saves to equal JSON; mutated copies (a source with revision 0, duplicate source
  ids, a binding with 9 columns, a remote binding with no key) refuse with Windows' message.
- [ ] **Step 2: Failing tests:** `testWindowsDataDocumentRoundTrips`, `testDataDocumentVectorsReplay`,
  `testMacWritesADataSourceWindowsCanRead` (Swift builds a document from `windows-show.json` + one new source and one
  design, saves it through `GraphicsDocumentStore`, and in write mode
  (`COREVIDEO_WRITE_GRAPHICS_SHELL_VECTORS=1`) stores it as `CG/documents/mac-data-roundtrip.json`; in check mode
  compares). C#: `GraphicsShellMacDocumentTests.MacWrittenDataDocumentLoadsAndValidates` loads that file with the real
  store, asserts `Document.DataSources.Single().Valid()` and the design count. FAIL (all).
- [ ] **Step 3: Implement.** Generate `mac-data-roundtrip.json` once in write mode and commit it.
- [ ] **Step 4: Run** shell suite and exporter: PASS. **Mutations:** `receivedAt` written as epoch seconds (C# test
  red); `dataSources` setter rewriting `feeds` (red: `futureSection` and `feeds` must survive byte-equal); binding not
  validated on save (red on the 9-column copy).
- [ ] **Step 5: Commit** `feat(mac): the graphics document stores datasets and data designs (Windows-readable)`.

### Task 8: create a data design; evict when the library is full

**Files:** Create `M/GraphicsDataDesigns.swift`; modify `M/GraphicsPlayout.swift` (`install` gains eviction),
`M/GraphicsState.swift` (decode `cueTemplateVersion`, `programTemplateVersion`, `image`, `cueImage`, `programImage`,
`programFields`); tests `M/ShellTestsGraphicsData.swift`.

**Interfaces:**
- Produces: `GraphicsDesignIds.newTemplateId(name: String, existing: Set<String>, hex6: () -> String) throws -> String`
  (port, `GraphicsDesign.cs:30-53`; production `hex6` = 6 random lowercase hex);
  `GraphicsModel.createDataDesign(name: String, data: GraphicsLocalData, binding: GraphicsDataBinding) async -> GraphicsDesignOutcome`
  with `struct GraphicsDesignOutcome { let ok: Bool; let message: String; let rowId: String? }`. Sequence (port of
  `GraphicsDesignService.CreateDataTableAsync` + `Create` + `GraphicsDesignWorkflow.PlaylistRow` for a data design,
  D4): name rule (`CleanName`, 1-80 chars, `NameRule` message) → designs < 32 else `TooMany` message → manifest →
  install through the playout gate (`GraphicsPlayout.install`) → on success write the design
  `{designId: templateId, name, family, templateId, templateVersion: 1, definition: <installed definition or manifest>,
  assets: [], updatedAt}` and the data source in one save (`SaveDesignsAsync(..., source)`); a failed save removes the
  installed template (`Remove`) → add the playlist row: `id = newRowId(name)`, `kind` from family (`title`), name,
  `hotkey = next free F-key` (`NextFreeHotkey`), `playOutAfterMs 0`, `fields` = the manifest's field defaults,
  `dataBinding = binding`, `showClock false` (`GraphicsBuilder.RowFrom`, `GraphicsBuilder.cs:73-97`), inserted with
  `Upsert`'s rule (before the empty sponsor placeholder, F-key moved from any other row, `:100-110`). Messages are
  Windows' (`"Created <name>."` then the row sentence from `SaveToRowAsync` `:177`).
- Produces: `GraphicsDesignEviction.evictUnused(document: GraphicsDocument, state: GraphicsState, send:) async -> Int`
  (`MC/Services/GraphicsDesignEviction.cs`, minus editor-draft and unsaved-edit terms, D6) and `install` retries once
  after an eviction that removed ≥ 1 version when the refusal is the library-full text (`GraphicsTemplateInstall.RunAsync`).

- [ ] **Step 1: Failing tests (scripted core):** `testCreateDataDesignInstallsStoresAndAddsARow` (from `scores.csv`,
  binding columns `[team, points]`, sort `points`, limit 5: transcript = `install`, `list` until listed; document then
  has one design whose `definition` equals the C#-exported manifest for the same input (from `data.vectors.json`), one
  data source with revision 1, and a row bound to it with hotkey `F1` on an empty playlist);
  `testCreateRefusesAt32Designs` (exact `TooMany` text, zero commands); `testFailedSaveRemovesTheInstalledTemplate`
  (read-only document: a `graphics-template-remove` follows); `testCreateEvictsUnusedDesignWhenFull` (scripted core
  answers the first install `template-library-capacity`, has 16 templates of which 2 are unreferenced design ids and
  1 is referenced by a row and 1 by a live instance's `programTemplateVersion`: exactly the 2 unreferenced are removed,
  then one retry install); `testPlaylistPinInstallAlsoEvicts` (same, through `ensureCore`);
  `testMacDataDesignIsReinstalledAfterRestart` (create, then `coreGenerationChanged()` and a new-epoch node without
  the design: the warmup resync installs it from `designs`). FAIL.
- [ ] **Step 2: Implement.**
- [ ] **Step 3: Run** suite: PASS. **Mutations:** evict a curated template (red); evict a version a live instance's
  cue copy uses (red); no retry after eviction (red); design written before the install is confirmed (red: the
  install-refused case must leave the document unchanged).
- [ ] **Step 4: Commit** `feat(mac): create data designs and evict unused designs when the library is full`.

### Task 9: Design pane: YOUR DESIGNS bar and the Create data table sheet

**Files:** Create `M/GraphicsDesignPane.swift` (moved out of `GraphicsOperatePane.swift`), `M/GraphicsDataSheets.swift`;
tests `M/ShellTestsGraphicsData.swift`.

**Interfaces:**
- Consumes: `GraphicsLocalData.read`, `GraphicsModel.createDataDesign` (T5, T8).
- Produces: `struct GraphicsDataTableForm { var name: String; var keyIndex: Int; var sortIndex: Int; var limit: Int; var selected: [String]; func binding(sourceId: String) -> GraphicsDataBinding; static func initial(fileName: String, data: GraphicsLocalData) -> GraphicsDataTableForm }`
  (pure; defaults from `GraphicsBuilderControl.Data.cs:22-27`: name = file display name cut to 64, key "Use file order",
  sort "Table · file order", limit 12, the first two columns selected; `sourceId = "data-" + 32 hex`).

**Layout:**
- Design pane top: a bordered panel (Studio.panel, corner 8, padding 10/8) laid out as Windows `BuildDesignsBar`
  (`GraphicsBuilderControl.cs:646-690`): row 1 = HStack["YOUR DESIGNS" mono label, a horizontal scroll of design
  chips (each design's `name`, or "No designs yet. New design starts one from blank or from a look." when empty),
  Spacer, buttons Open design, Duplicate, Delete, New design, Data table…, Data crawl…, Remote feeds… (D8)]; row 2 =
  the design status text spanning the width. Below the bar: the existing "Existing overlay controls" drawer. The
  "Designer arrives in a later release" line stays under the bar.
- "Create data table" sheet (`GraphicsBuilderControl.Data.cs:21-45`): title "Create data table"; a scrolling VStack
  (max height 440, spacing 8): "Design name" field (max 64), summary "<rows> rows · <cols> columns. Selected columns
  appear in source order.", "Row key" picker (Use file order + column ids), "Leaderboard order" picker (Table · file
  order + numeric column ids), "Visible rows" stepper 1-12, one checkbox per column labelled "<id> · <kind>", status
  text; buttons "Cancel" and primary "Create design". A refusal keeps the sheet open with the message (Windows
  `args.Cancel = !result.Ok`).

- [ ] **Step 1: Failing tests:** `testDataTableFormDefaults` (`scores.csv`: name "scores", limit 12, selected = first
  two column ids, key nil, sort nil); `testFormBindingUsesTheSelectedKeyAndSort` (key index 1, sort `points`,
  limit 5 → binding fields; its `isValid`); `testCreateFromFormRefusalKeepsTheSheet` (pure
  `GraphicsDataSheetState.apply(outcome:)` keeps `isPresented` on `ok == false`). FAIL.
- [ ] **Step 2: Implement** the bar and the sheet. File picking via `NSOpenPanel` (`.csv`, `.json`).
- [ ] **Step 3: Snapshot** the Design pane with the sheet open at 1360×768 (attach PNG).
- [ ] **Step 4: Run** suite: PASS. **Mutations:** all columns selected by default (red); limit default 5 (red).
- [ ] **Step 5: Commit** `feat(mac): Design pane designs bar and Create data table`.

### Task 10: Operate data actions: load, publish, revision line

**Files:** Create `M/GraphicsPlayout+Data.swift`; modify `M/GraphicsInspector.swift` (`.data` section),
`M/GraphicsModel+Playlist.swift`; exporter `DataTranscripts` → `CG/data-transcripts.vectors.json`; tests
`M/ShellTestsGraphicsData.swift`.

**Interfaces:**
- Produces on `GraphicsPlayout`: `stageData(rowId: String, binding: GraphicsDataBinding, data: GraphicsLocalData) async -> GraphicsOperationResult`
  and `publishData(rowId: String, waitForProgram: Bool = true) async -> GraphicsOperationResult` (ports of
  `StageDataAsync`/`PublishData`, `GraphicsPlayoutService.Data.cs:99-178`, with the document edits routed through a
  `GraphicsPlayout.saveDocument: (GraphicsDocument) async -> String?` closure the model supplies). Exact reasons are
  Windows' ("Data revision N loaded. Choose Publish data to apply it.", "Data revision N saved for the next Cue or Show.",
  "Data revision N is on Program.", "Data is saved. Show the current design version to put it on Program.", ...). The
  5 s Program-adoption wait polls on `sleep(50 ms)`/`now`.
- Produces: `GraphicsModel.dataLine(row:) -> String` = `"Data · published <binding.revision> · loaded <source revision | unavailable>"`
  (`GraphicsPlayoutControl.Data.cs:14-18`).

**Layout (`GraphicsPlayoutControl.Data.cs`, local rows):** in the inspector `.data` section, VStack(spacing 6): the data
line (wraps), then full-width "Load CSV / JSON…" (ghost) and full-width "Publish data" (accent), both disabled while
either runs. Results go to the status line.

- [ ] **Step 1: Exporter transcripts** from `windows-data.json` + `GraphicsScriptedCore` (rule 9: the scripted core
  applies `target: program` updates and publishes `programRevision`): publish when not on air (zero commands, "saved
  for the next Cue or Show", `documentAfter` row fields = projected revision 2 cells, binding revision 2); publish with
  the table on Program (one `graphics-instance-update target program`, then adoption: "is on Program"); publish with
  the instance on an older `programTemplateVersion` (refusal text); publish where the core never adopts (script
  `never-adopt`: "The Program update is still preparing." after the 5 s budget); stage with liveFollow false (no
  command, document source revision 3); stage a file whose columns no longer fit the binding (refusal = the
  `fields` error text). Write, check: PASS.
- [ ] **Step 2: Failing tests:** `testDataTranscriptsReplay` (driving `GraphicsPlayout` with ManualTimers-backed
  `sleep` and a stepped `now`), `testDataLine` (windows-data table row → "Data · published 1 · loaded 2"),
  `testPublishCommitsTypedTextFirst` (an inspector field typed but not committed is in the update's `fields`: Windows
  `_rowText.Flush()` before publish). FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** suite: PASS. **Mutations:** publish updates `target: cue` (red); skip the template-version
  check (red); wait loop on wall clock (the ManualTimers test hangs → red by timeout guard in the harness).
- [ ] **Step 5: Live proof** (D11): from a Mac-created table design (Task 8 path, `scores.csv`), Show, then stage
  `scores-v2.csv` (committed alongside, one changed score) and Publish: the Program surface changes inside the table
  bounds only, the instance id is unchanged, and no In motion replays (the instance's `visibility` never leaves its
  on-air state between the two samples). Record % changed.
- [ ] **Step 6: Commit** `feat(mac): load and publish local data revisions from Operate`.

### Review G5 (independent)

- [ ] Fresh reviewer with: Tasks 5-10, `GraphicsLocalData.cs`, `GraphicsDataTable.cs`, `GraphicsCrawlData.cs`,
  `GraphicsPlayoutService.Data.cs`, `GraphicsDesignService.cs:60-80,213-238`, `GraphicsDesignEviction.cs`, the diff and
  the live-proof numbers. Ask: which parse vectors could be deleted without a red; does any Mac write change a section
  Windows owns; can the Mac produce a document Windows refuses (run the C# round-trip test on two more Mac-written
  variants); is eviction able to remove something on air. Fix test-first.

---

## Slice G6: crawls

### Task 11: crawl rows play; Create data crawl; crawl controls and live follow

**Files:** Modify `M/GraphicsDocument.swift` (`needsDataPlayback` → `needsFeedPlayback`: remote only, D7),
`M/GraphicsPlayout+Data.swift` (+ `controlCrawl`, `setLiveFollow`, stage→publish when live follow),
`M/GraphicsDataSheets.swift` (crawl sheet), `M/GraphicsInspector.swift`; exporter transcripts (same area file); tests
`M/ShellTestsGraphicsCrawl.swift`.

**Interfaces:**
- Produces: `GraphicsPlayout.controlCrawl(rowId: String, action: String) async -> GraphicsOperationResult` (port of
  `ControlCrawlAsync`, `Data.cs:19-29`: only pause|resume|restart, only when `crawlOnProgram(rowId)` (instance
  `playlist-<rowId>` on Program, visibility ≠ exiting, `crawlPlayback` non-empty), else "This crawl is not on Program.";
  one `graphics.instance.trigger {instanceId, expectedRevision, action}`);
  `GraphicsPlayout.setLiveFollow(rowId: String, enabled: Bool) async -> GraphicsOperationResult` (`Data.cs:31-39`,
  messages verbatim); `GraphicsPlayout.crawlOnProgram(_ rowId: String) -> GraphicsInstance?`;
  `GraphicsDataCrawlForm` (pure, defaults from `GraphicsBuilderControl.Data.cs:85-92`: first text column, Left, Loop,
  120, 48, limit 64; `sample` = "<n> items in source order\n" + first 4 joined by "  •  ").
- `GraphicsState` decodes `instances[].programFields` (for `crawl-revision` / `data-revision`).

**Layout:**
- "Create data crawl" sheet (`GraphicsBuilderControl.Data.cs:79-130`): title "Create data crawl"; scrolling VStack (max
  440, spacing 8): Design name, Text column picker, sample text (max 4 lines), Direction (Left/Right), Playback
  (Loop/Single pass), Speed (px/s) 1-1200, Gap (px) 0-512, Item limit 1-64, status; Cancel / Create design. A file with
  no text column refuses before the sheet with "The crawl needs a text column.".
- Inspector `.data` section for a crawl row (`GraphicsPlayoutControl.Data.cs:96-146`), in this order: the data line;
  when the loaded source is newer than the published revision, "Loaded <n> item(s) · <first 3 joined by '  •  '>"
  (max 3 lines); toggle "Live follow local reloads"; when on Program, "Crawl <playback> · Program data <rev>[ · pending
  <rev> (<preparation>)][ · <warning>]" and an HStack of ghost buttons Pause (enabled when playing), Resume (enabled
  when paused), Restart (always); then Load CSV / JSON… and Publish data.

- [ ] **Step 1: Exporter transcripts:** crawl row from `windows-data.json` — Show (create → cue → trigger; fields carry
  `crawl-items` and `crawl-revision`), pause/resume/restart while on Program (scripted core sets `crawlPlayback`),
  pause when not on Program (refusal, zero commands), publish while the loop crawl plays ("queued for a loop
  boundary"), publish for a single-pass crawl ("saved for the next Cue or Show"), stage with live follow on (stage then
  one Program update), live follow toggle on a table row (refusal "Live follow is unavailable for this row.").
- [ ] **Step 2: Failing tests:** `testCrawlTranscriptsReplay`; `testCrawlRowsNoLongerRefuse` (the windows-data crawl
  row's line state is not `.failed`, Cue enabled); `testRemoteRowsStillRefuse` (a remote binding copy: existing
  G1-G3 text); `testCrawlFormDefaultsAndSample` (`crawl.csv`: sample's first line "64 items in source order",
  invariant: 70 rows with one empty → 64 after the cap); `testCrawlButtonsEnablement` (playing → Pause on, Resume off;
  paused → the reverse; Restart always). FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** suite: PASS. **Mutations:** Resume enabled while playing (red); `needsFeedPlayback` still true
  for liveFollow (red on the crawl row); live-follow stage not publishing (red).
- [ ] **Step 5: Commit** `feat(mac): crawls play on Mac with pause/resume/restart and live follow`.

### Task 12: crawl live proof across a loop boundary; drill with a crawl on air

**Files:** Modify `M/ShellTestsGraphicsLive.swift` (+ crawl leg); `scripts/mac-show-drill.py` (+ `--graphics-crawl`
only if Metal G4's `--graphics-cue` has not landed; otherwise extend that step to take the crawl manifest from
`CG/data/crawl.csv` via a small JSON sidecar).

- [ ] **Step 1: Live leg** (D11): create a loop crawl design (speed 1200, gap 0, `crawl.csv` limited to 3 short items
  so a pass is short), Show; wait on the core's `crawlPlayback == "playing"` (state, not time); Publish a staged
  revision; assert `crawlPendingRevision > 0` immediately after, then (polling the snapshot on the injected sleep, with
  a 20 s budget) that `programFields["crawl-revision"]` changes to the new revision while the instance id and
  `programRevision` lineage are unchanged and `visibility` never left on-air; Pause → two Program samples 0.5 s apart
  are identical; Resume → they differ; Hide → Program returns to baseline (< 0.1 %).
- [ ] **Step 2: Drill:** `python3 -I scripts/mac-show-drill.py --load 8 --graphics-crawl` (or Metal G4's flag) on a
  quiet machine (load < 2.5), judged on per-frame delivery, plus the same run without the crawl. Record both in the
  outcomes doc with hardware, duration and every failure. A run under load is marked advisory, never PASS.
- [ ] **Step 3: Commit** `test(mac): crawl live proof across a loop boundary; drill with a crawl on air`.

### Review G6 (independent)

- [ ] Fresh reviewer with Tasks 11-12, `GraphicsPlayoutControl.Data.cs`, `GraphicsPlayoutService.Data.cs:7-39`, the
  crawl spec, the live-proof log and drill output. Ask: is any crawl assertion satisfiable without a boundary
  adoption; can Pause/Resume act on an exiting crawl; does the drill pair compare like with like.

---

## Slice G7: Shot Layouts and the guarded Take

### Task 13: Shot Layout model, validation, persistence, defaults

**Files:** Create `M/GraphicsLayouts.swift`, `M/ShellTestsGraphicsLayouts.swift`; modify `M/GraphicsDocument.swift`
(typed `shotLayouts` get/set), `M/GraphicsModel+Playlist.swift` (`addDefaultRows` installs presets, D9); exporter
`LayoutVectors` → `CG/layouts.vectors.json`; extend `windows-data.json` with layouts; new fixture
`CG/documents/windows-layouts.json`.

**Interfaces:**
- Produces: `struct GraphicsShotLayout: Equatable { var json: JSONObject; id, name, sceneId, rowIds: [String], rowSettings: [String: GraphicsLayoutRowSettings], hotkey, playOutOthers; var isValid: Bool }`,
  `struct GraphicsLayoutRowSettings: Equatable { var dataPin: (sourceId: String, revision: Int64)?; var overrides: [(key: String, value: Any)] (document order); var isValid: Bool }`
  (`ShotLayoutRowSettings.cs`, `GraphicsPlaylist.cs:119-144`; duplicate keys inside `rowSettings`/`overrides` refused
  through the existing order-preserving key reader `GraphicsJSON.topLevelKeys`).
- Produces: `GraphicsModel.setLayout(_ layout: GraphicsShotLayout) -> String?` and `removeLayout(id:) -> String?`
  (ports of `GraphicsPlayoutActions.cs:87-101` + `SaveShotLayoutsAsync` `GraphicsWorkspaceLibrary.Playlist.cs:73-87`:
  invalid → "That shot layout is not valid."; scene not in the Mac `scenes` → "Scene '<id>' is not in this show.";
  the hotkey moves from any other layout; missing rows → "Shot layouts reference playlist rows that do not exist: …";
  remove of an unknown id → "That shot layout does not exist."); `GraphicsLayouts.slug(_ name: String, taken: [String]) -> String`
  (`GraphicsPlayoutControl.cs:420-429`); `GraphicsLayouts.defaultLayouts() -> [GraphicsShotLayout]`.

- [ ] **Step 1: Exporter:** `windows-layouts.json` from the real store: the four presets, a layout with `rowSettings`
  for a data row (`dataPin` revision 2 + overrides `{heading: "Final"}`), a layout whose override is invalid for its
  template (string on a number field), a layout naming a missing row, a `Shift+F12` copy, 17 rows. `layouts` vectors:
  `isValid` per layout and per settings clause (rule 3: each clause false once); `slug` (`"Two-up!"`, `"9 Panel"`,
  `""`, a 60-char name, a taken slug → `-2`); `setLayout` outcomes against the windows-layouts playlist.
- [ ] **Step 2: Failing tests:** `testLayoutVectorsReplay`, `testSetLayoutMovesTheHotkey`,
  `testDefaultRowsInstallPresetLayoutsOnlyWhenNone`, `testWindowsLayoutsRoundTrip` (load + re-save equal JSON),
  `testMissingSceneRefusesSet` (layout scene `intro` on a Mac scene list → exact message). FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** suite: PASS. **Mutations:** allow `rowSettings` keys not in `rowIds` (red); accept an object
  override (red); presets added on top of existing layouts (red).
- [ ] **Step 5: Commit** `feat(mac): Shot Layout model, validation and persistence (Windows schema)`.

### Task 14: layout resolver, Cue, prepare, cancel, confirm

**Files:** Create `M/GraphicsPlayout+Layouts.swift`; modify `M/GraphicsLayouts.swift` (resolver), `M/GraphicsPlayout.swift`
(`cueCore` gains `owner`/`deadline`/`attempted`, `ensureCore` gains `resolved`); exporter `LayoutTranscripts` +
resolver rows; tests `M/ShellTestsGraphicsLayouts.swift`.

**Interfaces:**
- Produces: `GraphicsLayouts.resolve(document: GraphicsDocument, state: GraphicsState, layout: GraphicsShotLayout, entry: GraphicsPlaylistEntry) throws -> GraphicsResolvedRow`
  with `struct GraphicsResolvedRow { var row: GraphicsRow /* fields replaced */; var identity: GraphicsSourceIdentity?; var dataRevision: Int64?; var dataReceivedAt: Date? }`
  (port of `GraphicsLayoutResolver.Resolve`/`ApplyDataPin`, messages verbatim); `GraphicsRowFields.canOverrideLayoutField`
  and `validLayoutOverride` (T1 file, ports of `GraphicsRowFields.cs:41-47`; `ValidField` per the gateway's field rules).
- Produces on `GraphicsPlayout`: `cueLayout(_ layoutId: String, sceneSelect: (String) async -> Bool, programSubject: GraphicsPlaylistEntry?, sceneGuests: [GraphicsPlaylistEntry], previewStamp: () -> String) async -> GraphicsOperationResult`
  (port of `CueLayoutAsync`/`CueLayout`/`PrepareLayout`, `GraphicsPlayoutService.cs:177-258` + `.Layouts.cs:62-125`);
  `cancelLayout() async -> GraphicsOperationResult` (`.Layouts.cs:46-60`); `confirmLayoutTake(attemptId: String)`
  (`:33-44`); `isPreparedLayoutCurrent(_ p: GraphicsPreparedLayout, previewStamp: String) -> Bool` (`:24-31`);
  `struct GraphicsPreparedLayout { let layout: GraphicsShotLayout; let coreEpoch: String; let attemptId: String; let cues: [(instanceId: String, cueRevision: Int64)]; let previewStamp: String? }`;
  `@Published` on the model: `layoutProgress: GraphicsLayoutProgress?`, `layoutPreparing: Bool`, `preparedLayout`.
  `epochChanged()` also drops `preparedLayout` and `layoutProgress` (restart rule).
- Produces on `GraphicsModel`: `cueLayout(_ id: String)` wiring `sceneSelect` to `AppModel.selectPreviewScene` and
  waiting until the **core** reports `previewScene.sceneId == id` (snapshot read, injected sleep, 5 s), `programSubject`
  = the Guest entry of the Program subject, `sceneGuests` = port of `GuestEntriesRoutedIn`
  (`StudioViewModel.GraphicsPlayout.cs:59-74`) over the Mac routes, `previewStamp` = JSON of
  `{programSceneId, previewSceneId, snapshot previewScene node, preview routes}` (Windows `CaptureLayoutPreviewStamp`).

- [ ] **Step 1: Exporter:** resolver rows over `windows-layouts.json` + `windows-data.json` (published pin, revision-2 pin
  materialized from the source, unavailable revision, changed source, invalid override, template missing, pin-hash
  mismatch, source unavailable). Transcripts: legacy layout (scene select recorded as a pseudo-step, cues with
  `rideTake: true`, PlayOutOthers hides); guarded layout with two rows (cues carry `cueOwner: ATTEMPT`,
  `rideTake: false`; final reason "<name> ready for Take."); second row fails preparation (releases the first with
  `uncue` + `cueOwner`; reason names the row); a required instance already cued (refusal before any command); > 8
  instances (refusal); cancel after ready (owned uncues only; an unrelated cue survives).
- [ ] **Step 2: Failing tests:** `testLayoutResolverVectorsReplay`, `testLayoutTranscriptsReplay`,
  `testPreviewEditDuringPreparationRefuses` (the stamp changes between scene select and the last cue → "Preview
  changed during preparation. Cue the layout again." and owned cues released),
  `testEpochChangeDropsThePreparedLayout`, `testCueLayoutWaitsForTheCoreToShowThePreviewScene` (scripted snapshot
  reports the old preview scene for two polls on ManualTimers; no cue is sent before it switches). FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** suite: PASS. **Mutations:** release cues without `cueOwner` (red); skip the capacity check (red);
  overrides applied before the data pin (red: the C# order is pin then overrides); no stamp re-check (red).
- [ ] **Step 5: Commit** `feat(mac): Shot Layout cue, guarded preparation, cancel and confirm`.

### Task 15: the guarded Take in `AppModel.take()`

**Files:** Create `M/GuardedLayoutTake.swift` (the resolution loop as a small coordinator); modify `M/AppModel.swift`
(`take()` gains the guard path, `canTake`, `pushScenes` hold), `M/GraphicsModel.swift` (`createGuardedTake`); tests
`M/ShellTestsGraphicsGuardedTake.swift`.

**Interfaces:**
- Produces: `struct GraphicsLayoutTakeRequest: Equatable { let operationId: String /* = attemptId */; let coreEpoch, sceneId: String; let previewRevision: Int64; let keepProgramScene, playOutOthers: Bool; let cues: [(String, Int64)]; var wire: JSONObject /* the graphicsLayout object, MediaCoreCommandBuilder.cs:141-152 */ }`;
  `GraphicsModel.createGuardedTake(snapshot: JSONObject, activeSceneId: String) -> GraphicsLayoutTakeRequest?`
  (port of `ITransportHost.CreateGuardedTake`, `StudioViewModel.Transport.cs:324-334`: nil when the prepared layout is
  not current, or (not keepProgram) the core's `previewScene.sceneId` ≠ layout scene or `revision <= 0`).
- `AppModel.take()` behaviour (port of `TransportCoordinator.TakeAsync` `:151-226` + `.Layouts.cs`): a pending guarded
  Take → resolve it (no new transition); `graphics.layoutPreparing` → refuse "Wait for all Shot Layout graphics to
  prepare."; a prepared layout with no current guard → refuse "The Shot Layout cue changed. Cancel and cue it again.";
  guard + no core → "The core is offline. Cue the Shot Layout again after it reconnects."; `keepProgramScene` → no
  scene swap and the batch's `load-scene-graph` is the current Program scene; else the normal swap. The transition
  carries `operationId = attemptId` and `graphicsLayout = request.wire`; the batch is the existing `sceneSyncBatch`
  (it contains no `graphics-*` command; asserted). Resolution: up to 16 rounds, sleeping `min(125 ms, …)` through
  `timerSleepForTesting`; each round reads `graphics.layoutTakeReceipts` for the operation id from the reply or a
  snapshot poll; `committed` → `confirmLayoutTake`, "Program updated"; `refused` → roll back with the existing Take
  rollback and show "Shot Layout Take refused. <reason>"; `ready` → resend the **same** transition (same operation id)
  with a fresh batch; epoch changed → "Core restarted. Cue the Shot Layout again." + rollback; exhausted with no
  receipt and same epoch → "The core could not confirm the Take. Cue the Shot Layout again." (rollback only when
  Program is still the previous scene); otherwise "Shot Layout Take is awaiting confirmation. Retry Take to check the
  same operation." and the guard stays pending. On refusal or failure the prepared layout is cancelled
  (`FinishGuardedTakeAsync`).
- `canTake` adds `!graphics.layoutPreparing` and, with a prepared guarded layout, ignores the "Preview equals Program"
  term when `keepProgramScene`. `pendingGuardedTake != nil` → the Take button reads "Check Take".
- D10: `pushScenes()` returns early while `pendingGuardedTake != nil` and records that one sync is owed; resolution
  runs it.

- [ ] **Step 1: Read and record** how Windows keeps ordinary syncs out during warming (`StudioViewModel.SyncActiveSceneAsync`
  and its callers). Write the answer as a comment in `GuardedLayoutTake.swift`. If Windows sends them anyway and
  tolerates the protocol failure, keep D10 and note the divergence in the outcomes.
- [ ] **Step 2: Failing tests** (scripted core with a `layoutTakeReceipts` script; every wait on ManualTimers):
  `testGuardedTakeBatchShape` (first command `begin-take-transition` with `graphicsLayout.cues` equal to the prepared
  cues and `operationId` = attempt id; exactly one `load-scene-graph`; no `graphics-` type);
  `testCommittedReceiptConfirms`; `testRefusedReceiptRollsBackScenes` (program/preview ids restored, status text);
  `testWarmingThenReadyReplaysTheSameOperation` (receipts warming, warming, ready, committed → two sends, same id);
  `testEpochChangeDuringResolution`; `testNoReceiptAfter16Rounds` (both outcomes: Program unchanged → rollback; changed
  → awaiting-confirmation text, guard kept, next Take resolves without a new transition);
  `testKeepProgramSceneDoesNotSwap` (layout `sceneId ""`); `testTakeRefusedWhilePreparing`;
  `testGuardedTakeHoldsSceneSync` (Review Focus 3: a `selectPreviewScene` during warming sends nothing until the
  receipt resolves, then exactly one sync); `testControlTakeUsesTheGuard` (the control surface's `transport.take`
  path reaches the same guard). FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** suite: PASS. **Mutations:** new operation id on replay (red); rollback skipped on refusal (red);
  scene sync not held (red); guard built without the core's preview revision (red: revision 0 must yield nil).
- [ ] **Step 5: Commit** `feat(mac): guarded Shot Layout Take with receipts, replay and rollback`.

### Task 16: SHOT LAYOUTS band, Edit layouts sheet, Shift+F1-F11

**Files:** Create `M/GraphicsLayoutsPane.swift`; modify `M/GraphicsPlaylistPane.swift` (band slot from Task 1),
`M/GraphicsHotkeys.swift` (`.layoutCue` performs `cueLayout`), `M/GraphicsModel+Playlist.swift`; tests
`M/ShellTestsGraphicsLayouts.swift`, `M/ShellTestsGraphicsHotkeys.swift`.

**Interfaces:**
- Consumes: Tasks 13-15. Produces: `GraphicsModel.layoutSummary(_ layout: GraphicsShotLayout) -> String` (port of
  `LayoutSummary`, `GraphicsPlayoutControl.cs:209-215`, scene names from the Mac scene list, "(missing)" suffixes);
  `GraphicsModel.layoutHeaderButtons() -> [GraphicsLayoutHeaderButton]` (pure, `:237-263`: "Check Take" when a guarded
  Take is pending, "Take" for a prepared guarded layout, "Cue and Take" for a selected legacy layout; "Cancel layout"
  while progress exists (disabled while a Take is pending); "Edit layouts" always);
  `struct GraphicsLayoutRowEditor` (pure model of `AddLayoutRowEditor`, `GraphicsPlayoutControl.Layouts.cs:18-129`:
  revision options "Use published data (revision N)", "Pin revision N", "Pin revision M" for a newer loaded source,
  "Revision K unavailable"; one override per `canOverrideLayoutField` field; "Replace invalid …"/"Remove unavailable
  override …"; `read() throws -> GraphicsLayoutRowSettings?`).

**Layout:**
- SHOT LAYOUTS band (`GraphicsPlayoutControl.cs:217-294`) inside the left column between the header and the playlist:
  header HStack["SHOT LAYOUTS" mono label, Spacer, header buttons]; with no rows: "Appear once you have graphics.";
  with no layouts: "No shot layouts yet. Edit layouts to make one: a scene plus the graphics that ride the Take with
  it."; progress block when present (`AddLayoutProgress`: "<state> · <scene>", one line per row "<name> · <state>[ ·
  data revision N][ · loaded <time>]", the reason in red); then a horizontal scroll of layout buttons (min 150, max
  240 wide; name + "on preview" badge + hotkey badge; second line the summary; accent border when selected). Clicking
  a button selects it and cues it.
- Edit layouts sheet (`OpenLayoutEditorAsync`, `:301-418`): title "Edit Shot Layout"; width 340-430, max height 460;
  a scroll region with: "Layout" picker (layout names + "New layout"), the missing-scene line when the saved scene is
  gone ("Scene '<id>' is no longer in this show; this layout is refused until you pick another."), a two-column row
  [Name field (max 128, placeholder "Open, Two-up, Panel..."), "Scene to Preview" picker ("No scene change" + scenes)],
  "Graphics that ride the Take", one checkbox per row each followed (when checked) by its collapsible "Values and data
  · <row>" editor, "Hotkey" picker (None, Shift+F1..F11), "Other graphics" toggle ("Play out Program graphics not in
  this layout" / "Leave other Program graphics alone"), the footnote "Shift+F1..F11 cue a layout; Shift+F12 plays
  everything out. Cue selects the scene to Preview and stages the graphics; Take carries both."; a pinned footer with
  the error text and buttons "Add layout"/"Save layout" (+ "Remove layout" for an existing one); Close.

- [ ] **Step 1: Failing tests:** `testLayoutSummaryVectors` (exporter rows from `LayoutSummary` over
  `windows-layouts.json`, scene names supplied as input); `testHeaderButtonsPerState` (table over pending × prepared
  guarded × selected legacy × progress); `testRowEditorOptionsAndRead` (windows-data table row at published 1 with
  source at 2 → three options; choosing "Pin revision 2" reads `dataPin(…, 2)`; an invalid saved override reads back
  verbatim unless replaced); `testShiftF2CuesTheLayoutWithThatHotkey` (router + model: Shift+F2 → `cueLayout("two-up")`
  transcript starts with the scene select); `testMissingSceneLayoutRefusesAtCue` (Review Focus 4: windows-layouts
  `open` on Mac scenes → "Open: scene 'intro' is not in this show, so the layout was not cued. Edit the layout to pick
  a scene." and zero commands). FAIL.
- [ ] **Step 2: Implement.**
- [ ] **Step 3: Snapshot** the Operate tab with three layouts and progress shown, and the Edit sheet open, at 1360×768
  (attach PNGs).
- [ ] **Step 4: Run** suite: PASS. **Mutations:** "Take" offered for an unprepared guarded layout (red); Shift+F1-F11
  still routing nowhere (red); editor dropping an invalid override silently (red).
- [ ] **Step 5: Commit** `feat(mac): SHOT LAYOUTS band, Edit layouts and Shift+F1-F11`.

### Task 17: restart audit, live checks, outcomes

**Files:** Modify `M/ShellTestsGraphicsLive.swift`; new `M/ShellTestsGraphicsRestart.swift`; create
`docs/superpowers/plans/2026-11-22-mac-graphics-shell-g4-g7-outcomes.md`.

- [ ] **Step 1: Restart audit tests** (one per feature that sends to the core; each drives `coreGenerationChanged()`
  then a new-epoch node): clock rows configure again on the next Ensure (`graphics-clock-control configure` in the new
  epoch's transcript); a Mac-created data design is reinstalled (Task 8 test reused, asserted here by name); a
  published data row's next Show carries the published cells; a prepared layout and its progress are gone and Take is
  not guarded; a pending guarded Take resolves to "Core restarted. Cue the Shot Layout again."; live follow survives
  in the document; a crawl's paused state is not replayed (Windows does not either; asserted as no trigger sent).
- [ ] **Step 2: Live checks** (D11, quiet machine): (a) a legacy layout (scene + title) via Cue and Take: the scene and
  the title are on the same Program frame (sample after the receipt); (b) a guarded layout with a data pin and an
  override: committed receipt, the pinned values on Program, playlist row fields unchanged on disk; (c) a guarded
  layout refused by the core (make a cue stale by updating it between prepare and Take): Program pixels and scene
  unchanged (< 0.1 %), status "Shot Layout Take refused. …"; (d) names-list editor save then F-key steps through the
  new names on Program; (e) countdown Start advances the readout and the Program pixels between two samples.
- [ ] **Step 3: Full verification:** shell suite (with and without `COREVIDEO_SHELL_LIVE_CORE`), exporter
  `GraphicsShell` filter, `git diff main..HEAD -- native/` empty, design lint. Record counts.
- [ ] **Step 4: Outcomes doc** with commits, verification counts, mutation results per task, live numbers, drill
  results, deviations and open items, then commit `docs(mac): graphics G4-G7 outcomes`.

### Review G7 (independent) and whole-branch review

- [ ] Fresh reviewer for G7 with Tasks 13-17, `GraphicsPlayoutService.Layouts.cs`, `GraphicsLayoutResolver.cs`,
  `TransportCoordinator.Layouts.cs` + `TakeAsync`, `GuardedLayoutTake.cpp:30-160`, the slice 4 spec and the live log.
  Ask: can a Take ever send a `graphics-*` command in a guarded batch; can a refused guarded Take leave Program changed;
  what happens to a scene edit made while warming; can a retried Take replay entrances.
- [ ] Then one whole-branch review on the most capable model against this plan and the four Windows specs, including
  a layout comparison of every new screen against the Windows sources named in each task.

---

## Sizes and order

| Task | Slice | Size | Depends on |
|---|---|---|---|
| 1 Operate layout parity | 0 | M (2-3 d) | none |
| 2 names-list parser | G4 | S | none |
| 3 names-list editor | G4 | S | 1, 2 |
| 4 countdown transport | G4 | S | 1 |
| 5 local data parse | G5 | M | none |
| 6 table/crawl fields + manifests | G5 | M | 5 |
| 7 document sections | G5 | M | 6 |
| 8 data design create + eviction | G5 | M | 7 |
| 9 Design bar + table sheet | G5 | S-M | 8 |
| 10 load/publish in Operate | G5 | M | 1, 8 |
| 11 crawls | G6 | M | 10 |
| 12 crawl live + drill | G6 | S-M | 11; soft: Metal G4 drill step |
| 13 layout model | G7 | M | 1 (7 for the data fixture) |
| 14 resolver/prepare | G7 | L | 13, 10 |
| 15 guarded Take | G7 | L | 14 |
| 16 band, editor, Shift+F | G7 | M | 14, 15 |
| 17 restart audit, live, outcomes | all | M | all |

Estimate for one shell lane: about 6-7 weeks (G4 ≈ 1 week incl. Task 1; G5 ≈ 2-2.5; G6 ≈ 1; G7 ≈ 2-2.5). Tasks 2 and 5
can start in parallel with Task 1.
