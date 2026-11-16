# Sources screen rebuild on macOS: table, inspector, signal facts, thumbnails and the slot law

Status: DRAFT design for owner review, 2026-11-15.

Covers consolidated gap rows **2.4** (Sources rebuild) and **2.6** (per-source live thumbnails and
tally) of `docs/superpowers/audits/2026-11-15-mac-ui-parity-consolidated.md`, owner priority #2,
first half. It also finishes the parts of Tier **0.8** (slot persistence) that branch
`mac-tier0b` left open. Row-level evidence: `docs/superpowers/audits/2026-11-15-mac-ui-parity-sources.md`
rows 1-30, 32, 34-51, 58-60, 75-82. Roadmap: `docs/superpowers/plans/2026-11-14-mac-parity-1.0-roadmap.md`
has no milestone for this work; it enters as a new tracking issue (roadmap §6, backlog first).

Paths: Windows paths are relative to `native-shell/CoreVideoPro.WinUI/`, Mac paths to
`mac-shell/Sources/CoreVideoProShell/`, core paths to `native/src/`. All Mac line numbers are
from branch `mac-tier0b` at `32231cbc`. They differ from the audit, which read `mac-zoom-signin`.

## 1. Why

On Windows the Sources tab is a ten-row table. Each row shows a live picture of the source, its
type, the resolution and frame rate actually being delivered, a signal chip (LIVE, TALKING,
STALLED, PGM), whether its ISO writer is really recording, and an optional inspector for the
name, lower-third text, role and dropout behaviour.

On a Mac the operator gets a list of plain rows with no picture and no health. They cannot:

- see which camera or guest is in which slot without going back to the Studio multiview;
- see that a guest is stuck at 360p, or that a feed stopped advancing, from the Sources tab;
- tell an armed ISO from one that is writing;
- tell a screen capture from a camera (every non-Zoom source is labelled "Camera");
- put a still from the media bin into a slot;
- stop auto-assign from moving guests back after replacing one by hand (section 4.2, a law violation);
- rely on a slot change surviving a crash in the following five seconds.

The owner noticed first ("Sources screen is way different than the windows version"). Any
operator trained on the Windows build or its documentation notices next.

## 2. Windows behaviour to match

### 2.1 The table (issue #590, contract `docs/reference/sources-table-workspace-spec.md`)

| Behaviour | Where | Test |
|---|---|---|
| Ten stable numbered inputs. Columns `#, Source, Type, Format, FPS, Signal, Audio, MV, ISO, Actions`. | `Controls/SourcesWorkspaceControl.cs:86` | `SourcesWorkspaceTests.cs:12` |
| Selection, search, category filter, column visibility and opening the inspector never assign a source, change MV or ISO, or Take. | spec "Interaction"; `ViewModels/SourcesWorkspaceState.cs` | `SourcesWorkspaceTests.InspectionFiltersAndColumnsNeverChangeAssignmentsOrLiveSelections` (`:12`) |
| Responsive columns: Type and Audio hide under 1050 px; Signal and the thumbnail under 750 px. No horizontal scroller. | `Controls/SourcesTableRowControl.cs:113-120` | offscreen consumer named in the spec |
| Inspector sits beside the table at 1500 px or wider (340 px column), else stacks below. | `Controls/SourcesWorkspaceControl.cs:156-161` | same |
| Summary: "x/10 assigned · n in MV · n ISO selected · n match filter". | `SourcesWorkspaceControl.cs:150` | — |
| Column preferences are UI-only and saved apart from production settings. | `SourcesWorkspaceControl.cs:54-68` | `SourcesWorkspaceTests.ColumnPreferencesRoundTrip…` (`:70`) |
| Picker groups Zoom / Camera / Screen / Browser / Media / SRT / NDI. An assigned source that is gone stays as "Saved source unavailable — …". It is never substituted. | `Services/ShowInputRosterService.cs:174-290` | `ShowInputUnifiedSourceTests`, `SourcesWorkspaceTests.OfflineNameAndSelectionStayBoundToTheirOwnSource` (`:47`) |
| Capture kind comes from the device id and vendor: `screen:` is Screen, `browser:` is Browser, vendor `srt`/`rtmp`/`ndi`, else a camera. | `ShowInputRosterService.cs:96-117` | `ShowInputRosterServiceTests` |

### 2.2 Observed facts per row (never requests, never inferred)

| Behaviour | Where | Test |
|---|---|---|
| Format and FPS are what is delivered. A stale rate never shows as live FPS. A request ceiling is never shown as a measurement. | `ViewModels/ShowInputs/MultiviewInputRow.cs:36-41` | `SourcesWorkspaceTests.MeasuredCellsSeparateFormatAndFps…` (`:60`) |
| Zoom row: IDLE (slot out of show or engine off), NO INCOMING (not in a meeting, or guest absent), VIDEO OFF, CONNECTING (no delivered frame yet), LIVE, TALKING, STALLED when the last frame is older than 1500 ms. | `ViewModels/ShowInputs/MultiviewInputRowsProjection.cs:362-486` | `ShowInputsCoordinatorTests.DeliveredFormatAndAgePatch…` (`:314`), `VersionedBridgeVideoOff…` (`:203`) |
| Capture row: LIVE only when connected with signal, format from the device's observed size and rate; else CONNECTING or NO INCOMING. | same file `:367-384` | `CaptureSignalFactPatchesHostUvcFormatWithoutRowClick` (`:704`) |
| The chip reads PGM when the row is LIVE and its tally is program. Thumbnail border: amber for program or hold, green for preview, white for talking. | `MultiviewInputRow.cs:45-65` | `PreviewTallyHonorsProgramPreviewTalkingAndStall` (`:664`), `ProgramTallyChangesStatusChip…` (`:694`) |
| The ISO chip is the observed writer lifecycle: preparing is ARMED, producing is REC, a warning or interrupted writer is ERROR, completed is OFF. It is separate from the ISO checkbox. | `MultiviewInputRowsProjection.cs:306-313` | `IsoChipUsesObservedWriterLifecycle` (`:413`) |
| A reused Zoom id clears the old format and rejects the old source's facts. | `MultiviewInputRowsProjection.cs:77-124` | `ReusedZoomIdClearsOldFormatAndRejectsOldSourceFactWithoutTake` (`:133`) |

