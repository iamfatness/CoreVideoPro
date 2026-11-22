# Mac UI parity audit — SOURCES

Date: 2026-11-15 · Branch: `mac-zoom-signin` @ `060d6fa7` (the build the owner has installed) · Read-only; no app launched.

Owner report: "Sources screen is way different than the windows version." This audit lists exactly how.

## What "Sources" means on each platform

- **Windows**: the nav button labelled **Sources** (`native-shell/CoreVideoPro.WinUI/Views/StudioWorkspace.xaml:189-194`, `CommandParameter="inputs"`) shows `SourcesInputsPage` (`StudioWorkspace.xaml:714-718`). That page holds:
  - the table-first `SourcesWorkspaceControl` (issue #590; contract in `docs/reference/sources-table-workspace-spec.md`), and
  - three collapsible catalogs: Capture devices + browser sources, SRT ingest, and RTMP ingest (`Views/SourcesInputsPage.xaml:41,159,256,312`).
- **Windows naming trap**: `SourcesPage.xaml` is **not** the Sources tab. It is the **Scenes** builder (`IsSourcesTab` maps to the "Scenes" button, `StudioWorkspace.xaml:195-204, 709-712`). Crop, position and scale live there. This audit only touches it where the brief asks (crop/position/scale).
- **Mac**: `StudioTab.sources` (`mac-shell/Sources/CoreVideoProShell/AppModel.swift:13`) shows `SourcesPane` (`ShellApp.swift:1221, 1447-1563`), with `SlotRow` (`1656-1801`) and `ChromaKeyControl` (`1592-1654`). The model is `ShowInputSlot` (`AppModel.swift:141-170`).

## Why it looks "way different" (summary)

1. **Different design generation.** Windows ships the #590 table-first workspace:
   - search and category filter, a columns menu, Zoom quality, and Source catalog,
   - a 10-column table with live thumbnails, format/FPS, a signal chip, an MV checkbox, ISO plus a writer chip, and Inspect/Grade/⋯,
   - an optional 3-tab inspector, plus collapsible catalogs.

   Mac still renders the older `03-sources.png` composition: a card of plain slot rows over two side-by-side reference cards, "Zoom feed health" and "Capture devices".
2. **Mac squeezes the pane to 720 pt.** `TabPage` wraps every non-studio tab in `.frame(maxWidth: 720)` (`ShellApp.swift:1232`), even though `SourcesPane` asks for 900 (`1561`). A `SlotRow` is built from fixed widths: 22 + 300(+20 pad) + 150 + 160(+16) + 130(+16), plus the checkboxes, Key and Unassign (`1673, 1716-1717, 1737, 1760-1761, 1785-1786`). That is about 1000 pt or more, so assigned rows almost certainly compress or clip. This is inferred from the code, not seen on screen.
3. **Per-source picture and health are absent.** Mac has no thumbnail, format, FPS, status chip or REC chip. Grading on Mac is a single program-wide grade window. Windows grades per source, from the Sources rows.
4. **Source types are limited.** The Mac picker offers only "Zoom participants" and "Capture devices". There are no Media, Browser, SRT, RTMP or NDI sources, and screens are labelled "Camera ·".

## Legend

- **Status**:
  - **Same**: equivalent behaviour.
  - **Partial**: exists but incomplete.
  - **Different**: same intent, materially different behaviour or placement.
  - **Missing**: absent on Mac.
- **Impact**: H/M/L for a live operator.
- **Size**: S < 1 day, M a few days, L about 1-2 weeks, XL more than that.
- **Core**:
  - **Shell**: the native core already supports it, so the work is in the mac shell.
  - **Native**: needs C++, build or packaging work.

Windows paths are relative to `native-shell/CoreVideoPro.WinUI/`. Mac paths are relative to `mac-shell/Sources/CoreVideoProShell/`. `SWC` = `Controls/SourcesWorkspaceControl.cs`, `SWC.I` = `Controls/SourcesWorkspaceControl.Inspector.cs`, `Row` = `Controls/SourcesTableRowControl.cs`, `SIP` = `Views/SourcesInputsPage.xaml`, `CGE` = `Views/ColorGradeEditorWindow.xaml`.

## A. Page framing and layout

| # | Feature | Windows (file:line) | Mac status + evidence | Impact | Size | Core |
|---|---|---|---|---|---|---|
| 1 | Tab identity | "Sources" nav → `SourcesInputsPage` (`StudioWorkspace.xaml:189-194,714-718`) | **Same**: "Sources" → `SourcesPane` (`AppModel.swift:13,30-33`; `ShellApp.swift:1221`) | — | — | — |
| 2 | Page width / fit | Stretches to the window, with a responsive tool row (`SIP:28-33`; `SWC:77-84`) | **Different**: capped at 720 pt (`ShellApp.swift:1232`). Fixed-width row controls total about 1000 pt (`1716,1737,1760,1785`), so the rows are cramped or clipped | H: the first thing the owner sees; controls may be unreachable | S (cap) / M (reflow) | Shell |
| 3 | Overall structure | Table-first workspace on top, then collapsible catalogs (`SIP:41,159,256,312-313`) | **Different**: a slot-list card plus two reference cards (`ShellApp.swift:1454-1559`) | H: a different mental model from the Windows training and docs | L | Shell |
| 4 | Title / workflow strip | "Show inputs" plus a one-line "1 Assign inputs / Multiview → 2 Route outputs → 3 Compose scenes / Take" (`SWC:41-42`) | **Different**: "Show inputs (multiview)", instruction copy and three `StepCard`s (`ShellApp.swift:1456-1476`) | L: cosmetic | S | Shell |
| 5 | Summary line | "x/10 assigned · n in MV · n ISO selected · n match filter" (`SWC:150`) | **Partial**: "n in show · n assigned" (`ShellApp.swift:1460`); no ISO count | L | S | Shell |
| 6 | Status / feedback line | Bottom status text and table note on observed format and ISO-vs-writer (`SWC:30,89,94`) | **Missing** | L | S | Shell |

## B. Inputs table (ten stable slots)

| # | Feature | Windows (file:line) | Mac status + evidence | Impact | Size | Core |
|---|---|---|---|---|---|---|
| 7 | Column headers | `#, Source, Type, Format, FPS, Signal, Audio, MV, ISO, Actions` (`SWC:86-87`) | **Missing**: no header row. The MV checkbox has no label (`.labelsHidden()`, `ShellApp.swift:1674-1678`) | M: an unlabeled checkbox is ambiguous mid-show | S | Shell |
| 8 | Slot number | "01"…"10" (`Row:27`) | **Same**: "1"…"10" (`ShellApp.swift:1670-1673`) | — | — | — |
| 9 | Live per-source thumbnail with tally border (PGM amber, PVW green, talking white) | `Row:31-36`; `ViewModels/ShowInputs/MultiviewInputRow.cs:45-51`; preview demand `Views/SourcesInputsPage.xaml.cs:18-40` | **Missing**: no surface in `SlotRow` | H: operators identify cameras and guests visually | L | **Native**. Core demand exists (`set-source-preview-demand`, `native/src/core/MediaCore.cpp:1275`; `MediaCoreMonitors.cpp:26-39`), but only D3D11 implements `submitMonitors` source exports (`native/src/modules/D3D11CompositorAdapter.cpp:282,890-896`). `MetalCompositorAdapter.mm` has none. Shell M after that |
| 10 | Inline row source picker (patch without opening the inspector) | `Row:38-40`; `Controls/SourceAssignmentPicker.cs` | **Same** in concept: a per-row `Menu` (`ShellApp.swift:1680-1700`) | — | — | — |
| 11 | Picker categories | Submenus Zoom / Camera / Screen / Browser / Media / SRT / NDI (`Services/ShowInputRosterService.cs:290`, built at `:202-237`) | **Partial**: only "Zoom participants" and "Capture devices" sections (`ShellApp.swift:1681-1700`) | M | S (grouping); the source kinds themselves are rows 54-59 | Shell |
| 12 | Picker safety (opening never writes; stale choice rejected) | Menu built on open, guarded by source identity (`SourceAssignmentPicker.cs:58-86`) | **Partial**: the menu reads the live roster and the buttons call `assignSlot` directly (`ShellApp.swift:1683-1697`; `AppModel.swift:1083-1092`). There is no stale-identity guard | L | S | Shell |
| 13 | Unavailable source keeps its binding (never substituted) | "Saved source unavailable — previous Zoom guest" entry (`ShowInputRosterService.cs:240-249`); hint copy (`ViewModels/ShowInputSlotViewModel.cs:360-363`) | **Partial**: same behaviour. `refreshSlotHealth` keeps `sourceId` and sets `offline` (`AppModel.swift:1125-1137`). Copy is "Source offline — choose or reconnect" plus an OFFLINE tag (`ShellApp.swift:1664,1707-1711`); no recovery hint | L | S | Shell |
| 14 | Source label and kind line | "Camera · Elgato HD60" from the group (`ShowInputRosterService.cs:292-295`), with a kind line under the picker (`Row:41-43`) | **Partial**: every non-Zoom source is labelled "Camera ·", screens included (`ShellApp.swift:1665`) | L | S | Shell |
| 15 | Type column | KindLabel (`Row:45`; width `Row:116`) | **Missing** | L | S | Shell |
| 16 | Format (resolution) column, amber when stale | `Row:46-48`; `MultiviewInputRow.cs:36,55-56` | **Missing**. `CaptureDeviceRow` has no resolution field (`AppModel.swift:117-125`) and the parser ignores it (`:897-906`), although the core publishes it (`MediaCore.cpp:299`). Zoom format needs the `zoom-source-format` event, which `handleEvent` drops (`AppModel.swift:1159-1180`) | M: catches a guest stuck at low resolution | M | Shell |
| 17 | FPS column (a stale frame rate never shows as live) | `MultiviewInputRow.cs:37-38`; `Row:49-50` | **Missing** (same data gap as row 16) | M | M (with 16) | Shell |
| 18 | Signal chip: IDLE / LIVE / TALKING / STALLED / PGM, coloured | `Row:51-53`; `MultiviewInputRow.cs:57-65`; projection `ViewModels/ShowInputs/MultiviewInputRowsProjection.cs:122,374,485` | **Partial**: only "no signal" for a connected capture source with no frames (`ShellApp.swift:1722-1727`) and OFFLINE (`1707-1711`). No stall or PGM state, and nothing for Zoom | H: the only at-a-glance "is this feed alive" readout | M | Shell (frame-age/stall data needs the observed-source projection; check the Metal path fills source health) |
| 19 | Audio column | AudioSummary: "Zoom source audio", the paired mic, "Media audio" or "No paired microphone" (`Row:56-58`; `ShowInputSlotViewModel.cs:381-383`) | **Partial**: only the capture-row mic menu label (`ShellApp.swift:1752-1753`) | L | S | Shell |
| 20 | MV (multiview) inclusion | Checkbox, disabled when unassigned (`Row:59-63`) | **Same**: In-show checkbox (`ShellApp.swift:1674-1679`) → `toggleSlotInShow` (`AppModel.swift:1103-1115`) → multiview sources (`:1547-1560`). Mac also drops offline slots from the wall | — | — | — |
| 21 | ISO arm and eligibility | `ShowIsoToggle` (`ShowInputSlotViewModel.cs:200-206`); checkbox (`Row:64-69`) | **Same**: ISO checkbox on assigned rows (`ShellApp.swift:1791-1795`) → `toggleSlotIso` (`AppModel.swift:1117-1122`) | — | — | — |
| 22 | ISO writer chip: REC / ARMED / OFF / ERROR (separate from the arm) | `Row:70-73`; `MultiviewInputRow.cs:58,66-72` | **Missing** on Sources | M: arming can't be confused with actually recording | S-M | Shell |
| 23 | Row actions | Inspect · Grade · ⋯ (`Row:75-85`) | **Different**: Unassign button only (`ShellApp.swift:1796-1797`). Name, mic, Key and dropout are inline in the row instead | M | M | Shell |
| 24 | Row details expander (7 facts: assignment, state, format, freshness, audio, ISO writer, recovery) | `Row:83-94` | **Missing** | L | S | Shell |
| 25 | Responsive columns (Type/Audio hidden under 1050 px, Signal and thumbnail under 750 px) | `Row:113-120` | **Missing**: fixed widths; see row 2 | M | M | Shell |
| 26 | Selected-row highlight and selection model | `Row:104`; `SWC:141,148` | **Missing**: there is no selection concept | L | S | Shell |

## C. Workspace tools and filters

| # | Feature | Windows (file:line) | Mac status + evidence | Impact | Size | Core |
|---|---|---|---|---|---|---|
| 27 | Search: "Find an input or source" | `SWC:46-47`; `ViewModels/SourcesWorkspaceState.cs:31-34` | **Missing** | L: only 10 rows | S | Shell |
| 28 | Category filter (All, Zoom, Cameras, Screens, Media, Network, Browser, Unassigned) | `SWC:48-49`; `SourcesWorkspaceState.cs:10-11,19-35` | **Missing** | L | S | Shell |
| 29 | Columns menu (Type / Signal / Audio), saved as a UI preference | `SWC:54-68`, load at `:40` | **Missing** | L | S | Shell |
| 30 | Filter empty state: "No matching inputs. Choose All sources or clear the search…" | `SWC:28,149` | **Missing** (moot without a filter) | L | S | Shell |
| 31 | Zoom quality flyout: preferred camera max resolution and max guest FPS, with request-vs-applied status | `SWC:69-70,175-199`; `ViewModels/StudioViewModel.ZoomCameraResolution.cs` | **Missing**: no `cameraMax`/`MaxResolution` anywhere in mac-shell (grep) | M: the bandwidth/quality lever for big rooms | M | Shell: the engine already accepts `cameraMaxResolution` (`native/src/modules/ZoomEngineRuntime.cpp:250-263`) |
| 32 | "Source catalog" button that expands and scrolls to the matching catalog | `SWC:71`; `SourcesInputsPage.xaml.cs:58-65` | **Missing** | L | S | Shell |
| 33 | "Grade selected…" toolbar button | `SWC:73,151,170-174` | **Missing** | M | S (after 61) | Shell |

## D. Inspector

| # | Feature | Windows (file:line) | Mac status + evidence | Impact | Size | Core |
|---|---|---|---|---|---|---|
| 34 | Optional inspector: Open/Close; beside the table at ≥ 1500 px, stacked below otherwise | `SWC:74,152-162`; `SourcesWorkspaceState.cs:18` | **Missing** | M | L | Shell |
| 35 | Inspector large preview (180 px) | `SWC.I:27-30` | **Missing** | M | M (after 9) | Native, via row 9 |
| 36 | Inspector format and freshness / request-cap label | `SWC.I:31-33`; `MultiviewInputRow.cs:53` | **Missing** | L | S | Shell |
| 37 | Assignment hint copy | `SWC.I:34`; `ShowInputSlotViewModel.cs:360-363` | **Missing** | L | S | Shell |
| 38 | Inspector tabs: Source / Picture-Grade / Audio | `SWC.I:35-44` | **Missing** | M | M | Shell |
| 39 | Display name | Stored per **source id**; blank resets to the derived name; feeds lower thirds and MV labels; persisted (`SWC.I:55-64,134-141`; `ShowInputSlotViewModel.cs:421-437`) | **Partial**: an inline field edits `slot.name` (`ShellApp.swift:1729-1737`), which reaches the MV label (`AppModel.swift:1555`). It is tied to the slot, not persisted (row 77), and blank does not reset | M | S | Shell |
| 40 | Per-source lower-third text (name, secondary line, mode) | `SWC.I:73-81`; `Views/SourceLowerThirdFlyout.cs:14-28` | **Missing** on Sources. Mac has one global lower third (`AppModel.swift:489-491`) | M | M | Shell |
| 41 | Graphics columns: per-slot variable values editor plus read-only table | `SWC.I:82-119`; `Controls/SourcesWorkspaceControl.GraphicsColumns.cs` | **Missing**. Depends on the Graphics Designer, which Mac lacks; see the graphics audit | L | L | Shell (blocked on Designer) |
| 42 | Source options: Zoom guest **Role** plus dropout policy; capture dropout when it applies | `SWC.I:120-121,173-181`; `Views/SourcePolicyFlyout.cs` (role `:23-25`, policy `:26`, notices `:41-56,70`) | **Partial**: Zoom-only "Hold last frame / Black" menu in the row (`ShellApp.swift:1770-1790`; `AppModel.swift:1654-1660`). No Role and no capture dropout | M | M | Shell: core `set-source-policy` (`MediaCore.cpp:1891-1906`) |
| 43 | Capture dropout status text | `SWC.I:122-129`; `ShowInputSlotViewModel.cs:392-415` | **Missing** | L | S | Shell |
| 44 | Unassign | Inspector "Unassign this input" (`SWC.I:130-131`) | **Same**, but placed in the row (`ShellApp.swift:1796-1797`), with a tombstone against Zoom auto-re-add (`AppModel.swift:1094-1101`) | — | — | — |
| 45 | Paired microphone (capture) | Inspector Audio tab (`SWC.I:152-172`); gain/DSP stay on the Audio tab (`:170`) | **Same**: row menu (`ShellApp.swift:1742-1765`; `AudioInputs.swift:94-99`), persisted per slot (`AppModel.swift:500-503`) | — | — | — |

## E. Adding and managing sources (catalogs)

| # | Feature | Windows (file:line) | Mac status + evidence | Impact | Size | Core |
|---|---|---|---|---|---|---|
| 46 | Capture device list | Every device, showing Format · Signal · Audio-sync labels and a connection state (`SIP:88-114`) | **Partial**: first 8 devices only, then "+N more" with no way to expand (`ShellApp.swift:1516,1549-1551`). Shows a dot, the name and the core warning; no format or sync | M | S | Shell |
| 47 | Refresh devices button | `SIP:50-52` | **Missing**: the list only updates from snapshots (`AppModel.swift:897`) | L | S | Shell: core `list-capture-devices` (`native/src/rpc/JsonRpcServer.cpp:243`) |
| 48 | Fleet summary | `SIP:59-60` | **Same**: "n connected · n detected" (`ShellApp.swift:1513-1515`) | — | — | — |
| 49 | Empty-state guidance: "Connect a webcam, capture card… then select Refresh" | `SIP:83-86`; `Models/ProductionModels.cs:971-972` | **Missing**: shows only "0 connected · 0 detected" | L | S | Shell |
| 50 | Connect | Shown when not connected (`SIP:135-141`) | **Same**: Connect/Disconnect toggle (`ShellApp.swift:1525-1528`; `AppModel.swift:3324-3341`) | — | — | — |
| 51 | Take offline: stop the session **and unassign its slots** | `SIP:142-149` | **Partial**: Disconnect only. The slot keeps its binding and turns offline (`AppModel.swift:1125-1137`) | L | S | Shell |
| 52 | Per-device audio sync nudge, −20 / +20 ms | `SIP:121-134`; `ViewModels/StudioViewModel.cs:7156` | **Missing** | M: lip-sync on capture cards | S | Shell: core `set-capture-audio-sync-offset` (`JsonRpcServer.cpp:255-261`; `MediaCore.cpp:530`) |
| 53 | Device no-frame / permission warnings | ObservedSignalLabel (`SIP:108`) | **Same**: core `warning` plus "Connected — waiting for the first frame" (`ShellApp.swift:1537-1547`) | — | — | — |
| 54 | Add browser source (URL, size preset, Add) | `SIP:65-81` | **Missing** | M | M-L | **Native / packaging**: core `browser-add` exists (`MediaCore.cpp:1355`) and a WKWebView host exists (`native/CMakeLists.txt:461-469`), but it is deliberately not bundled (`scripts/make-macos-shell-bundle.sh:112-114`) |
| 55 | SRT ingest: add/remove; Mode, Host, Port, Latency, Stream ID, Passphrase; runtime health | `SIP:159-255` | **Missing** | M | L | **Native build**: core `configure-srt-ingest-sources` exists (`MediaCore.cpp:1351`), but the mac release builds without SRT ingest (`scripts/release-macos.sh:48-51,157-161`) |
| 56 | RTMP ingest listener: add/remove, URL | `SIP:256-311` | **Missing** | M | L | **Native build**, same as row 55 (`MediaCore.cpp:1353`). Windows caveat: the picker omits an "RTMP" group, so `ShowInputRosterService.cs:208` options never render (`:290`) |
| 57 | NDI receive sources | "NDI" picker group (`ShowInputRosterService.cs:209,290`) | **Missing** | M | L | **Native**: the receive adapter is Windows-scoped (`native/CMakeLists.txt:87`) |
| 58 | Screen / window capture as a source | "Screen" picker group | **Partial**: ScreenCaptureKit is in the mac build (`native/src/modules/ModuleComposition.cpp:613-616`; `release-macos.sh:160`), so screens appear, but mixed into "Capture devices" and labelled "Camera ·" (`ShellApp.swift:1690-1700,1665`) | M | S | Shell |
| 59 | Media assets as slot sources ("No media assets — add them on the Media tab" hint) | `ShowInputRosterService.cs:220-237` | **Missing**: slot kinds are only `unassigned`, `zoom` and `capture` (`AppModel.swift:143`) | M | M | Shell for stills (ImageIO path, `native/src/modules/StillMediaFrameCache.cpp:25-28`). **Native** for video playback (Windows decodes through MediaFoundation) |
| 60 | Cameras (UVC / AVFoundation) | "Camera" group | **Same**: AVF adapter (`ModuleComposition.cpp:603-607`) | — | — | — |

## F. Colour grade (incl. LUTs and scopes)

| # | Feature | Windows (file:line) | Mac status + evidence | Impact | Size | Core |
|---|---|---|---|---|---|---|
| 61 | Grade target | **Per source**: `SourceColorGradeId` → `OpenColorGradeEditor` (`ViewModels/StudioViewModel.Grading.cs:63-75`) → core `set-source-grade` | **Different**: one **program-wide** grade (`set-color-grade`, `AppModel.swift:1272-1284`; copy at `ColorGrade.swift:37-38`). One mismatched camera cannot be fixed without shifting everyone | H | M | Shell: `set-source-grade` (`JsonRpcServer.cpp:291`; `MediaCore.cpp:1229-1239`) is refused only on the software renderer; Metal applies per-layer grades (`MetalCompositorAdapter.mm:937-938`) |
| 62 | Grade entry points | Row "Grade", toolbar "Grade selected…", inspector "Grade…" and "Open grade workspace…" (`Row:76-79`; `SWC:73`; `SWC.I:24,147`) | **Different**: only the Studio multiview "Grade" chip (`ShellApp.swift:792`) and the control `open grade` (`:184`). Nothing on Sources | M | S | Shell |
| 63 | Basic sliders: Exposure, Contrast, Saturation, Temperature | ±50 "points" with help text (`CGE:52-55`) | **Partial**: 4 sliders, ±10 step 0.1 (`ColorGrade.swift:32-35,49-61`). The code comment says the core clamps to ±10, but the core clamps to ±100 (`MediaCore.cpp:163-165`), so the range rationale is stale | M | S | Shell |
| 64 | Built-in looks: none, neutral, warm-film, cool-broadcast, punch | `CGE:51`; `ViewModels/ColorGradeEditorViewModel.cs:12-13` | **Missing** | M | S | Shell: `lut` read at `MediaCore.cpp:173`; maps live in the shared `native/src/compositor/ColorGradeParams.h:18-27` |
| 65 | Import a .cube LUT | `CGE:86` | **Missing** | M | M | Shell: `CubeLut.h`; Metal cube ops (`MetalCompositorAdapter.mm:1336-1337`) |
| 66 | Advanced stack: + Primaries, + Curves, Up/Down/Duplicate/Remove, intensity, bypass | `CGE:58-71`; `Controls/GradeCurveControl.cs`, `Controls/GradeAdjustmentControl.cs` | **Missing** | M | L | Shell: Metal consumes advanced grades (`MetalCompositorAdapter.mm:937,1319`) |
| 67 | Undo/Redo/Reset; Copy/Paste grade; Save/Load preset | `CGE:75-85` | **Partial**: Reset only (`ColorGrade.swift:27-29`) | L-M | M | Shell |
| 68 | Scopes: histogram, waveform, vectorscope; Luma/RGB/parade modes; expand; analyze original | `CGE:28-37,88-89,102-103` | **Missing** | M (H for graders) | XL | **Native**: scopes come from the D3D grade-preview worker (`compositor/D3DGradeScopes.h`; `D3D11CompositorAdapter.cpp:269`). Metal inherits `supportsGradePreview() = false` (`native/src/modules/Interfaces.h:1400`) |
| 69 | Scope ROI: draw rectangle/circle, precision sliders, arrow-key nudge | `CGE:14-19,90-99`; `Views/ColorGradeEditorWindow.Roi.cs`; `Controls/ScopeRoiControl.cs` | **Missing** | L | L | **Native** (with row 68) |
| 70 | Editor preview surface plus "Compare original (monitor only)" | `CGE:23,47` | **Missing**: the operator grades by watching program | M | L | **Native**: `set-grade-preview` needs Metal grade preview |
| 71 | Live edit toggle versus Apply Live / Apply and Done / Close-discard draft | `CGE:46,104,109-110` | **Different**: always live, with no draft or discard (`ColorGrade.swift:9-10,60`) | M: every exploratory nudge goes to air | S | Shell |
| 72 | Several editors (up to 3 sources) | `StudioViewModel.Grading.cs:72-75` | **Different**: one window (`ShellApp.swift:143`) | L | S (after 61) | Shell |
| 73 | Grade persistence | Per source (`StudioViewModel.Grading.cs:16,38,87`) | **Partial**: one program grade persisted (`AppModel.swift:499,551-556`) | M | S (after 61) | Shell |

## G. Framing, behaviour, persistence, accessibility

| # | Feature | Windows (file:line) | Mac status + evidence | Impact | Size | Core |
|---|---|---|---|---|---|---|
| 74 | Crop / position / scale | Deliberately **not** on Sources: "Scene positioning, crop and fit remain in Scene Builder" (`SWC.I:149`). Crop, fit and opacity are on the Scenes tab (`Views/SourcesPage.xaml:310-312,358,362,375`) | **Same** on Sources (neither has it). For the Scenes audit: Mac Scenes has drag, resize, fit and opacity but no crop (`SceneCanvas.swift:2-7,99-194,317-339`) | — | — | — |
| 75 | Persist slot assignments (kind, source, name, MV) across relaunch | Roster store load/save (`ViewModels/ShowInputs/ShowInputsCoordinator.cs:144-185,215-224`) | **Missing**: slots start empty (`AppModel.swift:1061`); there are no slot fields in `ShellPrefs` (`ShellPrefs.swift:86-125`). Key, mic pairing and dropout persist **by slot number** (`AppModel.swift:500-512,546-569`), so after a relaunch they attach to whatever lands in that slot. Zoom auto-assign refills slots in arrival order (`:1027-1045`) | H: capture rows vanish every launch, and keys or mics can land on the wrong source | M | Shell |
| 76 | ISO selection persistence | `IsoSelectedSourceIds` persisted and re-projected onto rows (`ShowInputsCoordinator.cs:280-284,302`) | **Partial**: saved (`AppModel.swift:487`) and restored (`:563`). But `slot.iso` is never re-projected on assign, and `recomputeFromSlots` rebuilds the set from slot flags (`:1074-1078`), so the first assignment wipes the restored selection. Only the routing-matrix path re-projects (`:2533-2537`) | M: armed ISOs silently drop | S | Shell |
| 77 | Auto-assign Zoom video guests into empty slots, respecting manual unassign | `ShowInputRosterService.cs:343` | **Same** (`AppModel.swift:1027-1045,1094-1101`) | — | — | — |
| 78 | Assigning a capture device auto-connects it | `SIP:54-58` | **Same** (`ShellApp.swift:1695-1697`) | — | — | — |
| 79 | Drag-reorder slots | Drag on the Studio multiview swaps slot assignments (`StudioViewModel.cs:4250-4268`). There is no drag on the Sources table | **Missing**: no `onDrag`/`onDrop` anywhere in mac-shell. Also tracked under the Studio audit | L (for Sources) | M | Shell |
| 80 | Keyboard shortcuts | No Sources accelerators (none in `Controls/Sources*.cs`); grade ROI arrow keys (`CGE:94`) | **Same** for Sources (Mac `.keyboardShortcut` appears only at `ShellApp.swift:426`). ROI keys are covered by row 69 | — | — | — |
| 81 | Accessibility names per row control | `Row:39,60,66,75,79,85` | **Partial**: MV checkbox labelled "" and hidden (`ShellApp.swift:1674-1678`); ISO is labelled | L | S | Shell |
| 82 | Error and notice copy | Column-save failure (`SWC:63-64`); policy notices (`SourcePolicyFlyout.cs:41,70`); "Select a source before editing its color grade" and the 3-editor limit (`StudioViewModel.Grading.cs:68,74`) | **Partial**: capture connect errors only go to the global warning feed (`AppModel.swift:3338`); no inline copy | L | S | Shell |

## Counts by status (82 rows)

| Status | Count |
|---|---|
| Same | 15 |
| Partial | 18 |
| Different | 8 |
| Missing | 41 |

## Top 10 gaps by operator impact

1. **Slot assignments are not persisted** (row 75). Capture rows vanish every launch, and per-slot key, mic and dropout reattach to whatever source lands in that slot. H · M · Shell.
2. **Layout** (rows 2, 3, 25). A 720 pt cap and fixed-width rows give a cramped, likely clipped list instead of the Windows responsive table plus catalogs. This is the main "way different" impression. H · S for the cap, L for the table · Shell.
3. **Grading is program-wide, not per source** (rows 61, 62, 33). There is no Grade from Sources rows; `set-source-grade` already works on Metal. H · M · Shell.
4. **No per-source live thumbnails or tally** (rows 9, 35). H · L · **Native** (Metal source exports).
5. **No per-source signal chip, resolution or FPS** (rows 16-18). LIVE / STALLED / PGM and stale-format are invisible. H/M · M · Shell (capture resolution already in the snapshot; Zoom needs `zoom-source-format`).
6. **Missing source types** (rows 54-59). Media, browser, SRT, RTMP and NDI are absent, and screens are mislabelled "Camera". M · S to L · mix: screens and stills are shell; browser, SRT, RTMP, NDI and video media are native or build.
7. **No inspector** (rows 34-43). Missing pieces:
   - per-source display name, persisted and keyed by source,
   - lower-third text,
   - guest Role,
   - capture dropout,
   - assignment hints.

   M · L · Shell.
8. **No capture audio-sync nudge** (row 52). The core command exists. M · S · Shell.
9. **No Zoom quality controls** (row 31): max resolution and FPS. The engine already accepts `cameraMaxResolution`. M · M · Shell.
10. **Grade tooling is thin** (rows 63-71). Missing:
    - looks and .cube LUTs, which are shell work,
    - the curves/primaries stack, also shell,
    - scopes, ROI and compare, which need native work (Metal grade preview).

    Also, Mac is always live with no discard. M · L/XL.

Also notable: the ISO writer chip is missing (row 22), and the restored ISO selection is wiped by the first assignment (row 76).

## On Mac but NOT on Windows (Sources tab)

- **Per-source chroma key** (green/blue presets; similarity, smoothness and spill sliders), persisted per slot (`ShellApp.swift:1592-1654`; `AppModel.swift:504-508,1672-1690`). Windows has no chroma key UI at all: no `ChromaKey` symbol in `native-shell/CoreVideoPro.WinUI`. The core supports it through the route `chromaKey` node.
- **Zoom feed health card on Sources** (`ShellApp.swift:1485-1509`). On Windows, feed health lives under Health (`Views/DiagnosticsView.xaml`, `FeedHealthRows`).
- **Roster recovery notice** on Sources: "Zoom roster reconciling with restarted core" (`ShellApp.swift:1462-1465`; `AppModel.swift:739`).
- **Program-wide colour grade window**, with its own window and control-API `open grade` (`ShellApp.swift:143-149,184,792`; `ColorGrade.swift`). Windows' UI grades per source only.
- **Disconnect** for a connected capture device without unassigning its slots (`ShellApp.swift:1525-1528`). Windows "Take offline" also unassigns.
- **Everything editable inline in the row** (name, mic, Key, dropout) without an inspector. This is a placement difference, but a deliberate Mac-only flow.
- **Three-step `StepCard` workflow strip** (`ShellApp.swift:1466-1476`). Windows uses a single text line.
