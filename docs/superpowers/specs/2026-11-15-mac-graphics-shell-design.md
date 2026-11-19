# Graphics in the Mac shell: Operate, data, Shot Layouts, feeds and the Designer

Status: DRAFT design for owner review, 2026-11-15.

Covers roadmap milestone **M4** (M4.1-M4.7) and the shell half of consolidated gap row **2.2**
(`docs/superpowers/audits/2026-11-15-mac-ui-parity-consolidated.md`). Row-level sources:
`docs/superpowers/audits/2026-11-14-mac-parity-graphics.md` (rows 1-9, T, 4a/4b) and the Overlays
rows O1-O17 and L1-L24 of `2026-11-15-mac-ui-parity-routing-overlays-media.md`. It also takes
roadmap M6.4 (the shared Keychain helper), because remote feeds are its first new consumer.

It depends on the separate **Metal graphics backend** spec (roadmap M3) for every pixel and for
the core accepting `graphics-*` commands on a Mac. Section 5.9 lists exactly what this spec needs
from it. Path conventions below: `W/` = `native-shell/CoreVideoPro.WinUI/`, `MC/` =
`native-shell/CoreVideoPro.MediaCore/`, `N/` = `native/src/`, `M/` =
`mac-shell/Sources/CoreVideoProShell/`.

## 1. Why

On Windows the Graphics tab is a playout product: a playlist of rows with Cue / Show / Hide, an On
Air strip that shows what the core is really drawing, F-keys, clocks, a names list, data tables and
crawls fed from files or live feeds, Shot Layouts that put a scene and its graphics on air with one
guarded Take, and a designer for the looks. On a Mac the same tab is one card that keys the Program
source's name as a legacy lower third, plus a brand kit and a captions toggle
(`M/Overlays.swift:160-180`). A Mac operator cannot put a title, a bug, a sponsor card, a
countdown, a table or a crawl on air at all, cannot cue a graphic to ride a Take, and cannot open a
show whose graphics were built on Windows. Every one of the 17 Operate/Design rows in the Overlays
audit is Missing on Mac. The owner ranks this fourth among the large features, after media
playback, the Sources rebuild and remote control.

## 2. Windows behaviour to match

### 2.1 Where the logic lives

The graphics logic that is not UI is 8,882 lines in plain `net9.0` `MC/` (65 files: `Services/Graphics*.cs`,
`Models/Graphics*.cs`, `Models/NativeGraphicsState.cs`, `Models/ShotLayoutRowSettings.cs`,
`Services/StudioLowerThirdController.cs`, two social feeds). Its only package dependency is
`Svg.Skia` (`MC/CoreVideoPro.MediaCore.csproj:9`). The WinUI side is 8,741 lines of controls and
view-model partials. The tests are 42 files and about 394 test methods in
`native-shell/CoreVideoPro.MediaCore.Tests/Graphics*.cs`, plus 10 small WinUI test files.

### 2.2 Command gateway and state (#926)

- One boundary: `GraphicsCommandGateway.InvokeAsync(action, requestJson)`
  (`MC/Services/GraphicsCommandGateway.cs:28`). It refuses with
  `graphics-native-state-unavailable` unless the core is running, advertises the
  `graphics-instances` capability (`:19`) and has published a `graphics` node with
  `schemaVersion == 1` and a non-empty `coreEpoch` (`:39-41`).
- The request is checked by `GraphicsCommandRequest.TryBuild` (`:102` onward): a fixed action table
  with the allowed fields per action (`:78-91`), a 32 KiB size cap (64 KiB for automation, 2 MiB
  for a template install), JSON depth 4, `coreEpoch` must equal the observed epoch
  (`core-epoch-conflict`), unknown or duplicate fields are refused, and style / animation / clock /
  automation bodies are validated. The comment at `:74-75` states the rule: native validation
  stays authoritative; this is a bounded pre-check, not a second renderer.
- The wire command is `{type: <action with "." replaced by "-">, graphics: <the request body>}`
  (`:269-273`), sent as one command of a `media-core-sync` batch.
- One command in flight; a second is refused `graphics-command-overloaded` (`:61`). The result is
  read from `graphics.operations[]` by `requestId` + `clientId`, with `graphics.lastResult` as the
  fallback, and must carry the same `coreEpoch`, else `graphics-result-not-correlated` (`:65-67`).
- A template install by `packagePath` is expanded to `manifest` + `packageRoot` outside the
  in-flight gate, so an import cannot block a Hide (`:47-60`).
- Social graphics are refused unless they come through the review client (`:44-46`).
- An accepted result is admission only. On-air truth is `NativeGraphicsState.Instances`
  (`MC/Models/NativeGraphicsState.cs:8-26,109-168`): `visibility`, `programRevision`,
  `cueRevision`, `cuePreparation`, `preparation`, `cued`, `showPending`, `currentlyComposed`,
  `warning`, `cueWarning`, `clock`, `crawlPlayback`.
- Proof: `GraphicsCommandGatewayTests.cs` (21 tests, e.g.
  `NativeRefusalAndUncorrelatedResponsesCannotBecomeSuccess`,
  `BusyGatewayRefusesNewIntentAndRecoversAfterTheFirstAdmission`,
  `InvalidOrIgnoredLookingControlsNeverReachTheCore`), `NativeGraphicsStateTests.cs`.