### 2.3 Thumbnails

| Behaviour | Where | Test |
|---|---|---|
| A row shows a picture only for a slot that is assigned AND in the show AND has a live source. | `MultiviewInputRowsProjection.cs:264-296` | — |
| The picture is an identity-bound per-source export, never a crop of the multiview wall. A wall reflow cannot supply or move it. Reassigning the slot clears it before another identity can show. | spec "Preservation map"; `MultiviewInputRowsProjection.cs:291-296` | `ShowInputsCoordinatorTests.CompositeLayoutCannotSupplyOrMoveAnIdentityBoundThumbnail` (`:435`) |
| The core only exports while the Sources tab is visible (`set-source-preview-demand`), bounded to the ten assigned multiview rows. | `Views/SourcesInputsPage.xaml.cs:18-40`; `core/MediaCoreMonitors.cpp:26-39`; `core/MediaCore.cpp:1221-1226` | — |
| The export runs on the D3D11 monitor worker, off Program's render thread. | `modules/D3D11CompositorAdapter.cpp:281,897` | — |

### 2.4 The slot law (CLAUDE.md "Live-meeting QA day", "Sources kept reverting")

"Sources appear in slots by OPERATOR action or newcomer auto-assign ONLY."

| Rule | Where | Test |
|---|---|---|
| Auto-assign places only participant ids it has never seen in this meeting. A leaver is forgotten, so a real rejoin is a newcomer again. | `ViewModels/ShowInputs/ShowInputsCoordinator.cs:366-399`; `ShowInputRosterService.cs:354-416` | `ShowInputsCoordinatorTests.ARosterSyncNeverRefillsASlotTheOperatorUnassigned` (`:839`) |
| A slot the operator emptied is reserved against auto-assign until the meeting roster empties (owner ruling 2026-09-12). | `ShowInputsCoordinator.cs:377-399,505-521` | `AnEmptyRosterReleasesTheOperatorsClearedSlots` (`:875`) |
| The auto-assign toggle lives on the Automation page. Flipping it means "assign everyone now": it clears both memories and fills from the whole room, camera on or off. | `Views/AutomationPage.xaml:10`; `ShowInputsCoordinator.cs:434-453` | `ReapplyAutoAssign_FillsAFreeSlot…` (`:785`), `ReapplyAutoAssign_Disabled…` (`:800`) |
| A Zoom slot whose participant has left the meeting is freed. | `ShowInputRosterService.cs:365-376` | `ShowInputRosterServiceTests` |
| Never invent a source: an in-show but unassigned slot just leaves the show. | CLAUDE.md, same section | `ShowInputAssignmentLawTests.TheDualCaptureSlotStufferStaysDead` (`:68`) |
| Every slot write is logged `slot-write: slotN field old->new by=<reason>`. An untracked write is a bug. | `ShowInputWriteScope` | `ShowInputAssignmentLawTests.WriteScope_*` (`:21,:43`) |
| The roster saves synchronously on every change, and a second load is refused. | `ShowInputsCoordinator.cs:144-185,215-224` | `OperatorAssignment_PersistsImmediately…` (`:1017`), `SecondLoad_IsRefused…` (`:1074`) |
| ISO selection is canonical source ids, re-projected onto rows in place, and survives a roster refresh. | `ShowInputsCoordinator.cs:141,276-304` | `IsoSelection_SurvivesARosterRefresh_ViaInPlaceReProjection` (`:955`) |

Only sources are subscribed (CLAUDE.md "ONLY SOURCES ARE SUBSCRIBED",
`CoreVideoPro.MediaCore/Services/ZoomSourceSetPolicy.cs`). An assigned slot that is out of the
show and on no bus has no frames. Its row has no picture and no format. That is expected.

### 2.5 The inspector

| Behaviour | Where | Test |
|---|---|---|
| Three tabs: Source, Picture/Grade, Audio. | `Controls/SourcesWorkspaceControl.Inspector.cs:35-44` | — |
| Display name is stored per source id. Blank, or equal to the derived name, resets it. It feeds lower thirds and multiview labels. | `ViewModels/ShowInputSlotViewModel.cs:421-437` | `ShowInputSlotViewModelTests` |
| Lower-third text per source: name, secondary line, mode (source default, custom, hidden). | `Views/SourceLowerThirdFlyout.cs:12-40` | `SourceLowerThirdTextTests` |
| Role (Zoom guests): meeting-scoped, one holder per role, enabled only while the guest is present. | `Views/SourcePolicyFlyout.cs:23-25,43-48`; `ViewModels/StudioViewModel.cs:5020-5058` | — |
| Dropout policy for Zoom and capture sources, saved. Capture shows a status line and is disabled when the policy does not apply. | `SourcePolicyFlyout.cs:26,50-56`; `ShowInputSlotViewModel.cs:392-415`; `core/MediaCore.cpp:1906-1923` | — |
| An open editor is bound to its source id. If the assignment changes underneath it, it disables itself and says so. | `SourcePolicyFlyout.cs:34-42` | — |
| Assignment hint copy for the four states. | `ShowInputSlotViewModel.cs:360-363` | — |
| Paired microphone on the Audio tab. Gain and DSP stay on the Audio page. | `SourcesWorkspaceControl.Inspector.cs:152-172` | — |

