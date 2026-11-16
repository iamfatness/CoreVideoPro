# CoreVideo Tiles on macOS

Status: DRAFT design for owner review, 2026-11-15.
Covers roadmap milestone **M5** (M5.1, M5.2) in `docs/superpowers/plans/2026-11-14-mac-parity-1.0-roadmap.md`
and gap row **2.3** in `docs/superpowers/audits/2026-11-15-mac-ui-parity-consolidated.md`
(plus graphics-audit rows 11 and 12 in `docs/superpowers/audits/2026-11-14-mac-parity-graphics.md`).
Owner priority #4 (graphics + Tiles).

The roadmap says M5 was never sized by an audit. Sections 2 to 4 are that audit. Every claim in
them was re-read in code on branch `mac-tier0b` on 2026-11-15. Nothing was built or run (a release
build held the worktree), so every statement about Metal PIXELS is marked UNVERIFIED until slice N1.

Paths are relative to the repo root. `MediaCore.cpp` means `native/src/core/MediaCore.cpp`,
`Metal` means `native/src/compositor/MetalCompositorAdapter.mm`, `AppModel` means
`mac-shell/Sources/CoreVideoProShell/AppModel.swift`, `SVM` means
`native-shell/CoreVideoPro.WinUI/ViewModels/StudioViewModel.cs`.

## 1. Why

The 1.0 product decision is "CoreVideo Tiles owns composition": the auto-reflowing guest wall is
the scene an operator reaches for when more than two people are on a show. On Windows the New
scene menu offers "CoreVideo Tiles · Auto-reflow wall", the Studio rail carries an Edit Tiles
button, and the Sources workspace has a four-tab wall inspector. On a Mac none of that exists. The
Mac operator has three fixed layouts (`AppModel:414-416`: single, two-up, pip) and must hand-build
a canvas for a panel, re-editing it every time a guest joins or leaves. A show file that carries a
Tiles scene cannot be represented at all. Anyone running a panel, a town hall or a gallery opener
from a Mac notices in the first minute.

## 2. Audit summary: what Tiles is, and how much of it already runs on a Mac

Tiles is three things, and they are in three different states on macOS.

| Part | Where it lives | macOS state |
|---|---|---|
| The wall itself: parse, membership admission, layout solve, pins, animation, background, name lines, take continuity, registry, snapshot | shared C++ core, platform-neutral | **Compiled and unit-tested on Mac CI today.** No Mac producer ever sends it a `tiles` node. |
| The pixels: tile rects, rounded mask, inset border, analytic glow, solid background, live background | per-platform compositor | **Metal code is present** (it is not missing, as the roadmap assumes). It has **no Metal pixel test**. Name lines have a Metal performance defect. |
| The shell: scene model, payload builder, membership / identity policies, subscriptions, sticky audio, starter, inspector, control actions | WinUI + plain-.NET libraries | **Entirely absent from the Mac shell.** |

Consequence for sizing: roadmap M5.1 ("Metal Tiles effects, L") shrinks to a proof-and-fix slice
(S to M). The weight of M5 is the shell (M5.2), and inside the shell the subscription policy and
the identity policy are the parts that can hurt a live show if ported wrong.

## 3. Windows behaviour to match

### 3.1 The wall is a composed source owned by the core

- **Wire.** A Tiles scene sends a `tiles` object on `load-scene-graph` and `set-preview-scene`.
  Serializer: `native-shell/CoreVideoPro.MediaCore/Services/MediaCoreCommandBuilder.cs:206-244`.
  Parser: `parseTilesLayer`, `MediaCore.cpp:1780-1876`; consumed at `MediaCore.cpp:2281` (Program)
  and `:3814` (Preview). Keys: `layerId`, `order`, `rect{x,y,w,h}`, `opacity`,
  `transparentBackground`, `members[]` (scheme-qualified ids, capped at 64), `overrides{id:{rect,
  cropLeftPercent, cropRightPercent, z}}`, and `style{tileAspect, customAspectRatio, gutterPercent,
  marginPercent, backgroundColor, backgroundSourceId, borderShape, borderColor, borderThickness,
  cornerRadius, glowColor, glowSize, glowIntensity, glowSoftness, animateLayout,
  animationDurationMs, fillMode, nameEachTile}`.
- **The shell sends membership POLICY only.** It never sends a rect per tile and never filters on
  liveness beyond video-off (`TilesLayerPayloadBuilder.cs:5-21,37-55`; test
  `TilesLayerPayloadBuilderTests.Build_DoesNotDropAParticipantMerelyBecauseItLooksUnhealthy`).
