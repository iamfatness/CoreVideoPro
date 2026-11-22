# macOS per-source grading Phase 2 (G3-G7): outcomes

Date 2026-11-21 (resumed 2026-11-22 after the machine rebooted mid-Task 9). Branch
`mac-grading-phase2` from `fc675541` (Mac Phase 1, PR #1220). Plan:
`docs/superpowers/plans/2026-11-21-mac-grading-phase2.md`. Nothing pushed; no PR opened.

Rig: Apple M3 Max, macOS 26.3. Other lanes were building on this Mac throughout (load average
3-7 during the resume, up to ~12 the evening before), so **every timing result is UNVERIFIED
under load** (P13).

## Commits

| Commit | Task | What |
|---|---|---|
| `ed67eb1c` | plan | Plan, verified facts, defaults taken |
| `a1318578` | 1 (G3) | One `.cube` verdict on three platforms (shared fixtures, C#, C++, Swift) |
| `a013197b` | 3 (G3) | Test only: a parsed channel-swap `.cube` swaps red and blue on Program (Metal) |
| `796c376a` | 5 (G5, shared) | Grade-preview textures carry an IOSurface id; the shed divisor reaches the compositor |
| `47c710d0` | 6 (G5) | `MetalGradePreview`: private per-source preview and Compare original, off the render path |
| `4445c6cc` | 7 (G6) | Metal histogram / waveform / vectorscope, a 1:1 port of `D3DGradeScopes` |
| `1b388ef6` | 2 (G3) | Curves editor and `.cube` LUT import in the shell |
| `2d0f418c` | 4 (G4) | Grade on the Sources row, toolbar and inspector header |
| `de67524e` | 8 (G5+G6) | Shell preview session, Compare, status lines, scope panel |
| `01ab1299` | 9 (G7) | Scope region of interest: draw, move, resize, keys, precision, per-person persistence |
| `8f0586bb` | 10 | Real-core preview proof + `mac-show-drill.py --grade-previews N [--scopes]` |
| `ee64fb54` | review | The ROI guide takes focus once when a tool is armed, not on every update |

Each commit body lists its tests and its mutation results.

## Test counts (exact commands, at `ee64fb54`)

- Shell: `cd mac-shell && swift build -c release && COREVIDEO_SHELL_TESTS=1 .build/release/CoreVideoProShell`
  → **5907 checks passed, 0 failed** (run three times at head). Baseline at `fc675541`'s
  successor commits: 5718 (Task 1) → 5765 → 5785 → 5835 (Task 8) → 5907.
  `COREVIDEO_SHELL_SELFCHECK=1` passes. `bash scripts/lint-mac-design.sh` passes.
  One run during the mutation pass showed three `ControlApiTests` WebSocket failures (port
  contention with another lane); they did not recur in three clean runs.
- Native (Metal): `native/build-metal/corevideo-native-tests` → **2119 passed, 0 failed** (whole binary, realtime included; built before the toolchain update, no native change since 20:30).
- C#: `~/.dotnet/dotnet test native-shell/CoreVideoPro.MediaCore.Tests --filter "FullyQualifiedName~GradeFixture|FullyQualifiedName~ScopeRoi"`
  → **13 passed, 0 failed**. The full WinUI suite cannot run on a Mac.
- `python3 -m unittest scripts/qa/test_mac_grade_preview_proof.py` → 7 passed.

## Proven (by a test that goes red when the code is wrong)

**G3.** One `.cube` verdict on three platforms from one fixture file; a parsed channel-swap LUT
swaps red and blue on Program (Metal readback); curves editor and LUT import (2 MB cap) in the
shell.

**G4.** Grade entry points on the Sources row, toolbar and inspector header, routed by the
frame key.

**G5.** Private preview and Compare original off the render path: drafts and Compare never
reach Program (Metal readback and the real-core proof); the shell's session follows the
Windows coordinator (full send on a new revision, renew every 500 ms, full resend after a
failure, `enabled:false` on close, reset on a new core generation).

**G6.** Metal scopes are a 1:1 port of `D3DGradeScopes` (1536x512, circle mask); the shell
accepts a measurement only when region, revision, epoch, original/graded and view all match.

**G7 (Task 9).** Ported from `GradeScopeRoi.cs`, `ScopeRoiGeometry.cs`, `ScopeRoiControl.cs`,
`ColorGradeEditorViewModel.Roi.cs`, `ColorGradeEditorWindow.Roi.cs` and
`StudioViewModel.ScopePreferences.cs`:
- the geometry cases from `ScopeRoiGeometryTests.cs` (letterbox/pillarbox, circle round in
  pixels in all four drag directions, 1-pixel minimum at the edge, CircleSize clamp);
- draw / move / corner and edge resize / Escape restores / under-3 px is not a drag;
- arrows move one source pixel, Shift+arrows resize (a circle in both axes);
- the revision rule (a new selection only for a real valid change), wire, Clear, Reset, the
  "centre from full frame" toggle, labels and hint text;
- precision sliders (a circle follows the axis moved) and re-rounding when the picture's shape
  changes;
- persistence: 64 cap, revision 0, invalid/foreign keys dropped on load, seeded on reopen, and
  never written after the source's person changed (key and binding captured once at open).
- Mutations, each red: forget `revision + 1`; let a circle stretch; write after the person
  changed.
- **A port bug the tests caught:** `CGRect.contains` excludes the max edge while WinRT
  `Rect.Contains` includes it, so a region touching the picture's right/bottom edge could not
  be grabbed by its edge handles. Fixed with an inclusive check.

**Task 10, real core** (`python3 scripts/qa/mac-grade-preview-proof.py`, own core + fake engine):
PASS — graded preview centre (0,0,0); Program (255,232,176) before, during and after; Compare
original (255,232,176); scope surface 1536x512, 183 trace samples, status ready, 36864 sample
pixels; 0 events after `enabled:false`.

## Not proven / UNVERIFIED

- **Performance with editors open (owner acceptance step 2).** `--load 8`, six alternating runs
  with 0 and 3 editors + scopes: decoded-frame delivery 86%, 95%, 98% with no editors and 94%,
  99%, 80% with three. Load dominates the signal. Program-buffer underruns were 0 in all six;
  render deadline misses 0-5 with no relation to the editor count. The editors themselves
  worked under load: 1161 ready events, worker 388 submitted / 388 completed / 0 failed /
  0 superseded, last work 1.4 ms. **Must be re-run on a quiet machine** (three runs each way)
  before installer 12.
- **ROI overlay interaction in a real window.** The pure gesture and editor rules are tested;
  the AppKit view (`ScopeRoiOverlay`) has no headless test (mouse, first responder, drawing).
  Owner acceptance step 3 covers it.
- **The proof's Compare check is fragile:** it compares against a Program pixel sampled seconds
  earlier. That held here because the fake pattern's centre is constant; an animated centre
  would fail it for the wrong reason.
- Installed-build behaviour (signed pkg, core run from `/Library/Application Support/CoreVideo
  Pro/`): nothing in this lane loads files from next to the core executable (shaders are
  compiled from source strings; LUTs come over stdio), so the `/Library` rule has no new
  surface here. Unverified on a real install until installer 12.

## Open items

- Merge hotspot: `MetalCompositorAdapter.mm` (+18 lines in `796c376a`/`47c710d0`); other lanes
  also touch it.
- `Interfaces.h` changed (additive), so merges need a clean native rebuild (`--clean-first`).
- Toolchain: since the 2026-11-21 23:32 Command Line Tools update, native links need
  `DEVELOPER_DIR=/Library/Developer/CommandLineTools`; the Swift shell builds with the default
  Xcode toolchain. Not exercised here: the native build was already current.
- Owner acceptance steps 1-4 (plan) are not done; they need a signed installed build.
