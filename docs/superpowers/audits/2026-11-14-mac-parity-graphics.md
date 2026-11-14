# macOS parity audit — Graphics (2026-11-14)

Scope: the Graphics area on `main` @ `00838940` (Release 1.0.0), every graphics /
lower-third / Tiles-starter commit since the 2026-10-09 audit
(`git log --since=2026-10-09 origin/main -- native-shell native mac-shell`, ~150
graphics-relevant commits of 603). Method follows `docs/mac-parity-plan.md` §1-§3.
OHG ignored. "Do not re-litigate": §2 (core gated on macOS CI), §3 (OHG), §5.3
(Tiles colour pickers are N/A-until-Tiles). Read-only audit; nothing was built or run.

## 0. The finding that changes every row below

**The native graphics renderer does not exist on Metal.** The template/instance
*state machine* is portable C++ and built on every platform, but the core refuses
every `graphics-*` command on a Metal core:

- `native/src/core/MediaCore.cpp:1256-1259`: any `type` starting with `graphics-`
  is refused with `graphics-instance-backend-not-built` unless
  `modules_.compositor->supportsGraphicsInstances()`.
- `native/src/modules/Interfaces.h:1480`: default `supportsGraphicsInstances()`
  returns `false`. Only `native/src/modules/D3D11CompositorAdapter.cpp:268` overrides
  it to `true`. `MetalCompositorAdapter.mm` (class at `:240`) has no override.
- `native/src/core/CoreStateCapture.cpp:253`: the `graphics-instances` capability is
  advertised from that same flag, so a Metal core says it is unavailable. The
  Windows gateway checks that capability first
  (`GraphicsCommandGateway.cs:19`).
- The graphics raster itself, `native/src/compositor/CompositorOverlayRaster.cpp:6`,
  compiles only under `COREVIDEO_WITH_D3D11` (it is DirectWrite/D2D). That file
  draws prepared text, shapes, outlines and shadows, tables
  (`acbf8590`) and crawls (`4affc3db`).
- The docs agree. `docs/reference/volt-native-instances.md:335` says *"Metal does
  not announce this instance capability yet."* `docs/reference/graphics-playout.md`
  (Where things live) says motion is *"drawn by the Windows D3D11 compositor"*.
  `volt-graphics-spec.md:654` says *"Metal claims require their own measured
  hardware trials."*
- Also D3D11-only: the NDI graphics key (`D3DGraphicsKeyTap.h`, `a9f43745`/`221252da`).

What does reach Mac: `GraphicsInstances.cpp`, templates, curated looks, motion
sampling, crawl timing (`GraphicsCrawl.h`), clocks, automation and
`GuardedLayoutTake.cpp` are in the shared `corevideo_native` source list
(`native/CMakeLists.txt:125-134`), and their tests run on macOS CI. The snapshot
`graphics` node is published on every core (`CoreStateCapture.cpp:484,804`). So
the core's *logic* is SHARED, but on Mac it cannot render, and it cannot accept a
command either.

**Consequence:** every native-graphics feature below is gated by a core work item
(G0, "Metal graphics backend") before any Swift UI can be accepted, so I list it
as its own row. The prompt's four classes don't cover this case. I mark those rows
**MAC-UI-ONLY / SHELL-LOGIC + G0**, meaning the shell work is real and the core
half is also missing on Metal.