- **Admission is the core's.** `compositor::classifyTilesMember` / `admitTilesMembersWithHold`
  (`native/src/compositor/TilesMembership.h:71,91`; used at `MediaCore.cpp:4934`). A `zoom:` member
  is Fresh up to `kTilesStaleFrameMs` = 1500 ms, Held (same slot, labelled) up to
  `kTilesHoldWindowMs` = 4000 ms, then dropped and the wall reflows. A local (`capture:`) member is
  admitted while its frame is in this tick's gather and never ages (`tilesMemberAgesOut`, `:58`).
  Tests: `native/tests/TilesMembershipTest.cpp` (21 cases);
  `TilesRenderPlan.AFrozenFrameIdAgesTheMemberOutEvenAsTimestampKeepsAdvancing`.
- **`wall.present` alone gates the wall** (`MediaCore.cpp:4666`), never `present && !members.empty()`.
  A configured wall with nobody live shows its background and still suppresses the legacy
  one-cell-per-decoded-frame fallback. The background layer is pushed above the admission gate.
  Tests: `TilesRenderPlan.AnAllStaleWallStillEmitsItsBackground` (`TilesRenderPlanTest.cpp:693`),
  `AMemberLessWallStillOwnsTheSceneAndEmitsItsBackground` (`:729`). A wall counts as a layer in
  `hasPreviewScene()` (`MediaCore.cpp:4586,4615`); test `APreviewSceneCarryingOnlyAWallStillComposites` (`:773`).
- **Two backgrounds.** `tiles-bg:<layerId>` is a sourceless solid (`hasFillColor`), suppressed when
  the scene has its own media background. `tiles-source-bg:<layerId>` is the live background feed,
  gated by `tilesBackgroundSourceIsDrawable` (`MediaCore.cpp:4918`): held for as long as a frame
  exists, never fabricated. Tests: `AWallsLiveBackgroundSurvivesATakeAcrossAStaleBeat` (`:1782`),
  `AStaleBackgroundIsHeldButAnAbsentOneIsNeverFabricated` (`:1843`).
- **One order namespace.** tiles-bg at `wall.order`, tile i at `wall.order + 1 + i`
  (`MediaCore.cpp:4829-5016`), name lines at `+129 + order`. Test
  `TilesRenderPlan.RoutesAndAWallShareOneOrderNamespace` (`:878`). The core pushes a deduped scene
  warning when routes and a wall arrive together (`warnIfRoutesAndWallCollide`, `MediaCore.cpp:1771`).
- **One animator per wall.** `core::TilesWallSource` (`native/src/core/TilesWallSource.h:24`) wraps
  `TilesPlanAnimation` / `TilesAnimator`; `TilesWallSources::releaseAllExcept` (`:75`) frees a wall
  the tick no bus names it (`MediaCore.cpp:5933-5935`). A live wall is registered in
  `SourceRegistry` as `Kind::Composed` (`native/src/core/SourceRegistry.h:26`) and erased, never
  tombstoned. Tests: `TilesWallSourceTest.cpp`, `SourceRegistryComposedTest.cpp`.
- **Observability.** `sessionState.tiles` = `{layerId, members[{sourceId, held, showing, rect}],
  heldCount, blackCount, staleAfterMs, holdWindowMs}`, published whenever the Program wall is
  present (`native/src/core/CoreStateCapture.cpp:1124-1163`).

### 3.2 Take continuity: a wall taken from Preview is cut to, never redrawn

The wall key is `sceneId + ":" + layerId` and `layerId` is `tiles:<sceneId>`
(`TilesLayerPayloadBuilder.cs:58`), so the same gallery has the same key on both buses and the one
`TilesWallSource` survives the Take with its generation intact. The take record
(`MediaCore.cpp:2026-2090`, `native/src/core/TakeRecordPolicy.h:64-86`) carries `wallAdoptedSettled`,
whether `tiles-source-bg:` made the first Program frame, the subscription-churn delta, and
per-source continuity; any restart, missing source, dropped background or churn forces
`verdict=rebuilt`. It lands as a `[take]` log line and the 8-deep `takeRecords` snapshot node.
Tests: `TilesRenderPlan.AWallSettledInPreviewIsAlreadySettledOnItsFirstProgramFrame` (`:1669`),
`AWallTakenWhileItsFramesLapseIsStillCutToNotRedrawn`.

The shell's half of this rule is that a Take sends ONE batch (`load-scene-graph` incoming +
`set-preview-scene` outgoing) and that the subscription set does not change because of the Take.

### 3.3 Shell: membership, identity, subscriptions, audio

- **Scene model.** `DynamicGallerySettings` (`native-shell/CoreVideoPro.WinUI/Models/ProductionModels.cs:100-137`):
  wall rect / order / opacity / visible / locked / transparent background; membership mode
  (`eligible` | `routed` | `manual`), `ManualSlots`, `ExcludedSourceIds`, `OmittedSourceIds`,
  `BoundMeetingId`, `MaxTiles` (default 16, 1 to 64); style fields; `Overrides`; `NameEachTile`.
  Persisted by `ScenePersistenceService.cs:18-22,87-117`.
