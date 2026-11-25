# macOS parity Phase 3: CoreVideo Tiles — outcomes ledger

Running record for `docs/superpowers/plans/2026-11-22-mac-tiles.md`. Finished in Task 22.

## Baseline (Task 1, 2026-11-22)

- Branch `mac-tiles`, cut from `mac-phase3-plans` at `163c4a3f` (= `origin/main` `e117a8fa` + the committed Phase 3 plans).
- Shell: `COREVIDEO_SHELL_TESTS=1` → **7841 checks passed, 0 failed** (the plan's 7851 was not re-measured at this base; 7841 is the measured number).
- Metal native (`native/build-metal`, CI `native-metal-macos` configure, Release): `ctest -LE realtime` → **2230 passed, 1 failed**. The failure is `MediaTransports.AnAudioBedStalledPastTheBackstopRecoversWhenItsAudioComesBack`, a timing test that fails under load (load average ~11 from other lane agents at the time) and passes 3/3 in isolation. Pre-existing, not caused by this branch.
- Stub build: measured at Task 3 Step 4.

## Verified starting state — re-check at the new base

Every line of the plan's "Verified starting state" was re-read at `163c4a3f`. Lines that moved:

- `main-macos.mm` share-frame copy is at `:1481` (plan: `:1383`); the video copy call is at `:1155` inside `video_on_frame` (`:1092`, unchanged).
- `MetalCompositorAdapter.mm`: library compile fallback `:350-361`, `hasFillColor` `:1026-1035`, `applyTilesDecoration` call `:1165`, glow pass `:1352-1360` — all unchanged.
- No other line moved.

## Metal lane coordination (`MetalCompositorAdapter.mm`)

- The Metal G3-G5 plan (`2026-11-22-mac-metal-graphics-g3-g5.md`, Global Constraints) plans **no** adapter edit; any fix a ported test forces goes inside `drawOverlayLayer` or the graphics facts loop, as its own commit. Its branch `mac-metal-graphics-g3-g5` had no adapter diff against `e117a8fa` when checked.
- Line ranges this lane touches in `MetalCompositorAdapter.mm` are listed per task below.

### Line ranges touched by this lane

| Task | Range (at the commit) | What |
|---|---|---|
| 2 | none | test-only |
| 3 | new `:64-93` (file scope, before `namespace corevideo::modules`): `compileMetalCompositorLibrary` + `metalCompositorLibraryFallbackForTest`; `ensurePipeline` `:380-384` (was `:350-364`): the inline compile + fallback replaced by one call | effect-compile seam, behaviour identical (same retry, same `[tiles] shader effect unavailable` log) |

## Slice N1

### Task 2 — Metal twin of the D3D decoration proof

- `MetalTilesPixels.RoundedMaskInsetBorderAndAnalyticGlowMatchTheD3DProof`: **green on the first run** — Metal already implemented decoration correctly. Thresholds are Windows' and untouched.
- Mutations (each a single change, rebuilt, run, restored):

| Mutation | Result |
|---|---|
| (a) `decorateTile` returns `sampleColor` immediately | red (first failure: `(83,90)` border) |
| (b) border `inner = 1` | red at `(83,90)` |
| (c) glow pass skips its draw | red at `(72,90)` |
| (d) `(1-t)^2` → `(1-t)` | red at `(72,90)` |

### Task 3 — fallback, wall opacity, clip, live background

- New header `native/src/compositor/MetalCompositorLibrary.h` (empty when Metal is off; the ObjC declaration only under `__OBJC__`).
- `ABrokenEffectStillCompilesACleanLibrary` was red at compile (header missing), then green with the seam. The three pixel tests (`WallOpacityBlendsTilesTowardTheBackground`, `ATileClippedToTheWallRectDrawsNothingOutsideIt`, `ALiveBackgroundFrameDrawsUnderTheTiles`) were **green on first run**: Metal already blends wall opacity, scissors the glow to the clip rect and draws a live background feed. No pixel defect found.
- Stub build (`-DCOREVIDEO_STUB=ON`, Metal off): builds, ctest 100% pass — the header compiles to nothing.

| Mutation | Result |
|---|---|
| fallback returns nil instead of retrying | red (`ABroken…`, `compiled` false) |
| glow branch skips `setScissorFromRect` | red (`ATileClipped…`, x=64 G channel green) |
| `layerAlpha = 1` | red (`WallOpacity…`, centre G=255) |

### Task 4 — the wall pixel oracle on Metal, drill `--tiles`

- `node scripts/validate-tiles.mjs --build-dir native/build-metal --participants 3 --keep-artifacts` → **TILES PIXEL ORACLE: PASS** on the first run, with **no script change** (its `darwin` branch already worked). All ten checks passed: fingerprints, per-tile identity, background colour (6% and default spacing), tile boundary vs published rect (≤0.9 px), structural invariants, aspect (every rect, every phase), gutter/margin (0.00 px error), fill-not-letterbox, reflow after genuine staleness (dropped after 3939 ms). Glow and at-rest checks are SKIP in the script itself (its T2/T1 notes). Recording: `native/build-metal/Recordings/CoreVideoPro/validate-tiles/tiles-program-0.mp4`.
- `mac-show-drill.py --tiles [--tiles-style rounded-glow]`: Program and Preview get scene `tiles-a`, `routes: []`, the plan's `tiles` node; the 5-tick re-cue re-sends that Preview wall; the run FAILS unless the snapshot `tiles` node has `len(members) == N` and `heldCount == 0`. `--tiles` without `--load` refuses. `tiles_verdict` checked directly: no node / 1 of 2 members / heldCount 1 all fail, 2 of 2 with 0 held passes.
- Smoke runs (functional only, load average 5-14 from other lanes, **not a perf verdict**): `--tiles --load 2 --seconds 10` PASSED (2 members drawn, heldCount 0, 60.0 fps, 100% delivery). Later runs on the same busy box — plain and rounded-glow alike — drew the full wall every time but failed the decoded-frame delivery gate at 72-84% (plain 81%, 84%; rounded-glow 75%, 80%, 72%; one rounded-glow run passed). Delivery fell for both styles together as load rose, so this is the machine, not the style. The A1 quiet-machine drill decides.

### Task 5 — Mac engine per-frame limited-range normalization (D1) — **UNVERIFIED UNTIL A1**

- `native/zoom-engine/engine/main-macos.mm` only. Each `VideoSubscription` holds an `I420ContentRangeTracker`, a `ZoomRangeExcursionProbe` and a limited-frame counter, under `g_video_mtx`. `video_on_frame` classifies **once per frame**, on the SDK's raw Y plane with the even-cropped `y_len` (exactly like Windows `engine-video.cpp:228-250`), then expands each target's SHM copy in place (`i420_expand_limited_to_full`) before the sequence publish. Same debug lines as Windows: `video_range_classified` on transitions, `video_limited_range_expanded` (first and every 100th), `video_range_excursion` under `COREVIDEO_ZOOM_RANGE_DIAGNOSTICS=1`. `COREVIDEO_ZOOM_RANGE_CORRECTION=0` falls back to the SDK flag alone.
- Share frames: expand when `[data isLimitedI420]`, with `share_limited_range_expanded` — the Windows twin (`engine-share.cpp:326`) uses the SDK flag alone, with no content tracker.
- The core's `VideoFrame::i420FullRange` defaults to `true` for Zoom frames, so the core already assumed full-range input on Mac: limited content was being drawn washed out, and expanding in the engine is the Windows contract (no double expansion).
- Proof so far: (a) shared-header tests green (`I420ContentRangeTracker.*` 4/4, `I420RangeExpand*` 4/4, `ZoomRangeExcursion*` 4/4); (b) the engine builds and bundles (`native/build-engine`, CLT toolchain + CLT `MacOSX26.5.sdk`, see Environment event; `scripts/make-macos-engine-bundle.sh --build-dir native/build-engine --link-sdk` OK). (c) The live count of `video_range_classified` lines and the pulse detector are **A1 step 3** — not run.
- PR #1226 (`mac-zoom-hd-video`) touches `main-macos.mm:236-252` (`onZoomSDKAuthReturn`) only; this task's hunks are `:78-80` (includes), `:949-987` (struct + helpers), `:1157-1185`, `:1224-1243` (video path), `:1535-1541`, `:1577` (share path). No overlap.

### N1 review round 1 (independent review of `163c4a3f..cd5d6ffd`)

Verdict: no correctness bug; parity claims hold. Findings and what was done:

- **MEDIUM — engine range expansion placement (fixed).** Task 5 expanded each target's SHM copy in place *after* the copy, while the target's sequence was odd (a several-fold wider seqlock write window for limited frames), under the global `g_video_mtx` that all feeds `try_lock` (other feeds silently drop frames for the duration), and once per fan-out target. Now, mirroring Windows' `I420RangeNormalizer`: classify once under the lock (sampled, cheap); for a limited frame, release `g_video_mtx`, pack + expand **once** into a per-thread scratch frame (`I420EvenFrame`, new shared header `zoom-engine/shared/i420-even-frame.h`, reallocated only when the frame grows), re-take the lock with `try_lock` and re-find the subscription, then each target only `memcpy`s the prepared frame inside its window. Share frames expand once before `g_share_mtx` is taken. A full-range frame keeps the old direct copy (`copy_i420_even`, now a wrapper over the shared `i420_pack_even`). Test seam: `native/tests/I420EvenFrameTest.cpp` (crop with source stride, expand-once, full-range unchanged, scratch reuse + oversize refusal). The `--load 8 --tiles` drill at A1 is the real gate.
- **LOW 1 (fixed).** `ATileClippedToTheWallRectDrawsNothingOutsideIt` used a symmetric clip probed on the centre row, blind to a y-flip or x/y swap of the scissor. Now clip `{0.2,0.1,0.5,0.4}`, probed inside, left, right, above (the glow band over the tile's top edge) and below.
- **LOW 2 (fixed).** `mac-show-drill.py` refuses `--tiles --media` explicitly.
- **LOW 3 (fixed).** `validate-tiles.mjs` SKIP lines no longer say glow is "not built yet"; they say the oracle does not judge glow/at-rest and name the unit pixel tests that do.
- **LOW 4 (fixed).** `tiles_verdict` path now also FAILS on `blackCount > 0`.
- **LOW 5 (follow-up, not changed).** `MetalGradePreview.mm:138` carries its own clean-shader fallback outside the new `compileMetalCompositorLibrary` seam.

**Lock placement is now a tested seam.** The release / prepare / `try_lock` step is `i420_prepare_with_lock_released` in `i420-even-frame.h`, which `video_on_frame` calls. `I420EvenFrame.TheExpansionRunsWithTheVideoMutexReleasedAndRetakesIt` holds a real `std::mutex` the way the engine does and, from inside the prepare step, has a second thread `try_lock` it (exactly what another feed's callback does): it must succeed, and the lock must be owned again afterwards. `AContendedRetakeOrARefusedFrameDropsTheFrameWithoutTheLock` covers both drop paths.

Verification (2026-11-22, rebased onto `origin/main` `3529d568`; `native/build-metal` reconfigured fresh with `DEVELOPER_DIR=/Library/Developer/CommandLineTools`, `SDKROOT`/`CMAKE_OSX_SYSROOT` = CLT `MacOSX26.5.sdk`):

- `I420EvenFrame.*` 6/6, `MetalTilesPixels.*` 5/5, range tracker / expand tests green. Metal `ctest -LE realtime`: 100% passed.
- Engine (`native/build-engine`, same CLT toolchain it was configured with) rebuilds with the new `main-macos.mm`.

| Mutation | Result |
|---|---|
| helper runs `prepare()` before `lock.unlock()` (expansion under the lock) | red: "another feed's try_lock failed during the expansion" |
| helper never unlocks | red (both lock tests) |
| scissor y flipped (`scissor.y = targetHeight - bottom`) | red: `ATileClippedToTheWallRectDrawsNothingOutsideIt` (`:215`) |

### N1 review round 2 (independent review of `cd03236c`)

- **MEDIUM (fixed): share path expanded before it knew anyone would read the frame.** `share_on_frame` packed and expanded a limited frame (up to ~12 MB at 4K) before `try_lock(g_share_mtx)` and before checking for targets. It now takes the lock, returns if there are no targets, and expands through the same `i420_prepare_with_lock_released` seam as video.
- **Test gap (fixed): the call sites were unpinned.** `main-macos.mm` links the Zoom SDK and is not in the test binary, so calling `scratch.prepare` directly under the lock would have passed every test. New source-level law test `I420EvenFrame.EveryEngineScratchPrepareRunsInsideTheLockReleasingSeam`: every scratch prepare in the engine sits inside the seam's lambda, and there are exactly two (video and share). Mutation: add a direct `shareScratch.prepare(...)` before the seam. Red.
- **LOW (fixed):** comments. `kMaxPreparedYLen` is a sanity bound, not the SHM capacity. A contended re-take drops an already-expanded frame, and that is accepted.
- **LOW (not changed):** glow clipping is proven on the top edge only. The `thread_local` scratch keeps its high-water size, bounded by the largest real frame (4K share ≈ 12 MB per callback thread).
- Engine rebuilds (`native/build-engine`). `I420EvenFrame.*` 7/7.

## Environment event (2026-11-22 ~19:25)

The App Store began updating Xcode in place to 27.2 mid-session. From then on every `/usr/bin` shim (`c++`, `git`, `swift`, `xcrun`, `python3`) failed with "You have not agreed to the Xcode license agreements", and `Xcode.app`'s `MacOSX26.5.sdk` path disappeared. Workaround used: `DEVELOPER_DIR=/Library/Developer/CommandLineTools` with `SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk` (compiles and links C++, ObjC++ and Metal); Homebrew `python3`. Builds made before the update used the Xcode 26.5 SDK.

## Slice S1

### Task 6 — `TilesSettings` model and persistence

- New `TilesSettings.swift` (`TilesRect`, `TilesMemberOverride`, `TilesSettings`, `PersistedTilesSettings`, `TilesHex`), `TilesPolicies.swift` (`TilesMembershipPolicy.normalizeMode` so far), `TilesLayerPayload.swift` (`normalizeAspect` so far). `SceneDef.tiles`, `PersistedScene.tiles`, save, restore (`AppModel.restoredTiles` = `SceneFromPersisted`) and `duplicateScene` carry it.
- `init(persisted:)` follows `FromPersisted` clamp for clamp. Like Windows, only `borderColor`/`glowColor` are normalized on restore; `backgroundColor` passes through (the payload normalizes it, Task 7). Override cap of 64 takes keys in sorted order (Windows takes an arbitrary 64 of its dictionary).
- `PersistedTilesSettings.init(from:)`: every key optional, and a key of the wrong JSON type also falls back to its default (`try?` per key), so one bad `tiles` object cannot empty `ShellPrefs.scenes`.
- Shell: **7905 checks, 2 failed**; the 2 are `ShellTestsMedia.swift:278/286` (a generated `.m4a` probes 1068 ms vs 1000±25). Untouched by this branch; first seen after the macOS 27.2.1 / Xcode 27.2 update, AAC priming differs. Candidate issue. `tiles/` filter: 46/46.

| Mutation | Result |
|---|---|
| `wallOrder = try c.decode(...)` (one key non-optional) | red: partial-object test (every scene lost) + clamp fixture |
| `restoredTiles` drops the legacy `dynamic-gallery` default | red: legacy-layout test |
| shallow copy of settings in `duplicateScene` | n/a: value types, the test is confirmatory (documented in its comment) |

### Task 6 review round 1 (independent review of `e4cce344`)

- **MEDIUM (fixed): a `tiles` value that is not an object dropped every scene.** The decoder's unguarded `container(keyedBy:)` threw, the synthesized `PersistedScene` failed, and `ShellPrefs` fell back to `scenes = []`. Now a non-object keeps the defaults.
- **MEDIUM (fixed): nested values were all-or-nothing.** `TilesRect` and `TilesMemberOverride` now decode key by key (a partial `wallRect` keeps the keys it has; an override missing its crop keys survives). A non-object override is dropped on its own. A bad element in a string list is dropped on its own, and in `manualSlots` it becomes a hole so slot positions are kept.
- **Test gaps (fixed):** the other clamp ends and all three list caps; the save path through `AppModel.currentPrefs()` → JSON → `applyRestoredPrefs`; `duplicateScene` with non-default settings.
- Parity questions checked against the C# and unchanged on purpose: `backgroundColor` is not normalized on restore, override values are not clamped, and `manualSlots` are not blank-filtered. All three match `FromPersisted` exactly.

| Mutation | Result |
|---|---|
| `currentPrefs` saves `tiles: nil` | red: save-and-relaunch |
| the whole-object guard throws again | red: "a non-object tiles keeps every scene" |

### Task 7 — the `tiles` payload, confirmed against the Windows builder

- `contracts/tiles.fixtures.json`: the 5 required cases, `expected` written by hand from the C#.
- **Windows confirmation, run on this Mac:** I compiled the real Windows sources (`TilesLayerPayloadBuilder`, `TilesMembershipPolicy`, `TilesLocalSourcePolicy`, `TilesIdentityPolicy`, `ScenePersistenceService`, `SceneRoutingService`, `MediaCoreCommandBuilder` via the MediaCore project, the new `TilesLayerWire`, and the new `TilesFixtureTests.cs` itself) into a scratch net9 console project. Shims cover only the XAML brush types in `ProductionModels.cs` and the four xUnit asserts. **All 5 cases pass.** Control: changing case 4's `borderThickness` to 31 fails with "expected 31, got 32". The formal gate is still `TilesFixtureTests` on the `native-shell-windows` CI job.
- `native-shell/CoreVideoPro.WinUI/Services/TilesLayerWire.cs`: the flattening from `StudioViewModel.BuildTilesLayerWire`, moved verbatim; the view model now calls it.
- Swift: `TilesMembershipPolicy.resolve`, `TilesLocalSourcePolicy`, `TilesIdentityPolicy.resolveLiveSourceId` (needed now because Windows' builder always passes a roster; the rest of identity is Task 13), and `TilesLayerPayload.build`. `nameEachTile` is always false (D3). `overrides` is always present.
- Swift tests: the fixture twin (`checked == 5`, plus the invariant checks) and the `TilesLayerPayloadBuilderTests` / `TilesLocalSourcePolicyTests` facts ported by name. `tiles/` filter: 131/131.

| Mutation (Task 7) | Result |
|---|---|
| drop the video-off filter | red: fixture cases 1 and 4 plus the `Build_SkipsParticipantsWithVideoOff` fact |
| qualify an already-qualified id | red: `Build_PassesThroughAnAlreadyQualifiedMemberUnchanged`. Case 3 stays green because its `capture:` member arrives through the local-id path, which never qualifies |
| skip the `init(persisted:)` thickness clamp | red: the Task 6 clamp test. Case 4 stays green because the payload's `FiniteClamp` clamps thickness to 32 again. Case 4's ratio 9→4 is clamped only at restore |
| send `nameEachTile` from settings | red: the D3 check |
| omit `overrides` when empty | red: key-set difference in every empty-override fixture case |

### Task 7 review round 1 (independent review of `e0a3c092`)

The reviewer found no behavioural divergence from the C#. Its findings were coverage gaps and one naming difference:

- **HIGH (fixed): membership and identity had no fixture coverage.** The five cases were all eligible mode. Five cases were added and confirmed against the real Windows sources with the local oracle (10/10 pass):
  - `manual-holes-excluded-duplicate`: a hole, an excluded name, a duplicate, a local id, and slots past `maxTiles`.
  - `routed-mixed-ids-and-omitted`: routed mode with local ids, and an omitted name matched case-insensitively.
  - `eligible-ignores-omitted-honours-excluded-and-pins`: omitted ignored in eligible mode, a `zoom-pid:` pin, a missing pid, an excluded `capture:` id, and a bare `zoom:2` that is never seated.
  - `ambiguous-name-seats-nobody`.
  - `wall-rect-degenerate`: width 0, x < 0, height > 1.
- **MEDIUM (fixed): local-source group and name were barely covered.** The tests now cover the SRT, RTMP, NDI, Blackmagic, AJA and webcam groups.
- **MEDIUM (fixed): the fallback name used the slot's display name.** It is now the slot label "Input NN", like Windows `SlotLabel`.
- **LOW (fixed): both harnesses hard-coded 5 cases.** They now require "every case in the file, and at least 10".
- **LOW (not changed): non-ASCII string semantics differ.** Swift grapheme counting and canonical equality versus C# ordinal comparison. Not reachable with Zoom ids.

### Task 8 — the wall in scene sync, the starter and the guards

- `AppModel+Tiles.swift`: `tilesLayout`, `tilesStarterName`, `tilesLayersRefusal`, `isTilesScene` (by settings, not layout word), `tilesPayload(for:)`, `uniqueSceneName`, `newTilesScene`, `refuseLayerEditOnTiles`.
- Call sites:
  - `buildRoutes` returns `[]` for a Tiles scene before any layer or logo-bug logic.
  - `sceneSyncBatch` adds `tiles` to Program `load-scene-graph` and Preview `set-preview-scene`.
  - `syncScenes` sleeps through `timerSleep`.
  - Add layer, Edit layout and Add overlay are refused on a wall with the D5 text.
  - The New scene menu has "New — CoreVideo Tiles · Auto-reflow wall".
  - The `SceneRow` subtitle is the Windows `LayoutLabel` ("dynamic gallery").
- **Two deviations from the plan text, both deliberate:**
  - `ensureLayers` returns silently on a wall instead of setting the refusal. The scene editor calls it on appear, so the refusal would show just from looking at a wall. The explicit actions still say why.
  - `cueSelectedInPreview` (media "cue in preview" appends a layer) is guarded too, with the refusal in `mediaStatus`.
- Tests: the plan's six cases, plus a wall carrying injected layers and a wall on a preset layout.

| Mutation (Task 8) | Result |
|---|---|
| the logo bug rides Tiles scenes (no early `[]` in `buildRoutes`) | red: "a wall on a preset layout sends no routes". On a `dynamic-gallery` layout the mutant is equivalent, because `layoutRects` has no entry for that word. That is why the test also puts a wall on a preset layout |
| drop `tiles` from the Preview command | red: three failures (the members check, the respawn test, and the Preview tiles checks) |
| `syncScenes` back on `Task.sleep` | red: "no scene sync is sent before the harness fires the debounce". It does not hang |

### Task 8 review round 1 (independent review of `09328716`)

- **HIGH (fixed): the wall was not re-sent when the roster changed.** A wall's members are the camera-on roster, but a roster change only re-pushed the spine. Now a roster change, either a camera flipping or someone joining or leaving, re-sends the scene graph whenever a wall is on either bus. The slot-law path already did this through `recomputeFromSlots`. Test: `testARosterChangeResendsAWallOnABus`. The roster arrives first, so the slot law seats both guests; then only a camera flips. Mutation: drop the new re-send and it goes red. A first version used a newcomer, so the slot law re-sent the wall and the mutant survived.
- **MEDIUM (fixed): Volt rundown could resolve a clip route on a wall.** On a preset-layout wall it would point Preview at the wall and then fail. On a wall with leftover layers it would "apply" a clip that never decodes. `voltCandidateLayers` now offers no route on a wall. Mutation: drop the guard and it goes red.
- **MEDIUM (fixed): `newScene` could collide with a scene made in the same second.** It now uses the same uniqueness loop as the Tiles starter.
- **MEDIUM (checked, no change):** moving `syncScenes` onto the timer seam does not disturb the media tests that count `timers.pending`. The full suite is green apart from the two known `.m4a` checks.
- **LOW (fixed):** the `ensureLayers` guard is now pinned on a preset-layout wall. On a `dynamic-gallery` layout that mutant is equivalent.
- **LOW (not changed):** `updateLayer`, `removeLayer`, `moveLayer` and `resetLayers` set no refusal on a wall. They cannot reach the wire, because `buildRoutes` is the single choke point.
- Shell, full suite: **8053 checks, 2 failed**, the known `.m4a` AAC-priming pair.

### Task 9 — S1 live proof against a real Metal core (fake engine)

`ShellTestsTilesLive.swift`, `tiles-live/wall-cuts-and-owns-an-empty-scene`. It is opt-in through `COREVIDEO_SHELL_LIVE_CORE` and `COREVIDEO_SHELL_LIVE_ENGINE`, and otherwise skips loudly. It ran against `native/build-metal` with the fake engine: 4 participants, primary colours, 30 fps, no churn. **11/11 checks passed** on the first run that got that far:

1. `newTilesScene()` puts the wall on Preview. The Preview composite is sampled before the Take, and it showed a picture at all 4 tile centres the Program wall later published.
2. `take()` gave a take record of `verdict cut`, `wall continuous`, `subscriptionChurnDelta 0`, `restartedSources []` and `missingSources []`. The `tiles` node then had 4 members with `layerId tiles:<wall>`, and all 4 Program tile centres showed a picture rather than the background.
3. All frames stop, and after the hold window plus 1 s the `tiles` node is present with `members: []`. Program is `#000000` (0,0,0) at every old tile centre and at the canvas centre, so the empty-plan guard holds and nothing improvises a grid.
4. No "tiles wall on the" routes+wall warning appears anywhere in the snapshot.

**Deviation:** plan step 3 says "Capture off". The fake engine keeps producing frames across `zoom-stop-capture`; only a leave destroys its targets. The proof turns Capture off and then leaves the meeting, which is the only way to make every frame stop. The first run did only Capture off and correctly failed "timed out waiting for an empty wall".

### S1 slice review (independent, `e4cce344..0eac78ba`) and the Windows fixture gate

- **Windows confirmation of the payload fixture:** CI `native-shell-windows` at `09328716`, which includes the 10-case fixture, shows `CoreVideoPro.WinUI.Tests` 2037/2037 passed. `TilesFixtureTests` requires every case in the file and at least 10, so all 10 are Windows-confirmed. It also passed at `e0a3c092` with the first 5. **S1's "not done until CI confirms" gate is met.**
- No HIGH. The reviewer traced wire shape, both buses, Take, respawn, roster change, persistence, solo, multiview, media and Volt paths, and all were correct. Findings:
  - **MEDIUM: the S1 live test cannot see the S2 subscription gap.** The wall seats every camera-on guest, while the S1 spine subscribes only slotted guests; the fake engine's 4 guests are all slotted. This is S2's own subject: Task 10 ports the Tiles tiers, Task 11 wires them, and Task 12 adds a live check with an unslotted wall guest. S1 must not run on a real show without S2, as the spec already says.
  - **MEDIUM (not changed, recorded): an opt-in live test that skips counts as a pass.** This is the existing convention (`graphics-live` does the same). The skip line is printed loudly. A release gate must set `COREVIDEO_SHELL_LIVE_CORE`/`_ENGINE`; A1 does.
  - **LOW (deferred to S4):** "Use Edit Tiles" names a control that arrives in S4, and the Scenes tab shows `sceneEditMessage` rather than `studioCommandStatus`.
  - **LOW (recorded):** the collision assertion has no positive control. The shell never sends routes plus a wall, and I have not verified that the warning reaches the snapshot.
  - **LOW (candidate follow-up):** `automationLowerThirdSource` treats a wall on Program as a preset layout.
  - **LOW (recorded):** a roster that only reorders does not re-send the wall, which only affects tile order.

## Slice S2

### Task 10 — Windows-generated spine fixtures and the source-set port

- `native-shell/CoreVideoPro.MediaCore.Tests/TilesSpineFixtureTests.cs` runs the real `ZoomMediaSpinePayloadBuilder` and writes `expected` under `COREVIDEO_REGENERATE_FIXTURES=1`; otherwise it asserts. It ran **on this Mac**: generated, then green (1/1).
- `contracts/tiles-spine.fixtures.json`: the plan's 7 cases. Every generated `expected` was checked against the invariants:
  - `meeting-audio` priority 0 comes first.
  - Every source id is roster-present.
  - The shortfall equals camera-on sources minus 10: 2, 1, 1 (multiview), and 0 for the rest.
  - The preview notice names the cued guest; the 11th multiview label carries the unsubscribed suffix.
  - Follow-speaker adds nobody, and the talking non-source appears in no list.
  - The sticky id is audio-only, and the camera-off member gets audio without video.
  - The wall background adds 108 at `program-tiles`.
- Swift: `ZoomSourceBudget.resolve`, `decideVideo`, `audioIds`, `speakerCandidateIds` and the purpose predicates, ported verbatim (tier order and the purpose rule). Also `spineParts`, which covers subscriptions, source ids, shortfall, warning, notices and labels. The old `videoEntries` and `audioParticipantIds` are now thin wrappers. `resolveSpineSubscriptions` now reads both buses' walls from the same `tilesPayload(for:)` the scene sync sends. The Mac multiview `kind` `participant-video` counts as Windows' `zoom`.
- Swift tests: the fixture twin (every case, at least 7) and the 8 `ZoomMediaSpinePayloadBuilderTests` facts by name, plus the `LiveCase478` audio fact and the Mac-kind mapping. `tiles/` filter: 211/211.

| Mutation (Task 10) | Result |
|---|---|
| Preview routes above Program Tiles | red: `program-first-at-the-cap` plus the at-the-cap facts, 4 failures |
| follow-speaker routes grant the bus purpose | **equivalent on Mac**. A follow route carries no participant, and the Mac source set takes no talking/directed-speaker input, so there is nobody to grant a purpose to. The fixture `follow-speaker-adds-nobody` and the `AFollowSpeakerRouteGrantsNoFeedAndNoPurpose` fact pin the observable rule: the talking non-source is in no list |
| drop the wall background | red: `wall-background-source` and the both-buses fact |
| cap audio | red: 4 fixture cases plus the camera-off and sticky facts, 6 failures |

Shell, full suite: **8070 checks, 2 failed**, the known `.m4a` pair.

### Task 11 — sticky audio latch, spine additions, re-push triggers

- New `TilesAudioSourceLatch.swift`, a verbatim port of `TilesAudioSourceLatch.Observe`/`Clear`. `AppModel+Tiles` adds:
  - `tilesLatchMeetingState`: nil until a Zoom snapshot with a non-empty `meetingState` arrives on this core generation; reset in `onConnected`.
  - `busCaption`, the Windows `WithBusNotice`.
- The spine build (`resolveSpineParts`) observes the latch **once per build, with the full in-room roster**. The payload gains `sourceParticipantIds`, `videoSubscriptionShortfall`, the Windows over-budget warning, `multiview.programNotice`/`previewNotice` (always present, `""` when clear) and the unsubscribed tile labels. `zoomAudioSourceIds` covers every source, sticky ones included.
- Triggers: `setCapture(false)` clears the latch. `syncSpine` uses the timer seam and is re-pushed after every `pushScenes` and after a successful Take. `MultiviewTileChrome` shows `busCaption` for PGM/PVW.
- `pushSpine` has its own test seam (`spineRequestForTesting`). Harnesses that count scene-sync bodies through `quitRequestForTesting` are not handed spine requests.
- Tests:
  - the 8 `TilesAudioSourceLatchTests` facts by name;
  - `testCueingAWallRepushesTheSpineWithItsMembers` (1, 2 and 3 at `preview-tiles`, sources `[1,2,3]`, notices present);
  - `testACameraOffWallMemberKeepsAudioWhileTheWallIsOnABus`;
  - `testCaptureOffForgetsTheLatch`;
  - `testACoreRespawnResendsBothWallsAndTheSpineAndKeepsTheLatch` (Review Focus 4);
  - `testBusCaptionCarriesTheCoresNotice`.
- `tiles/` filter: 247/247. Full suite: **8106 checks, 2 failed**, the known `.m4a` pair.

| Mutation (Task 11) | Result |
|---|---|
| the latch observes the video-only present set | red: the camera-off member loses audio (2 failures) |
| an unknown meeting state treated as not-in-meeting | red: `AnUnknownMeetingStateTickNeverWipesTheLatch` plus the respawn test (3 failures) |
| no spine re-push after a scene push | red: the cue test (5 failures) |
| the bus caption ignores the notice | red: the caption test |

### Task 12 — churn across a wall Take, and follow-speaker evidence (D6)

Both run in `ShellTestsTilesLive.swift` against the real Metal core with the fake engine, and both pass. The `tiles-live/` filter passes all 18 checks across its 3 cases.

- **`tiles-live/take-and-speaker-change-churn-nothing`.**
  - Setup: 6 participants with `COREVIDEO_FAKE_ENGINE_AUTOSUBSCRIBE=0`, so a member gets frames only through an explicit spine subscription. No slot is in the show, so the wall alone makes the guests sources; this answers the S1 slice review's MEDIUM 1. The wall goes on Preview, settles 3 s, is taken, and then the fake's speaker rotation runs for 15 s.
  - Take record: verdict `cut`, wall `continuous`, `subscriptionChurnDelta 0`. The Program wall drew ≥5 members from spine subscriptions alone.
  - Per-source churn did not move for 101, 102, 103, 105 or 106 (all 0 → 0). Only `participant-video-104` moved, 0 → 1. That is the fake engine's own roster oscillation (guest 104 leaves and rejoins every 10 s), not a speaker change, so the test excludes 104 by design.
- **`tiles-live/follow-speaker-evidence` (D6, before/after for the owner).**
  - Setup: 4 participants with autosubscribe on, so everyone has frames. Only slot 1 is in the show. Program is a two-up scene whose second cell follows the speaker. Each window records 30 s of directed-speaker changes (`activeSpeakerId` in the zoom snapshot).
  - "Before" is the base commit's wire. The spine is identical except that `sourceParticipantIds` is stripped, which is the only D6 wire change, so the same core and engine judge both.
  - **BEFORE (no `sourceParticipantIds`):** `101, 102, 103, 104, 101, 102, 103, 104, 101, 102, 103, 101, 102, 103, 101`. The director followed every talker, including guests on no bus and no slot.
  - **AFTER (this branch, sources `[101, 104]`):** `101, 104, 101, 104, 101`. The director chose only among sources. 104 is a source because the slot law seated it in the show when it rejoined (the fake's oscillation).
  - Assertion: AFTER ⊆ sources. **For the owner:** follow-speaker on Mac now follows only show sources, which is the Windows rule (#478 R1). A guest who is on no bus, no wall slot and no ISO is never directed.

## Slice S3

### Task 13 — `TilesIdentityPolicy` and roster-aware membership

- `TilesIdentityPolicy` is now complete: `StaleEntry`, `person`, `persist`, `resolveLiveSourceId`, `label`, `describeStale` and `clearStale`. The Windows strings are copied exactly ("Unavailable source", "saved Zoom participant", "not in this meeting", "not in the show", "<n>: <label>: <reason>"). `AppModel+Tiles` adds `tilesIdentityRoster` (the full roster) and `currentTilesMeetingId` (the parsed `joinMeetingId`, nil when blank, like Windows `CurrentTilesMeetingId`). `tilesPayload(for:)` now passes both.
- `contracts/tiles.fixtures.json` has 12 cases:
  - Cases 1-10 already covered the plan's `manual-holes` and `routed-mixed-ids` (`manual-holes-excluded-duplicate`, `routed-mixed-ids-and-omitted`).
  - Added `never-show-by-name`, matched case-insensitively.
  - Added `legacy-session-id-in-new-meeting`: a leftover session id seats nobody, and a `zoom-pid:` slot follows the rejoined guest.
  - All 12 pass against the real Windows sources with the local oracle. Windows CI confirms them on the next run.
- Tests:
  - all 9 `TilesIdentityPolicyTests` facts by name, the 3-id theory included;
  - the 8 `TilesMembershipPolicyTests` facts the plan names, the 4 `Local1071_*` included;
  - `testARejoinedGuestKeepsTheirSlotAndADuplicateNameSeatsNobody` (Review Focus 5).
- `tiles/` filter: 315/315. Full suite: **8174 checks, 2 failed**, the known `.m4a` pair.

| Mutation (Task 13) | Result |
|---|---|
| a legacy `zoom:<n>` is applied | red: 7 failures (fixture case 12, LiveCase479, the unbound id, etc.) |
| names match case-sensitively | red: 3 failures (`never-show-by-name`, the routed omitted name) |
| a duplicate name counts as a match | red: 4 failures (the ambiguous-name case, `DuplicateDisplayNames…`, Review Focus 5) |
| local ids resolve against the roster | **equivalent**. Without the early return, a `capture:`/`media:` id falls through every Zoom-prefix branch to `return persisted`, so the result is identical. The early return is kept as documentation and parity |

### S2 slice review (independent, `4d2107c0..e8a025da`)

No HIGH. The reviewer confirmed tier-for-tier parity of the source set, the payload additions, the latch port, and that the latch is observed exactly once per build with the full roster and kept across a respawn. Findings:

- **MEDIUM (fixed): arming ISO never re-pushed the spine.** The footer's "Program + ISOs" picker and the Routing matrix ISO cells changed the source set without a push. `isoRecordingEnabled` now has a `didSet` that calls `syncSpine()`, and `toggleIsoCell` pushes too. Test: `testArmingIsoRepushesTheSpine`. Mutation: drop the `didSet` push and it goes red.
- **MEDIUM (fixed): the follow-speaker evidence could pass with nobody directed.** It now also asserts that AFTER is not empty (no speaker-following deadlock) and that BEFORE is not a subset of the sources, so the evidence must show a difference. Re-run live: 20/20.
- **MEDIUM (recorded): the churn test's speaker half is close to vacuous on Mac.** A speaker change triggers no spine push here, by design: talking changes nothing. The Take half is judged by the core's own `subscriptionChurnDelta`.
- **MEDIUM (fixed in part): the fixtures cannot see the production wiring.** Added `testAnOverBudgetMacMultiviewTileCarriesTheLabel`: the Mac's `participant-video` kind gets the unsubscribed label and the shortfall. Mutation: label only `zoom`-kind tiles and it goes red. ISO wiring is now covered (above), and Take wiring by the live test. A Zoom wall *background* rides `style.backgroundSourceId` on the Mac wire. Windows' record has it top-level, and `resolve` reads both; the hand-built both-buses fact covers the `style` path.
- **LOW (fixed):** `endMeetingAudioSession()` clears the latch, so it never outlives a meeting even without a spine build.
- **LOW (fixed):** `tilesMeetingStateObserved` is set only when the snapshot's roster and `meetingState` are adopted, never from a rejected stale snapshot.
- **LOW (recorded, not changed):**
  - Engine off still lets spine builds run; the clear still forgets camera-off members, which matches the observable behaviour.
  - A roster change with a wall on a bus can push the spine twice with identical content.
  - The Mac spine sends no "Media core is not running"/OAuth warnings, no `directorExcludedParticipantIds` and no breakout filter. That is the Mac shell's existing payload and out of Tiles scope; candidate follow-up.
- **CI fix:** `busCaption` concatenated `String + Substring`, which Xcode 27.2 accepts and CI's older Swift rejects (`mac-shell-tests` failed to compile). Now `String(label[…])`. I found no other instance of the pattern on the branch.
- Full suite: **8177 checks, 2 failed**, the known `.m4a` pair. `tiles/` filter: 318/318. `tiles-live/`: 20/20.

### Rebase onto `origin/main` `1f0075d8` (2026-11-25)

- 41 upstream commits. The Phase 3 plans commit was already on main, so it was dropped.
- Conflicts were resolved by keeping both sides:
  - `native/CMakeLists.txt`: the test list, with `MetalGraphicsBoundsTest` from main.
  - The `ShellTests.swift` case list: the comms cases from main.
  - `AppModel.onConnected`: the comms re-arm from main.
- After the rebase:
  - Shell: 8658 checks, 2 failed (the known `.m4a` pair).
  - `TilesSpineFixtureTests` 1/1.
  - Metal `ctest -LE realtime`: 100% (5/5 ctest entries).
  - Design lint passes.
- Force-pushed; the PR body notes it.

### Task 14 — AppModel Tiles actions (Preview wall only)

- `TilesPolicies.swift`: `TilesMemberChoice`/`TilesMemberChoicePolicy` (`GalleryMemberChoice`; group order Zoom, Camera, Screen, Browser, Media, SRT, NDI), `TilesMemberState`/`TilesWorkspaceMembership` (`SceneWorkspaceMembershipPolicy.Build`, verbatim).
- `AppModel+Tiles.swift`:
  - `TilesEditError`, carrying the Windows strings;
  - `tilesEditTargetId`: the Preview scene only when it is an editable wall, never Program;
  - `tilesMemberChoices`, `updateTiles`, `setTilesMembershipMode` (which also sets `autoFill`);
  - `assignTilesSlot`: the guest moves, is saved as a stable identity, and the wall is bound to the meeting;
  - `setTilesExclusion`, `includeTilesMember` (`WorkspaceIncludeMember`), `clearStaleTiles`;
  - `tilesLiveLocalSourceIds`, `tilesMembershipSummary`, `tilesMemberStates`.
- Tests:
  - the 4 `TilesMemberChoicePolicyTests` facts;
  - the `SceneWorkspaceParityTests` capacity rule plus the manual and pinned rules;
  - `InvalidApiSlotNeverMutates` (0 and 65) and `ApiSlotPassesCanonicalIdentity…`;
  - `CloneDoesNotAliasMembershipIntent` (confirmatory, because these are value types);
  - `testEveryTilesActionRefusesWhenPreviewIsNotATilesScene`;
  - `testAssigningAGuestMovesThemNotDuplicatesThem` (`[nil, nil, "zoom-pid:P2"]`);
  - `testIncludeInManualModeTakesTheFirstEmptySlot`, including a full 64-slot wall;
  - the summary and clear-stale behaviour.
- `tiles/` filter: 371/371. Full suite: **8711 checks, 2 failed**, the known `.m4a` pair.

| Mutation (Task 14) | Result |
|---|---|
| skip the Preview-only guard (edit a wall on Program) | red: 3 failures |
| persist the session id instead of `persist()` | red: 8 failures |
| a reassign keeps the old slot | red: 2 failures |

### S3 slice review (independent; Tasks 13-14)

No HIGH. The reviewer confirmed statement-level parity for identity, membership, member choices, workspace states and the action semantics, and found the Preview-only guard sound. Findings:

- **MEDIUM (fixed): nothing asserted that an edit is re-sent.** `testEveryTilesEditIsSentAndSaved` covers assign, exclude, mode, include and clear-stale. Mutation: drop `syncScenes()` from `updateTiles` and it goes red (5 failures).
- **MEDIUM (fixed): edits were not saved at once.** They relied on the 5 s autosave, so a crash could lose a fresh "Never show". `updateTiles` now calls `saveScenes()`, like the starter, and the same test checks `lastSavedPrefs` (now `private(set)`). Mutation: drop the save and it goes red (5 failures).
- **MEDIUM (fixed): the identity roster was not pinned to the full roster.** `testACameraOffGuestsSavedSlotIsNotStale`. Mutation: a video-on-only identity roster turns red (2 failures).
- **LOW (fixed):** a blank exclude now shows the Windows "Choose a source to exclude or allow."
- **LOW (fixed):** `testTilesMoveAndIncludeCoverEveryPath` covers:
  - a move clearing a raw-id copy and a name-keyed copy;
  - exclusion binding the meeting;
  - include being a no-op in eligible mode;
  - routed include changing only the omitted list;
  - an existing member keeping one slot.
- **LOW (recorded):** `currentTilesMeetingId` reads the typed field like Windows, and no code reads meeting ids today. Case folding is `caseInsensitiveCompare`, not ordinal-ignore-case, and only differs for exotic names.
- `tiles/` filter: 392/392. Full suite: **8732 checks, 2 failed**, the known `.m4a` pair.

## Slice S4

### Task 15 — Edit Tiles, page layout and the workspace shell

- `TabPageLayout.maxWidth(for:tilesEditing:)` and `scrolls(_:tilesEditing:)`. While a wall is being edited, the Scenes tab takes the window's width and stops scrolling as a page. `TabPage` and `TabPageContent` follow these rules.
- `AppModel.editTiles(_:)` is Windows `EditTiles`: Tiles scenes only. It cues the wall on Preview, opens the Scenes tab and sets the status "Editing Tiles in <name>". Program is never touched. `tilesInspectorTab` is the dock's Pivot selection.
- `SceneRow` gets an accent "Edit Tiles" button with the `square.grid.3x3` symbol, inside a Tiles scene's row card.
- New `TilesInspector.swift`:
  - `TilesWorkspace` is the centre column: the scene name in 18 pt, a PVW chip with "Changes ride Preview until Take.", the live Preview monitor at 16:9, and the Program strip with "Take to Program →".
  - `TilesInspectorDock`: 292 pt, panel background, 1 pt border, radius 9, padding 12. It shows the name, "Tiles wall · n selected / m available", segmented Layout | Style | Members tabs, the selected tab in **its own** vertical ScrollView, and the footer "Changes affect this Preview draft."
  - Helpers: `TilesSection`, `TilesHelp`, `TilesSliderRow` (double-click resets to the Windows default).
  - The tab bodies are filled by Tasks 16, 17 and 19.
- `ScenesPane` shows the Tiles workspace in place of the canvas whenever Preview is a wall. "Add layer" and "Reset to preset" are disabled there (D5).
- Snapshot harness: `COREVIDEO_SHELL_TAB_SNAPSHOT_TILES=1` (or `style`/`members`) seeds a wall on Preview. Ran with `COREVIDEO_SHELL_TAB_LIST=Scenes`. **Checked by eye against the layout contract:**
  - two columns, with a 292 pt dock on the right of a flexible centre;
  - 12 pt spacing;
  - no page scroll bar;
  - the dock's own scroll area between the tabs and the footer;
  - the header row unchanged.

  Rendered at 1360×865 and 1920×1180 in the scratchpad. Not committed, because binary snapshots are not kept in the repo.
- Tests:
  - `testEditTilesCuesPreviewOpensTheInspectorAndNeverTouchesProgram`: a non-Tiles or unknown id does nothing, and the status text is exact;
  - `testTheScenesTabStopsScrollingAndWidensOnlyWhileEditingTiles`: a table over every `StudioTab` × editing.
- `tiles/` filter: 451/451.

### Task 16 — Layout tab

- `TilesLayoutTab` follows Windows `SourcesPage.xaml` Layout order:
  - AUTOMATIC LAYOUT: tile aspect ratio with the 7 Windows labels, custom aspect ratio (shown only for custom), tile spacing and outer margin (format `0.###`), and the help text.
  - WALL CAPACITY: maximum visible tiles, and the ceiling help text.
  - "Advanced layout": animate roster changes and animation duration.
  - Everything is disabled with "Queue this scene in Preview to edit it." when no Preview wall is being edited.
- Setters follow the Windows `Gallery*` setters, clamp for clamp, with C# banker's rounding (`Math.Round`) for the integer ones.
- `updateTiles` changes:
  - A **re-entrancy guard**: a re-entrant edit is dropped, never recursed into. This answers the Windows `_galleryEditGuard` stack overflow. The depth is observable for tests.
  - `save: .now | .debounced`. Continuous controls save once, 500 ms after the drag settles, through the timer seam. Discrete actions still save at once.
- Tests:
  - `testLayoutSettersClampLikeWindows`: 0 → 1, 99.6 → 64, 3.4 → 3, 4.5 → 4; 21:9 → 16:9; custom 9 → 4; gutter −1 → 0; margin 30 → 20; duration 50 → 100 and 1234.6 → 1235; a refusal without a Preview wall.
  - `testAFortyTickSliderDragSendsOneSceneSyncWithTheLastValue` (Review Focus 2): 40 requests and a re-entrancy depth ≤ 1. Nothing is sent before the timer fires; then exactly **one** `set-preview-scene` with gutter 3.9, saved once.
- `tiles/` filter: 471/471. Full suite: **8811 checks, 2 failed**, the known `.m4a` pair.

| Mutation (Task 16) | Result |
|---|---|
| the scene-sync debounce does not cancel (a sync per tick) | red: "expected 1, got 40" |
| drop the gutter clamp | red |

### Task 17 — Style tab, presets and colour fields

- `TilesStyleTab` follows Windows `SourcesPage.xaml` Style order:
  - QUICK LOOKS: Clean, Rounded and Broadcast as equal columns, the help text, and the Mac-only D5 logo-bug note.
  - TILE EDGES: shape, radius (shown only when rounded), thickness, and the border colour field.
  - WALL BACKGROUND: the transparent checkbox, the background colour (hidden when transparent), the "Live background" disclosure (filled by Task 20), and wall opacity as a percentage.
  - GLOW: size, colour, and the "Advanced glow" disclosure (intensity, softness).
  - "Reset wall style".
- `applyTilesPreset` / `resetTilesStyle` are `OnTilesPreset` / `OnTilesResetStyle`, ported verbatim. The style setters use the Windows clamps.
- Colours: like Windows, every colour setter normalizes (`backgroundColor` included). `TilesColorField` is a 36 pt swatch with a `ColorPicker(supportsOpacity: false)` popover, beside a hex field. **The hex string is the only truth:** the picker reads `TilesHex.parse(hex) ?? fallback`, where the fallback is black for background and border and white for glow, and writes `TilesHex.format`. The field commits through the normalizer.
- Snapshot of the Style tab (1360 and 1920) checked by eye. The first render wrapped "Broadcast" onto two lines; the preset buttons are now equal one-line columns. The Layout tab was rendered too.
- Tests:
  - `testPresetsAreTheWindowsPresetsAndKeepMembership`: every field, plus membership, overrides, capacity and glow colour untouched;
  - `testResetWallStyleIsTheWindowsReset`;
  - `testTheHexStringIsTheOnlyColourTruth`: 8ae0ca → (138,224,202); picker (1,2,3) → `#010203`; "zz" → `#44C1A1`; blank glow → white; an unparseable border → black; the picker follows a hex edit;
  - `testWallOpacityAndTransparencyReachThePayload`: 40 % → 0.4, transparent on the wire, 140 % ignored.
- `tiles/` filter: 509/509. Full suite: **8849 checks, 2 failed**, the known `.m4a` pair.

| Mutation (Task 17) | Result |
|---|---|
| swap the Rounded and Broadcast radii | red: 3 failures |
| a colour bypasses the normalizer | red |
| the picker keeps its own colour copy | not built as a mutant: there is no stored colour anywhere to drift. The truth test's "the picker follows a hex edit" pins the property |

### Task 18 — S4 pixel proofs

`tiles-live/presets-change-program-pixels` runs against the real Metal core with the fake engine: 4 participants in primary colours. Wall A is on Program and wall B on Preview. Rounded is applied on Preview and taken, then Broadcast is applied to the other wall and taken. Program pixels are read at `tiles.members[0].rect`. **5/5 passed.**

The measurements:
- **Rounded** (2 px `#657D87`, radius 24): corner (2,2) is `(0,0,0)`, the background.
  - Scanline across the left edge at mid-height (x = 28.08): `bg bg bg (80,100,108) (90,113,147) (26,43,222) tile…`.
  - A 2 px border at a fractional position, with the shader's ~1 px anti-aliasing ramp on each side, has **no pure pixel**. The best one is 12 off the border colour.
- **Broadcast** (5 px `#8AE0CA`, radius 12, 12 px white glow):
  - Left edge: three pure `(138,224,202)` pixels, then the anti-aliasing ramp into the tile. 6 px outside the edge is `(72,72,72)`, the glow.
  - Corner (2,2) is `(183,183,183)`. The corner is masked off the tile, but the glow, which is drawn outside the tile shape, fills it.

**Plan fixture corrections (recorded as deviations; Windows thresholds are untouched).** The plan's Task 18 numbers were hand-written, not Windows-derived:
- "The corner equals the wall background" holds only without glow. With Broadcast the test asserts a neutral grey glow corner, never the tile's colour.
- "The border colour within 6 at 1 px inside" holds for the 5 px border. For the 2 px border the best of the first two pixels must be within 14, because anti-aliasing makes a pure pixel impossible at that width.
- I treated the measurements as evidence of correct rendering, not as a defect. They are consistent with Task 2's Windows-derived decoration proof (border 8 px pure at its centre) and with the glow being clipped only by the wall rect.

Tab snapshots of Layout and Style were rendered with the Task 15 harness and checked by eye (Task 17 entry). The S4 independent review is next.

### S4 slice review (independent; Tasks 15-18)

No HIGH. The reviewer confirmed that Layout and Style match the XAML in order and labels, that clamps, rounding, presets, reset, the hex normalizer and the picker fallbacks all match, and that the 292 pt dock and the scrolling rules are right. Findings:

- **MEDIUM (fixed): a ColorPicker inside a popover.** On macOS that well drives the shared colour panel, and a transient popover would close and orphan it. The well is now inline, at its native size, beside the hex field. Re-checked in the snapshot.
- **MEDIUM (fixed): every colour-panel tick wrote the prefs file.** The picker path now saves debounced. The text field still saves at once.
- **MEDIUM (fixed): the hex field committed only on Return.** It now also commits on focus loss, through `@FocusState`, like Windows' LostFocus. An external change does not overwrite a draft being typed.
- **MEDIUM (fixed): Edit Tiles on the Program wall cued Program onto Preview and opened nothing.** It now refuses with "<name> is on Program. Queue it in Preview to edit it." and leaves Preview alone. A test was added.
- **LOW (fixed): the re-entrancy test could not fail.** The 40-tick test now also drives a genuinely re-entrant edit. The inner edit is dropped, the outer one lands, and the depth stays 1.
- **LOW (fixed): one scroll view served all three tabs.** It is now keyed by tab (`.id`), so each tab keeps its own scroll position, like Windows' one ScrollViewer per PivotItem.
- **LOW (fixed):** wall opacity no longer has a double-click default; Windows sets none. `TilesSliderRow.defaultValue` is now optional.
- **LOW (recorded, not changed):**
  - The dock's disabled state is unreachable, because the dock renders only for an editable Preview wall; a read-only dock for an on-air wall is S6.
  - The debounced save cannot lose an edit on quit (`persistPrefsNow`) or on respawn; only a crash inside the 500 ms window can.
  - The live preset test does not check Clean or the spacing values on Program.
  - Nothing exercises the views themselves beyond the snapshots.
- `tiles/` filter: 513/513.