## 3. Mac today

### 3.1 What branch `mac-tier0b` already fixed (verified in code)

- **Page width (Tier 0.10).** `TabPageLayout.maxWidth` returns `.infinity` for Sources
  (`TabPageLayout.swift:16-25`). The 720 pt cap in audit row 2 is gone. The rows are still
  fixed-width controls (`ShellApp.swift:1713-1846`), so the layout is still not a table.
- **Slot persistence (Tier 0.8, audit row 75).** `PersistedSlot` (`ShellPrefs.swift:65-71`) is
  written in `currentPrefs()` (`AppModel.swift:625-628`) and restored by
  `restoreSlotAssignments` (`ShowInputPersistence.swift:33-51`). A restored source reads offline
  until it appears. A duplicate source is dropped. Restored capture devices reconnect
  (`ShowInputPersistence.swift:63-72`).
- **Per-source settings follow the source.** Chroma key, microphone pairing and dropout policy
  carry a `sourceKey` and reattach through `restoredSlotIndex` (`AppModel.swift:659-679`,
  `ShowInputPersistence.swift:55-60`). The slot number is only a legacy fallback.
- **ISO selection (audit row 76).** `recomputeFromSlots` projects the selection instead of
  rebuilding it (`AppModel.swift:1804-1814`, `projectIsoSelection` at `:3446-3461`). The take's
  ISO set survives passive source loss (`RecordingIsoSelection.swift`, a port of the Windows type).
- Tests exist for the above in `ShellTestsTier0.swift:235-365`.

### 3.2 What Tier 0.8 left open

1. **Auto-assign still breaks the law.** The fill loop places every camera-on participant that
   is not slotted and not tombstoned (`AppModel.swift:1756-1767`). Only an explicit Unassign or
   an In-show untick writes a tombstone (`:1827-1848`). When the operator replaces guest A with
   guest B through the row picker (`assignSlot`, `:1816-1825`), A has no tombstone, so the next
   roster snapshot puts A into the first empty slot. This is the "sources kept reverting" defect.
2. **An emptied slot is not reserved.** `firstEmptySlotIndex` (`:1880-1882`) returns the slot the
   operator just cleared, so the next joiner lands in it (the Windows 2026-09-12 report).
3. **There is no auto-assign toggle.** Nothing in `AutomationExtras.swift` or the model.
4. **Saves are delayed.** Slot changes ride the 5 s autosave. `persistPrefsNow` is called only
   from the virtual-camera paths (`AppModel.swift:1021,1062,1084-1090`). A crash inside that
   window restores the previous roster.
5. **Slot writes are not logged**, apart from one "auto-assigned into slots" line (`:1770`).
6. **Zoom slots outlive their meeting.** A departed guest's slot is marked offline and kept
   (`refreshSlotHealth`, `:1864-1878`), and a restored Zoom slot is keyed by the Zoom user id
   alone. Zoom reuses user ids across meetings (CLAUDE.md, Tiles audio latch). A different
   person can inherit a restored slot's name, key and dropout policy. Windows frees the slot.
7. **The display name belongs to the slot.** `slot.name` is set only when empty (`:1820`), so a
   slot reassigned to another source keeps the previous source's name.

### 3.3 What is still missing or wrong

- **Table.** No headers, no Type, Format, FPS, Signal, Audio or writer columns, no selection, no
  search, filter or column menu, no status line (`ShellApp.swift:1504-1640`). The MV checkbox
  has no label (`:1731-1736`).
- **Signal facts.** `CaptureDeviceRow` has no resolution or rate (`AppModel.swift:120-128`). The
  parser reads `kind`, which the core never sends for a capture device, so it is always
  `"video"` (`AppModel.swift:1610-1619` against `core/MediaCore.cpp:296-305`). The shell reads
  the snapshot's `sources[]` only to count stalled feeds (`StudioHealth.swift:76`). It drops the
  `zoom-source-format` event (`handleEvent`, `AppModel.swift:1898-1917`).
- **Types.** The picker has two sections (`ShellApp.swift:1738-1756`). Screens are labelled
  "Camera ·" (`:1722`). Slot kinds are `unassigned | zoom | capture` (`AppModel.swift:149`), so a
  media still cannot be a slot source.
- **Inspector.** None. Name, microphone, Key and dropout are inline in the row. There is no
  Role (the spine sends `"role": "guest"` for everyone, `AppModel.swift:2301`), no per-source
  lower-third text, and no capture dropout.
- **A stale rule.** The comment at `AppModel.swift:173-176` says the core refuses a dropout
  policy on anything but `zoom:<pid>`. The core accepts `capture:<id>` too
  (`core/MediaCore.cpp:1907-1922`) and publishes `dropoutPolicyApplies` for capture sources
  (`core/SourceVideoIngress.cpp:128`).
- **Thumbnails and tally.** No surface in the row.
- **Capture list.** Capped at eight devices with no way to see the rest (`ShellApp.swift:1572,1607-1610`).

### 3.4 What the shared core gives macOS today

Shared and working on the Mac build:

- `sources[]` in every snapshot: `sourceId`, `kind`, `width`, `height`, `framesIngested`,
  `health`, `sinceLastNewFrameMs`, `dropoutPolicy`, `displayName`, and for capture sources
  `liveness`, `onAir`, `lastSampleAgeMs`, `lossReason`, `dropoutPolicyApplies`
  (`core/SourceVideoIngress.cpp:91-131`). UNVERIFIED: whether the AVFoundation and
  ScreenCaptureKit adapters fill the capture liveness fields, or report `"unreported"`.
- Zoom subscriptions: `deliveredWidth`, `deliveredHeight`, `deliveredFps`, `lastFrameAgeMs`,
  `lastFrameAtMs`, `sourceGeneration`, plus `rosterEpoch` and `rosterRevision`
  (`modules/ZoomEngineRuntime.cpp:1463-1512`). The `zoom-source-format` event carries the same
  at up to 5 per second (`:1945-1966`).
- Capture devices: `resolution{width,height}`, `frameRate`, `connectionState`, `signalPresent`,
  `droppedFrames`, `audioSyncOffsetMs` (`core/MediaCore.cpp:283-305`).
- Screen and window ids `screen:<n>` and `window:<id>` (`modules/SckScreenCaptureAdapter.mm:269,287`).
- `set-source-policy` with `dropoutPolicy` and `displayName`, present-or-keep (`core/MediaCore.cpp:1891-1928`).
- A multiview source of kind `media` (`core/MultiviewFeed.h:14-40`), a per-source `slot` field
  (`core/MediaCore.cpp:3656`), and stills through `StillMediaFrameCache`.
- ISO stream nodes under `recording` (`core/CoreStateCapture.cpp:2001-2022`). UNVERIFIED: that
  the AVFoundation sink fills per-writer lifecycle fields. If it does not, the nodes read
  `status: "unreported"` (`:2028-2040`).
- `programFrame.videoSources`, the sources the core attributes to the rendered Program frame
  (already read by `ProgramLowerThird.swift:26-40`).
- The per-route `chromaKey` node (`AppModel.swift:2523`), drawn by the Metal compositor.

Windows-only today:

- **Per-source GPU exports.** `set-source-preview-demand` is accepted on every platform
  (`core/MediaCore.cpp:1221-1226,1290`), but it only produces pictures through
  `ICompositor::submitMonitors`, which only the D3D11 adapter implements. Metal inherits
  `hasIsolatedMonitors() == false` (`modules/Interfaces.h:1404-1405`).
- **What the Metal compositor publishes** (`compositor/MetalCompositorAdapter.mm`): the Program
  IOSurface (`:308`), a multiview IOSurface (`renderMultiview`, `:381-384`), a Preview IOSurface
  (`renderPreview`, `:388-391`), virtual-camera frames, and the full-resolution Program BGRA for
  recording (`:345`). Nothing per source.
- **Media clip decode** (gap 2.1), browser sources, SRT, RTMP and NDI ingest (2.10, 2.11).

## 4. Design

### 4.1 Components

Four new Swift files, each pure where it can be, so they are testable without a core:

- `ShowInputAssignmentLaw.swift` (new). The slot-write rules of section 4.2 as a value type.
- `SourceRowFacts.swift` (new). A pure projection from snapshot nodes to one row's observed
  cells. It is the Swift twin of `MultiviewInputRowsProjection`.
- `SourcesTable.swift` (new). Replaces `SourcesPane` and `SlotRow`: header, ten rows, tools,
  summary, catalog.
- `SourceInspector.swift` (new). The inspector panel. `ChromaKeyControl` moves into it.

One new core surface, the **source sheet** (section 4.5), in shared `MediaCore` plus a small
Metal addition.

### 4.2 Slot assignment law on Mac

`ShowInputAssignmentLaw` owns the memory Windows keeps in `ShowInputsCoordinator`:

- `seenThisMeeting: Set<String>`, replaced by the roster on every accepted roster snapshot.
- `operatorClearedSlots: Set<Int>`, added to by Unassign and cleared when an accepted roster is empty.
- `autoAssignEnabled: Bool`, persisted, default on (new `ShellPrefs` key `autoAssignInputs`,
  optional so older files load).

Rules, each quoting section 2.4:

1. On an accepted roster (the existing `rosterProjection.observe` gate, `AppModel.swift:1740`),
   newcomers are roster ids not in `seenThisMeeting`. Only newcomers are placed, and only into
   unassigned slots outside `operatorClearedSlots`. The `manuallyUnassigned` tombstones are deleted.
2. Flipping the toggle on clears both memories and fills from the whole roster.
3. A Zoom slot whose participant is absent from an accepted in-meeting roster is freed. A
   roster rejected by `rosterProjection` (core restart, reconciling) frees nothing. This
   replaces the offline-forever state for Zoom slots. Capture and media slots keep the Windows
   "saved source unavailable" behaviour.
4. Zoom slots are not restored across launches. `restoreSlotAssignments` keeps capture and
   media records and drops Zoom ones. See owner decision 2.
5. Every write to `slots` goes through one function that takes a reason
   (`operator-assign`, `operator-unassign`, `roster-sync`, `auto-assign-toggle`, `restore`,
   `drag-swap`, `capture-offline`) and logs `slot-write: slot<N> <field> <old>-><new> by=<reason>`
   through `ShellLog`. A debug build asserts when `slots` changes outside it.
6. Every slot write calls `persistPrefsNow()`. The existing `lastSavedPrefs` equality check
   (`AppModel.swift:1087`) keeps that cheap.
