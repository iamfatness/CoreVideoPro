# macOS UI parity audit: Routing, Overlays (Graphics), Media (2026-11-15)

Scope: what the operator sees and can do on the Windows **Routing**, **Overlays** (tab label
"Graphics") and **Media** pages, compared with the macOS shell in this worktree
(`mac-zoom-signin`, HEAD `060d6fa7`, the build the owner has installed). This was a read-only
audit. Nothing was built, run or launched. Every Mac claim below was checked in source, and each
row cites the Swift line it rests on. Where a conclusion depends on reading core code and has not
been seen on screen, the row says **UNVERIFIED on screen**.

This audit does **not** redo the GPU graphics-layer gap. For that, see
`docs/superpowers/audits/2026-11-14-mac-parity-graphics.md`: G0 (the Metal graphics backend:
`MediaCore.cpp:1256` refuses `graphics-*` without `supportsGraphicsInstances()`), T (the Mac Take
does not send `begin-take-transition`), and items 1-14 (roadmap M3/M4). Here those rows only point
at the item that blocks them.

Legend
- **Status**: Same / Partial / Different / Missing (Mac compared with Windows).
- **Impact**: H/M/L, for a live operator.
- **Size**: S (≤1 day), M (days), L (1-2 weeks), XL (more).
- **Where**: *shell* (Swift only; the core already accepts it) or *core* (native work is
  needed, with the evidence).

Paths: `W/` = `native-shell/CoreVideoPro.WinUI/`, `M/` = `mac-shell/Sources/CoreVideoProShell/`,
`N/` = `native/src/`.

---

## 0. Findings that change rows below

1. **The Mac manual lower third is probably never drawn on Program (H, S, shell). UNVERIFIED on screen.**
   - `M/AppModel.swift:2835-2848` (`showLowerThird`) sends `set-overlay-asset` with
     `position:"lower-third"` and `sourceId:"mac:manual"`.
   - Since `d6b5f740` (2026-09-06, "bind lower thirds to rendered Program"), the shared render plan
     drops any lower-third asset whose non-empty `sourceId` matches no video layer on Program
     (`N/core/MediaCore.cpp:5142-5153`). No layer ever has id `mac:manual`. The asset is
     `continue`d past, and the legacy placeholder is deliberately not substituted (`:5149-5152`).
   - The phase still comes back as `building-in`/`on-air` (`M/AppModel.swift:1001`), so the Mac
     readout says "Key on air" while nothing is keyed.
   - The Windows key always carries the Program source's id
     (`W/ViewModels/StudioViewModel.LegacyLowerThirdCommands.cs:28-38`), which matches.
   - The Mac **auto** lower third sends `zoom:<pid>`/`capture:<id>`
     (`M/AutomationExtras.swift:188,248`). That form does match, so only the manual key is
     affected.
   - The shell fix is S: send the Program source's id, which is also what Windows does (see L2).
     Check on screen first.
2. **There is no media clip playout on Mac (H, L, core).**
   - The only `IMediaDecoder` factory is Media Foundation
     (`N/modules/ModuleComposition.cpp:440`). Its non-Windows build returns `{}`
     (`N/modules/MediaFoundationMediaFrameSourceAdapter.cpp:1312-1314`).
   - As a result `mediaTransports_` stays null (`N/core/MediaCore.cpp:469-471`, `MediaCore.h:751`),
     and `set-media-transport` is refused with "no media transports are running"
     (`MediaCore.cpp:3555-3558`).
   - Video and audio clips (stingers, audio beds) cannot play on a Mac core. Still images do work:
     `stillMediaCache_` is default-constructed (`N/core/MediaCore.h:1100-1101`), and the Apple
     ImageIO decoder exists (`N/modules/StillMediaFrameCache.cpp:~150-210`).
   - Building an AVFoundation `IMediaDecoder` is the prerequisite for every Media playback row.
3. **On Windows, the ISO crosspoints on the Routing page do not drive recording. On Mac they do.**
   - Windows `OnVideoRoutingMatrixChanged` only re-syncs and sets a status line
     (`W/ViewModels/StudioViewModel.cs:4629-4633`).
   - Windows ISO targets come from `IsoSelectedSourceIds`, which the Sources-tab ISO toggle owns
     (`W/ViewModels/ShowInputs/ShowInputsCoordinator.cs:313-323`). Nothing reads
     `VideoRoutingMatrix` except the page itself.
   - Mac `toggleIsoCell` writes `isoSelectedSourceIds` and `isoRecordingEnabled`
     (`M/AppModel.swift:2519-2537`).
   - So the Mac is "Different, and stronger" here. This is a Windows defect, not a Mac gap. The Mac
     has its own drift bug instead (R13).
