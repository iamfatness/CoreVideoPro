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