7. Camera-off newcomers. Windows passes the whole roster to auto-assign. The Mac fills only
   camera-on guests (`AppModel.swift:1757`). See owner decision 3.

The toggle is drawn in the Sources tool row and on the Automation tab, bound to one property.

### 4.3 Source identity, kinds and names

- `ShowInputSlot.kind` gains `media`. `sourceKey` (`ShowInputPersistence.swift:17-24`) returns
  `media:<assetId>` for it. `PersistedSlot` already stores `kind` as a string, so the file
  format does not change.
- Type is derived, never stored, by a pure function that mirrors `InferCaptureDeviceKind`:
  Zoom guest, Camera, Capture card (vendor `blackmagic` or `aja`), Screen (`screen:`), Window
  (`window:`), Still, Media clip. The picker groups in the Windows order: Zoom, Camera, Screen,
  Media. Browser, SRT, RTMP and NDI groups are omitted until their ingest exists on Mac. An
  empty group is not drawn.
- Media in a slot. Stills are offered now and feed the multiview as `kind: "media"` with
  `mediaAssetId`. Clips are listed disabled with "Clip playback is not available on Mac yet"
  until the media playback spec (gap 2.1) lands. That gate is one predicate.
- Display name moves from the slot to the source: a new `sourceDisplayNames: [String: String]`
  keyed by `sourceKey`, persisted (new optional `ShellPrefs` key) for capture and media, and
  meeting-scoped for Zoom. Blank or equal-to-derived removes the entry. The resolved name feeds
  the multiview label, the ISO display name, and `set-source-policy.displayName` (the name the
  core draws on a failed-source slate). `PersistedSlot.name` is read once as a migration.
- The picker is built when it opens and captures the row's `sourceKey`. A choice made after
  that key changed is refused with a status-line message (audit row 12).

### 4.4 Observed facts per row

`SourceRowFacts.resolve(...)` is pure. Its inputs are the slot, its `sources[]` row, its Zoom
subscription row, its capture device row, the roster entry, `programFrame.videoSources`, the
Preview scene's routed source ids, and the `recording` streams. Its output:

| Cell | Rule | Source of truth |
|---|---|---|
| Format | `W×H` | Zoom: `deliveredWidth/Height`, updated by `zoom-source-format` between snapshots. Capture: `resolution`. Media: `sources[].width/height`. |
| FPS | "—" when stale | Zoom: `deliveredFps`. Capture: `frameRate`. |
| Signal | The exact Windows state table of section 2.2. STALLED at a frame age above 1500 ms. | Zoom: `lastFrameAgeMs`. Others: `sources[].sinceLastNewFrameMs` and capture `connectionState` and `signalPresent`. |
| Tally | PGM when the source is in `programFrame.videoSources`. PVW when the Preview scene routes it. TALKING from the roster. HOLD when stalled. | Core truth for PGM. Shell intent for PVW, because the core's multiview tiles never report `pvw` for a source tile (`core/MediaCore.cpp:4103-4105,4170`). |
| Writer chip | OFF, ARMED, REC, ERROR by the Windows mapping | the `recording` ISO stream node for this `sourceId` |
| Audio | "Zoom source audio", the paired microphone, "Still (no audio)", or "No paired microphone" | slot |

Identity fencing, ported from Windows: a Zoom fact is applied only when its `rosterEpoch`
matches the current roster and its `sourceGeneration` matches the participant's. A changed
epoch clears every Zoom row's format.

`handleEvent` gains a `zoom-source-format` case. The event arrives up to 5 times per second per
guest, so it patches one row's stored facts and never triggers a spine or scene sync. Facts live
in their own `@Published` dictionary keyed by slot, so a fact update redraws one row and does
not invalidate `slots`.

### 4.5 Thumbnails: the mechanism, and why

Three candidates were checked against what the Metal compositor publishes today.

**A. The core's thumbnail events.** `zoom-video-frame` carries a 640x360 BGRA frame as base64
in a JSON line, at 2 per second per guest, gated by `set-zoom-thumbnail-demand`
(`modules/ZoomEngineRuntime.cpp:1666,1710,1785-1795`). Rejected: Zoom only (cameras, screens and
stills get nothing), 2 fps, and each frame is about 1.2 MB of base64 through the stdout pipe
that also carries commands and snapshots. It is a CPU path, against the north star.

**B. Per-source exports, as on Windows.** Rejected for Metal today. On D3D11 the exports run on
an isolated monitor worker. Metal has none: every secondary pass runs on Program's render thread
and blocks on `waitUntilCompleted` (`MetalCompositorAdapter.mm:1019-1043`). Ten per-source
targets mean ten more blocking passes per cycle and ten more global IOSurfaces. Porting the
monitor worker to Metal is a separate, larger piece of work.

**C. Cropping the multiview IOSurface.** The shell already shows that surface
(`ShellApp.swift:1300-1337`) and already receives each tile's rectangle and `sourceId`
(`AppModel.swift:1921-1936`). It costs no native work and no GPU time. Rejected as the design,
kept as the fallback, for three reasons:

- Windows forbids it and pins that with a test (section 2.3). The wall's geometry changes with
  the layout mode and the tile count (`core/MediaCore.cpp:3915-3918`), so a 4-tile wall leaves
  slots 5 to 10 with no picture.
- Tile rectangles arrive as JSON, apart from the pixels. During a reflow a row can show another
  guest's picture for a few frames. On a screen whose job is identifying sources, that is the
  wrong failure.
- The tile is drawn with a baked border and the multiview's own grade, and is too small for
  the inspector's large preview on a Retina display.

