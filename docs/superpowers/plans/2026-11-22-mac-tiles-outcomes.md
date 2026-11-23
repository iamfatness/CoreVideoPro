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

## Environment event (2026-11-22 ~19:25)

The App Store began updating Xcode in place to 27.2 mid-session. From then on every `/usr/bin` shim (`c++`, `git`, `swift`, `xcrun`, `python3`) failed with "You have not agreed to the Xcode license agreements", and `Xcode.app`'s `MacOSX26.5.sdk` path disappeared. Workaround used: `DEVELOPER_DIR=/Library/Developer/CommandLineTools` with `SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk` (compiles and links C++, ObjC++ and Metal); Homebrew `python3`. Builds made before the update used the Xcode 26.5 SDK.