### 2.3 Operate (#1076)

- `GraphicsPlayoutService` (`MC/Services/GraphicsPlayoutService.cs`, client id `graphics-playout`
  `:41`). Instances are created lazily and named `playlist-<rowId>[-<suffix>]`
  (`InstanceIdFor`, `:93`). On Program means `programRevision > 0` and visibility is neither
  `hidden` nor `building-out`; on Preview means `cueRevision > 0` (`:96-97`).
- Show is create-or-update, cue, then `graphics-instance-trigger {target:"cue", action:"show"}`
  with the cue's revision (`:300-310`). Hide is `trigger {action:"hide"}` (`:448-454`); Clear cue is
  `trigger {action:"uncue"}` (`:457-462`); Clear all and Clear cued are `:317` and `:325`.
- The strip (`GraphicsStripState`, `:19-31`) is derived only from core instances.
- Rows (`MC/Models/GraphicsPlaylist.cs:12-75`): kinds guest / title / logo / liveBug / sponsor;
  hotkey `F1`-`F12`; auto-out choices 0, 8000 or 15000 ms; up to 32 fields; up to 64 names-list
  variants; optional data binding, placement, motion and clock.
- Guest rows expand to one entry per bound roster slot, names-list rows to one per variant; a bare
  hotkey steps to the next variant (`GraphicsPlayoutService.cs:101-135`).
- Clocks: `graphics-clock-control` with `expectedRevision = max(1, instance.revision)`
  (`:403,:411-415`).
- F-keys: `W/Services/GraphicsHotkeyRouter.cs:17-34`. F1-F12 play in the row with that hotkey,
  Shift+F12 plays everything out, Shift+F1-F11 cue a Shot Layout, and nothing routes while a text
  input has focus. Proof: `GraphicsHotkeyRouterTests.cs`.
- The core caps instances at eight (`N/graphics/GraphicsInstances.cpp:522`, `instance-capacity`),
  which is why rows never pre-create instances.
- Operator rulings, quoted by the graphics audit from `docs/reference/graphics-playout.md` (the
  quotes were not re-read line by line for this spec): vocabulary is Cue / Show / Hide; never a
  Show without its Hide in the same place; the strip derives from core instance state only; text
  still being typed is committed before Show, and a failed row save never blocks Show; clock Start
  never shows the graphic.
- Proof: `GraphicsPlayoutServiceTests.cs` (1,077 lines), `GraphicsRowFieldsTests.cs`,
  `GraphicsRowTextCommitsTests.cs`, `GraphicsNamesListColumnsTests.cs`.

### 2.4 Lower-third authority (#1166)

`ApplyStudioLowerThirdIntentAsync` (`W/ViewModels/StudioViewModel.StudioLowerThird.cs:49-72`)
routes Show / Hide / toggle to whichever authority owns Program. With a Guest row present, a Show
goes to the playlist and adopts `graphics-authority-set {family:"lower-third",
authority:"instances"}` only after the Show was accepted
(`GraphicsPlayoutService.ApplyLowerThirdIntentAsync`, `:146-166`); a Hide goes to the playlist only
when the authority is already instances, otherwise to the legacy key. Every accepted Hide sets the
automation-suppressed flag (`:58,:69`). Spec: `docs/reference/lower-third-authority-1166.md`. Proof:
`LegacyLowerThirdVisibilityPolicyTests.cs`, `StudioLowerThirdAssignmentTests.cs`.

### 2.5 Persistence

One graphics document per show, separate from every other setting: `GraphicsShowDocument`, schema
3, 2 MiB limit, atomic and version-guarded, with a read-only mode and backup recovery
(`MC/Services/GraphicsDocumentStore.cs:36-70,78,117`). It holds playlist rows, Shot Layouts,
designs, saved templates, data sources, feeds, source columns, automation rules and the authority.
Windows keeps it at `%LOCALAPPDATA%\CoreVideoPro\graphics\default.json`
(`W/ViewModels/StudioViewModel.GraphicsCommands.cs:32-34`). Nothing about air state is persisted.
Proof: `GraphicsDocumentStoreTests.cs`, `GraphicsDataPersistenceTests.cs`,
`GraphicsWorkspaceLibraryTests.cs`.

### 2.6 Local data, crawls, feeds

- `GraphicsLocalData` (`MC/Services/GraphicsLocalData.cs:10-16`): CSV or JSON, at most 1 MiB,
  1,000 rows, 32 columns; 12 x 8 visible. `GraphicsDataTable` builds the instance fields
  (`table-row-N`, `data-revision`) and the table template manifest (`GraphicsDataTable.cs:31-58`).
- Crawls: up to 64 items; speed 1-1200, gap 0-512, direction, loop or single
  (`MC/Services/GraphicsCrawlData.cs:9-23`). Timing and boundary adoption are in the shared core
  (`N/graphics/GraphicsCrawl.h`, `GraphicsInstances::advanceCrawls`).
- Staging and publication: `StageDataAsync`, `PublishDataAsync`, `ControlCrawlAsync`,
  `SetDataLiveFollowAsync`, `StageFeedAsync`, `StagePushSnapshotAsync`
  (`MC/Services/GraphicsPlayoutService.Data.cs:17-116`).