Second structural gap: **the Mac Take never sends `begin-take-transition`.**
`AppModel.swift:1776-1784` swaps scene ids and resends both scene graphs. On
Windows, the cued-graphics Take promotion runs from `beginTakeTransition` →
`GraphicsInstances::promoteCuedForTake()` (graphics-playout.md "One truth for
Preview and Program"). The guarded Shot Layout Take also rides that command
(`MediaCoreCommandBuilder.cs:142-155` `graphicsLayout`; parsed at
`native/src/core/GuardedLayoutTake.cpp:38,78`). So even with G0 done, Cue→Take
cannot work on Mac until the Mac Take uses the core transition command (item T).

Third: **the Mac shell has no HTTP/OSC control surface** (no listener in
`mac-shell/Sources`), so none of the `graphics.*` / `graphics.layout.*` control
actions or Companion/Stream Deck hooks have a Mac analogue yet. This is outside
the Graphics area and only noted here.

## 1. Summary table

| # | Feature | Class | Effort | Deps | Mac evidence |
|---|---|---|---|---|---|
| G0 | Metal graphics backend (`supportsGraphicsInstances`, CoreText raster of prepared text/shapes/images/tables/crawls, motion atlas) | CORE GAP (native, Metal) | XL | — | `MetalCompositorAdapter.mm` has no override; `Interfaces.h:1480` returns false |
| T | Take via `begin-take-transition` (cued graphics ride Take) | SHELL-LOGIC | M | — | `AppModel.swift:1776-1784` swaps ids only; no `begin-take-transition` in mac-shell |
| 1 | Native graphics instances & async preparation (#926): command gateway + `graphics` snapshot decode | SHELL-LOGIC (+G0) | M | G0 | zero `"graphics-` strings in mac-shell; only generated contract validators (`Lifecycle.generated.swift:1757-1830`, used only by `ShellTests.swift:294-332`) |
| 2 | Graphics **Operate**: playlist rows, On Air strip, Cue/Show/Hide, auto-out, names list, clocks, F-key router | SHELL-LOGIC (+G0) | L | 1, T | absent; Mac has a single legacy LT card (`Overlays.swift:134-230`) |
| 3 | Graphics **Designer / Design**: designs (schema 3), template editor, layers/inspector, motion, image import, starters, private preview | SHELL-LOGIC (+G0) | XL | 1 | absent |
| 4a | Lower-third authority #1166, **legacy path**: manual Hide must survive Auto | SHELL-LOGIC (**live defect on Mac**) | S | — | `AppModel.swift:2279-2304` Hide does not set any suppression; `AutomationExtras.swift:227-251` re-keys |
| 4b | Lower-third authority #1166, **instances path**: route Hide/toggle to the active owner | SHELL-LOGIC | S (inside 2) | 2 | N/A until 2 exists (Mac has one authority) |
| 5 | Crawls (local data, timing, pause/resume/restart, boundary adoption) | SHELL-LOGIC (+G0) | M | 2, 3, 6 | absent |
| 6 | Local data tables / keyed CSV & JSON datasets, staged vs published revisions | SHELL-LOGIC (+G0) | M | 2, 3 | absent |
| 7 | Remote feeds: HTTPS polling, WSS push, custom API mapping + bearer credentials | SHELL-LOGIC (+G0) | L | 6 | absent; credential store must move from DPAPI to Keychain (pattern: `StreamKeychain.swift`) |
| 8 | Shot Layouts + data-aware Shot Layouts + guarded Take | SHELL-LOGIC (+G0) | L | 2, 6, T | absent |
| 9 | Staging / publication at safe boundaries | SHARED (core) + SHELL-LOGIC | M (inside 5-8) | 5-8 | core in `GraphicsInstances.cpp`/`GraphicsCrawl.h` (shared); shell revision model absent |
| 10 | Social review (YouTube/Twitch) — adjacent, found in the commit range | SHELL-LOGIC (+G0) | L | 1, 2 | absent |
| 11 | Tiles scene starter (#1199) | MAC-UI-ONLY | S on top of a Tiles surface (L) | Tiles wall on Mac | no `"tiles"` key in any mac-shell `load-scene-graph`; no `DynamicGallery` |
| 12 | Tiles colour pickers (bg/border/glow) | N/A-until-Tiles (§5.3, unchanged) | S once Tiles exists | 11 | plus a Metal gap: no `hasFillColor` branch (`MetalCompositorAdapter.mm:820-840`) |
| 13 | Legacy overlays / brand kit / build timing (`set-overlay-asset`) | SHARED-DONE | — | — | `Overlays.swift:8-14,100-130`; `AppModel.swift:2265-2330` |
| 14 | NDI graphics key output | CORE GAP (D3D11-only tap) | M | G0 | not in mac-shell; `D3DGraphicsKeyTap.h` |

**Where the logic lives:** about 9.0k dense lines of graphics logic are in
**plain-net9.0 `CoreVideoPro.MediaCore`** (`Services/Graphics*.cs`,
`Models/Graphics*.cs`, `ShotLayoutRowSettings.cs`, `StudioLowerThirdController.cs`,
the social feeds). About 9.2k are in **WinUI** (`Controls/Graphics*.cs`,
`ViewModels/StudioViewModel.Graphics*.cs`, the lower-third partials), and about
4.3k are portable native. The MediaCore half is UI-free and tested, and per memory
those tests run under `~/.dotnet` on this Mac. It is not callable from Swift, so it
still has to be re-implemented in Swift (classed SHELL-LOGIC). Its tests and
`contracts/graphics.fixtures.json` are the parity oracle. Hosting MediaCore as a
.NET sidecar is a design alternative, not evaluated here.

## 2. Per-feature detail

### G0. Metal graphics backend (core prerequisite)

- **Windows:** `D3D11CompositorAdapter` advertises instances and consumes
  `GraphicsPreparedResource` (`native/src/graphics/GraphicsPreparedResource.h:13`
  already has `Backend { D3D11, Metal, Test }`). It rasterizes in
  `CompositorOverlayRaster.cpp` (D2D/DirectWrite: text outline/shadow `0f51412f`,
  ellipses/lines `3597a7b4`, fit/shrink `e06600ff`, tables `acbf8590`, crawls
  `4affc3db`) and draws the per-layer motion atlas runs (`6d6d45c1`, `4c08c23f`).
- **Mac:** Metal has a Core Text/ImageIO raster for the *legacy* overlay only
  (`MetalCompositorAdapter.mm:56-221`). It has one last-signature overlay texture
  cache, which `docs/reference/volt-graphics-reconciliation.md:25` calls a static
  risk for generalized graphics.
- **Work:** override `supportsGraphicsInstances`, port the prepared-resource raster
  to Core Text/CoreGraphics, then do atlas/motion draws, multi-entry byte
  admission, crawl text, and table primitives. Gate G22 in
  `volt-graphics-implementation-plan.md:148` asks for named Metal trials at every
  advertised resolution and rate. **XL**, native-only. This is a core-team item,
  not Swift work.

### T. Take through `begin-take-transition`

- **Windows:** `TransportCoordinator` sends `begin-take-transition`
  (`MediaCoreCommandBuilder.cs:~130-155`: `operationId, revision, mode, durationMs,
  direction, dipColor`, optional `graphicsLayout{coreEpoch, sceneId,
  previewRevision, keepProgramScene, playOutOthers, cues[{instanceId,
  cueRevision}]}`). The core promotes `rideTake` cues there.
- **Mac:** `AppModel.swift:1776` says *"There is no `take` wire command"*. That is
  stale for graphics: without this command, cued graphics never ride a Mac Take.
- **Class:** SHELL-LOGIC. **M**. Prerequisite for 2 and 8. It may also interact
  with the RC native FTB/transition work (`87bebbd7`). UNVERIFIED whether the Mac
  already uses transitions elsewhere; grep found no `begin-take-transition` in
  `mac-shell/Sources`.

### 1. Native graphics instances & async preparation (#926, `7e75dce2`)

- **Windows:** `GraphicsCommandGateway` (MediaCore,
  `Services/GraphicsCommandGateway.cs`, 349 lines) is the single command boundary.
  It builds `{type: "<action with . → ->", graphics: {schemaVersion:1, requestId,
  clientId, coreEpoch, instanceId?, expectedRevision?, …}}` (`:263-267`) inside a
  `media-core-sync` batch. Actions and their allowed fields are at `:79-91`:
  `graphics-instance-create|update|cue|trigger|remove`, `graphics-program-clear`,
  `graphics-template-install|list|remove`, `graphics-clock-control`,
  `graphics-automation-configure|control`, `graphics-authority-set`. The gateway
  correlates the reply by `requestId`+`clientId`+`coreEpoch` (`:62-68`), refuses
  concurrent sends (`graphics-command-overloaded`), and expands package files.
  Results are native admission, not delivery. On-air state comes only from
  `NativeGraphicsState.Instances` in the snapshot (`Models/NativeGraphicsState.cs`).
- **Portable:** the gateway, validation, and state model are MediaCore. The core
  state machine (`GraphicsInstances.cpp`, `PreparationCache.h`) is shared.
- **Mac:** no `graphics-*` sender and no `graphics` snapshot decode. The only
  graphics code is the generated contract validators from `7e75dce2`
  (`Lifecycle.generated.swift:1757-1830`), exercised only by fixtures in
  `ShellTests.swift:294-332`.
- **Class:** SHELL-LOGIC (+G0). **M**. Port the gateway rules and fixtures first.

### 2. Graphics Operate surface (#1076 stages A-E, slice 2, clocks, names list)

- **Windows behaviour** (docs/reference/graphics-playout.md §"The operator model",
  §"Show vs Cue", §"Names list", §"Clocks"):
  - playlist rows with F-keys and auto-out;
  - an On Air strip built only from core instance state;
  - Cue / Show / Hide / Clear cued / Clear all;
  - lazily created instances `playlist-<rowId>[-<sha256(sourceId)[:12]>]`;
  - Guest rows bound to Sources text; a names list (one row, many people);
  - countdown clocks (`graphics-clock-control` with instance and clock revision
    >= 1, `5a091f45`);
  - an Operate | Design toggle.
- **Logic:** `GraphicsPlayoutService*.cs`, `GraphicsPlaylist.cs`,
  `GraphicsRowFields.cs`, `GraphicsRowTextCommits.cs` and
  `GraphicsWorkspaceLibrary.Playlist.cs` are MediaCore. `GraphicsHotkeyRouter.cs`
  is pure but lives in WinUI. The views are WinUI
  (`GraphicsPlayoutControl*.cs`, `GraphicsOnAirStripControl.cs`).
- **Mac:** one legacy lower-third card with name/title/position/Show
  (`Overlays.swift:134-230`). There is no playlist, strip or clock.
- **Class:** SHELL-LOGIC (+G0). **L**. Deps: 1, T.
- **Rulings to honour (quoted):**
  - *"Vocabulary (owner, 2026-10-31): Cue, Show, Hide"* (graphics-playout.md:42).
  - *"Never add a Show or toggle on the Graphics tab without its Hide / Remove in
    the same place while the graphic is live (owner, 2026-10-31)."* (:704)
  - *"The strip derives from the core's instance state only. A chip that is not
    there is a bug in the mapping, not a reason to add local state."*
  - *"Do not flip lower-third authority to instances before an accepted Show."*
  - *"Instance capacity is eight per bus … a 10-guest roster never pre-creates."*
  - Show/Cue/Take must first commit text still being typed (`69fa6f70`,
    `fb0e19e3`: *"a failed row save never blocks Show"*).
  - Clock: *"Start never shows the graphic and Reset never …"* (:599).

### 3. Graphics Designer / Design

- **Windows:**
  - Designs are stored in the show document (schema 3, `00cd8608`), with
    create/save/duplicate/delete and install-on-demand (`a852f820`). Built-ins are
    never edited; a design is copied to `design-<slug>-<6 hex>` (graphics-playout.md:251-276).
  - Layers, canvas, inspector, snapping, group/ungroup (`6e922a08`, `1ffa6432`,
    `64aa646f`).
  - Per-layer motion with Out mirroring In and Play preview at 30 fps on the core
    clock (`b3661c5f`, `cab95b71`).
  - PNG/JPEG/SVG import into the private draft (`0bd204db`), shapes and text
    effects (`43c242ef`), and blank seeds shared with native (`8fa1eb04`).
  - Starters and the "Use for Zoom lower thirds" action (`8831fa9e`).
- **Logic:** most decisions are MediaCore: `GraphicsDesignService`,
  `GraphicsDesignWorkflow`, `GraphicsDesignerModel`, `GraphicsTemplateEditor.*`,
  `GraphicsMotionModel`, `GraphicsSnap`, `GraphicsSvgRaster`,
  `GraphicsImageImport`, `GraphicsPackageReader/Writer`. Private preview rendering
  is native (`GraphicsInstances::authoringPreview`, `GraphicsInstances.cpp:99`,
  using `GraphicsTemplateDraft`). UI is WinUI `GraphicsDesignerControl*.cs`,
  `GraphicsBuilderControl*.cs`, and `GraphicsWorkspaceControl*.cs`.
- **Mac:** absent.
- **Class:** SHELL-LOGIC (+G0). **XL**, the largest single item. SVG rasterization
  needs a Mac equivalent (UNVERIFIED which library `GraphicsSvgRaster` uses).

### 4. Lower-thirds / overlays authority (#1166) — manual Hide vs Auto

- **Windows** (`1fb785f5`, `c4b7e3c0`, `docs/reference/lower-third-authority-1166.md`):
  - `ApplyStudioLowerThirdIntentAsync` (`StudioViewModel.StudioLowerThird.cs:51-72`)
    routes Show/Hide/toggle to the authority currently rendering Program. That is
    the playlist via `GraphicsPlayout.ApplyLowerThirdIntentAsync`, which flips
    `graphics-authority-set {family:"lower-third", authority:"instances"}` only
    after an accepted Show. Otherwise it is the legacy key.
  - Every successful Hide sets `_programLowerThirdAutomationSuppressed = true`
    (`StudioViewModel.LegacyLowerThirdCommands.cs:17`, `StudioLowerThird.cs:58,69`).
    Only a deliberate Show or Rebuild clears it (`:37,60`).
  - `ShouldEnableProgramLowerThird` (`StudioViewModel.cs:13854-13862`) disables
    the Auto arm while suppressed.
  - `LegacyLowerThirdVisibilityPolicy.HasEnabledGraphic` (WinUI,
    `Services/LegacyLowerThirdVisibilityPolicy.cs`) excludes the
    `automation-lower-third` row while suppressed, so restarting Auto cannot
    resurrect it.
- **Logic:** all of it is WinUI (pure statics, but not in MediaCore).
- **Mac (verified, defect):** `hideLowerThird()` (`AppModel.swift:2279-2304`) sends
  build-out and then the hidden ack. It records no suppression and does not clear
  `autoLowerThirdSourceId`. `evaluateAutomationLowerThird()`
  (`AutomationExtras.swift:227-251`) runs on the 0.5 s auto tick. Its guards (`:240-241`)
  return only while the *same* source stays on Program. As traced:
  1. After a manual Hide of the Auto key, the next Program-source change re-keys
     the lower third.
  2. Toggling Auto off and on (retire `:256-276` clears `autoLowerThirdSourceId`;
     then `lowerThirdPhase == "hidden"` passes `:240`) re-keys it immediately.

  That is exactly the #1166 failure: *"restarting Auto re-enabled its
  `automation-lower-third` graphic row"*. Minor, also traced: `hideLowerThird()`
  rewrites the key's `sourceId` to `mac:manual` and its text to the manual
  `lowerThirdName` during the build-out of an Auto-owned key, so the outgoing
  slide can show the wrong name. Neither was run (UNVERIFIED on screen).
- **Class:**
  - 4a (legacy): SHELL-LOGIC, **S**. Add an `automationSuppressed` flag that
    successful Hide sets and Show/Rebuild clears; gate
    `evaluateAutomationLowerThird` on it; keep it across an Auto restart.
  - 4b (instances routing): built inside item 2.
- **Rulings to honour (quoted from the 1166 spec / commits):**
  - *"Hide and toggle must target the authority that currently owns Program,
    including a legacy key when playlist rows exist."*
  - *"A manual Hide suppresses later Auto lower-third enablement until a
    deliberate Show/rebuild action resets that intent."*
  - *"Explicit successful legacy Hide records automation suppression even when
    already hidden."*
  - Acceptance: *"legacy graphic keys out after accepted Hide; toggle keys out
    rather than adopting/showing another row; Auto does not restore it"*. Live
    proof: hidden *"for 10 seconds under Auto and another 10 seconds after Auto
    restart"*. Commit title: *"preserve manual Hide when Auto restarts"*.
  - *"A successful no-op is valid only when the owning authority already has no
    visible lower third."*
  - Carry-over (2026-07-11), which the Mac already honours at
    `AutomationExtras.swift:166-176`: un-forced refresh, so there is no "goes up
    and down".

### 5. Crawls

- **Windows:**
  - Design → Data crawl… picks a CSV/JSON text column. Limits: up to 64 nonempty
    single-line items within the 4 KiB field limit; speed, gap, direction, mode.
  - Operate offers Cue/Show/Take/Hide and pause/resume/restart.
  - Native deterministic timing is in `GraphicsCrawl.h` (`b65fb2a2`).
  - Revisions publish at safe boundaries (`1547dfa7`, touching
    `GraphicsInstances.cpp`, `GraphicsCommandGateway.cs` and
    `ControlActionRegistry.cs`).
  - Shell code: `GraphicsCrawlData.cs`, `GraphicsDataTable.cs`,
    `GraphicsPlayoutService.Data.cs` (MediaCore), and
    `GraphicsBuilderControl.Data.cs` (WinUI).
- **Mac:** absent.
- **Class:** SHELL-LOGIC (+G0; the crawl raster is in the D3D-only file). **M**.
  Deps: 2, 3, 6.
- **Rulings** (graphics-playout.md:135-147):
  - *"A Cue pins its data and runs on its own clock. Program starts from the edge
    on Take."*
  - *"Hide is available throughout, including after a single pass finishes."*
  - *"A playing loop adopts prepared data at its next boundary. A paused loop holds
    its current picture … A single pass keeps its original list through Restart."*
  - *"Failed preparation leaves the accepted Program list playing."*

### 6. Local data tables / CSV / JSON datasets

- **Windows:**
  - `GraphicsLocalData.cs` (`a65e43f3`) parses bounded keyed CSV/JSON.
  - `GraphicsDataTable.cs` handles up to 12 rows × 8 columns, ranking, and rows
    capped at 4096 UTF-8 bytes.
  - Staged datasets persist with show revisions (`e1586081`,
    `GraphicsDocumentStore.cs`, `GraphicsWorkspaceLibrary.Data.cs`).
  - Explicit publication (`96a6080a`).
  - The table renders as native primitives (`acbf8590`, `GraphicsTemplate.cpp`
    shared; raster D3D-only).
- **Portable:** all data logic is MediaCore.
- **Mac:** absent.
- **Class:** SHELL-LOGIC (+G0). **M**.
- **Rulings** (graphics-playout.md:795-806):
  - *"Publish data saves that revision for the next Cue or Show and asks an on-air
    instance of the same design version to update in place. A cue already on
    Preview keeps the revision it captured. If preparation fails, Program keeps
    the last accepted pixels."*
  - *"Loading a file has no effect on Program by itself."*

### 7. Remote graphics feeds

- **Windows** (`0c408137` … `3f37f426`):
  - `GraphicsFeedPoller` (deterministic polling: 15 s default, 5 s floor).
  - `GraphicsFeedTransport` (bounded HTTPS; Sheets CSV / RSS 2.0 / flat JSON;
    redirects, URL credentials, query strings and fragments refused; public port 443).
  - `GraphicsPushTransport` + `GraphicsPushFeedCoordinator` (WSS snapshots
    `{schemaVersion:1, sequence, rows}`; max 4 push feeds; 30 s stale, 60 s idle
    retry; reconnect safely).
  - `GraphicsFeedMapping` (JSON Pointer column mapping) and
    `GraphicsFeedSnapshot` (persisted last sequence).
  - Credentials: validation in MediaCore (`GraphicsFeedCredentials.cs`). Storage is
    WinUI `GraphicsFeedCredentialStore.cs:22`, which uses
    `DpapiSecretProtector.Protect`. The document stores only a reference.
  - UI: `GraphicsBuilderControl.Feeds.cs` and `GraphicsFeedStatusText.cs`.
  - No new core wire: feeds publish through the existing instance update/cue path
    (`bf155b77`).
- **Mac:** absent. The storage analogue is Keychain (`StreamKeychain.swift`
  already exists).
- **Class:** SHELL-LOGIC (+G0 for the on-air result). **L**. Dep: 6.
- **Rulings:**
  - *"New feeds start disabled."*
  - *"Feed polling starts off."*
  - *"Publish data is manual by default."*
  - *"Editing the endpoint clears the old binding."*
  - *"An identical replay confirms freshness, older data is ignored, and a
    sequence with conflicting data is rejected."*
  - *"An invalid or unavailable feed retains the last accepted data and Program
    content."*
  - *"A cued revision stays pinned through Take."*

### 8. Shot Layouts, data-aware Shot Layouts, guarded Take

- **Windows:**
  - `ShotLayout {Id, Name, SceneId, RowIds[], Hotkey, PlayOutOthers}` on the
    graphics document, with four default presets (`e2bb296c`).
  - Typed values and pinned data revisions (`7cccf90b`,
    `GraphicsLayoutResolver.cs`, `ShotLayoutRowSettings.cs`).
  - Prepare and cancel guarded cues (`8adb6145`).
  - Scene and graphics guarded on Take (`16efa2c6`): it touches
    `TransportCoordinator.Layouts.cs` and `ITransportHost` (WinUI) plus
    `MediaCoreCommandBuilder` `graphicsLayout`.
  - The core validates the prepared scene and all cue revisions together
    (`GuardedLayoutTake.cpp`) and publishes `layoutTakeReceipts`
    (`CoreStateCapture.cpp:486`) for **Check Take**.
- **Portable:** `GraphicsPlayoutService.Layouts.cs` and the resolver are MediaCore.
  The guard is shared native. Transport integration is WinUI.
- **Mac:** absent. It also needs T.
- **Class:** SHELL-LOGIC (+G0). **L**. Deps: 2, 6, T.
- **Rulings** (graphics-playout.md:513-560):
  - *"A layout whose scene is no longer in the show, or that names a playlist row
    that no longer exists, is refused whole with the reason, never silently
    skipped."*
  - *"A pin that has aged out or a field that has changed type blocks Cue."*
  - *"A data refresh after Cue leaves the captured values intact."*
  - *"A failed check holds Program."*
  - *"A refused attempt needs a fresh Cue."*
  - *"A cold cut holds the outgoing picture and graphics while the incoming media
    warms."*

### 9. Graphics staging / publication at safe boundaries

The core half (boundary adoption, keep the previous Program revision until the
new cue is ready, prepared-resource admission) is in shared
`GraphicsInstances.cpp`/`PreparationCache.h` and needs G0 to be observable on Mac.
The shell half is the loaded/selected/Program/pending revision model in
`GraphicsPlayoutService.Data.cs` / `GraphicsWorkspaceLibrary.Data.cs`. It is
implemented inside items 5-8, not separately.

Ruling (graphics-playout.md "Rules to keep"): *"Apply a saved row's pinned version
and motion to the cue copy before Show. Keep the previous Program revision until
the new cue is ready."*

### 10. Social review (adjacent; in the commit range)

- `GraphicsSocialReview`, `GraphicsSocialPlayout`, `YouTubeSocialFeed`, and
  `TwitchSocialFeed` are MediaCore. `GraphicsSocialReviewDialog` is WinUI.
- The gateway refuses social graphics outside the review client
  (`GraphicsCommandGateway.cs:43-45`).
- Rulings:
  - *"Ordinary Take, Shot Layouts, playlist hotkeys and generic graphics commands
    cannot put a social message on air."*
  - The key is stored *"with Windows data protection"*, which on Mac means
    Keychain.
- SHELL-LOGIC (+G0), **L**. Out of the requested list. Included so it is not
  missed.

### 11. Tiles scene starter (#1199, `9e28def1`)

- **Windows:** `StarterSceneFactory.cs:17,27-33` (WinUI). The `"tiles"` template
  produces scene "CoreVideo Tiles", layout `dynamic-gallery`,
  `DynamicGallerySettings`, and **empty routes** (*"Tiles membership is composed
  by the core, not duplicate shell routes"*). It is created in Preview only and
  persisted.
- **Core:** `load-scene-graph` `tiles` object (`MediaCore.cpp:1765-1830,2266`) is
  shared and on Mac.
- **Mac:** there is no Tiles wall at all. `"tiles"` occurs only as the multiview
  tiles field (`AppModel.swift:820,1015`), and there is no `DynamicGallery`. The
  member list and style are built by WinUI `TilesLayerPayloadBuilder.cs` (125
  lines) and `StudioViewModel.Tiles.cs` (243), which must be ported. Rule from
  CLAUDE.md: *"routes and the wall share ONE order namespace … serializes an EMPTY
  route list for a DynamicGallery scene"*, and the wall stays present when
  `members: []`.
- **Class:** MAC-UI-ONLY. Starter **S**, but it depends on a Mac Tiles surface
  (**L**, outside the Graphics area).

### 12. Tiles colour pickers

The 2026-10-09 ruling stands: N/A-until-Tiles (§5.3). New evidence for whoever
builds Tiles on Mac:

- Border and glow (`borderColor`, `glowColor`, … `MediaCore.cpp:1812-1819`) do
  render on Metal (`applyTilesDecoration`, `MetalCompositorAdapter.mm:973`).
- **The background colour does not:** Metal has no `hasFillColor` branch
  (`MetalCompositorAdapter.mm:820-840`, *"KNOWN GAP, DELIBERATELY NAMED"*). A
  background picker would be a no-op on Mac until that core fix lands.

### 13. Legacy overlays / brand kit — SHARED-DONE

Mac speaks `set-overlay-asset` for the lower third, brand kit, captions and build
timing (`Overlays.swift:8-14,100-130`; `AppModel.swift:2265-2330`), with
two-step retire matching WinUI. Nothing to do apart from 4a.

### 14. NDI graphics key

The graphics-only key composite and the `ndi-graphics` sender are D3D11-only
(`D3DGraphicsKeyTap.h`; `6f77187c`, `221252da`). It is a CORE GAP behind G0, and
its shell toggle is trivial once the core supports it.

## 3. Suggested order

1. 4a now (S, real defect, no dependencies).
2. G0, the core Metal backend. This is the true critical path; nothing visible
   ships without it.
3. T and 1, in parallel with G0 (both can be tested against the shared state
   machine and fixtures).
4. 2, then 6, 5, 8, 7.
5. 3 (Designer) last, or descope it for Mac 1.x: Operate with the curated looks and
   starters covers the live-show path.
6. Tiles (11/12) is a separate decision.

## 4. Unverified

- On-screen Mac behaviour of 4a. It was traced from code only; no build or run.
- Whether `hideLowerThird`'s build-out on a hidden key during Auto retire causes a
  visible flash.
- Which SVG library `GraphicsSvgRaster` depends on.
- Whether any Mac code path already uses `begin-take-transition` for non-graphics
  transitions (grep says no).
- Effort figures are estimates against dense C# line counts, not measured.
