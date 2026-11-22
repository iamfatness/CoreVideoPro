# macOS Sources, Phase 2 (S4, S6, R1-R3) Implementation Plan

Status: DRAFT, 2026-11-21. Branch `mac-sources-phase2`, cut from `fc675541` (Mac Phase 1, PR #1220).

**Goal.** Stills and clips can be slot sources. Each Sources row shows a live, identity-bound
thumbnail and a tally border taken from what the core actually rendered on Program and
Preview. The input-row facts finish the Windows accept and reset rules, show frame age and the
applied Zoom cap. Subscription evidence is pinned by a native test and shown on Health. Mac
capture adapters report liveness, so capture rows stop reading "unreported".

**Specs.** `docs/superpowers/specs/2026-11-15-mac-sources-rebuild-design.md` (S4, S6, §4.4-4.5),
`docs/superpowers/specs/2026-11-15-mac-comms-social-remaining-design.md` (C1, C2; slices R1-R3).
Phase 1: `2026-11-16-mac-sources-phase1.md` and its outcomes.

**Paths.** Mac shell: `mac-shell/Sources/CoreVideoProShell/`. Windows: `native-shell/CoreVideoPro.WinUI/`.
Core: `native/src/`. Line numbers were read at `fc675541`.

## Global constraints

- Commit per task, ending `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. No push, no PR.
- Shell build/test: `cd mac-shell && swift build -c release && COREVIDEO_SHELL_TESTS=1 ./.build/release/CoreVideoProShell`;
  `bash scripts/lint-mac-design.sh`; the ContractParity scan (`scripts/contract-parity` or the test that runs it).
  Baseline at `fc675541`, measured 2026-11-21: **5676 shell checks passed, 0 failed, 389 cases**.
- Native build: CI's `native-metal-macos` configure (`.github/workflows/ci.yml:295-305`) into `native/build-metal`.
  Baseline: builds clean in 1:33 (`-j 6`).
- New shell tests go in a new `ShellTestsSources2.swift` (`sourcesPhase2Cases`), registered next to `sourcesCases`.
- THE SLOT LAW stands: nothing in this plan writes `slots` except an operator action (picker, unassign) through the
  existing `ShowInputWriteScope` reasons. Media slots never auto-assign. `SlotNamePolicy` (#1213) governs names.
- The multiview `kind` is `"capture"` / `"media"` / participant, never `"capture-input"` (that is the scene-route mode).
- `MetalCompositorAdapter.mm` is being edited by another agent: this plan's hook there is one member, one override and
  one defaulted parameter. Everything else is in new files or shared core code.
- `plan-authoring-rules.md` binds: each task names mutations; each mutation's result goes in the outcomes doc.

## Defaults taken (owner may override)

| # | Default | Why | Changes if overridden |
|---|---|---|---|
| D1 | Thumbnails come from a core **source sheet** (spec §4.5, master-plan owner decision 1 recommendation), not a multiview crop. The comms spec C1/R2 asks for a crop for the R2 "row preview"; the Sources spec rejects the crop because a reflow can show the wrong guest and a 4-tile wall leaves slots 5-10 blank (Windows forbids it: `ShowInputsCoordinatorTests.CompositeLayoutCannotSupplyOrMoveAnIdentityBoundThumbnail`). R2's row preview IS the sheet cell. | Identity by construction | Tasks 9-10 |
| D2 | The sheet pass does NOT wait for GPU completion (Program's own IOSurface is not waited either). It runs every 4th display tick on its own phase, and only while demand is live. Its CPU encode time joins the monitor shed cost. | "Thumbnails must not cost Program frames" | Task 9 |
| D3 | No `source-sheet-shared-texture` event. The `sourceSheet` snapshot node carries the surface id and cells; the shell reads it at snapshot rate. The IOSurface id is stable, so only cell identity rides the snapshot. | One fewer wire item; cells change only on reassignment | Task 10 |
| D4 | PVW tally comes from the core: a new `previewFrame.videoSources` node, published from the Preview plan the core actually built (the twin of `programFrame.videoSources`). The shell stops deriving PVW from its own Preview routes. | Brief: tally from core evidence, not shell intent | Task 6 |
| D5 | Media rows: signal LIVE when the bus has a `media:<id>` row with a picture, else NO INCOMING; never STALLED (a still never advances). Windows reads every non-Zoom slot as a capture device and shows NO INCOMING for media (`MultiviewInputRowsProjection.cs:367-384`): this is a deliberate departure. | A still is not a stalled feed | Task 4 |
| D6 | The cap note reads the core's applied ceiling (`zoomSubscriptionChurn.cameraMaxResolution` tier 0/1/2, `cameraMaxFps`), captured at join (`ZoomEngineRuntime.cpp:250-265`). The Mac has no quality picker (Z1 not built), so the note says "up to 1080p requested" when a guest delivers below 1920×1080. Copy is Windows `ApplyCameraCap` (`MultiviewInputRowsProjection.cs:144-159`). | Core truth, "the ceiling accepted at this meeting's join" | Task 3 |
| D7 | Capture liveness on Mac: AVFoundation cameras report `cadenceTrusted = true`; ScreenCaptureKit reports `cadenceTrusted = false` (it sends frames only when the screen changes, so silence is not loss). This turns on the Windows on-air dropout rules for Mac cameras: a camera silent for 1 s reads stalled (hold or black per policy), and after 5 s a failed slate. **Program behaviour change**, flagged for the owner. | Windows parity (#535 slice 4a) | Task 7 |
| D8 | One inspector close control: the tool-row toggle (Windows `SourcesWorkspaceControl.cs:74,152`). The inspector header's own "Close inspector" button (Windows `SourcesWorkspaceControl.Inspector.cs:26`) is dropped on Mac, because two buttons with the same name confuse VoiceOver and the owner flagged it. | Orchestrator polish item | Task 1 |
| D9 | The Media status line is not drawn when it says the same thing as the selected-media summary line above it. | Orchestrator polish item | Task 1 |
| D10 | Clips are offered as slot sources. A clip slot plays through its scene routes like any clip (Phase 1 transport). In the multiview, a still in a slot is decoded for the wall even when no scene uses it (new `mediaAssetPath` on the multiview entry); a clip tile shows a picture only while a bus is playing it. | Clips have transport state; a wall-only decoder would be a second, unowned transport | Tasks 4-5 |
| D11 | Sheet cell 640×360, 5×2 (3200×720). | Spec §4.5 | Task 9 |

## Spec corrections (verified in code)

1. `set-source-preview-demand` is a **3-second lease** (`core/SourcePreviewDemand.h:9-12`), not a toggle. The shell
   renews it every second while the Sources tab is visible.
2. The Mac multiview payload carries no `slot` field (`AppModel.swift:2765-2783`); the core auto-numbers by position
   (`MediaCore.cpp:3650-3661`), so a filtered list mis-numbers slots. Task 9 adds `slot: id-1` (Windows sends
   `SlotNumber - 1`, `ShowInputRosterService.cs:735-755`).
3. A multiview `media` source has no picture unless a scene route names the asset with a path: the still cache is fed
   only from routes (`MediaCore.cpp:2313-2339`). Task 5 adds the multiview path.
4. R1's accept rules are mostly enforced already, split between `ZoomRosterProjection` (stale/retired/out-of-order
   rosters) and `SourceRowFactsProjection` (epoch, generation, frame clock). The remaining gap: an unrevisioned or
   empty-epoch roster still reaches `applyRoster` ("older core" path, `ZoomRosterProjection.swift:34-39`); Windows
   ignores it (`MultiviewInputRowsProjection.cs:80-82`).
5. R2's REC chip exists (`IsoWriterChip`, Phase 1 Task 4). R2 adds the wired four-state proof and the cap note.
6. R3: the five Mac evidence lines are written inline in `main-macos.mm:1290-1351`, matching `engine-video.cpp:69-115`.
7. Capture liveness: confirmed unreported on Mac. Only `ShmCapturePreparation.cpp` writes `captureLiveness`; the
   AVFoundation (`AvfCaptureSessionState.h`) and ScreenCaptureKit adapters never do, so `SourceVideoIngress.cpp:124`
   writes `"unreported"` and `dropoutPolicyApplies:false`.
8. Windows tally comes from the core's multiview tile list (`MultiviewTileCropPolicy.Tally`), and the core never marks a
   source tile `pvw` (`MediaCore.cpp:4108-4110`). So Windows has no core PVW either; D4 adds one.

## Tasks

| # | What | Lane | Shared native? |
|---|---|---|---|
| 1 | Polish: one inspector close, Media status line once | shell | no |
| 2 | R1: facts only from revisioned rosters; frame age on the row; `W×H@fps` in the inspector | shell | no |
| 3 | R2: cap note from the core's applied cap; REC chip wired four states | shell | no |
| 4 | S4 shell: media slots (stills and clips), picker Media group, routes, persistence, facts | shell | no |
| 5 | S4 core: multiview stills decode without a scene route | native (shared `MediaCore.cpp`) | **yes** |
| 6 | PVW tally from the core (`previewFrame.videoSources`) | native (shared) + shell | **yes** |
| 7 | Mac capture liveness (AVF trusted, SCK untrusted) | native (Mac adapters) | Mac-only |
| 8 | R3: shared evidence line builders, round-trip test, `platform`, Health row | native + shell | **yes** (evidence json) |
| 9 | S6 core: `buildSourceSheetRenderPlan`, demand-gated pass, `sourceSheet` node, Metal hook | native (shared + Metal) | **yes** |
| 10 | S6 shell: demand lease, thumbnails, inspector preview, tally borders | shell | no |
| 11 | Drill gate demand off/on; outcomes | drill | no |

### Task 1: Polish (shell)

**Files:** `SourceInspector.swift`, `ShellApp.swift` (`MediaPane`), `MediaPlayback.swift`.

**Interfaces:**
```swift
extension MediaRoutePlayback {
    /// nil when the status line would repeat the summary line (D9).
    static func statusLine(status: String, summary: String) -> String?
}
```
Behaviour: `MediaPane` draws `statusLine(...)` instead of `mediaStatus` raw. The inspector header loses its button.

**Tests:** `sources2-polish/media-status-not-repeated` (equal → nil; "Playing X on Program" vs summary
"Playing X on Program" → nil; "Could not pause X…" vs summary → shown; empty → nil).
`sources2-polish/one-inspector-close` renders `SourcesTable` with the inspector open through `ImageRenderer`-free
introspection: count `Button` labels "Close inspector" in the tab snapshot text dump is not available, so the test
renders the tab snapshot harness and asserts on `SourceInspector.headerActions` (a static list the header draws) being
empty. **Mutations:** return `status` unconditionally → red; restore the header button → red.

### Task 2: R1 accept rules and frame age (shell)

**Interfaces:**
```swift
struct SourceRowFacts { var formatDetail: String }   // Windows FormatDetailLabel: "age 2.3s stale" or the cap note
extension SourceRowFacts { var formatWithRate: String }  // "1920×1080@60", "—" parts as Windows ResolutionLabel/FrameRateLabel
mutating func SourceRowFactsProjection.applyRoster(_:)   // ignores epoch "" (Windows :80-82)
```
Behaviour: a roster fact with an empty epoch changes nothing in the facts projection (it may still drive the slot law,
which has its own gate). `formatDetail` is `"age \(age/1000, 1dp)s stale"` when stale, else the cap note (Task 3), else "".
The table's Format cell shows `formatDetail` under the format in amber when stale. The inspector shows `formatWithRate`.

**Tests** (driven through `applySnapshotForTesting` + `handleEventForTesting`; fixture invariants: revision strictly
increasing, epoch `p:m:x` with numeric p, m):
- `sources2-r1/unrevisioned-roster-is-no-fact`: revisioned roster (format 1920×1080 applied), then an epoch-less
  roster with the guest camera-off → row still LIVE 1920×1080 (Windows `ApplyRosterFact` :80).
- `sources2-r1/stale-revision-ignored`, `epoch-change-resets`, `retired-epoch-ignored-after-core-restart`
  (`onConnected` then a fact and a roster from the old epoch → row stays CONNECTING / "—").
- `sources2-r1/stale-format-ages-out`: bus `sinceLastNewFrameMs` 2300 → STALLED, fps "—", `formatDetail` "age 2.3s stale";
  constant 2300 comes from the fixture.
**Mutations:** drop the empty-epoch guard → unrevisioned test red; drop `retiredEpochs` insert in `resetForProcess` →
retired test red; print age in ms → age test red.

### Task 3: R2 cap note and REC chip (shell)

**Interfaces:**
```swift
struct ZoomAppliedCap: Equatable { let tier: Int; let fps: Int     // from zoomSubscriptionChurn
    static func parse(_ snapshot: JSONObject) -> ZoomAppliedCap?
    var label: String  /* "360p"|"720p"|"1080p" */ var dimensions: (Int, Int) }
static func SourceRowFactsProjection.capNote(width:height:fps:cap:) -> String   // Windows ApplyCameraCap, verbatim
mutating func applyCap(_ cap: ZoomAppliedCap?)
```
Behaviour: cap note only on Zoom rows with an observed format; "up to 1080p requested" when width < 1920 or
height < 1080; "local max 30 fps" when cap fps < 60 and observed fps > cap; joined by " · ".
**Tests:** `sources2-r2/cap-note-table` (quantified: tiers 0-2 × observed 640×360/1280×720/1920×1080 × fps cap 30/60 ×
observed 25/60; oracle written in the test from `:150-158`); `sources2-r2/cap-from-core-not-preference` (snapshot
`cameraMaxResolution: 1` → "up to 720p requested" for 640×360 and none for 1280×720);
`sources2-r2/rec-chip-four-states-wired` (four recorded `recording` snapshots through `applySnapshotForTesting` →
row writer OFF / ARMED / REC / ERROR, plus "—" for `status: unreported`).
**Mutations:** `<` → `<=` on width → red at 1920; read cap from a shell constant → cap-from-core red.

### Task 4: S4 shell, media slots

**Interfaces:**
```swift
// ShowInputSlot.kind gains "media"; sourceId = asset id; sourceKey "media:<id>" (Windows SlotSourceId :313-321)
enum SourceType { case still = "Still", mediaClip = "Media clip" }    // category "Media"
SourcePickerCatalog.groups(roster:devices:media:current:)             // Zoom, Camera, Screen, Media (Windows order)
AppModel.slotRouteFields(_ slot: ShowInputSlot) -> JSONObject?        // ONE place every route builder asks
```
Behaviour: the picker lists every bin asset under "Media" (Windows `BuildSourceOptions` :437-443, label
`"<name> - <kind>"` → Mac `"Media · <name>"`). Choosing one goes through `assignSlot` (`operator-assign`). Routes for a
media slot carry `mode: fixed` + `mediaAssetId/Name/Kind/Path/Loop` (the existing layer shape, `AppModel.swift:2969-2980`)
in solo, edited-canvas and preset routes. The multiview entry is `kind: "media", sourceId: "media:<id>", mediaAssetId`
(+ `mediaAssetPath`, `mediaAssetKind`, Task 5). Persisted like capture (`PersistedSlot.kind = "media"`), restored
offline until the bin has the asset; a removed asset marks the slot offline (never substituted). Auto-assign never
touches media. Removing an asset that a slot holds is refused like any in-use asset. Facts per D5. Audio cell
"Media audio" (Windows `ShowInputSlotViewModel.cs:383`).
**Tests:** `sources2-s4/multiview-payload-media` (kind media, sourceId `media:a1`, mediaAssetId, never capture fields);
`sources2-s4/routes-carry-media-identity-every-builder` (preset, edited layer bound to the slot, solo cue: all three
carry `mediaAssetPath`); `sources2-s4/picker-media-group-order`; `sources2-s4/persist-restore-media`
(restored offline until the bin lists it; asset gone → offline, not substituted); `sources2-s4/media-never-auto-assigned`
(roster sync with free slots leaves media placement alone and never fills from the bin);
`sources2-s4/media-row-facts` (bus `media:a1` 1920×1080 with `sinceLastNewFrameMs` 9000 → LIVE, never STALLED).
**Mutations:** route builder falls through to `capture-input` for media → routes test red; `sourceKey` nil for media →
persist test red; staleness applied to media → facts test red.

### Task 5: S4 core, wall stills (shared native)

**Interfaces:** `MultiviewSource` gains `mediaAssetPath`, `mediaAssetKind` (additive wire fields on
`set-multiview-layout`). `syncStillMediaDesired()` adds every multiview media source whose path is a still.
`applyMultiviewLayout` calls `syncStillMediaDesired()` when the media set changed.
**Tests (gtest):** `MultiviewStillMedia.WallOnlyStillIsDesired` (a still named only by the multiview reaches the cache's
desired set; a clip does not; an entry without a path does not). Pixel: `MetalCompositor.*` readback of a multiview plan
with a `media-video` tile fed by a BGRA still frame shows the still's colour at the tile centre.
**Mutations:** skip the multiview loop → red; include clips → red.

### Task 6: PVW tally from the core (shared native + shell)

**Interfaces:** `MediaCore::renderedPreviewSources_` (a second `RenderedProgramSources`), published wherever the
Preview plan is built for a render (non-isolated monitor section and `submitIsolatedMonitorTick`), invalidated when no
Preview scene is active. Snapshot: `previewFrame: {sceneId, videoSources}`. Shell: `applySourceFacts` reads
`previewFrame.videoSources` for PVW; `previewSourceKeys()` is deleted.
**Tests:** gtest `RenderedPreviewSources.PublishedFromTheBuiltPreviewPlanOnly`; shell
`sources2-tally/pvw-from-core-not-routes` (Preview routes name zoom:42 but `previewFrame.videoSources` is empty → tally
none; core names zoom:42 → pvw). **Mutations:** shell falls back to routes → red.

### Task 7: Mac capture liveness (native, Mac adapters)

**Interfaces:**
```cpp
// modules/MacCaptureLiveness.h (new, pure)
struct MacCaptureLiveness {
  explicit MacCaptureLiveness(bool cadenceTrusted);
  void sample(int64_t nowNs);                         // a real delivery
  void lost(CaptureLossReason reason, int64_t nowNs); // explicit loss; idempotent per reason
  CaptureLiveness value() const;
};
```
Behaviour: AVF `AvfSessionState::publish` calls `sample`, the stall/no-longer-present/error paths call `lost`
(`DeviceRemoved` for "no longer present", `StreamError` for a runtime error), and every emitted frame carries
`value()`. SCK the same with `cadenceTrusted=false` and `lost(StreamError)` on `didStopWithError`.
**Tests:** `MacCaptureLiveness.*` drives `decideCaptureOnAir` with the produced value: trusted + 1.2 s silence → Stalled;
+5 s → Failed; untrusted + 60 s silence → Live; lost → LostHolding then Failed after 2 s; a sample after loss → Live.
`AvfSessionState` test: publish stamps Live with a non-zero `lastSampleNs`; a re-sampled held frame keeps the original
`lastSampleNs`. **Mutations:** stamp `lastSampleNs` at emit time instead of delivery → held-frame test red; SCK trusted
→ untrusted-silence test red.

### Task 8: R3 subscription evidence (native + shell)

**Interfaces:** `native/zoom-engine/shared/SubscriptionEvidenceLines.h` (new, header-only, no SDK types): five
builders `createRendererFailedLine`, `setResolutionLine`, `videoSubscribeLine`, `resolutionDowngradedLine`,
`subscribeFailedAllLine`, each returning the exact JSON line `main-macos.mm` writes today. `main-macos.mm` calls them.
`ZoomSubscriptionSdkEvidence::json()` gains `"platform": "macos" | "windows" | "linux"`. Shell: a Health row
"Zoom subscription evidence" listing the last five rows (`stage`, participant, code, resolution / requested→actual).
**Tests:** gtest `SubscriptionEvidenceLines.EveryMacLineIsRetained` (each builder → `parseZoomEngineEvent` →
`ZoomSubscriptionSdkEvidence::observe` → one retained row with the fixture's code and resolution) and
`…PlatformIsReported`. Shell `sources2-r3/health-evidence-rows`. **Mutations:** rename one stage in the header → its
case red; drop `"code"` key → sdkCode assertion red.

### Task 9: S6 core, the source sheet (shared native + Metal hook)

**Interfaces:**
```cpp
// core/SourceSheetPlan.h (new, pure)
struct SourceSheetInput { int slot; ResolvedMultiviewFeed feed; };
inline constexpr int kSourceSheetColumns = 5, kSourceSheetRows = 2, kSourceSheetCellWidth = 640, kSourceSheetCellHeight = 360;
modules::CompositorRenderPlan buildSourceSheetRenderPlan(const std::vector<SourceSheetInput>&);
struct SourceSheetCell { int slot; std::string sourceId; std::string state; };  // live | held | empty
std::vector<SourceSheetCell> sourceSheetCells(const std::vector<SourceSheetInput>&,
    const std::vector<modules::VideoFrame>&, const std::function<std::string(const std::string& key)>& health);
// modules/Interfaces.h
virtual ProgramFrameSharedTexture renderSourceSheet(const CompositorRenderPlan&, const std::vector<VideoFrame>&) { return {}; }
```
Behaviour: cell N is slot N+1 for every layout mode and tile count; a slot outside 0-9 or unassigned has no layer; no
border; fit; per-source grade applied, no program look, no chroma key; dropout held (monitor rule). MediaCore renders it
in the non-isolated monitor branch only while `sourcePreviewDemand_.active()` and at least one multiview source exists,
every 4th tick at phase 2 (`counter % (4·divisor) == 2`), and adds its CPU time to `monitorCycleCostNs`. Demand off →
no pass, node `active:false`. Snapshot node `sourceSheet: {active, texture{iosurfaceId,width,height,frameNumber},
columns, rows, cellWidth, cellHeight, cells[]}`. Metal: `renderSourceSheet` → `renderSecondaryPass(sourceSheet_, …,
/*wait*/ false)`.
**Tests:** `SourceSheetPlan.CellIsSlotForEveryLayout` (quantified over slots 0-9 in any list order and lists of 1-10),
`…NoBorderNoUnassigned`, `…CellsState` (frame present → live, stalled → held, none → empty);
`MediaCoreSourceSheet.DemandGatesThePass` (stub compositor counts calls: demand off → 0 over 40 ticks; on → 10 ± 0 over
40 ticks; node active matches); `MetalCompositor.SourceSheetTwoCells` (two solid sources, readback: each cell centre is
its own colour, the others clear). **Mutations:** cell from list index instead of slot → red; pass with demand off → red;
wait flag ignored is not observable in a unit test (stated; the drill covers it).

### Task 10: S6 shell

**Interfaces:**
```swift
struct SourceSheetState: Equatable { let surfaceId: UInt32; let columns, rows: Int; let cells: [Int: SheetCell]
    static func parse(_ snapshot: JSONObject) -> SourceSheetState? }
enum SourceThumbnailPolicy {
    /// The cell rect for the row, or the reason it shows a placeholder.
    static func resolve(slot: ShowInputSlot, facts: SourceRowFacts, sheet: SourceSheetState?) -> Result }
final class SourcePreviewDemandLease  // sends true every 1 s while the Sources tab is visible, false on leave, re-asserts per core generation
```
Behaviour: a row shows its cell only when `cells[slot-1].sourceId == slot.sourceKey`, state live/held, slot in show
(Windows `PatchPreview` :262-289). Placeholders: "Not in the multiview", "Video off", "No signal", "Waiting for video",
"Preview unavailable" (no sheet). The thumbnail is a `CALayer` with the shared IOSurface and `contentsRect` = the cell;
one 15 Hz timer re-marks every thumbnail layer. Border colours per Windows `MultiviewInputRow.cs:45-51` mapped to
Studio tokens (pgm/hold amber, pvw green, talking white). The inspector header shows the same cell at 320×180.
**Tests:** `sources2-s6/thumbnail-identity` (quantified: sheet cell names A/B/none × slot source A/B × state
live/held/empty × in show yes/no; oracle from `PatchPreview`); `sources2-s6/demand-lease-follows-tab` (tab enter →
true now and every second; leave → false once; `onConnected` while visible → true re-sent);
`sources2-s6/reassign-blanks-before-new-identity`; `sources2-s6/border-colours`.
Snapshot: Sources tab at 1360 and 1920 with a seeded sheet.
**Mutations:** drop the sourceId equality → identity test red; send demand once (no renew) → lease test red.

### Task 11: Drill gate and outcomes

`scripts/mac-show-drill.py` gains `--source-preview-demand` (renews the lease each poll). Run
`--seconds 40 --load 8` with `COREVIDEO_FAKE_ENGINE_FPS=60`, demand off and on, twice each, judged on per-frame
delivery, underruns, deadline misses, shed level and over-budget ratio. Five other builds share this Mac: a delta is
believed only if it repeats. Outcomes: `2026-11-21-mac-sources-phase2-outcomes.md`.

## Out of scope

Lower-third text per source (Phase 1 Task 8, superseded by #1213), Role exposure, the Zoom quality pickers (Z1), a
Metal monitor worker, clip playback in a wall-only tile, Windows changes beyond the additive core fields.