- Feeds: `GraphicsFeedPoller.cs`, `GraphicsFeedTransport.cs` (HTTPS only, public addresses only:
  `ResolvePublicAsync` `:132`, `ValidateEndpoint` `:78`), `GraphicsPushTransport.cs` and
  `GraphicsPushFeedCoordinator.cs` (WSS, at most four push feeds, `GraphicsFeedPoller.cs:9`),
  `GraphicsFeedMapping.cs` (JSON Pointer columns), `GraphicsFeedSnapshot.cs` (sequence rules).
  Credentials: bearer validation in `MC/Services/GraphicsFeedCredentials.cs`; storage in
  `W/Services/GraphicsFeedCredentialStore.cs:14-30` (DPAPI file keyed by a random reference, bound
  to show id, feed id, format and endpoint). The document stores only the reference.
- Proof: `GraphicsLocalDataTests`, `GraphicsFeedPollerTests`, `GraphicsFeedTransportTests`,
  `GraphicsFeedSnapshotTests`, `GraphicsCustomFeedTests`, `GraphicsPushTransportTests`,
  `GraphicsPushTransportTlsTests`, `GraphicsPushFeedCoordinatorTests`,
  `GraphicsFeedCredentialStoreTests` (WinUI).

### 2.7 Shot Layouts and the guarded Take

- `ShotLayout {Id, Name, SceneId, RowIds (<=16), RowSettings, Hotkey Shift+F1..F11, PlayOutOthers}`
  (`MC/Models/GraphicsPlaylist.cs:119-144`). Preparation, cancel and confirmation:
  `MC/Services/GraphicsPlayoutService.Layouts.cs:5-60`; preflight:
  `MC/Services/GraphicsLayoutResolver.cs`.
- The Take carries `graphicsLayout {coreEpoch, sceneId, previewRevision, keepProgramScene,
  playOutOthers, cues[{instanceId, cueRevision}]}` on `begin-take-transition`
  (`MC/Services/MediaCoreCommandBuilder.cs:131-155`).
- The core validates the whole batch (`N/core/GuardedLayoutTake.cpp:31-123`): the transition must
  be first, exactly one transition and one `load-scene-graph`, no `graphics-*` command in the
  batch, 1-8 cues, and epoch, preview revision and every cue revision must still match. A cold
  media cut is held as `warming` and applied when the shell replays the same `operationId`
  (`:52-60,:139-160`), and expires after 5 s. Every outcome is a row in
  `graphics.layoutTakeReceipts` (`operationId`, `disposition` = warming / ready / committed /
  refused, `reason`; `:184-190`, `N/core/CoreStateCapture.cpp:485-486`).
- The shell waits on the receipt and rolls back on refusal:
  `W/ViewModels/Transport/TransportCoordinator.Layouts.cs:15-68`.
- Proof: the layout tests in `GraphicsPlayoutServiceTests.cs`, `TransportCoordinatorTests.cs`, and
  the native guarded-layout tests (file name UNVERIFIED).

### 2.8 Designer and Builder

`GraphicsWorkspaceSession` (557 lines: private draft, undo, gestures, preview samples, compare),
`GraphicsDesignService` (design lifecycle: create blank / from a look, open, save as next version,
duplicate, delete, reinstall after a core restart, `:52-179`), `GraphicsTemplateEditor.*` (1,083
lines of manifest edits), `GraphicsDesignerModel`, `GraphicsMotionModel`, `GraphicsSnap`,
`GraphicsImageImport`, `GraphicsSvgRaster` (Svg.Skia), `GraphicsPackageReader/Writer`,
`GraphicsBuilder`, `GraphicsShowStarters`. The private preview is rendered by the core
(`GraphicsInstances::authoringPreview`, `N/graphics/GraphicsInstances.h:44`) and its node geometry
is published as `graphics.authoring.layout` (`NativeGraphicsState.cs:231-267`).

## 3. Mac today

**Shell.**
- `M/Overlays.swift:160-180` is the whole Graphics tab: live keyer, brand kit, a "browser overlays
  unavailable" card and a captions toggle.
- The manual lower third now keys the Program source (`M/ProgramLowerThird.swift:16-59`,
  `AppModel.swift:3841,3869,3911`) and reads its on-air state from the core's `overlayState`
  (`AppModel.swift:1709`). The consolidated list's Tier 0.6 defect is therefore already fixed in
  this worktree.
- Take already sends `begin-take-transition` first in the same batch as the scene graphs
  (`AppModel.swift:2999-3046`), with no `graphicsLayout` field. The graphics audit's item T is done.
- A control surface exists (`M/ControlHttpServer.swift`, `ControlSurface.swift`) with 13 actions,
  all `scene.*` or `transport.*`. Windows registers 44 `graphics.*` action ids
  (`native-shell/CoreVideoPro.Control/ControlActionRegistry.cs`).
- Graphics code: only the generated contract validators, exercised by
  `M/ShellTests.swift:323-377`. No `"graphics-` command string, no decode of the `graphics`
  snapshot node, no document store, no F-key handling (`M/TransportKeys.swift` handles Space,
  Return and B only).
- Keychain code is duplicated in `M/StreamKeychain.swift` and `M/ZoomOAuth.swift`.
- The snapshot is held as an untyped `JSONObject` (`AppModel.swift:476`); the profile's capability
  list is read only by Diagnostics (`M/Diagnostics.swift:961-962`).