**Chosen: a source sheet.** One more IOSurface, drawn by the existing generic Metal pass, with
fixed geometry: 5 columns by 2 rows, cell N is slot N+1, always. The plan is built in shared
`MediaCore` code (a new pure `buildSourceSheetRenderPlan`, the shape of
`buildMultiviewRenderPlan`), so it is unit-testable on every platform, and the Metal work is one
more `PassTarget` routed through `renderSecondaryPass`. It keeps the house rule of one stable
surface per purpose and matches the Windows identity rule by construction: a cell never moves,
and the core that draws the pixels also names the source in each cell.

Wire:

- Command: the existing `set-source-preview-demand` `{enabled: bool}`. The shell sends
  `true` while the Sources tab is visible and `false` otherwise, and re-sends it on every core
  generation (CLAUDE.md, "a one-shot command must be re-applied on every core generation").
- The multiview sources the shell already sends gain the existing `slot` field (0-based).
- New `ICompositor::renderSourceSheet(plan, frames)`, defaulting to an empty texture. Called
  from the non-isolated branch of the monitor section (`core/MediaCore.cpp:6247`) only while
  demand is active and at least one multiview source exists.
- New event `source-sheet-shared-texture` and a snapshot node `sourceSheet` (published
  unconditionally, `active:false` when off):

  ```
  { "texture": { "iosurfaceId", "width", "height", "format", "frameNumber" },
    "columns": 5, "rows": 2, "cellWidth": 640, "cellHeight": 360,
    "cells": [ { "slot": 0, "sourceId": "zoom:101", "state": "live" | "held" | "empty" } ] }
  ```

- Cell content: the source fitted in 16:9, no border, no label, with the per-source grade and
  without the per-slot chroma key (the row shows the camera, not the keyed result). A stalled
  source holds its last frame, as the multiview does.

Cadence and load:

- Every 4th display tick (15 fps at 60), on its own phase so it never stacks on the multiview
  or Preview pass.
- Its measured cost is fed into `MonitorShedPolicy` as part of the monitor cycle. Under
  overload it sheds before Program does.
- With demand off, the cost is exactly zero: no target, no pass.

Shell: one `IOSurface` lookup. Each row's thumbnail and the inspector's large preview are
layers whose `contentsRect` is the fixed cell of that slot. A row shows its cell only when
`cells[slot].sourceId` equals the row's `sourceKey` and the state is `live` or `held`.
Otherwise it shows a placeholder that says why: "Not in the multiview", "Video off", "No
signal", "Waiting for video". On reassignment the shell blanks the row at once and waits for a
cell event naming the new source. One 15 Hz timer re-marks all thumbnail layers. The existing
`SurfaceView` re-marks at 60 Hz per view (`ShellApp.swift:1306-1318`), which is not repeated.

The border on the thumbnail is drawn by SwiftUI from the tally in section 4.4, with the
Windows colours (`MultiviewInputRow.cs:45-51`).

Gate: the sheet ships only if `scripts/mac-show-drill.py --seconds 40 --load 8` with demand on
shows no change in per-frame delivery, underruns or shed level against demand off. If it fails,
slice S6 ships candidate C instead, with its limits stated in the UI, and the Metal monitor
worker becomes its own spec.

### 4.6 The table

- Header and rows share one column-width function, the Swift twin of
  `SourcesTableRowControl.ColumnWidths` (`:115-120`), with the same breakpoints, driven by the
  page width from `TabPageLayout.pageWidth`.
- Row: slot number, thumbnail (96 x 54 pt), source picker with a type line under it, Type,
  Format, FPS, Signal chip, Audio, MV checkbox, ISO checkbox with the writer chip, and actions
  Inspect, Grade and a "…" menu. Grade is hidden until the grading spec ships.
- Tools: search, category filter (All, Zoom, Cameras, Screens, Media, Unassigned), a Columns
  menu (Type, Signal, Audio), the auto-assign toggle, and Open or Close inspector.
- Selection is by slot id and survives filtering and roster updates. A filter can hide the
  selected row without selecting another.
- Summary and status line use the Windows copy.
- Below the table, a Capture devices section lists every device, grouped Cameras and Screens,
  with Connect, Take offline (disconnect and unassign its slots, with a slot-write reason),
  Refresh (`list-capture-devices`) and the Windows empty-state copy. The Zoom feed health card
  stays where it is (Mac-only, kept).
- Column visibility and the inspector's open state are UI preferences under a new optional
  `ShellPrefs.sourcesWorkspace` key, apart from production settings.
- Every control has an accessibility label that includes the slot number.

### 4.7 The inspector

It sits beside the table when the page is 1500 pt or wider, in a 340 pt column, and below it
otherwise. It is bound to the selected slot's `sourceKey`. When that key changes under an open
editor, the editor's fields disable and it says "Source assignment changed. Reopen these
options for the current source."

- Header: the large preview (320 x 180 pt, the same sheet cell), format and age, and the
  assignment hint (Windows copy).
- **Source tab**
  - Display name, with "Reset to source name".
  - Lower-third text: name, secondary line, mode. Stored per `sourceKey` (new optional
    `ShellPrefs.sourceLowerThirdText`). Consumer today: when the manual lower third keys a
    Program source (`ProgramLowerThird.swift`), it uses that source's stored text in place of
    the typed global text when the operator chooses "Use source text" on the Overlays tab. The
    Graphics engine (roadmap M4) becomes the second consumer.
  - Role (Zoom guests only): the Windows role list, one holder per role, meeting-scoped,
    disabled when the guest is absent. It is sent as `role` in the spine participants
    (`AppModel.swift:2301`). See owner decision 4.
  - Dropout: Hold last frame or Black, for Zoom and capture. Capture shows the status line
    from `sources[].liveness`, `lossReason` and `failInMs`, and is disabled when
    `dropoutPolicyApplies` is false. The policy is sent with `set-source-policy`, after
    `load-scene-graph` in the batch (`AppModel.swift:2488-2493`).
  - Unassign this input.
