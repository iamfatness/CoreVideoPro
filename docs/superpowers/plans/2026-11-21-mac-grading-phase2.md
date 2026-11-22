# macOS per-source colour grading, Phase 2 (G3-G7): implementation plan

Status: executing, 2026-11-21. Branch `mac-grading-phase2` (worktree
`.claude/worktrees/mac-grading-phase2`), based on `fc675541` (Phase 1, PR #1220).

> `docs/superpowers/plan-authoring-rules.md` is binding. Every task is test-first: the test is
> seen RED against the pre-task code before the code lands, and each task names the mutations
> that must turn it red. When a supplied test contradicts a reasonable implementation, the test
> is the first suspect (rule 4, corollary).

**Goal.** The Mac per-source grade editor reaches the Windows workspace: curves and `.cube` LUT
import (G3), the Sources table/toolbar/inspector entry points (G4), a private Metal preview with
Compare original (G5), the three Metal scopes (G6) and a measurable region of interest (G7). The
private preview and scopes never cost Program frames.

**Spec:** `docs/superpowers/specs/2026-11-15-mac-source-grading-scopes-design.md` §3.2, §3.6,
§5.4-5.8, §6 rows G3-G7, §7. Master plan rows: `docs/superpowers/plans/2026-11-15-mac-parity-master-plan.md:90-94`.
Phase 1: `docs/superpowers/plans/2026-11-16-mac-grading-phase1.md` (its "Seam for the Sources
inspector (G4)" table is the G4 contract).

## Verified facts (re-read in code on this branch, 2026-11-21)

Line numbers are for `fc675541`.

1. **Metal has no grade preview.** `ICompositor::supportsGradePreview()` defaults false
   (`native/src/modules/Interfaces.h:1408`); `MetalCompositorAdapter.mm` does not override it, so
   `GradePreviewController::configure` answers `unavailable / native-grade-preview-not-built`
   (`native/src/core/GradePreviewController.h:87`). Spec §2 confirmed.
2. **The `grade-preview` event has no IOSurface id.** `GradePreviewController::emit` writes only
   `sharedHandleHex/width/height/format/frameNumber` for both the preview texture
   (`GradePreviewController.h:257`) and the scope texture (`:255`), and `ParticipantSharedTexture`
   has no `iosurfaceId` field (`Interfaces.h:257-266`), unlike `ProgramFrameSharedTexture`
   (`Interfaces.h:244`). Spec §5.5's wire change is needed. Confirmed.
3. **R1 resolved by reading: grade-preview events reach the Swift bridge.** The core's resource
   pump drains `captureGradePreviewEvents()` and enqueues each on the same structural lane as
   `multiview-shared-texture` (`native/src/rpc/JsonRpcServer.cpp:937-950`); the Swift bridge hands
   every non-reply line to `onEvent` (`mac-shell/Sources/CoreVideoProShell/MediaCoreBridge.swift:294`),
   which `AppModel.handleEvent` switches on (`AppModel.swift:2165-2190`). Lane key is
   `type:participantId:instanceId` (`JsonRpcServer.cpp:516`), latest-per-key. Task 10 proves it on a
   real core.
4. **R6 resolved: `MonitorRenderWorker` runs on macOS.** Its Windows-shaped helpers are no-ops off
   Windows (`core/ComApartmentLifetime.h:17`, `core/ScopedVideoScheduling.h:4-36` all `#ifdef
   _WIN32`), and `tests/MonitorRenderWorkerTest.cpp` is in the common test list
   (`native/CMakeLists.txt:984`), passing on this Mac (7/7). Its `prepareInputs` path
   (`MonitorInputCache`) is what turns a vanished source into `held` with its last frame
   (`modules/MonitorInputCache.h:20-40`), which the D3D grade worker relies on
   (`D3D11CompositorAdapter.cpp:223-238`).
5. **The D3D grade worker is the reference.** One `MonitorRenderWorker`, backend created on first
   non-empty request and reset on an empty one (`D3D11CompositorAdapter.cpp:223-238`); per demand:
   the source drawn with the draft grade or neutral when `compareOriginal`
   (`:1018-1027`), status `ready`/`held` from `unavailableInputs` (`:1011-1014`), `source-unavailable`
   when no frame (`:996-1001`); scopes into a 256x144 target cropped to the ROI with pixel-centre UV
   (`:1197-1229`, `:2524-2532`), then `D3DGradeScopes` (`compositor/D3DGradeScopes.h`). Preview
   exports at the full source size (`:2799-2834`).
6. **The scope programs.** Compute: 16x16 threads over 256x144, circle mask when `modes.w`, bins
   at offsets 0/256/512/768 (histograms), 1024/66560/132096/197632 (waveforms, 256x256), 263168
   (vectorscope 256x256), 328704..328707 (peaks) = 328,708 uints (`D3DGradeScopes.h:9-26`). Peak:
   256 threads, `InterlockedMax` per channel (`:27-32`). Pixel: three panels into 1536x512
   (`:33-72`). Metal has `atomic_fetch_max_explicit` on `atomic_uint` in device memory (MSL 2.0+,
   every Apple-silicon GPU), so R5 needs no fallback.
7. **Curves and cubes already shade on Metal.** `advancedGrade` in
   `compositor/MetalCompositorShaders.h:91-116` applies curves (16x32 RG32F texture) and 3D LUTs;
   the textures are built per document in `MetalCompositorAdapter.mm:1624-1656`. Existing pixel
   test feeds a hand-built `GradeCubeLut` (`tests/MetalCompositorTest.cpp:256-266`), never parsed
   `.cube` text.
8. **Parser rules (three implementations).** Core `parseGradeCube` (`compositor/CubeLut.h:9-41`),
   C# `CubeLutParser.Parse` (`native-shell/CoreVideoPro.MediaCore/Models/CubeLutParser.cs`), and
   the C# document validator calls it for every `cube` op
   (`Models/AdvancedGradeDocument.cs:86-90`). Swift validates only text-present + SHA-256 (Phase 1
   D8, `AdvancedGrade.swift:69-90`), so `cube-malformed` is skipped
   (`ShellTestsGrade.swift:757`). Divergence found: the core caps total bytes, the C# parser caps
   UTF-8 bytes and compares domains as `float`; both accept `TITLE`, `#` comments, one size line,
   optional single DOMAIN_MIN/MAX with |v| ≤ 16, exactly size³ triples, |v| ≤ 16.
9. **Windows curve editor** (`native-shell/CoreVideoPro.WinUI/Controls/GradeCurveControl.cs`):
   hit radius 13 px; click on empty space adds a point when fewer than 16 and no point within
   0.002 in x, x clamped 0.001…0.999; drag moves the selected point; endpoints keep x = 0/1; an
   interior point's x is clamped between its neighbours ± `min((next-prev)/4, 0.000001)`
   (`:80-84`); Home/End select an endpoint, Ctrl+Left/Right step selection, arrows nudge 0.001
   (Shift 0.01), Delete/Backspace removes an interior point, double-click removes; Reset restores
   `[(0,0),(1,1)]`. Channel picker Master/Red/Green/Blue, point input/output sliders, "Reset
   channel" (`GradeAdjustmentControl.cs:20-60`). `+ Curves` and LUT import live in
   `ColorGradeEditorViewModel.Advanced.cs:107-121,181-190`; import caps 2 MB
   (`Views/ColorGradeEditorWindow.Advanced.cs:94-100`).
10. **Windows entry points**: row "Grade" (`Controls/SourcesTableRowControl.cs:74-78`, enabled
    only when assigned, a11y "Grade input N"); toolbar "Grade selected…"
    (`Controls/SourcesWorkspaceControl.cs:170-171,174-178`); inspector header "Grade…"
    (`Controls/SourcesWorkspaceControl.Inspector.cs:24-26`); Picture tab text "Source grading" /
    "Open grade workspace…" (`:139-147`). Mac today has only the Picture-tab "Grade…"
    (`mac-shell/.../GradeEditorWindow.swift:449-459`) and no row or toolbar action
    (`SourcesTable.swift:323-344`, `:170-206`).
11. **Windows preview client** (`Services/GradePreviewCoordinator.cs`): 100 ms tick; a new draft
    revision sends a full `set-grade-preview` (advanced registered first); the same revision renews
    with `renewOnly:true` every 500 ms; a failed send clears `_lastRevision` so the next tick resends
    in full; close sends `enabled:false`; a core recovery resets and shows "Native preview
    reconnecting." (`:41-110`; wire `MediaCore/Services/MediaCoreSupervisor.cs:478-502`). Status
    lines `ViewModels/ColorGradeEditorViewModel.cs:103-111`; scope line and its acceptance rule
    `ColorGradeEditorViewModel.Advanced.cs:49-62`; scopes only demanded while the advanced panel is
    expanded (`ColorGradeEditorViewModel.cs` CaptureDraft, coordinator `:39-40`).
12. **Windows ROI** (`MediaCore/Models/GradeScopeRoi.cs`, `ScopeRoiGeometry.cs`,
    `WinUI/Controls/ScopeRoiControl.cs`, `Views/ColorGradeEditorWindow.Roi.cs`,
    `ViewModels/ColorGradeEditorViewModel.Roi.cs`, persistence
    `ViewModels/StudioViewModel.ScopePreferences.cs`). Core validation `compositor/ScopeRoi.h:13-16`.
13. **The monitor-shed divisor never reaches the compositor.** `MonitorShedPolicy::observe` runs
    in `MediaCore::renderSyntheticTick` (`MediaCore.cpp:6499`) and the divisor only scales
    `kMultiviewTickDivisor` (`:6289`). Grade previews are ticked separately, at most every 100 ms,
    from the render thread (`MediaCore.cpp:6224`, `GradePreviewController.h:126-146`).
14. **The Mac IOSurface presenter** `SurfaceView` re-latches a stable surface at 60 Hz
    (`ShellApp.swift:1299-1352`); a stable per-editor surface id is enough for the shell.

## Defaults taken (owner may override)

| # | Default | Why | Tasks that change on override |
|---|---|---|---|
| P1 | The Metal worker renders **synchronously** on its own thread and its own `MTLCommandQueue`, waiting on its own command buffer, inside the shared `MonitorRenderWorker` (pending capacity 1). Spec §5.7 suggested a completion handler; the worker thread waiting on its OWN buffer touches nothing the render thread owns and keeps the result stamped with exactly the job that produced it. | simplicity; same structure as D3D (`D3D11CompositorAdapter.cpp:223-238`) | T6 |
| P2 | One IOSurface per editor for the preview and one for the scopes, overwritten in place each job (stable `iosurfaceId`), like Program/multiview/preview on Mac today. No double buffering; a 10 Hz private monitor may tear for one refresh. | matches `SurfaceView` (fact 14) | T6, T8 |
| P3 | Preview target = the source's size, scaled down to fit 1920x1080 (aspect kept). Windows exports full size. | spec §5.5 | T6 |
| P4 | **Shedding (spec §5.7 / R2).** Additive shared hook `ICompositor::setMonitorShedDivisor(int)` (default no-op; D3D11 does not override, so Windows behaviour is unchanged); `MediaCore` calls it on each shed transition. The Metal worker then accepts only every d-th preview submit while d > 1, and independently skips the next submit after a job whose GPU time exceeded 4 ms (spec §5.7). A teardown (empty) request is never shed. This is the Mac-only option of spec R2; `GradePreviewController` is NOT changed. | owner rule "never cost Program frames"; keep Windows untouched | T5, T6 |
| P5 | A `lowerThirdPreview` demand answers `unavailable / native-appearance-preview-not-built` (spec §5.5); that preview belongs to the Metal graphics workstream. | spec | T6 |
| P6 | The `grade-preview` texture objects carry `iosurfaceId` **only when non-zero**, so a Windows event is byte-identical (the `ProgramFramePreview.cpp:812-813` rule). | Windows unchanged | T5 |
| P7 | Shell takes the source epoch from a `ready`/`held` preview observation **only when the store holds none** (saves the first `query-source-grade`); the store's query and D7 retry stay the authority. Windows uses only the observation. | spec §5.5 last paragraph | T8 |
| P8 | The curve editor and ROI overlay are AppKit views (`NSViewRepresentable`) because the Mac target is macOS 13 and SwiftUI key handling (`onKeyPress`) is 14+. All geometry is in pure Swift types with tests; the views are thin. Command replaces Control in the key map (spec §5.4). | platform | T2, T9 |
| P9 | Shared cube fixtures live in `contracts/grade.fixtures.json` under a new `cubes` array, read by Swift, the core (`AdvancedGradeTest.cpp`) and C# (`CubeLutTests.cs`). Each row carries `valid` only (messages differ by platform and are not pinned cross-platform; Swift pins the C# messages in its own test). | spec §7 | T1 |
| P10 | Swift parser follows the **C#** parser's messages and number rules (it is the twin the editor copies), and the shared fixtures pin that the core agrees on every row. | Windows parity | T1 |
| P11 | Scope regions persist in `source-grades.json` as `scopeRegions: {stableKey: roi}` (spec §5.9), revision stored as 0, at most 64, only `capture:`/`zoom-person:` keys, invalid rows dropped (`StudioViewModel.ScopePreferences.cs:7-12`). | spec | T9 |
| P12 | G4 keeps the Picture-tab section and adds Windows' copy; the "graded" row mark is a `GRADED` chip next to `KEY` (spec §5.8 item 4, optional). | spec | T4 |
| P13 | Perf/drill numbers measured while five other agents build on this Mac are recorded as **unverified** with the load stated; no gate is loosened. The owner's quiet-machine drill is an acceptance step. | task brief | T10 |

## Task order

```
T1 cube parser + fixtures ─► T2 curves + LUT import (shell) ─► T3 parsed-LUT pixel proof (native)
T4 G4 entry points (shell, independent)
T5 shared wire + shed hook ─► T6 Metal preview worker ─► T7 Metal scopes ─► T8 shell preview/scopes ─► T9 ROI
T10 real-core proof + drill flag (after T8)          T11 outcomes
```

---

### Task 1 (G3): one `.cube` verdict on three platforms

**Files:** `mac-shell/.../CubeLut.swift` (new), `AdvancedGrade.swift` (validate calls the parser),
`ShellTestsGrade3.swift` (new, appended to the case list), `contracts/grade.fixtures.json`
(`cubes`), `native/tests/AdvancedGradeTest.cpp`, `native-shell/CoreVideoPro.MediaCore.Tests/CubeLutTests.cs`.

**Interface.**
```swift
struct CubeLut: Equatable { let size: Int; let domainMin: [Double]; let domainMax: [Double]; let samples: [[Double]] }
enum CubeLutParser { static func parse(_ text: String) throws -> CubeLut }   // GradeValidationError, C# messages
```
`AdvancedGradeDocument.validate()` calls `CubeLutParser.parse` for a `cube` op before the hash
check (C# order, fact 8); `needsCubeParser` skips are removed.

**Fixtures (`cubes`, invariants stated per rule 1).** Each `{name, text, valid}`; every text is
the exact UTF-8 bytes; `valid` is the verdict of all three parsers. Rows: identity-2, title+comments,
domain-min-max, duplicate-size, size-1, size-34, missing-size, short (7 of 8 triples), long
(9 triples), extra-token, value-17, domain-inverted, domain-duplicate, nan, crlf-lines (Windows
line ends; C# splits on `\n` and trims, core `getline` + `>>` tolerates `\r`), channel-swap-2 (the
red/blue swap used by Task 3; valid).

**Tests (must fail first).**
- Swift: every `cubes` row matches `valid`; the C# messages for size, sample count, out-of-range
  and non-finite; `cube-malformed` document is now checked (25 checked, 0 skipped); a
  2,000,001-byte text is refused "LUT files are limited to 2 MB.".
- Native: `AdvancedGrade.CubeFixturesHaveTheSharedVerdicts` runs `parseGradeCube(text, sha256(text))`
  over every row.
- C#: `CubeLutTests.SharedFixturesHaveTheSharedVerdicts`.

**Mutations (each must go red):** drop the sample-count check (short/long rows); allow size 34;
drop the domain-order check; skip the parser in `validate()` (the `cube-malformed` document row).

### Task 2 (G3): curves editor and LUT import in the shell

**Files:** `GradeCurveEditing.swift` (pure), `GradeCurveView.swift` (AppKit), `GradeEditorModel.swift`
(`addCurves`, `setCurve`, `importCube`), `GradeEditorWindow.swift` (panel), tests.

**Interface.**
```swift
struct GradeCurveEditing {            // port of GradeCurveControl.cs (fact 9)
    var points: [GradeCurvePoint]; var selectedIndex: Int
    mutating func press(at p: CGPoint, size: CGSize) -> Bool        // true when the curve changed
    mutating func drag(to p: CGPoint, size: CGSize) -> Bool
    mutating func setSelected(x: Double, y: Double) -> Bool
    mutating func removeSelected() -> Bool
    mutating func key(_ key: CurveKey, shift: Bool, command: Bool) -> Bool
    mutating func reset()
}
// GradeEditorModel
func addCurves()                                   // "+ Curves", 8-op limit
func setCurve(channel: Int, points: [GradeCurvePoint])   // one validated edit, one undo step per gesture
func importCube(text: String, name: String)        // parse, cap, 8-op limit, sha256, select it
```
**Tests:** add-on-click (and the 0.002 refusal, the 16-point cap), endpoints keep x, interior
clamp between neighbours, arrows 0.001 / Shift 0.01, Home/End/Command-arrows selection,
Delete/double-click keep endpoints, Reset; `setCurve` produces exactly the curve the core validates
(document `validate()` passes, Float-collapse refused); a drag is one undo step; `importCube`
selects the new `cube` op with the right SHA and refuses a bad file with the parser's message and
no document change; the D9 "arrive in a later update" line is gone.
**Mutations:** remove the neighbour clamp (an interior point can cross → validation refusal test
red); let an endpoint move in x; skip the 8-op limit on import.

### Task 3 (G3, native test): a parsed channel-swap `.cube` changes pixels by the expected amount

`MetalCompositor.ParsedCubeTextSwapsRedAndBlueOnProgram`: the `channel-swap-2` fixture text →
`parseGradeCube` → per-layer advanced grade → Metal render of a (51,102,153) patch reads
(153,102,51) ± 3; the identity row leaves the patch unchanged ± 3. Also prints the first-use
33³ LUT upload time on the render call (R4) — diagnostic, not a gate.
**Mutation:** swap the fixture's R/B order in the parser (`rgba.push_back` order) → red.

### Task 4 (G4): Sources table row, toolbar and inspector header

**Files:** `SourcesTable.swift`, `SourceInspector.swift`, `GradeEditorWindow.swift`, tests.
`AppModel.gradeSelectedSource(slotId:) -> GradeEditorOpenResult` (shared by all three call sites;
nil/unassigned → "Select a source before editing its color grade"). Row "Grade" (disabled when
unassigned, a11y "Grade input N"), toolbar "Grade selected…" (disabled with no selection or an
unassigned selection), inspector header "Grade…", Picture tab "Source grading" + Windows copy +
"Open grade workspace…", a `GRADED` row chip from `SourceGradeStore.isGraded`.
**Tests:** all three routes open the same editor for the same source id (Zoom raw pid, capture
key); unassigned refuses with the Windows text; the fourth editor refuses; the chip follows the store.
**Mutation:** route the toolbar to the slot's `sourceKey` instead of `gradeSourceId` (Zoom test red).

### Task 5 (G5, shared native — reaches Windows): `iosurfaceId` on the wire and the shed hook

- `ParticipantSharedTexture` gains `uint32_t iosurfaceId = 0` (additive).
- `GradePreviewController::emit` adds `iosurfaceId` to the preview and scope `texture` objects
  only when non-zero (P6).
- `ICompositor::setMonitorShedDivisor(int)`: default no-op. `MediaCore` calls it whenever
  `MonitorShedPolicy::observe` reports a transition (fact 13).

**Tests (GradePreviewTest.cpp, MediaCore test):** a surface with `iosurfaceId` 0 emits a texture
object with exactly the old key set (Windows unchanged); a non-zero id appears on both textures;
a fake compositor sees the divisor change when the shed policy enters and exits.
**Mutations:** always emit `iosurfaceId` (the key-set assertion goes red); never call the hook.
Rebuild `--clean-first` after the struct change (CLAUDE.md output-supervisor gotcha); run the stub
build too (shared code).

### Task 6 (G5, native): `MetalGradePreview` — the private preview and Compare

**Files:** `native/src/compositor/MetalGradePreview.h` / `.mm` (new), `MetalCompositorAdapter.mm`
(four delegating overrides + one member + the shed forward; no other change), `native/CMakeLists.txt`,
`native/tests/MetalGradePreviewTest.cpp` (new).

**Interface.**
```cpp
class MetalGradePreview {
 public:
  static std::unique_ptr<MetalGradePreview> create();          // nullptr when not built with Metal
  void submit(MonitorRenderRequest request);                   // never waits; may shed (P4)
  std::shared_ptr<const MonitorRenderResult> latest() const;
  MonitorRenderDiagnostics diagnostics() const;
  void setMonitorShedDivisor(int divisor);
  uint64_t shedForTest() const;
  static void setRenderStallForTest(std::function<void()> stall);   // test seam, worker thread only
};
```
Behaviour (copying the D3D worker, fact 5): own device queue, library compiled from the same
`kMetalCompositorShaderSource`, own source textures (never the render thread's cache), own
curve/LUT textures; per demand `source-unavailable` / `ready` / `held`; preview drawn with the draft
grade or neutral for `compareOriginal`; stamp epoch, frame id, capture time; resources for an
instance dropped when it is no longer demanded; everything released on an empty request.

**Tests (pixel readback via `IOSurfaceLookup`, ports of `IsolatedMonitorPixelTest.cpp:131-185`):**
BGRA and I420 (full/studio, 709/601) draft preview equals the Program pixel for the same grade ±1
and differs from neutral; Program is unchanged while a draft is previewed; Compare original equals
the neutral Program pixel; a vanished source is `held` with its epoch; a second editor has its own
surface; `submit` returns in < 2 ms while the worker is stalled 300 ms (seam) and the stalled job is
superseded, not queued; shed divisor 3 forwards exactly one of every three submits; an empty
request releases the backend.
**Mutations:** draw the preview with `{}` instead of `demand.grade` (parity red); ignore
`compareOriginal` (compare red); submit synchronously (bounded-time red); drop the shed check.

### Task 7 (G6, native): Metal scopes, a 1:1 port of `D3DGradeScopes`

MSL compute (bins + circle mask), peak (`atomic_fetch_max_explicit`), and the scope fragment shader
into a 1536x512 IOSurface; the 256x144 analysis draw uses the ROI pixel-centre UV rule
(`D3D11CompositorAdapter.cpp:2524-2532`); stamps `scopes.{revision, original, roi, roiPixels,
sourceEpoch, sourceFrameId, status}`.
**Tests (ports of `IsolatedMonitorPixelTest.cpp:263-360`, judged on IOSurface pixels with
independent Rec.709 equations):** neutral/RGB/skin patches land in the predicted histogram,
waveform and vectorscope cells; curves-graded vs `scopesOriginal` move the red histogram peak to
the predicted code; a 1-pixel ROI samples only that pixel; a circle ROI removes corner colours from
all three scopes; Program unchanged throughout.
**Mutations:** drop the circle mask; drop the ROI UV crop; analyse graded when `scopesOriginal`.

### Task 8 (G5+G6, shell): preview session, Compare, status lines, scope panel

**Files:** `GradePreviewSession.swift` (new), `GradeEditorModel.swift`, `GradeEditorWindow.swift`,
`AppModel.swift` (route `grade-preview` by `instanceId`; reset sessions on a new core generation).
Session = the Windows coordinator (fact 11) with an injectable clock: full send on a new revision
(document registered first through the store's helper), renew every 500 ms, resend in full after a
failure, `enabled:false` on close. Editor gains `compareOriginal`, `scopesEnabled`,
`scopesOriginal`, `histogramMode`, `waveformMode`, `scopeView`, `previewRevision`, `previewStatus`,
`previewSurfaceId`, `scopeStatus`, `scopeSurfaceId`, and the Windows status/scope wording.
**Tests:** a draft edit or a compare toggle sends a preview and never `set-source-grade`
(`ColorGradeEditorViewModelTests.cs:68`); the same revision renews, a new one sends in full, a failure
resends in full; close disables; an observation for another instance or an old revision is
ignored; the status lines and scope line are Windows' strings; scopes are demanded only while the
advanced panel is expanded and scopes are on; an observation's epoch seeds the store (P7); the D3
"no private preview" notice is gone when the core reports `supported`.
**Mutations:** renew on every tick (count red); accept an old revision; demand scopes while collapsed.

### Task 9 (G7, shell): region of interest

**Files:** `ScopeRoi.swift` (model + geometry + gesture), `ScopeRoiOverlay.swift` (AppKit),
`GradeEditorModel.swift`, `GradeEditorWindow.swift`, `SourceGradeFile` (`scopeRegions`).
Port `GradeScopeRoi` (valid, pixels, sample count, measurement match), `ScopeRoiGeometry`
(FitImage, Draw, CircleSize), the gesture modes of `ScopeRoiControl.cs` (draw / move / eight
resize handles, 14 px hit band, circle stays round in pixels, Escape cancels), arrow keys move and
Shift+arrows resize, precision sliders, "Reset region to center", "Clear", the editor's
`setScopeRoi` revision rule, and per-source persistence.
**Tests:** geometry cases from the Windows tests (FitImage letterbox, Draw clamp and 1-pixel minimum,
circle round in pixels at 16:9, CircleSize clamp), every resize mode, move clamp, keys, revision bump
only on a real change, scopeRoi on the wire, persistence round trip + 64 cap + invalid row dropped +
a different person's key never written (binding captured once).
**Mutations:** forget `revision + 1`; let a circle stretch; write the region under the live key
after the person changed.

### Task 10: real-core proof and the drill flag

`scripts/qa/mac-grade-preview-proof.py` (+ unit test): its own core and fake engine, one source on
Program, `set-grade-preview` for 101 with a gain-0 document, read the `grade-preview` events from
stdout, `IOSurfaceLookup` the preview id through ctypes, judge pixels (preview black, Program
not), then Compare original (preview not black), then scopes on (scope surface non-empty). Exit 2
on a harness failure. `scripts/mac-show-drill.py --grade-previews N` opens N editors over stdio for
the whole run (with `--scopes`). Run `--load 8` with 0/3 editors under the current machine load and
record as UNVERIFIED (P13).

### Task 11: outcomes

`docs/superpowers/plans/2026-11-21-mac-grading-phase2-outcomes.md`.

## Owner acceptance (signed, installed build; a simulated check is not proof)

1. Grade a camera with a curve and a `.cube` LUT; Program, recording and stream change.
2. Open three editors with preview and scopes during an 8-guest show; drill
   `python3 scripts/mac-show-drill.py --seconds 40 --load 8 --grade-previews 3 --scopes` on a quiet
   machine, three runs, against the no-editor run: zero added underruns/missed deadlines.
3. Match two cameras with the waveform; draw a circle ROI on a face; compare with Windows on the
   same test pattern (spec R8).
4. Load a Windows `.cvgrade` with curves + LUT on the Mac and the reverse.

## Out of scope

Graphics Designer appearance preview on Metal (P5); a Mac-only scope output size (R8); the Windows
re-assert issue (spec R3); any change to `GradePreviewController` cadence.