**Shared core on macOS.** Built and tested on every platform: the instance state machine,
templates and registry, curated looks, motion, clocks, crawl timing, automation and the guarded
layout Take (`native/CMakeLists.txt:125-135`). The `graphics` snapshot node is published on every
core (`N/core/CoreStateCapture.cpp:484-487,804`).

**Windows-only today.** The core refuses every `graphics-*` command with
`graphics-instance-backend-not-built` unless the compositor supports instances
(`N/core/MediaCore.cpp:1271-1274`); the default is false (`N/modules/Interfaces.h:1488`) and only
D3D11 overrides it (`N/modules/D3D11CompositorAdapter.cpp:268`). The capability is advertised from
the same flag (`CoreStateCapture.cpp:253`). The raster is D2D/DirectWrite. So today a Mac core
publishes an empty, truthful `graphics` node and accepts nothing.

**Contract fixtures.** `contracts/graphics.fixtures.json` has 41 rows covering four identity and
evidence contracts only (operation identity, revision identity, preparation fact, composition
fact). It does not cover gateway rules, playout decisions, data parsing or documents. The roadmap's
phrase "the contract fixtures are the acceptance oracle" is therefore true only for those four
types; for everything else the oracle is the C# tests, which Swift cannot run. Section 5.2 closes
that gap.

## 4. Port, move to the core, or fixture: the decision per piece

Three options were weighed for each piece: re-implement in Swift; move the logic into the shared
C++ core so both shells call one implementation; or host the .NET library on the Mac as a sidecar
process. The sidecar is rejected for the whole workstream: it adds a fourth process and a .NET
runtime to a signed and notarized package, needs a new RPC facade around services that are driven
by in-process callbacks, and contradicts the settled direction (a Swift shell, and a Swift-native
control server chosen over reusing the .NET one).

Moving logic into the core is right only where the core already owns the truth, because any move
also rewires the shipped Windows 1.0 path. The ruling per piece:

| Piece (Windows type) | Lines | Ruling | Conformance |
|---|---|---|---|
| Request pre-check (`GraphicsCommandRequest`) | ~270 | Swift, kept thin. The core already re-validates everything. | `gateway` vectors: request in, wire command or refusal reason out |
| Gateway send / correlate (`GraphicsCommandGateway`) | ~80 | Swift | Swift tests against a scripted core, one per C# gateway test |
| Snapshot model (`NativeGraphicsState`) | 267 | Swift `Decodable`, lossless for unknown fields | `state` vectors: real snapshot nodes in, projected strip out |
| Strip, instance ids, entry expansion, next variant | ~150 | Swift | `playout` vectors (the instance id suffix is a hash and must match byte for byte) |
| Playout sequencing (`GraphicsPlayoutService`) | 567 | Swift | Scripted-core transcripts: intent in, exact ordered command list out |
| Row and layout validation (`GraphicsPlaylistRow.IsValid`, `ShotLayout.IsValid`) | 144 | Swift | `document` vectors: accept / reject |
| Document store (schema 3) | 236 | Swift | `document` vectors: a Windows-written file loads and re-saves to equal JSON; oversize, newer-schema and corrupt cases |
| Local data parse, table and crawl fields + manifests | ~340 | Swift now. **Best later candidate for the core**: the manifest must hash identically on both platforms. | `data` vectors: file text in; rows, fields and manifest JSON out |
| Feed mapping, snapshot sequence rules, poll schedule | ~300 | Swift | `feeds` vectors; a fake clock for the schedule |
| Feed transports (HTTPS, WSS) | ~415 | Swift on `URLSession`; platform code, not portable logic | Local TLS test server, same cases as the C# transport tests |
| Credentials | 35 + 105 | Swift on Keychain | Swift tests; reference binding rules from `GraphicsFeedCredentialStoreTests` |
| Layout resolver and prepared-layout checks | ~235 | Swift. The guard itself is already core. | `layouts` vectors + scripted receipts |
| Lower-third authority routing | ~90 (WinUI) | Swift | Table of (authority, guest row, visible) to target |
| F-key router | 35 (WinUI) | Swift | Port of `GraphicsHotkeyRouterTests` as a table |
| Design lifecycle, template editor, motion, snap | ~2,000 | Swift if the Designer is built as specified. **Second candidate for the core** (see section 9, decision 3). | `designer` vectors: manifest + edit in, manifest out |
| SVG raster | 72 | Mac-native rasterizer; pixel-different from Skia by nature | Bounded-size and refusal cases only |
| Social review | ~600 | Out of this spec (roadmap M6) | - |

Nothing moves into the core inside this workstream. The two candidates are recorded so the choice
is made deliberately later, not by accident.

## 5. Design

### 5.1 Components (all new Swift files in `M/`, names proposed)

- `GraphicsState.swift`: typed decode of the `graphics` snapshot node.
- `GraphicsGateway.swift`: the one sender of `graphics-*` commands.
- `GraphicsDocument.swift`: schema-3 models, validation and the atomic store.
- `GraphicsPlayout.swift`: the port of `GraphicsPlayoutService` (strip, rows, Show / Cue / Hide,
  auto-out, clocks, names list, lower-third intent), later extended for data and layouts.