- **Picture tab**
  - **Key** (Mac-only, kept): the existing `ChromaKeyControl` fields, unchanged in behaviour.
    It is persisted by `sourceKey` (`ShellPrefs.swift:85-93`) and sent only when enabled
    (`AppModel.swift:2518-2533`). The row keeps a small "KEY" chip when a key is on, so the
    state stays visible without the inspector.
  - **Grade** (reserved): a titled, empty section with one line, "Per-source colour grade
    arrives with the grading update." The per-source grading spec owns everything inside it and
    the row's Grade action. This spec defines only its position: below Key, in this tab.
- **Audio tab**: paired microphone (capture only) and the note that gain, mute and processing
  live on the Audio page.

### 4.8 Threading and performance

- The shell does no per-frame work. Thumbnails are zero-copy layer contents.
- Facts update at snapshot cadence and at up to 5 per second per guest from format events.
- The rows are a fixed array of ten. Roster and fact updates patch in place.
- Opening the Sources tab must not change Program: only `set-source-preview-demand` is sent.
  Closing it must return the core to zero sheet cost.

### 4.9 Degraded states the operator sees

| Situation | What shows |
|---|---|
| Core without the sheet (older core, or the gate in 4.5 failed) | Thumbnail column shows the placeholder "Preview unavailable". Everything else works. |
| Sheet shed under load | Thumbnails update slower. The status line says "Source previews reduced to protect Program". |
| Slot assigned, not in the show | Signal IDLE, no picture, hint "Assigned, but hidden from the show". |
| Capture device unplugged | Signal NO INCOMING, the binding kept, picker label "Saved source unavailable — <name>". |
| Guest leaves | The slot is freed (rule 3) and the status line names who left which slot. |
| ISO writer fields unreported | Writer chip reads "—", never OFF. |
| Stale picker choice | Refused. The status line says the source changed. |

## 5. Slices

Each slice ships alone and has a real consumer.

| Id | What lands | Lane | Size | Depends on | Proven by |
|---|---|---|---|---|---|
| S1 | The slot law (4.2): newcomer-only auto-assign, reserved cleared slots, toggle, Zoom slots freed on leave and not restored, slot-write log, immediate save. | shell | M | — | Swift ports of the Windows tests in 2.4, each failing on today's code first. A fake-engine run: replace A with B, wait three roster ticks, A is not re-added. |
| S2 | `SourceRowFacts` and its data: capture resolution and rate, `sources[]`, `zoom-source-format`, ISO writer state, tally. Consumer: chips added to the existing rows, plus the row-12 picker guard. | shell | M | — | Unit tests from the Windows cases in 2.2. Tab snapshot at 1360 and 1920. |
| S3 | The table (4.6): header, columns, responsive widths, selection, search, filter, columns menu, summary, capture section. Screens labelled; picker grouped. | shell | L | S2 | Width-function tests against the Windows breakpoints. Tab snapshots. Owner side-by-side with Windows. |
| S4 | Stills as slot sources; clips listed and gated. | shell | S | S3 | Unit: multiview payload carries `kind: media`. Pixel: the multiview IOSurface shows the still (Metal readback). |
| S5 | The inspector (4.7): names per source, dropout for capture, Role, lower-third text with its Overlays consumer, Key moved in, Grade section reserved. | shell | L | S3 | Unit tests for name reset, identity guard, role exclusivity, policy ordering. Pixel: a keyed capture source on Program before and after the move is identical. |
| S6 | The source sheet (4.5): shared plan builder, Metal target, event and snapshot node, shell thumbnails and inspector preview, tally borders. | native + shell | L | S3 (S5 for the large preview) | Native: plan-builder unit tests; a Metal readback test of two cells. Drill gate with demand on. Owner live check. |

S1 and S2 are independent and can start at once. S6's native half can run in parallel with S3.

Rough calendar for one shell lane: 4 to 5 weeks, plus about 1 week of native lane for S6.

## 6. Acceptance and test strategy

**Unit tests** (`COREVIDEO_SHELL_TESTS`). Each port quotes the Windows ruling and names the
Windows test, and must fail on the pre-slice Mac code (roadmap §6).

- Law: replace does not re-add; an emptied slot is not refilled; an empty roster releases
  reserved slots; the toggle fills everyone; a rejected roster frees nothing; a second restore
  is refused; every write carries a reason; a write reaches disk without the autosave.
- Facts: the full state table of 2.2; a stale rate is not shown as live; a request cap is not
  shown as a format; a reused Zoom id clears old facts; the writer chip mapping
  (`IsoChipUsesObservedWriterLifecycle`).
- Table: column widths at 700, 900, 1100 and 1600; a filter never changes assignments,
  MV or ISO (`InspectionFiltersAndColumnsNeverChangeAssignmentsOrLiveSelections`).
- Inspector: a blank name resets; the identity guard disables a stale editor; one holder per role.
- Wire: `set-source-preview-demand` follows tab visibility and is re-sent after a core respawn;
  `set-source-policy` follows `load-scene-graph`.

**Native tests** (`ctest`, Mac CI and Windows).