4. **The main Windows Overlays page is the native-graphics Operate | Design surface.**
   - The legacy keyer, brand kit, browser DSK and captions now sit in a right-hand drawer, "existing
     overlay controls" (`W/Views/OverlaysPage.xaml:17,55-58`), opened from the Design workspace
     (`OverlaysPage.xaml.cs:84-87`).
   - On Mac, those legacy sections *are* the whole tab (`M/Overlays.swift:167-180`). Every
     Operate/Design row is therefore Missing, and is blocked by G0 (+T) from the graphics audit.

---

## 1. ROUTING (`W/Views/RoutingPage.xaml`, `W/ViewModels/VideoRoutingMatrixViewModel.cs`) vs `M/ShellApp.swift:512-598` `RoutingPane`/`CrosspointCell`

| # | Feature / behaviour | Windows (file:line) | Mac | Mac evidence | Impact | Size | Where |
|---|---|---|---|---|---|---|---|
| R1 | Header "Video Routing" plus help line ("…Audio routing is available on the Audio tab") | `RoutingPage.xaml:25-35` | Same | `ShellApp.swift:519-527` | L | — | — |
| R2 | Empty state, "Assign inputs on the Sources tab…" | `RoutingPage.xaml:45-51` (bound to `HasRows`). This state is effectively unreachable, because `BuildVideoRoutingMatrix` always adds 3 synthetic rows (`StudioViewModel.cs:4617-4619`) | Different | Hint "Sources appear here as participants join…" when `routingRows.count <= 3` (`ShellApp.swift:565-568`) | L (Mac's version is the more useful one) | — | — |
| R3 | Column headers ISO 1-8, MULTIVIEW, AUX | `VideoRoutingMatrixViewModel.cs:80-92`, `RoutingPage.xaml:57-73` | Same | `ShellApp.swift:529-535` (AUX dimmed) | L | — | — |
| R4 | Row set: assigned inputs + Active Speaker, Screen Share, Media | `StudioViewModel.cs:4543-4554, 4614-4620` | Same | `AppModel.swift:2497-2513` | L | — | — |
| R5 | Row label "Input NN - name" | `StudioViewModel.cs:4548-4550` | Same | `AppModel.swift:2502` ("Input 01 · name") | L | — | — |
| R6 | ISO cell: click toggles; column-exclusive (one source per ISO) | `VideoRoutingMatrixViewModel.cs:102-124, 198-199`. Effect: re-sync plus status only, with no recording change (finding 0.3) | Different (Mac stronger) | `AppModel.swift:2519-2537`: also one column per source, and it **drives** `isoSelectedSourceIds`/`isoRecordingEnabled` and the slot `iso` flags | M (Mac does what the label promises) | — | shell |
| R7 | ISO cells on synthetic rows (Active Speaker / Screen Share / Media) | Clickable and stateful, with no effect (`VideoRoutingMatrixViewModel.cs:102-124`; handler ignores them `StudioViewModel.cs:4636-4641`) | Different | Disabled unless `zoom:`/`capture:` (`ShellApp.swift:547-549`) | L | — | — |
| R8 | MULTIVIEW cell toggles input "in show" and refreshes the multiview | `StudioViewModel.cs:4643-4647` | Same | `ShellApp.swift:555-558` → `AppModel.swift:2540-2546` → `toggleSlotInShow` `:1103-1115` | M | — | — |
| R9 | MULTIVIEW on synthetic rows | Clickable, local state only (handler returns, `:4636-4641`) | Same (in effect) | Disabled (`AppModel.swift:2506-2511` `multiviewToggleable:false`) | L | — | — |
| R10 | AUX column present but inert | Toggles cell state; no handler (`StudioViewModel.cs:4627-4648`) | Same | `ShellApp.swift:559-560` (disabled) | L | — | — |
| R11 | Per-cell tooltip (destination label) | `RoutingPage.xaml:102` | Missing | No `.help` in `CrosspointCell` (`ShellApp.swift:577-598`) | L | S | shell |
| R12 | Status feedback "X routed to / removed from ISO n / multiview" | `StudioViewModel.cs:4632, 4647` (`CommandStatus`) | Missing | `toggleIsoCell`/`toggleMultiviewCell` set no status (`AppModel.swift:2519-2546`) | L | S | shell |
| R13 | Matrix stays coherent with the real ISO selection: preserved across rebuilds, exclusivity normalised | `VideoRoutingMatrixViewModel.cs:130-157, 182-196` | Partial | `isoColumnAssignments` (`AppModel.swift:2488`) is not persisted and is never rebuilt from `isoSelectedSourceIds`. That selection is restored from prefs (`:563`) and rewritten by the Sources-tab ISO toggle (`:1074-1078, 1117-1122`), so after a relaunch or a Sources-tab change the matrix shows no ISO dots while ISOs are armed. The next matrix click then *replaces* the whole selection with the matrix's view (`:2531`). Columns for departed sources are not pruned. | M (a silent change to which cameras record) | S | shell |
| R14 | Matrix scrolls both ways at any size | `RoutingPage.xaml:53` (`ScrollViewer` H+V) | Partial | `TabPage` scrolls vertically only and caps width at 720 (`ShellApp.swift:1214, 1232`). The grid needs about 724 pt (170 + 8×48 + 72 + 48 + gaps + 28 padding, `ShellApp.swift:528-562`), so the last column can clip | L | S | shell |

Audio routing: both shells place it on the Audio tab (`RoutingPage.xaml:37-39`;
`M/ShellApp.swift:509-511`). `M/AudioRouteControlProjection.swift` serves that tab and is out of
scope here.

**Routing counts:** Same 7 · Partial 2 · Different 3 · Missing 2 (14 rows).

---

## 2. OVERLAYS / GRAPHICS (`W/Views/OverlaysPage.xaml(.cs)`, `W/ViewModels/OverlaysViewModel*.cs`, `W/Controls/Graphics*.cs`, `LowerThirdDesignerControl.cs`) vs `M/Overlays.swift`

### 2a. Graphics Operate | Design (the Windows main page). All rows are blocked by the graphics audit's G0, and Take-riding rows also by T.

| # | Feature / behaviour | Windows (file:line) | Mac | Mac evidence | Impact | Size | Where |
|---|---|---|---|---|---|---|---|
| O1 | Operate / Design segmented toggle (preview lease swaps between surfaces) | `OverlaysPage.xaml:33-40`; `.xaml.cs:46-82` | Missing | `OverlaysPane` has no modes (`Overlays.swift:167-180`) | M | S (after O4/O14) | shell + G0 |
| O2 | Graphics playout status line | `OverlaysPage.xaml:41-42` | Missing | none in `Overlays.swift` | L | S | shell + G0 |
| O3 | On Air strip: Program chips with Hide, cued chips, **Clear all** (Shift+F12), **Clear cued**, Armed-automation chip with Disarm | `Controls/GraphicsOnAirStripControl.cs:39-42, 93-123` | Missing | none (`grep '"graphics-'` in `M/` finds only the generated validators) | H | M | shell + G0 (graphics audit #2) |
| O4 | Playlist rows with per-state **Cue / Show / Hide / Clear cue** | `Controls/GraphicsPlayoutControl.cs:494-505` | Missing | none | H | L | shell + G0 + T (#2) |
| O5 | "+ Add graphic", default five rows, first-run onboarding | `GraphicsPlayoutControl.cs:65-70, 522-527` | Missing | none | M | S | shell + G0 |
| O6 | Row inspector: name, title line, typed variables, "From Sources" follow, **Hides after** (auto-out), Hotkey, Open in Design | `GraphicsPlayoutControl.cs:589-683` | Missing | none | H | M | shell + G0 |
| O7 | F-key hotkeys per row, Shift+F12 plays everything out | `Services/GraphicsHotkeyRouter.cs`; hint `GraphicsPlayoutControl.cs:676` | Missing | Mac key handling knows only space/return/B (`TransportKeys.swift:13, 83-87`) | H | S | shell + G0 |
| O8 | Guest rows bound to Sources names ("Edit names in Sources →") | `GraphicsPlayoutControl.cs:458, 603` | Missing | none | H | M | shell + G0 |
| O9 | Names list (CSV paste/import, hotkey steps through) | `GraphicsPlayoutControl.NamesList.cs:14-65` | Missing | none | M | S | shell + G0 |
| O10 | Countdown clocks: Start / Pause / Resume / Reset | `GraphicsPlayoutControl.cs:445-452` | Missing | none | M | S | shell + G0 (#2) |
| O11 | Shot Layouts: Cue and Take / Take / Cancel layout, Edit layouts dialog, Shift+F1-F11 | `GraphicsPlayoutControl.cs:222-394`; `.Layouts.cs` | Missing | none; the Mac Take swaps scene ids only (graphics audit T) | H | L | shell + G0 + T (#8) |
| O12 | Data publish (staged → published revision) | `GraphicsPlayoutControl.Data.cs:47` | Missing | none | M | M | shell + G0 (#6) |
| O13 | Social review (YouTube/Twitch) | `GraphicsPlayoutControl.cs:46, 138` | Missing | none | M | L | shell + G0 (#10) |
| O14 | Design → Builder (templates, data, remote feeds) | `Controls/GraphicsBuilderControl*.cs` | Missing | none | H | L | shell + G0 (#3, #6, #7) |
| O15 | Design → Designer (layers, inspector, snapping, motion, private preview) | `Controls/GraphicsDesignerControl*.cs` | Missing | none | M | XL | shell + G0 (#3) |
| O16 | Design → Workspace (starters, library, image import, cues & automation) | `Controls/GraphicsWorkspaceControl*.cs` | Missing | none | M | L | shell + G0 (#3) |
| O17 | "Existing overlay controls" drawer holding the legacy sections | `OverlaysPage.xaml:17, 55-58`; `.xaml.cs:84-94` | Different | The legacy sections are the whole Mac tab (`Overlays.swift:167-180`) | L | — | — |

### 2b. Legacy keyer, brand kit, DSK and captions (the Windows drawer; the Mac tab)

| # | Feature / behaviour | Windows (file:line) | Mac | Mac evidence | Impact | Size | Where |
|---|---|---|---|---|---|---|---|
| L1 | Keyer status: phase + source, "sync pending — media core has not confirmed", summary | `OverlaysPage.xaml:77-95`; `StudioViewModel.cs:1171-1179` | Partial | Dot plus "Key on air"/"Key off" and "N on air" (0/1) from `lowerThirdPhase` (`Overlays.swift:188-197`; `AppModel.swift:1001, 1218-1220`). There is no source name and no pending state, and it reads on-air even when the key is suppressed (finding 0.1) | M | S | shell |
| L2 | **Key in / Key out** button: keys the *Program source's* name; refuses "Lower third needs a program source" | `OverlaysPage.xaml:102-106`; `StudioViewModel.LegacyLowerThirdCommands.cs:7-45` (+ instances routing `ApplyStudioLowerThirdIntentAsync`) | Different | Keys a typed Name/Title as `sourceId:"mac:manual"` (`Overlays.swift:203-210, 235-240`; `AppModel.swift:2835-2848`). That id is likely dropped by the core guard `N/core/MediaCore.cpp:5142-5153` (UNVERIFIED on screen) | **H** | S | shell |
| L3 | Rebuild (build out then in; also works from hidden) | `OverlaysPage.xaml:107-111`; `LegacyLowerThirdCommands.cs:47-65` | Partial | Disabled unless on air (`Overlays.swift:241-247`); out-then-in (`AppModel.swift:2896-2903`) | L | S | shell |
| L4 | Explainer: "keyed name stays locked while that source remains on program…" | `OverlaysPage.xaml:114-121` | Partial | The text is present (`Overlays.swift:198-201`) but untrue for the Mac manual key, which never follows the source. Only the Auto key does (`AutomationExtras.swift:227-253`) | L | S | shell |
| L5 | Lower-third position (a read-only applied-look anchor; options lower-left/right, upper-left/right) | `OverlaysPage.xaml:128-131`; `Services/LowerThirdLayoutHelper.cs:10` | Different | Picker with Lower left / Lower right / **Center** (`Overlays.swift:211-217`). There is no upper position, and "center" is not a Windows option (the core's slide direction keys on `"right"`, `N/compositor/CompositorLayout.h:449`) | L | S | shell |
| L6 | Applied style readout (`AppliedLookLabel`) | `OverlaysPage.xaml:132-135` | Missing | none | L | S | shell |
| L7 | Build in / Build out ms, 50-2000 step 50, **applied live to an on-air key** | `OverlaysPage.xaml:143-157`; `StudioViewModel.cs:3492-3540` | Partial | Steppers 50-2000/50 (`Overlays.swift:221-222, 253-264`). They apply only on the next Show/Hide (`AppModel.swift:2845, 2873`), and the **Auto** key ignores them, hard-coding 250/200 (`AutomationExtras.swift:251, 265, 272`) | M | S | shell |
| L8 | Motion timing presets **Quick 220/160, Smooth 420/320, Gentle 700/550, Custom** | `StudioViewModel.cs:764-768, 3476-3490` | Different | **Snap 150/120, Standard 1000/500, Gentle 1600/900, Custom** (`Overlays.swift:45-67`), even though the comment says "matching the WinUI dropdown". Defaults differ too: Windows 220/160 (`StudioViewModel.cs:758-761`), Mac 1000/500 (`Overlays.swift:71-72`) | M (a 1 s build-in looks sluggish on air) | S | shell |
| L9 | Timing summary text | `OverlaysPage.xaml:168-171`; `StudioViewModel.cs:1212` | Same | `Overlays.swift:230-231` | L | — | — |
| L10 | #1166: a manual Hide suppresses Auto re-keying until a deliberate Show/Rebuild | `LegacyLowerThirdCommands.cs:17, 37, 60` | Missing (live defect, still present) | No suppression flag anywhere in `M/` (`grep -i suppress`: only an unrelated audio hit). `evaluateAutomationLowerThird` guards are `AutomationExtras.swift:240-241` (graphics audit 4a) | H | S | shell |
| L11 | Add graphic rows: Lower third / Bug / Image | `OverlaysPage.xaml:174-195`; `StudioViewModel.cs:5751-5795` | Missing | none (the Mac bug lives on the Media tab, M17) | L | M | shell |
| L12 | Graphics list: each row toggles on Program; "No graphics configured" | `OverlaysPage.xaml:197-247`; `StudioViewModel.cs:5722-5746` | Missing | none | L | M | shell |
| L13 | "Design lower thirds" look editor: starter/saved looks, anchor, font, name/title/bg/accent colours, source logo, compare, private preview, Apply look | `OverlaysPage.xaml:74-76`; `Controls/LowerThirdDesignerControl.cs:16-30` | Missing | none | M | M shell + M core | **core**: the core parses `lowerThirdAppearance` from `set-brand-kit` (`N/core/MediaCore.cpp:3483-3490`) and lays out the rect (`:5161-5168`), but `MetalCompositorAdapter.mm` never reads `appearance` (`grep` = 0), so the styled raster is D2D-only |
| L14 | Brand kit: kit name | `OverlaysPage.xaml:275-281` | Same | `Overlays.swift:270-275` | L | — | — |
| L15 | Brand kit: logo asset readout | `OverlaysPage.xaml:283-290` | Partial | Shows the Media-tab **bug** asset (`Overlays.swift:276-280`), which is a scene route (`AppModel.swift:1752-1764`) and not the brand-kit logo | L | S | shell |
| L16 | "Use selected media as logo" | `OverlaysPage.xaml:259-262`; `StudioViewModel.cs:1036-1056` | Different | No such button. The Mac uses Bug/Unbug per still on the Media tab (`ShellApp.swift:1845-1850`) | L | S | shell |
| L17 | Default overlay (all-off / manual) | `OverlaysPage.xaml:291-298`; `OverlaysViewModel.cs:67` | Same | `Overlays.swift:281-287`. It is a no-op on Mac because there are no graphics rows (`:133-141`), which is equivalent today | L | — | — |
| L18 | Brand colours Primary / Accent / Background: hex + swatch + picker | `OverlaysPage.xaml:301-386` | Partial | The UI matches (`Overlays.swift:288-317`). Behaviour differs in three ways: (a) the persisted kit is only sent on an edit, because the only callers are the binding setters `Overlays.swift:273-296`, so after a relaunch the core runs its defaults; (b) Metal reads only `brandBackgroundColor` and `brandColor` (`N/compositor/MetalCompositorAdapter.mm:1243-1244`), so **Accent** changes nothing visible; (c) the core snapshot `brandKit` is never read back | M | S shell, S core (Accent) | shell + core |
| L19 | Browser overlays (DSK): URL + preset + **Add overlay** | `OverlaysPage.xaml:399-414` | Missing (honest card) | "Unavailable on macOS…" (`Overlays.swift:321-328`) | M | L | **core**: `browser-add` exists (`N/core/MediaCore.cpp:1355-1360`) but the host is WebView2 (`BrowserSourceHostAdapter`), so a WKWebView host is needed |
| L20 | DSK row: **Reload** | `OverlaysPage.xaml:445-448` | Missing | as L19 | L | S (after L19) | core + shell |
| L21 | DSK row: **Save to scene** (USK-style) | `OverlaysPage.xaml:449-452` | Missing | as L19 | L | S (after L19) | core + shell |
| L22 | DSK row: **Preview** stage over PVW + **DSK in/out** on Program | `OverlaysPage.xaml:453-460`; `StudioViewModel.cs:14324` | Missing | as L19 | M | M (after L19) | core + shell |
| L23 | Caption cues transcript (speaker, role, text) + empty guidance | `OverlaysPage.xaml:469-506` | Missing | The Mac has an enable toggle and an amber "no transcription source" note instead (`Overlays.swift:332-346`), with no cue list. Also, the core defaults to `captionEnabled_ = true` (`N/core/MediaCore.h:1513`) while the Mac pref defaults to false and is never sent at launch, so the toggle can disagree with the core | L | S | shell |
| L24 | Remote control: `graphics.lowerThird.toggle/set`, `graphics.graphic.toggle`, `graphics.logo.import`, `browser.add/reload/remove`, all `graphics.*` playout | `native-shell/CoreVideoPro.Control/ControlActionRegistry.cs:171, 216, 318` | Missing | The Mac control surface registers only `transport.*` and `scene.*` (`ControlSurface.swift:80-119`) | M (Companion/Stream Deck lower-third buttons) | S (legacy LT) / L (graphics) | shell (+G0 for graphics.*) |

**Overlays counts:** Same 3 · Partial 6 · Different 5 · Missing 27 (41 rows: 17 in 2a and 24 in 2b).

---

## 3. MEDIA (`W/Views/MediaPage.xaml`, `StudioViewModel.cs` media commands, `Controls/MediaAssetPreviewHost`) vs `M/ShellApp.swift:1807-1857` `MediaPane`, `M/MediaBin.swift`

| # | Feature / behaviour | Windows (file:line) | Mac | Mac evidence | Impact | Size | Where |
|---|---|---|---|---|---|---|---|
| M1 | **Cue in Preview** (adds the selected asset as a media layer on the Preview scene; "Take to show it / start playback") | `MediaPage.xaml:37-41`; `StudioViewModel.cs:14557-14585` | Missing | No cue action. Media routes exist only for the logo bug (`AppModel.swift:1752-1764, 1791-1800`) | H | M | shell for stills (the core decodes stills on Mac: `MediaCore.h:1100`, ImageIO decoder) / **core** for clips (finding 0.2) |
| M2 | **Play / Pause** of the selected clip (one-shot `set-media-transport`, never restarts an on-air clip) | `MediaPage.xaml:42-46`; `StudioViewModel.cs:6326-6370, 6492-6501` | Missing | No `set-media-transport`/`set-media-playback` in `M/` (`grep` = 0) | H | L | **core**: no Mac `IMediaDecoder` (`ModuleComposition.cpp:440`; MF stub `MediaFoundationMediaFrameSourceAdapter.cpp:1312`), so `mediaTransports_` is null and the core refuses (`MediaCore.cpp:3555-3558`) |
| M3 | Playout status text ("X cued in Preview; Take to…", "No media asset playing") | `MediaPage.xaml:48-49, 92-97` | Missing | none | M | S | shell |
| M4 | Local preview player for the selected asset (image or video, mirrors play state) | `MediaPage.xaml:50-58`; `Controls/MediaAssetPreviewHost.xaml.cs:57-190` | Missing | none | M | M | shell (AVPlayer/NSImage) |
| M5 | Selected-asset readout ("on Program / cued / playing") | `MediaPage.xaml:59-64, 86-91`; `StudioViewModel.cs:5829-5834` | Missing | `selectedAssetId` is set on import (`AppModel.swift:2824`) and read nowhere else (`grep`: `:401, :2824` only) | M | S | shell |
| M6 | Import (multi-select, filtered to 11 supported extensions, status message) | `MediaPage.xaml:77-79`; `StudioViewModel.cs:5946-5982`; `Services/MediaBinService.cs:22-23` | Partial | `NSOpenPanel` multi-select with **no type filter** (`ShellApp.swift:1815-1823`). Unknown extensions are copied in and classed `lower-third` (`MediaBin.swift:46`). No status message | L | S | shell |
| M7 | Refresh | `MediaPage.xaml:80-82` | Same | `ShellApp.swift:1824`; `AppModel.swift:2817-2819` | L | — | — |
| M8 | Bin summary ("N assets") | `MediaPage.xaml:84-85`; `StudioViewModel.cs:7703` | Missing | The Mac shows the bin folder path instead (`ShellApp.swift:1826-1828`) | L | S | shell |
| M9 | Empty-bin guidance | `MediaPage.xaml:98-101` | Same | `ShellApp.swift:1829-1832` | L | — | — |
| M10 | Assets grouped under kind headers | `MediaPage.xaml:102-108`; `MediaBinService.cs:25-31` | Partial | Flat list in kind-folder order, kind shown per row (`ShellApp.swift:1833-1843`; `MediaBin.swift:93-106`) | L | S | shell |
| M11 | Row detail: duration + detail labels | `MediaPage.xaml:122-127` | Partial | Kind, plus W×H for images only (`ShellApp.swift:1836-1841`). No duration | L | S | shell |
| M12 | Per-row **Select** | `MediaPage.xaml:128-131`; `StudioViewModel.cs:6047-6074` | Missing | none | M | S | shell |
| M13 | Per-row **Play** (pause when tapped again; Program clips go through the transport ledger) | `MediaPage.xaml:132-136`; `StudioViewModel.cs:6326-6370` | Missing | none | H | S shell after M2 | core (as M2) |
| M14 | Per-row **Remove** → recoverable `.trash`, **refused while a scene or input uses it** | `MediaPage.xaml:137-142`; `StudioViewModel.cs:6083-6125` | Missing | No delete anywhere in `MediaBin.swift`. Assets can only be removed in Finder, which leaves the logo-bug and scene-background references dangling | M | S | shell |
| M15 | Colour grade readouts (LUT preset, exposure, contrast, saturation) | `MediaPage.xaml:154-194` | Different | No readout on the Media tab. The Mac has a full live editor in a separate "Color Grade" window (`ColorGrade.swift:14-45`; `ShellApp.swift:141-149`) with exposure/contrast/saturation/temperature. There is no LUT | L | — | — |
| M16 | Chapter markers card | `MediaPage.xaml:196-204` (`ChapterSummary => "No chapters"`, `StudioViewModel.cs:1241`, a static placeholder) | Missing | none | L | S | shell |
| M17 | Remote control `media.rundown.load/preview/take` | `ControlActionRegistry.cs:136` | Missing | `ControlSurface.swift:80-119` | L | M | shell + core (as M2) |

**Media counts:** Same 2 · Partial 3 · Different 1 · Missing 11 (17 rows).

---

## 4. Totals

| Screen | Same | Partial | Different | Missing | Rows |
|---|---|---|---|---|---|
| Routing | 7 | 2 | 3 | 2 | 14 |
| Overlays / Graphics | 3 | 6 | 5 | 27 | 41 |
| Media | 2 | 3 | 1 | 11 | 17 |

## 5. Top gaps by operator impact

### Routing (it has fewer than 10 real gaps, so all are listed)
1. **R13**: the ISO matrix drifts from the real ISO selection (prefs restore and the Sources-tab
   toggle), and one click then replaces the whole selection. M, S, shell.
2. **R12**: no status feedback after a crosspoint click. L, S, shell.
3. **R14**: the grid can clip at the 720 pt tab width. L, S, shell.
4. **R11**: no per-cell tooltip. L, S, shell.

(R6, ISO driving recording, favours the Mac. Port the Mac behaviour back to Windows.)

### Overlays / Graphics
1. **L2 / finding 0.1**: the Mac manual lower third is likely dropped by the core's rendered-source
   guard, so it reads "on air" while nothing is keyed, and it keys typed text rather than the
   Program source. H, S, shell. Verify on screen first.
2. **O3 + O4**: no On Air strip and no Cue/Show/Hide playlist. H, L, shell + G0/T.
3. **L10**: a manual Hide does not stop Auto re-keying (#1166). Live defect. H, S, shell.
4. **O11**: no Shot Layouts or guarded Take. H, L, shell + G0 + T.
5. **O7**: no graphics F-keys (Shift+F12 play-out-all). H, S, shell + G0.
6. **O6 + O8**: no row inspector, no Sources-bound guest names, no auto-out. H, M, shell + G0.
7. **O14**: no Builder (templates, data, feeds). H, L, shell + G0.
8. **L19-L22**: no browser DSK. M, L, core (macOS browser host).
9. **L7 + L8**: timing presets and defaults differ (1 s build-in), timing is not applied live, and
   the Auto key ignores timing. M, S, shell.
10. **L13**: no lower-third look designer, and Metal never draws `lowerThirdAppearance`. M,
    M + M, core + shell.

### Media
1. **M2 / M13**: clips (stingers, audio beds) cannot play. There is no AVFoundation media decoder
   and `set-media-transport` is refused. H, L, core.
2. **M1**: no Cue in Preview. Stills could ship today as shell-only work. H, M.
3. **M5 + M12**: no asset selection or selected-asset readout. M, S, shell.
4. **M14**: no Remove and no in-use guard; deleting in Finder leaves bug/background references
   dangling. M, S, shell.
5. **M4**: no local preview player. M, M, shell.
6. **M3**: no playout status line. M, S, shell.
7. **M6**: import has no type filter, and unknown files are classed as lower-third. L, S, shell.
8. **M10 + M11**: no kind groups and no durations. L, S, shell.
9. **M17**: no `media.rundown.*` remote actions. L, M, shell + core.
10. **M8 / M16**: no bin count, and no chapters card (a placeholder on Windows). L, S, shell.

### Overall top 10 (all three screens)
1. L2 / 0.1: Mac manual lower third likely suppressed by the core (verify, then S shell fix).
2. M2/M13: no media clip playout on Mac (core: AVFoundation `IMediaDecoder`, L).
3. O3+O4: graphics On Air strip and Cue/Show/Hide playlist missing (G0 + L shell).
4. L10: manual Hide is overridden by Auto (#1166), S shell.
5. M1: Cue media to Preview missing (stills shell-only, M).
6. O11: Shot Layouts + Take via `begin-take-transition` missing (G0 + T, L).
7. O7: graphics F-key hotkeys missing (S once O4 exists).
8. L19-L22: browser DSK unavailable (core WKWebView host, L).
9. R13: ISO matrix and recording selection drift (S shell).
10. L7/L8: lower-third timing presets and defaults differ, and Auto ignores operator timing (S shell).

## 6. Things the Mac has that Windows does not

- **The ISO matrix drives recording** (`M/AppModel.swift:2519-2537`). On Windows the matrix ISO
  column is cosmetic (finding 0.3), which should be filed as a Windows defect. The Mac also
  enforces one ISO column per source.
- **Inert cells are disabled** (synthetic rows, AUX: `ShellApp.swift:547-549, 559-560`), so they
  do not look routable.
- **Typed manual lower third** (Name/Title fields, `Overlays.swift:203-210`). Windows always keys
  the Program source's name. Note that this design is what collides with the core guard in 0.1.
- **"Center" lower-third position** (`Overlays.swift:215`). Windows has upper-left/right instead.
- **Captions enable toggle** with an honest "no transcription source" warning
  (`Overlays.swift:332-346`). Windows shows only the transcript list.
- **Logo bug straight from the Media bin** (Bug/Unbug per still, `ShellApp.swift:1845-1850`; it
  persists, `AppModel.swift:495, 574-576`). Windows goes through a Bug graphic row and "Use
  selected media as logo".
- **Media bin folder path** shown on the tab (`ShellApp.swift:1826-1828`).
- **Brand-kit sends are coalesced** at 200 ms (`Overlays.swift:91-98`), so a colour drag cannot
  flood the queue.
- **Colour grade as a dedicated live window** with Temperature and a neutral/active indicator
  (`ColorGrade.swift:14-45`). Windows has its own editor window too; the Mac simply has no
  read-only readout on the Media tab.

## 7. Incidental findings (outside these three screens, found while tracing)

- **The SuperSource background picker can never list an asset.** It filters
  `kind == "image" || kind == "background"` (`M/ShellApp.swift:1002-1004`), but the Mac classifier
  only produces `stinger` / `lower-third` / `audio-bed` / `slate` (`M/MediaBin.swift:27, 36-47`),
  so the menu is always empty. Fix: S, shell.
- **Windows: the routing empty state cannot be reached** (R2). **Windows: the Media page shows
  "Selected asset" and "Playout status" twice** (`MediaPage.xaml:48/92-97, 59-64/86-91`).
- The Mac comment "Motion timing presets, matching the WinUI dropdown" (`Overlays.swift:45-46`) is
  false (L8).