- `GraphicsData.swift`, `GraphicsFeeds.swift`, `GraphicsLayouts.swift`: sections 5.6-5.8.
- `GraphicsOperatePane.swift`, `GraphicsOnAirStrip.swift`, `GraphicsDesignPane.swift`: views.
- `KeychainStore.swift`: one generic-password helper replacing the two copies.

`AppModel` gains one `graphics` sub-model object; no graphics behaviour is added as methods on
`AppModel` itself (the Windows strangler rule, applied to the Mac god object).

### 5.2 Conformance fixtures (new)

A new fixture family, `contracts/graphics-shell/<area>.vectors.json`, where area is one of
gateway, state, playout, document, data, feeds, layouts, designer. Each row is
`{id, operation, input, expected}`.

- A new exporter in `native-shell/CoreVideoPro.MediaCore.Tests` (proposed name
  `GraphicsShellVectorTests`) computes `expected` by calling the real MediaCore types, and fails
  when the committed file differs from what it computes. Windows therefore cannot change a rule
  without the vectors changing in the same commit.
- The Mac shell's test runner (`COREVIDEO_SHELL_TESTS`) replays every row against the Swift
  implementation.
- Sequenced behaviour (Show, layout prepare, publish) uses transcripts: a scripted core returns
  recorded `graphics` nodes, and the vector's `expected` is the exact ordered list of wire
  commands plus the final result reason.
- Each slice adds its own area file together with its consumer. There is no fixtures-only slice.
- The MediaCore tests run on this Mac under `~/.dotnet`, so the exporter is runnable locally.

### 5.3 Gateway and snapshot

- Availability: graphics is available when `profile.capabilities` contains `graphics-instances`
  and the `graphics` node has `schemaVersion == 1` and a non-empty `coreEpoch`. Otherwise every
  intent returns `graphics-native-state-unavailable` and nothing is sent.
- Wire: existing command types only, exactly as Windows: `graphics-instance-create`, `-update`,
  `-cue`, `-trigger`, `-remove`, `graphics-program-clear`, `graphics-template-install`, `-list`,
  `-remove`, `graphics-clock-control`, `graphics-automation-configure`, `-control`,
  `graphics-authority-set`. Each is `{type, graphics:{schemaVersion:1, requestId, clientId,
  coreEpoch, ...}}` inside `media-core-sync`. No new wire command is proposed anywhere in this spec.
- One command in flight, same refusal reason as Windows. Client ids are the Windows ones
  (`graphics-playout`, `graphics-designs`), so a core log reads the same on both platforms.
- Decode: the graphics node is decoded off the main actor and only when its bytes changed since
  the last snapshot. Template thumbnails (base64 PNG) are decoded lazily, per card.
- Core generation change: when `coreEpoch` changes, all local in-flight state (prepared layout,
  auto-out timers) is dropped, the templates the playlist pins are reinstalled
  (`EnsureInstalledForPlaylistAsync`), and automation configuration is re-sent. This is the
  "one-shot command must be re-applied on every core generation" rule from `CLAUDE.md`.

### 5.4 Operate surface

The Graphics tab becomes **Operate | Design** when graphics is available. Layout top to bottom:
status line, On Air strip, Shot Layout buttons, playlist.

- On Air strip: Program chips each with Hide; cued chips each with Clear cue; Clear all; Clear
  cued; an Armed chip with Disarm when native automation is armed. Built from core instances only.
- Playlist row states and their buttons: idle (Cue, Show), cued (Show, Clear cue), on Program
  (Hide), preparing (disabled, with the core's preparation text), failed (the core's `warning`).
  A Show never appears without its Hide in the same row.
- Row inspector: name, title, typed variables from the template's `fields`, "From Sources",
  Hides after (Off / 8 s / 15 s), Hotkey, Open in Design.
- Guest rows read names from the Sources page. Until the Sources rebuild ships a per-source
  lower-third text field, the name is the roster or device name and the title is empty.
- Text being typed is committed before Cue, Show or Take; a failed save does not block Show.
- Keys: F1-F12, Shift+F12 and Shift+F1-F11 are added to `M/TransportKeys.swift` through a pure
  router. They do not fire while a text field has focus. macOS reserves some F-keys for system
  functions unless "Use F1, F2, etc. keys as standard function keys" is on; the pane shows a
  one-line hint when a hotkey is assigned (see Risks).
- Cued graphics ride an ordinary Take with no shell work: the Mac already sends
  `begin-take-transition`, and the core promotes ride-Take cues there
  (`GraphicsInstances::promoteCuedForTake`, `N/graphics/GraphicsInstances.h:37-39`). This is
  proven by a test, not assumed.
- Degraded states the operator sees: "Graphics need a newer media core" (capability absent);
  "Graphics busy, try again" (overloaded); the core's refusal reason verbatim for a refused
  command; "Media core restarted: graphics cleared" on an epoch change.

### 5.5 Lower-third authority and the legacy pane

- One function, `applyLowerThirdIntent(visible:)`, is the only entry for the footer button, the
  legacy Key in / Key out button and the control actions `graphics.lowerThird.toggle` / `.set`. It
  follows the Windows routing in section 2.4.