- `buildSourceSheetRenderPlan`: cell N is slot N+1 for every layout mode and tile count; no
  border; an unassigned slot has no layer; the plan is never built with demand off.
- `MetalCompositorTest`: two sources with distinct solid colours, read back the sheet, each
  cell's centre pixel matches its own source and the others are clear.
- A swap test: reassign slot 1 from A to B. The first sheet event naming B has B's pixels.

**Pixel proofs.**

- `COREVIDEO_SHELL_TAB_SNAPSHOT` (`TabPageLayout.swift:63-125`) for the Sources tab at 1360 and
  1920, with the seed extended to a mixed roster (Zoom, camera, screen, still, an offline
  capture, a stalled guest) and the inspector open and closed.
- Metal readback for the sheet, as above. `ImageRenderer` cannot prove IOSurface pixels.
- Chroma key: Program readback of a keyed source is unchanged by slice S5.

**Drills.** `scripts/mac-show-drill.py --seconds 40 --load 8` with
`COREVIDEO_FAKE_ENGINE_FPS=60`, run twice, with source preview demand off and on. Pass means
no difference in per-frame delivery, underruns, shed level or coreMutex over-budget ratio. Mean
fps is not accepted as evidence.

**Live owner checks**, in a real meeting with at least one camera and one screen capture.

1. Each row's picture is the right person, including straight after a reassignment.
2. Replace a guest by hand, then wait a minute. Nobody comes back by themselves.
3. Unassign a slot, then have someone join. They do not take that slot.
4. A guest turns their camera off, then on. The chip goes VIDEO OFF, then LIVE.
5. Arm an ISO, start and stop recording. The chip goes ARMED, REC, OFF.
6. Quit and relaunch outside a meeting. Capture and still slots return with names, keys and
   microphones on the right sources.
7. Watch Program while opening and closing the Sources tab. No visible change.

## 7. Risks and unknowns

| Risk | How it is resolved |
|---|---|
| The sheet pass costs Program frames, because Metal monitors share the render thread. | The drill gate in 4.5, before S6 merges. Fallback: candidate C, and a separate Metal monitor-worker spec. |
| Capture liveness and ISO writer lifecycle fields may be unreported on Mac. | A half-day spike at the start of S2: dump a live snapshot with a camera, a screen and a recording. If absent, the chip shows only what is observed, and a native follow-up is filed. |
| Freeing Zoom slots on leave could empty the wall during a core restart. | Rule 3 acts only on rosters `rosterProjection` accepts. Test: a rejected roster frees nothing. Fault drill: kill the core mid-meeting and check that the slots survive the respawn. |
| The Role control sends a role the Mac has no consumer for (role-targeted routes are unported). | Owner decision 4. If kept, a spike confirms what the core does with `role`. |
| The per-source lower-third text has only a small consumer until Graphics lands. | Stated in the UI copy. It is the same store Graphics will read, so no migration follows. |
| Sheet cell size (640 x 360) may be more than the rows need. | Measured in the drill. 480 x 270 is the first reduction. |
| The display-name migration from `PersistedSlot.name` could attach an old name to the wrong source. | Migrate only when the persisted slot's own source key matches. Covered by a test. |
| A SwiftUI table of ten rows with live facts could redraw too often. | Facts are published per slot. Measured with Instruments during S3. |

## 8. Owner decisions needed

1. **Thumbnail mechanism.** A fixed source sheet from the core (identity-safe, matches the
   Windows rule, one extra GPU pass while the Sources tab is open) or a crop of the multiview
   (no cost, but tied to the wall layout and able to show the wrong guest for a few frames).
   Recommendation: the source sheet, with the crop as the fallback if the drill gate fails.
2. **Zoom slots after the meeting.** Today the Mac restores last meeting's Zoom slots by Zoom
   user id. Recommendation: do not restore Zoom slots, and free a slot when its guest leaves, as
   Windows does. Zoom reuses ids, so a restored slot can attach to a different person.
3. **Auto-assign and camera-off guests.** Windows places every newcomer. The Mac places only
   camera-on newcomers. Recommendation: match Windows, so a guest who joins camera-off already
   has a slot when they turn it on. The cost is that camera-off joiners use slots.
4. **Role in the inspector.** Role-targeted routes are not on the Mac, so the control would
   store and send a role that changes nothing visible. Recommendation: ship the control hidden
   behind the same predicate as role-targeted routes, and show it when they are ported.
5. **Inline row controls.** The Mac row carries name, microphone, Key and dropout inline.
   Recommendation: move them into the inspector as on Windows, keeping only a KEY chip in the
   row. A ten-column table cannot fit them at 1360 pt.

## 9. Out of scope

- Per-source colour grading, looks, LUTs, scopes and the grade editor (separate spec, gap 2.5).
  This spec reserves only the Grade section and the row action's position.
- Zoom quality caps and the capture audio-sync nudge (gap 2.13).
- Media clip playback (gap 2.1). Clips appear in the picker, disabled, until it lands.
- Browser, SRT, RTMP and NDI sources (gaps 2.10, 2.11). Their picker groups and catalogs are omitted.
- Graphics columns in the inspector (audit row 41), which depend on the Graphics Designer.
- A Metal isolated monitor worker. It is named in section 7 as the follow-up if the sheet fails its gate.
- Crop, position and scale. They stay in the Scenes builder on both platforms (audit row 74).
- Control-surface actions for Sources (gap 2.7).
- Drag-reorder on the Sources table (Windows has none; the Studio drag-swap already exists).
- Windows changes. The core additions are inert on D3D11, which keeps its per-source exports.