- **Membership.** `TilesMembershipPolicy.Resolve` (`Services/TilesMembershipPolicy.cs:14-70`): manual
  mode keeps slot positions with empty strings for absent members; otherwise manual slots are
  honoured first and the rest fill in eligible order up to `MaxTiles`. Eligible = video-on Zoom
  participants in roster order, then in-show local capture slots (`TilesLocalSourcePolicy.cs`).
  Media is not a member (reference doc `docs/reference/tiles-composed-source.md`).
  Tests: `TilesMembershipPolicyTests`, `TilesLocalSourcePolicyTests`, `TilesLayerPayloadBuilderTests`.
- **Identity (#479).** Manual slots and never-show persist `zoom-pid:<persistentId>`, else
  `zoom-name:<displayName>`, never the per-meeting session id; a legacy `zoom:<sessionId>` is only
  honoured inside `BoundMeetingId` (`Services/TilesIdentityPolicy.cs:12-60`). Tests: `TilesIdentityPolicyTests`.
- **Empty route list.** A Tiles scene contributes no generated routes: `ReconcileDynamicGalleryRoutes`
  (`SVM:13482-13491`) and, at the point the wire is built, the `IsAuthoredRoute` filter plus "no
  empty-scene route for a gallery" (`SVM:9150-9158`). Since the scene-builder workspace (#879) a
  Tiles scene MAY carry workspace-authored layers above or below the wall at a 256 stride
  (`Services/SceneWorkspacePolicy.cs:13-17,36-43`; test `TilesWorkspace.WallBoundsOpacityAndAuthoredLayerOrderReachThePlan`).
- **Subscriptions.** `ZoomSourceSetPolicy.Resolve` (`native-shell/CoreVideoPro.MediaCore/Services/ZoomSourceSetPolicy.cs:106-200`),
  budget order: Program routes, Program Tiles members (+ Zoom wall background, purpose
  `program-tiles`), Preview routes, Preview Tiles members (`preview-tiles`), multiview slots, ISO
  guests, sticky Tiles audio (`tiles-audio`, audio only). Video = camera-on sources capped at 10
  (`ZoomMediaSpinePayloadBuilder.cs:19`); audio = every source, uncapped. The spine payload also
  carries `sourceParticipantIds` (`:163`), `videoSubscriptionShortfall` (`:175`) and the multiview
  `programNotice` / `previewNotice` (`:209-211`). The core grants 1080P to every camera purpose, at
  most `kMaxConcurrentFullResolutionCameras` = 8, in payload order
  (`native/src/modules/ZoomSubscriptionResolutionPolicy.h:97,108-135`).
  Tests: `ZoomMediaSpinePayloadBuilderTests` (`LiveCase478_*`, Program-first, at-the-cap, sticky).
- **Sticky audio.** `TilesAudioSourceLatch` (`native-shell/CoreVideoPro.MediaCore/Services/TilesAudioSourceLatch.cs:30-134`):
  keyed by scene; a member stays an audio source while its scene is on either bus; forgets a scene
  on neither bus, a participant who left, everything on a KNOWN not-in-meeting or Engine off, and
  nothing on an UNKNOWN meeting state. Tests: `TilesAudioSourceLatchTests`.

### 3.4 Shell: operator surface

- **Starter (#1199).** `StarterSceneFactory.Create(..., "tiles", ...)` makes scene "CoreVideo Tiles",
  layout `dynamic-gallery`, default settings, empty routes (`Services/StarterSceneFactory.cs:17,27-33`);
  menu item `Views/SourcesPage.xaml:80`. Test: `StarterSceneFactoryTests`.
- **Edit Tiles.** Studio rail button (`Views/StudioWorkspace.xaml:359`), which cues the scene to
  Preview and opens the wall inspector without touching Program
  (`ViewModels/StudioViewModel.TilesNavigation.cs:6-14`; probe `StudioWorkspaceLayoutProbe.cs:93-98`).
- **Inspector** (`Views/SourcesPage.xaml:196-335`). Layout: aspect, custom ratio, spacing 0-10,
  margin 0-20, max tiles 1-64, animate, duration 100-2000 ms. Style: Clean / Rounded / Broadcast
  presets (`SourcesPage.Workspace.cs:192`), shape, corner radius 0-100, border 0-32, glow size 0-64,
  intensity, softness, transparent background, reset. Members: mode, member list, never-show,
  per-member pin rect + left/right crop + order, name each tile, manual slot assign / clear,
  summary, clear stale. Every edit applies to the PREVIEW scene only (`RequireTilesPreview`,
  `StudioViewModel.Tiles.cs:216`).
- **Colour pickers (#476).** Background, border and glow each pair a hex field with a picker; the
  hex string is the single source of truth (`StudioViewModel.Tiles.cs:38-67`).
- **Control actions** (`native-shell/CoreVideoPro.Control/ControlActionRegistry.cs:110-127`):
  `scene.dynamicGallery.create`, `scene.tiles.autofill.set`, `scene.tiles.slot.assign`,
  `scene.tiles.exclusion.set`, `scene.tiles.background.set`, `scene.tiles.override.set`,
  `scene.tiles.override.clear`, `scene.tiles.names.set`; state field `previewTilesNamesOn`
  (`ControlState.cs:67`). Test: `ControlActionRegistryTests.TilesNamesSet_*`.

## 4. Mac today

### 4.1 Shared core: supported on macOS

Everything in 3.1 and 3.2 is platform-neutral C++ with no `_WIN32` or D3D guard: `TilesWallSource.h`,
`TilesMembership.h`, `TilesLayout.h`, `TilesPinnedLayout.h`, `TilesAnimator.h`,
`TilesPlanAnimation.h`, `TilesNameLine.h`, `TilesHoldEpisodeLog.h`, `SourceRegistry.h`,
`TakeRecordPolicy.h`, and the parse / plan / snapshot code in `MediaCore.cpp` and
`CoreStateCapture.cpp`. The test files `TilesRenderPlanTest.cpp` (51 tests), `TilesMembershipTest.cpp`,
`TilesAnimatorTest.cpp`, `TilesWallSourceTest.cpp`, `TilesNameLineTest.cpp`, `TilesLayerTest.cpp`,
`TilesLayoutTest.cpp` are in the unconditional test list (`native/CMakeLists.txt:987,1025`) and carry
no platform guard, so they run in the Mac ctest the roadmap reports green. The Mac engine already
emits `persistent_id` per participant (`native/zoom-engine/engine/main-macos.mm:498-501,558`) and
the core republishes `persistentId` (`native/src/modules/ZoomEngineRuntime.cpp:1443`).

### 4.2 Metal compositor: present, unproven

- **`hasFillColor`: fixed.** `Metal:1126-1136` paints the parsed fill colour; tests
  `MetalCompositor.SourcelessFillColorLayerPaintsItsParsedColour` and
  `FadeToBlackProducesBlackPixelsAndRestoresHeldVideo` (`native/tests/MetalCompositorTest.cpp:604,564`).
  The CLAUDE.md sentence "Metal has no hasFillColor branch" and graphics-audit row 12 are stale.
- **Tile rects, per-tile crop, clip rect.** `Metal::resolveLayers` (`:1085-1163`) takes each plan
  layer's rect as given and resolves the frame by `participantId`; `drawLayer` applies
  `sourceCropLeft/RightPercent` and the clip-rect scissor (`:1456,1470,1501-1507`). Test for clip:
  `MetalCompositor.ClipRectConfinesLayerPixels` (`:337`).
- **Rounded mask, inset border, analytic glow: implemented.** The MSL `decorateTile`
  (`native/src/compositor/MetalCompositorShaders.h:43-63`) is the same signed-distance mask, border
  mix and `(1-t)^2 (1+kt)` glow as HLSL; constants come from the shared `applyTilesDecoration` /
  `tilesGlowRect` (`native/src/compositor/TilesDecorationParams.h`), applied at `Metal:1265` and the
  glow pass at `Metal:1422-1430`. A shader-compile failure degrades to clean tiles and logs
  (`Metal:462-467`), which is the "effects degrade independently" rule from
  `docs/corevideo-tiles-iso-scaling-plan.md` slice 3.
- **No Metal pixel proof exists.** The only decoration pixel test is
  `TilesDecoration.D3DPixelsHaveRoundedMaskInsetBorderAndAnalyticGlow`, compiled only under
  `_WIN32 && COREVIDEO_WITH_D3D11` (`native/tests/TilesDecorationTest.cpp:34-35`). Whether the Metal
  wall looks right is therefore **UNVERIFIED**.
- **Empty-plan fallback** exists on Metal as on D3D11 (`Metal:1165-`), so the `wall.present` rule
  protects the Mac identically.
- **Name lines have a real defect on Metal.** A name line is an ordinary overlay layer drawn by
  `drawOverlayLayer` (`Metal:1525-1582`). The Metal raster cache is ONE texture
  (`OverlayTex overlayTex_`, `Metal:1706`; lookup `:1592-1595`, overwrite `:1617`), and the Core Text
  raster runs synchronously inside the draw (`:1599`). With a scene lower third plus N name lines
  (or simply two name lines) every layer evicts the previous one, so every band is re-rasterized
  and re-uploaded on the render thread every frame. `holdLastRaster` and
  `OverlayRasterRetention` are not referenced by the Metal adapter at all. This breaks the
  per-frame delivery rule and must be fixed before "Name each tile" ships on a Mac.
- **Not implemented anywhere:** the `tiles-wall:<wallId>` render-once layer kind proposed in the
  archived `2026-09-12-tiles-wall-persistent-source-design.md` §3 never landed on either platform
  (no occurrence in `native/src`). Mac parity does not need it.

### 4.3 Mac shell: missing

- **No Tiles scene model.** `SceneDef` is `{id, name, layout, layers, backgroundAssetId}`
  (`AppModel:92-102`), persisted as `PersistedScene` (`ShellPrefs.swift:39-45`). Starters are
  single / two-up / pip (`SceneCanvas.swift:41-43`). `sceneSyncBatch` (`AppModel:3246-3268`) sends
  `routes` and `background` only; no producer emits a `tiles` key.
- **Subscriptions are a reduced port.** `ZoomSourceBudget` (`ZoomSourceBudget.swift:27-100`) says so
  in its header: "scaled to the Mac shell's model, which has no Tiles wall". Tiers are Program
  routes, Preview routes, multiview, ISO; cap 10; camera-on filter; audio = same set uncapped;
  `meeting-audio` under participant "0" (`AppModel:2376-2452`). Missing against Windows:
  the `program-tiles` / `preview-tiles` / `tiles-audio` tiers, the sticky latch,
  `sourceParticipantIds`, `videoSubscriptionShortfall`, and `programNotice` / `previewNotice` (no
  occurrence of any of the four keys in `mac-shell/Sources`). With `sourceParticipantIds` absent the
  core leaves every roster participant eligible for the speaker director
  (`native/src/modules/ZoomEngineRuntime.cpp:471-485`), the pre-#478 behaviour.
- **Roster drops the persistent id.** `RosterParticipant.parse` keeps id, name, video, mute,
  talking, sharing (`AppModel:308-321`); `persistentId` is not read.
- **`set-source-policy` carries no `displayName`** (`AppModel:2465-2483`, "a parity follow-up, not
  shipped blind"). Zoom tiles still get names from the core's roster fallback
  (`MediaCore::tilesMemberDisplayName`, `MediaCore.cpp:2836`); a capture tile would be unlabelled.
- **Take path is sound for a wall.** A Take sends `begin-take-transition` + `load-scene-graph` +
  `set-preview-scene` in one batch (`AppModel:3004-3040,3246-3268`), which is what the continuity
  rule needs. Nothing on the Mac reads `takeRecords`, but `GET /snapshot` exists
  (`ControlApi.swift:334`), so a judge can.
- **Control surface** has 13 actions (`ControlApi.swift`), none for Tiles.

## 5. Design

### 5.1 Principle: the Mac shell sends the same wire and owns only policy

The core already owns layout, admission, animation and continuity. The Mac shell adds a scene kind,
decides WHO is eligible, and sends the existing `tiles` node unchanged. No new core command and no
new snapshot field is required for parity. Two existing spine keys start being sent
(`sourceParticipantIds`, `videoSubscriptionShortfall`).

### 5.2 Components (all new Swift types, all pure unless stated)

| Type (new) | Mirrors | Responsibility |
|---|---|---|
| `TilesSettings` (+ `PersistedTilesSettings`, `TilesMemberOverride`) | `DynamicGallerySettings` | The scene's wall configuration, field for field, same defaults and clamps. Optional member of `SceneDef` and `PersistedScene` (absent = not a Tiles scene, so older prefs decode). |
| `TilesIdentityPolicy` | same name | `persist`, `resolveLiveSourceId`, `label`, `describeStale`, `clearStale`. |
| `TilesMembershipPolicy` | same name | `resolve(settings, eligible, routed, roster, meetingId) -> [String]`. |
| `TilesLocalSourcePolicy` | same name | In-show slots bound to a capture source, slot order; `routedSourceIds`. |
| `TilesLayerPayload.build` | `TilesLayerPayloadBuilder.Build` | Produces the `tiles` JSON object; `layerId = "tiles:" + sceneId`. Returns nil for a non-Tiles scene. |
| `TilesAudioSourceLatch` (stateful, small) | same name | `observe(inMeeting: Bool?, programSceneId, programMembers, previewSceneId, previewMembers, present) -> [String]`, `clear()`. |
| `ZoomSourceBudget` (extended) | `ZoomSourceSetPolicy` | Gains Program-Tiles, Preview-Tiles and sticky tiers, a `Source{participantId, purpose, videoOn}` result, `overBudget`, and `speakerCandidateIds`. |
| `TilesInspector` (SwiftUI) | `SourcesPage.xaml:196-335` | Layout / Style / Members tabs. |

The Windows C# tests named in section 3 are the oracle. Each Swift policy gets a test table ported
case for case into the in-binary `COREVIDEO_SHELL_TESTS` harness, including the Windows test name
in the assertion message, per roadmap rule 6.

### 5.3 Data flow

1. Roster or slot change, or an inspector edit, calls `syncScenes()`.
2. `sceneSyncBatch` builds, per bus: `routes` (for a Tiles scene: EMPTY in slices S1 to S5, see
   decision 2), `background`, and `tiles` from `TilesLayerPayload.build`. The Tiles scene never
   emits an empty-scene placeholder route.
3. `resolveSpineSubscriptions` calls the extended `ZoomSourceBudget` with the SAME two `tiles`
   payloads built in step 2 (one function builds them, both callers read it, so the wall and the
   subscription set can never describe different rosters). It observes the latch once, with the
   full in-room roster, and writes `subscriptions`, `sourceParticipantIds`,
   `videoSubscriptionShortfall`.
4. The core admits, solves, animates and draws. The shell reads `sessionState.tiles` for the
   inspector's live member list and held / black badges, and `sceneValidationWarnings` for drift.

### 5.4 Subscription rules the port must keep (each is a Windows ruling)

- Program first: Program routes, Program Tiles (+ Zoom background), Preview routes, Preview Tiles,
  multiview, ISO, sticky audio last and audio-only.
- A participant's purpose is `program` if on a fixed Program route, else `preview` if on a fixed
  Preview route, else the first tier that lists it. A follow-speaker route adds nobody.
- Video is camera-on only and capped at 10; the 9th and later camera past the core's 1080P cap of 8
  is demoted by the core, in this order, and reported in `zoomSubscriptionChurn.fullResolutionDemoted`.
- A Take changes nothing in the set when the same Tiles scene moves Preview to Program (members
  were already `preview-tiles`; they become `program-tiles`; the engine identity
  `participant-video-<pid>-camera` excludes purpose, so no renderer is rebuilt).
- The latch treats an unknown meeting state as "leave it alone" and a known not-in-meeting or
  Capture off as "forget everything". On the Mac "known" means the snapshot carried a non-empty
  `meetingState`; "Capture off" is the Mac's Engine-off equivalent.

### 5.5 Operator surface

- **New scene menu** gains "CoreVideo Tiles · Auto-reflow wall". It creates scene "CoreVideo Tiles"
  (uniquified), layout `dynamic-gallery`, default `TilesSettings`, in Preview only, persisted.
- **Edit Tiles** appears on the Studio scene row for a Tiles scene. It cues that scene to Preview
  (never Program) and opens the Scenes tab with the wall inspector selected.
- **Inspector**: the three Windows tabs with the same ranges, defaults, presets and help text.
  Colour fields are a hex text field plus a native `ColorPicker` bound to the same hex string
  (alpha disabled). All edits require the Tiles scene to be in Preview; otherwise the inspector is
  read-only and says "Queue this scene in Preview to edit it".
- **Members tab** lists Zoom participants (roster order) then in-show capture sources, each with
  Never show, pin / crop / order, and a live badge from `sessionState.tiles.members` (`held`,
  `showing: black`) when the scene is on Program. Stale saved identities are listed with "Clear stale".
- **Wall transform** (rect, opacity, visible, transparent background) is in decision 2's scope.

### 5.6 Native changes

- **N1, pixel proof.** A Metal twin of the D3D decoration test and a wall pixel test (section 7).
  Any defect it finds is fixed in `Metal` / `MetalCompositorShaders.h` only.
- **N2, name-line raster.** Replace the single `overlayTex_` with a bounded cache keyed by the
  existing `overlayContentSignature` + pixel size (bound 64, matching the D3D raster's resident
  cap), and honour `holdLastRaster` through the shared `compositor/OverlayRasterRetention.h`. The
  Core Text raster itself must leave the render tick. That is exactly roadmap M3.1 (the Core Text
  twin of `CompositorOverlayRaster` with async preparation), owned by the graphics workstream; N2
  consumes it rather than building a second async path. Until N2 lands the Mac inspector shows
  "Name each tile" disabled with the reason, and the payload sends `nameEachTile: false`.

No change to `MediaCore.cpp`, the wire, or the snapshot.

### 5.7 Threading, performance, persistence, degraded states

- Shell policy runs on the main actor inside the existing coalesced sync; it is O(roster) and
  allocates a few small arrays. The latch is touched only from the spine builder.
- Inspector sliders coalesce through the existing scene-sync debounce; a drag must not send more
  than the existing sync rate.
- Persistence: `PersistedScene.tiles` (optional). Manual slots and never-show are stored as
  `zoom-pid:` / `zoom-name:` / canonical `capture:` keys plus `boundMeetingId`.
- Degraded states the operator sees: an empty wall shows its background (never a grid of unrelated
  sources); a held tile is badged in the Members tab; a cued guest the budget left out is named
  (S3 sends the shortfall; the PVW-cell notice depends on the multiview workstream reading
  `previewNotice`, so until then the Members tab row reads "no video: subscription limit (10)");
  an unknown persisted identity reads "not in this meeting"; a `sceneValidationWarnings` entry
  about routes + wall is surfaced in Health.

## 6. Slices

| # | Slice | Lane | Size | Depends on | Proven by |
|---|---|---|---|---|---|
| N1 | Metal Tiles pixel proofs: decoration (mask, border, glow), solid + live background, clip, opacity; fix what fails | native | S to M | none | New `MetalCompositor.Tiles*` readback tests; first consumer is S1 |
| S1 | Tiles scene end to end: `TilesSettings` + persistence, `TilesLayerPayload` (eligible mode only), empty routes, starter (#1199), `tiles` on both buses, wall visible in Preview / Program / multiview | shell | M | N1 | Ported payload + starter tests; `COREVIDEO_SHELL_SNAPSHOT` pixel proof of a 4-member wall; `takeRecords` verdict `cut` on a Preview-to-Program Take of the same scene |
| S2 | Subscriptions: extended `ZoomSourceBudget` with Tiles tiers, `TilesAudioSourceLatch`, `sourceParticipantIds`, `videoSubscriptionShortfall` | shell | M | S1 | Ported `ZoomMediaSpinePayloadBuilderTests` / `TilesAudioSourceLatchTests` tables; fake-engine run: churn ledger flat across a wall Take and a speaker change |
| S3 | Identity + membership modes: `persistentId` in the roster, `TilesIdentityPolicy`, `routed` / `manual` modes, manual slots, never-show, max tiles, clear stale, local capture members | shell | M | S1 | Ported `TilesIdentityPolicyTests`, `TilesMembershipPolicyTests`, `TilesLocalSourcePolicyTests` |
| S4 | Inspector: Edit Tiles entry, Layout + Style tabs, presets, colour pickers (#476), animation | shell | L | S1 | View-model tests for clamps and hex round-trip; snapshot-harness pixels for the Rounded and Broadcast presets |
| S5 | Members tab: member list with live badges, pin / crop / order overrides, live background source picker | shell | M | S3, S4 | Ported `TilesOverridePolicy` and `TilesMemberChoicePolicy` tests; pixel proof of one pinned + cropped member |
| N2 | Metal overlay raster cache + `holdLastRaster`; enable "Name each tile" | native + shell | M | graphics spec M3.1; S4 | Metal test: 9 name lines, raster count flat after the first frame; drill with names on |
| C1 | The 8 Tiles control actions + `previewTilesNamesOn` | control | S | remote-control spec (manifest host); S3, S5 | Shared action-manifest fixtures against the Mac server |
| S6 | Wall as a layer: wall rect / opacity / visibility and authored layers above or below the wall (256 stride) | shell | M | Sources-rebuild spec (layer list); S4 | Ported `SceneWorkspacePolicy.NormalizeOrder` tests; pixel proof of a logo over the wall |
| A1 | Performance + live acceptance | native + owner | S | S2, S4 | Section 7 drill and owner checks |

S1 is the first shippable point: a Mac operator can create, cue and take a working auto-reflow
wall. S2 must ship before any live show uses it (without it wall members are subscribed only if
they also hold a multiview slot).

## 7. Acceptance and test strategy

- **Unit (Swift, in-binary harness).** One ported table per Windows test class named in section 3.
  Each must fail on the pre-slice Mac behaviour first.
- **Contract.** The `tiles` JSON the Swift builder emits for a fixed scene + roster is compared
  byte-for-key with the object the Windows `MediaCoreCommandBuilder` emits for the same inputs. The
  fixture is new (`contracts/tiles.fixtures.json`, proposed) and is generated once from the Windows
  builder test, then checked by both shells.
- **Metal pixels (N1).** Readback tests in `native/tests/MetalCompositorTest.cpp`: rounded corner
  pixel is background, edge pixel is border colour, a pixel `glowSize/2` outside is between glow and
  background, glow never brightens outside its extent; `tiles-bg` colour fills the wall rect under
  an empty wall; a `tiles-source-bg` frame draws under tiles; forced effect-compile failure still
  draws clean tiles.
- **Shell pixels.** `COREVIDEO_SHELL_SNAPSHOT` captures of a fake-engine 4-up and 9-up wall and of
  each style preset, compared against expected tile rects from `sessionState.tiles.members[].rect`.
- **Continuity.** With the fake engine: cue a Tiles scene, wait for settle, Take. Assert the newest
  `takeRecords` entry has `verdict: cut`, `wallAdoptedSettled: true`, empty `restartedSources` and
  `missingSources`, churn delta 0. Repeat with a live capture background.
- **Empty-plan guard.** All cameras off on the fake engine with the wall on Program: Program
  pixels equal the wall background colour, and `sessionState.tiles.members` is `[]` with the node present.
- **Drill.** `scripts/mac-show-drill.py --seconds 40 --load 8` with a new `--tiles` option
  (proposed) that puts an 8-member wall on Program and the same wall on Preview: 60.0 fps, zero
  missed deadlines, delivery >= 99%, on the owner's Apple Silicon machine, with and without glow +
  rounded corners, and (after N2) with names on.
- **Owner live checks.** Real meeting, 6 to 9 cameras: join / leave reflow; camera off then on
  (tile leaves, audio stays, tile returns); Take from Preview is a cut; solo a wall member in
  Preview with the wall on Program and confirm no tile flashes; uplink stall shows held tiles, not
  a reflow storm; saved manual slots survive a rejoin and a new meeting.

## 8. Risks and unknowns

1. **Metal wall pixels have never been looked at.** The code mirrors D3D, but nothing has rendered
   it. Resolved by N1, first, before any shell work is trusted.
2. **Eight 1080p Zoom feeds on Apple Silicon.** The cap of 8 was soak-proven on Windows (RTX 4090,
   2026-09-13) only. Mac Zoom frames are I420 over shared memory uploaded per frame. Resolved by the
   A1 drill, then a 30-minute owner soak with a full wall; if it fails, the fix is in the pipeline,
   never a lower cap or resolution.
3. **Subscription port drift.** A wrong tier order reproduces #478 (flashing tiles, a guest frozen
   off-bus). Resolved by porting the Windows test tables verbatim and by the churn-ledger assertion
   in the fake-engine run.
4. **Sending `sourceParticipantIds` changes follow-speaker behaviour for every Mac scene**, not
   only Tiles (the director starts ignoring non-sources). It is the Windows rule, but it lands in
   S2 and needs its own before / after check on a plain follow-speaker scene.
5. **`getPersistentId` on the Mac SDK** is called through `respondsToSelector`
   (`main-macos.mm:498`). If it returns empty in practice, identity falls back to `zoom-name:`,
   which collides for two guests with one name. Resolved by reading the live roster in S3's first
   hour; UNVERIFIED today.
6. **N2 depends on another workstream's raster.** If M3.1 slips, names stay disabled on Mac; the
   wall itself is unaffected.
7. **Mac capture members**: whether every Mac capture adapter re-serves a held frame each tick (the
   assumption behind "local members never age") is UNVERIFIED on macOS. Resolved in S3 with a
   static screen-share member left for 60 s.

## 9. Owner decisions needed

1. **Ship order inside M5.** Recommendation: N1, S1, S2 as one releasable unit (a working wall with
   correct subscriptions), then S3 to S5, then N2 / C1 / S6 as their dependencies arrive.
2. **Authored layers over the wall (S6) in Mac 1.0?** Windows allows a logo or camera above or
   below the wall. Recommendation: defer S6 until the Sources-rebuild workstream gives the Mac a
   layer list; until then a Mac Tiles scene sends an empty route list (the original, proven rule)
   and a show file with authored layers over a wall loads with those layers preserved but not
   drawn, with a visible notice.
3. **"Name each tile" before the Metal raster is fixed?** Recommendation: no. Keep it disabled with
   a stated reason until N2; do not ship a feature that rasterizes text on the render tick.
4. **Windows-authored show files.** Should a Mac open a Windows show's Tiles scene? Recommendation:
   out of scope here (no cross-platform show file exists today); keep the Swift field names and
   defaults identical so it stays possible.

## 10. Out of scope

- Media clips or stills as Tiles members (not supported on Windows either; needs a core change).
- The `tiles-wall:` render-once layer kind and transitions that blend finished bus images.
- Multiple walls per scene.
- ISO encoder placement / scaling (the second half of `docs/corevideo-tiles-iso-scaling-plan.md`).
- Show-engine `GalleryDirector`, OHG gallery, Magic Scene / Auto director use of Tiles.
- The multiview PGM / PVW `programNotice` / `previewNotice` overlay text (multiview workstream).
- Any change to `kTilesStaleFrameMs`, `kTilesHoldWindowMs`, or the 1080P cap of 8.