- The suppression flag is added even though Auto is retired on Mac (`sceneAutomationAvailable` is
  false), because the routing rule needs it to be correct the day Auto returns. Roadmap section 2.4
  records that the live defect is moot today; this spec does not claim to fix a visible bug.
- Migration of today's pane: when graphics is available, the live keyer, brand kit, browser card
  and captions move unchanged into an "Existing overlay controls" drawer opened from Design, as on
  Windows (`W/Views/OverlaysPage.xaml:17,55-58`). When graphics is not available (an older core),
  the tab shows today's pane as it is. Nothing is deleted.
- The first slice also fixes the small legacy differences the Overlays audit lists, because the
  drawer would otherwise carry them forward: timing presets and defaults (L8), timing applied live
  to an on-air key (L7), the brand kit re-sent at launch and on every core generation (L18a), and
  the position list (L5). The presets are still Snap 150/120, Standard 1000/500, Gentle 1600/900
  (`M/Overlays.swift:47-67`) against Windows' Quick 220/160, Smooth 420/320, Gentle 700/550. The
  Mac-only typed Title field (`M/Overlays.swift:212`) stays in the drawer.

### 5.6 Documents, data and crawls

- Path: `~/Library/Application Support/CoreVideoPro/graphics/default.json`, with the same
  `COREVIDEO_GRAPHICS_DOCUMENT_PATH` override as Windows. Same schema 3 JSON, same 2 MiB limit,
  atomic replace, one backup, read-only when the file is from a newer schema.
- Unknown properties are preserved on save, so a document written by a newer Windows build is not
  stripped by a Mac that opened it.
- Import: "Import graphics from another computer" takes a Windows `default.json` plus its graphics
  asset folder. Saved templates carry their definition inline (`GraphicsSavedTemplate`,
  `GraphicsDocumentStore.cs:21`) but also a `PackageRoot` path. How image assets are laid out under
  the Windows asset root, and whether paths are relative, is UNVERIFIED and is the subject of the
  spike in slice G2.
- Data: Design > Data loads a CSV or JSON file, shows the parsed table, and creates a table design
  through `graphics-template-install` with the generated manifest. Staged and published revisions
  follow `GraphicsPlayoutService.Data.cs`: loading a file changes nothing on Program; Publish saves
  the revision for the next Cue or Show and asks an on-air instance of the same design version to
  update in place; a cue already on Preview keeps the revision it captured.
- Crawls: Design > Data crawl picks a text column; Operate adds Pause / Resume / Restart. Boundary
  adoption is the core's; the shell shows `crawlPlayback` and `crawlPendingRevision`.

### 5.7 Shot Layouts and the guarded Take

- Buttons above the playlist: Cue and Take, Take, Cancel layout, Edit layouts, and Check Take.
- Prepare: resolve rows (refuse whole if a scene or row is missing), select the scene to Preview,
  cue each row with the layout's attempt id as `cueOwner`, wait for every cue to be ready.
- Take: `AppModel.take()` gains an optional guard argument. The batch is the existing Take batch
  with `graphicsLayout` added to `begin-take-transition`; no `graphics-*` command may be in it.
- The shell then reads `graphics.layoutTakeReceipts` for its `operationId`. On `warming` it replays
  the same batch (same `operationId`) until `committed` or `refused`. On `refused` it rolls the
  scene ids back with the existing Take rollback and shows the reason. A failed check holds
  Program; a refused attempt needs a fresh Cue.

### 5.8 Remote feeds and credentials

- `KeychainStore`: generic-password items under one service name per purpose; get, set, delete;
  no access group (the shell is not sandboxed). `StreamKeychain` and `ZoomOAuth` move onto it
  without changing their item names, so existing saved secrets keep working.
- Feed credential: service `CoreVideoPro.graphics-feed`, account = the random reference stored in
  the document. The secret is JSON carrying show id, feed id, format, endpoint and token, and is
  returned only when all four still match (the Windows binding rule). Editing the endpoint clears
  the binding.
- Polling: one poller per enabled feed, driven by a main-actor tick with an injected clock (15 s
  default, 5 s floor). Fetch and parse run off the main actor. Push: at most four WSS readers, each
  with one replaceable snapshot slot; 30 s stale, 60 s idle retry.
- Transport rules to hold: HTTPS / WSS only, port 443, no redirects, no credentials or fragments in
  the URL, bounded body, and **public addresses only**. The last rule is the hard one on Apple
  frameworks (see Risks).
- New feeds start disabled, polling starts off, publish is manual by default. An invalid or
  unavailable feed keeps the last accepted data and Program content.

### 5.9 What this spec needs from the Metal graphics backend spec

1. `supportsGraphicsInstances()` returns true on the Metal adapter, so `MediaCore.cpp:1271-1274`
   admits commands and `profile.capabilities` contains `graphics-instances`.
2. Program and Preview both composite instances (cues draw on Preview only), with in/out motion.
3. Preparation is asynchronous and truthful: `preparation` and `cuePreparation` reach their ready
   state or a failure with `warning` / `cueWarning`, and the `graphics-preparing` / `-failed`
   warnings are emitted. Shot Layout prepare waits on exactly these fields.
4. All four seed families and the curated looks render: text with fit and shrink, shapes, images
   (logo, bug, sponsor), clocks. Needed for slice G2.
5. Table primitives (needed for G5) and crawl text (needed for G6).
6. `graphics-template-list {includeThumbnails:true}` returns PNG thumbnails on Metal. Needed for
   the look picker in G8.
7. The private authoring preview (`authoringPreview`) rendered on Metal and delivered to the shell
   by some Mac surface, plus `graphics.authoring.layout`. Needed for G8 and every Designer slice.
   How Windows delivers that preview to its panel was not traced for this spec (UNVERIFIED).
8. A Metal readback test hook that a graphics pixel proof can call, and a graphics cue step in
   `scripts/mac-show-drill.py`.
9. Stated limits: instance capacity and resource budget on Metal equal to D3D11, or published.
10. Optional: Metal drawing `lowerThirdAppearance` for the legacy key (Overlays row L13).

Slices G1-G4 need items 1-4 and 8. Nothing in this spec needs a change to shared C++ logic.

### 5.10 Threading and performance

The shell is never on the render path. Constraints: one graphics command in flight; no polling
loop faster than the snapshot cadence; file parsing, feed fetches and document writes off the main
actor; SwiftUI lists diffed by stable ids (row id, instance id) and never rebuilt per snapshot. A
Show is two or three round trips (create or update, cue, trigger); the time from key press to the
trigger being sent is measured in the drill and must not regress the Take path.

## 6. Slices

| Id | What lands | Lane | Size | Depends on | Proven by |
|---|---|---|---|---|---|
| G1 | Snapshot decode, gateway, capability gate, Operate / Design tab frame, On Air strip with Hide / Clear all / Clear cued, legacy pane moved to the drawer with the L5/L7/L8/L18a fixes, `KeychainStore` with the two existing users moved onto it. `gateway` and `state` vectors. | shell | M | Metal items 1-3 for the live check; none for unit tests | Vectors; scripted-core tests; live: an instance created by a test command shows as a chip and Hide removes it from Program pixels |
| G2 | Document store and import spike, default rows, playlist with Cue / Show / Hide / Clear cue, row inspector, Guest rows, auto-out, template reinstall per core generation. `document` and `playout` vectors. | shell | L | G1; Metal item 4 | Vectors and transcripts; pixel proof per row kind; a Windows document opens and plays |
| G3 | F-keys and Shift+F12, cue rides Take, lower-third authority routing with the suppression flag, footer button routed through it. | shell | M | G2 | Router table tests; Take transcript; pixel proof that a cued title appears on the Take frame |
| G4 | Names list (paste / CSV import, hotkey steps) and countdown / wall clocks. | shell | S | G2 | Vectors; pixel proof of a running countdown |
| G5 | Local datasets: load, table design, staged and published revisions, persisted. `data` vectors. | shell | M | G2; Metal item 5 | Vectors incl. manifest equality; pixel proof of an in-place update |
| G6 | Crawls: create, Cue / Show / Hide, pause / resume / restart, boundary adoption shown. | shell | M | G5; Metal item 5 | Vectors; drill with a crawl on air; pixel proof across a boundary |
| G7 | Shot Layouts: edit dialog, Cue and Take, Take, Cancel, Check Take, Shift+F1-F11, guarded Take with receipts and rollback. `layouts` vectors. | shell | L | G3, G5 | Transcripts for committed / warming / refused; live refused-Take holds Program |
| G8 | Design > Builder: look and starter picker with thumbnails, style, placement, variables, private preview, logo and image import. | shell | L | G2; Metal items 6-7 | Vectors for builder outputs; pixel proof preview equals Program for one look |
| G9 | Remote feeds: HTTPS polling (Sheets CSV, RSS, flat JSON), custom API mapping, bearer credentials, status text. `feeds` vectors. | shell | M | G5, G1 (`KeychainStore`) | Vectors; local TLS server tests; live Google Sheet check |
| G10 | Push feeds over WSS with sequence rules and reconnect. | shell | M | G9 | Vectors; local WSS server tests incl. TLS failure cases |
| G11 | The `graphics.*` control actions and `graphics.strip`, registered in the Mac control surface against the shared manifest. | control | M | G3, G7; control-surface spec (M2.3) | The control contract fixtures; Companion button fires a row |
| D1 | Designer: designs lifecycle (create, open, save version, duplicate, delete), layer list, numeric inspector, undo, private preview. `designer` vectors. | shell | L | G8 | Vectors; a design saved on Mac opens on Windows and the reverse |
| D2 | Designer canvas: select, drag, resize, snapping, group / ungroup. | shell | L | D1 | Snap vectors; manual canvas checklist |
| D3 | Designer motion: per-layer in / out, Out mirrors In, Play preview. | shell | M | D1 | Motion vectors; pixel proof of sampled frames |
| D4 | Designer content: shapes, text effects, PNG / JPEG / SVG import. | shell | M | D1 | Bounds and refusal vectors; pixel proof per primitive |

Every slice ships its consumer: G1 is useful alone because Hide and Clear all are the safety
controls for any instance, including those a control client created.

Estimate for one shell lane: G1-G7 about 5-6 weeks (Operate, data, crawls, layouts); G8-G11 about
3-4 weeks; D1-D4 about 5-7 weeks. Total about 13-17 weeks. This is well above the roadmap's M4
sizing; see section 9, decision 1.

## 7. Acceptance and test strategy

- **Unit**: every area's vectors replayed in `COREVIDEO_SHELL_TESTS`; the exporter test green in
  `CoreVideoPro.MediaCore.Tests`. Each ported ruling is quoted in the plan task with the Windows
  test that proves it, and the Swift test must fail before the port exists.
- **Scripted core**: the existing fake bridge used by the Take tests (`M/ShellTests.swift`,
  `core.types()`) is extended to return recorded `graphics` nodes, so sequencing, correlation,
  overload and epoch-change cases run without a Metal backend.
- **Contract**: `contracts/graphics.fixtures.json` keeps running through the Swift validators.
  G11 is held to the shared control fixtures.
- **Pixels**: a graphic on air is proven by Metal readback or the `COREVIDEO_SHELL_SNAPSHOT`
  harness: one proof per row kind (G2), cue on the Take frame (G3), countdown advancing (G4), table
  update in place (G5), crawl across a boundary (G6), layout Take with scene and graphics on the
  same frame (G7), preview equals Program (G8).
- **Drill**: `scripts/mac-show-drill.py --load 8` with a title on air and a crawl running, judged
  on per-frame delivery, with no regression against the same run without graphics.
- **Interchange**: a document authored on Windows 1.0 opens, plays and re-saves on Mac, and the
  re-saved file opens on Windows.
- **Owner live checks**: a real show rundown on the Mac (guest lower thirds from F-keys, a title
  riding a Take, a sponsor bug, a countdown, a Shot Layout Take, a refused Shot Layout Take), a
  live Google Sheet feed, and a Companion page once G11 lands.

## 8. Risks and unknowns

1. **The schedule depends on a backend that does not exist yet.** Mitigation: G1-G3 are developed
   and unit-proven against the scripted core; only their live and pixel checks wait for Metal.
2. **Two implementations drift.** Mitigation: the exporter test in section 5.2 makes a Windows
   rule change fail until the vectors are regenerated, and the Mac run then fails until Swift
   matches. Resolved by building the exporter in G1.
3. **Public-address-only connections.** Windows resolves the host, filters to public addresses and
   dials them itself. `URLSession` does not expose the address it will connect to. Resolve with a
   one-day spike at the start of G9: resolve with `getaddrinfo`, reject non-public results, then
   connect with Network.framework to the vetted address with the original host as the TLS name. If
   that cannot carry HTTP cleanly, feeds ship behind a documented limitation only with owner sign-off.
4. **Document and asset portability from Windows** (absolute `PackageRoot` paths, image storage
   layout). UNVERIFIED. Resolve with the G2 spike on a real Windows show folder.
5. **F-keys on macOS** are media keys by default and F11 / F12 may be bound to Mission Control.
   Resolve by owner test in G3 on a MacBook and an external keyboard; fall back to an in-app hint
   and Control-digit alternates only if the owner asks.
6. **How the private authoring preview reaches a Mac surface** is unknown. It gates G8 and the
   Designer. Resolved in the Metal spec (item 7).
7. **Snapshot weight.** Templates with thumbnails and definitions can make the `graphics` node
   large. Resolve by measuring node size and decode time on a full library in G1.
8. **SVG import cannot match Skia pixel for pixel.** Accepted; D4 documents it.
9. **Instance id hashing and number formatting** must match C# exactly (invariant culture, UTF-8
   byte counts, integer versus decimal notation preserved). Covered by vectors; called out because
   Swift's defaults differ.
10. **Sources names for Guest rows** depend on the Sources rebuild. Until then Guest rows show
    roster names with empty titles.

## 9. Owner decisions needed

1. **May the first Mac parity release ship before the Designer?** Recommended: yes. Ship G1-G11
   (Operate, data, crawls, Shot Layouts, feeds, the Builder for choosing and styling looks,
   starters, and import of a show authored on Windows). Build D1-D4 last, after Tiles and social.
   A live show needs Operate; layer-level authoring is done before the show and can be done on
   Windows meanwhile. Full parity stays the goal; this only orders it.
2. **Ordering against Tiles.** Graphics and Tiles share priority 4. Recommended: G1-G7 first,
   then Tiles, then G8-G11, because Operate is used on every show.
3. **Should the Designer's manifest-editing logic move into the shared core before the Mac
   Designer is built?** Recommended: decide when D1 is planned, and lean yes. About 2,000 lines of
   pure manifest edits would otherwise exist twice; the core already compiles and previews drafts.
   The cost is rewiring the shipped Windows Designer, so it must not be done inside this push.
4. **Guest row titles before the Sources rebuild.** Recommended: ship with roster names and empty
   titles rather than add a temporary title editor that the Sources rebuild would replace.

## 10. Out of scope

- Every pixel and the core's admission of `graphics-*` commands on Metal (the Metal graphics
  backend spec).
- Social review and the YouTube / Twitch feeds (roadmap M6), though the gateway keeps the social
  client refusal.
- Browser overlays / DSK (gap row 2.10) and the legacy lower-third look designer's Metal raster
  (Overlays row L13).
- Tiles and the Tiles scene starter (roadmap M5).
- NDI graphics key, OHG, Auto director.
- The control server itself and its manifest (roadmap M2); this spec supplies only the graphics
  action bodies in G11.
- Any change to Windows behaviour. The only Windows-side addition is the vector exporter test.
