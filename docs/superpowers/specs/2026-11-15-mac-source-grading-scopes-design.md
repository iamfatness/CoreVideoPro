# Per-source colour grading, looks, LUTs, curves and scopes on macOS

Status: DRAFT design for owner review, 2026-11-15.

Covers consolidated gap row **2.5** (`docs/superpowers/audits/2026-11-15-mac-ui-parity-consolidated.md:51`)
and Sources audit rows **33, 61-73** plus the grade half of row 82
(`docs/superpowers/audits/2026-11-15-mac-ui-parity-sources.md:88,131-143,157`). Owner priority #2,
second half. The Sources table and inspector (row 2.4) are a separate spec; section 5.8 states the
seam. The roadmap (`docs/superpowers/plans/2026-11-14-mac-parity-1.0-roadmap.md`) has no milestone
for grading; this spec proposes one.

All file:line references are to the worktree `mac-tier0b` as read on 2026-11-15. The audit's Mac
line numbers were taken on another branch and do not match this tree; the ones below do.

## 1. Why

A Mac operator has one grade for the whole programme. Four sliders move every source at once
(`mac-shell/Sources/CoreVideoProShell/ColorGrade.swift:32-38`, `AppModel.swift:2018-2028`), so one
camera that is too warm cannot be corrected without making every guest cold. On Windows the
operator presses Grade on a source row and gets a per-source workspace: built-in looks, a stack of
primaries and curves, imported `.cube` LUTs, presets, a private preview with compare, and three
scopes with a measurable region.

Who notices: anyone matching two cameras, anyone correcting one badly lit Zoom guest, and anyone
trained on the Windows build. The owner ranked it second.

## 2. What the code says about the split (verified)

The audit says "basic grade is shell-only; looks, LUT and curves are shell-only; scopes, ROI and
compare are native". The pixel half of that is right. The control half is wrong for Zoom sources.

| Claim | Verdict | Evidence |
|---|---|---|
| Metal applies the basic four axes and named looks per layer | True | `native/src/compositor/MetalCompositorAdapter.mm:1241-1242`; shared translation `native/src/compositor/ColorGradeParams.h:14-33`; shader `MetalCompositorShaders.h:119-125` |
| Metal applies the advanced stack: primaries (exposure stops, contrast, pivot, saturation, temperature, tint, lift, gamma, gain), curves, 3D cube LUTs, per-operation and global intensity, bypass | True | constants and textures bound at `MetalCompositorAdapter.mm:1243-1246`; curve texture `:1624-1639`; 3D LUT texture `:1641-1656`; shader `MetalCompositorShaders.h:82-117`; pixel test `native/tests/MetalCompositorTest.cpp:222` |
| A per-layer grade replaces the scene grade, it does not stack on it | True on Metal | `MetalCompositorAdapter.mm:1241` |
| `set-source-grade` is accepted on Metal | True | refused only for the software renderer, `native/src/core/MediaCore.cpp:1244-1245`; dispatch `native/src/rpc/JsonRpcServer.cpp:291-292` |
| The shell can send `set-source-grade` today with no native work | **False for Zoom sources** | see below |
| Private preview, compare and scopes need native work | True | `supportsGradePreview()` defaults false (`native/src/modules/Interfaces.h:1408`), only D3D11 overrides it (`native/src/modules/D3D11CompositorAdapter.cpp:269`); scope shaders are HLSL in `native/src/compositor/D3DGradeScopes.h` |

**The identity fence.** `set-source-grade` must carry the source's current `sourceEpoch` and the
grade's `expectedRevision`; a wrong epoch is refused with `source-identity-changed`
(`native/src/core/SourceGradeState.h:36-48`). On Windows the editor learns the epoch from the first
`grade-preview` event and refuses to apply before one arrives
(`native-shell/CoreVideoPro.WinUI/ViewModels/ColorGradeEditorViewModel.Apply.cs:23-32,44`). On Metal
that event is only ever `unavailable` with `sourceEpoch` 0
(`native/src/core/GradePreviewController.h:86`), and no other wire message carries the epoch for a
plain source (the only other emitters are D3D-only texture events,
`native/src/modules/ProgramFramePreview.cpp:839`).

- Zoom frames carry a non-zero epoch that changes when the stream's size or generation changes
  (`native/src/modules/ZoomEngineRuntime.cpp:880,1578-1582`). A Mac shell cannot know it.
- Mac capture frames never set an epoch (no `sourceEpoch` in `AvfCaptureDeviceAdapter.mm` or
  `SckScreenCaptureAdapter.mm`), so it is 0 and a request with `sourceEpoch: 0` is accepted.

So capture sources are shell-only today; Zoom sources need one small shared-core addition (slice
G0). This contradicts the audit and is the main surprise of this spec.

## 3. Windows behaviour to match

Paths are under `native-shell/CoreVideoPro.WinUI/` unless stated. `CGE` =
`Views/ColorGradeEditorWindow.xaml`, `VM` = `ViewModels/ColorGradeEditorViewModel`.

### 3.1 Target, keying and entry points

- A grade belongs to a **source**, not a slot or a scene. The id is the Zoom participant id, or
  `capture:<deviceId>` (`ViewModels/ShowInputSlotViewModel.cs:474-477`;
  `ViewModels/StudioViewModel.Grading.cs:163-191`).
- Entry points: row "Grade" (`Controls/SourcesTableRowControl.cs:76-79`), toolbar "Grade
  selected…" (`Controls/SourcesWorkspaceControl.cs:73,170-173`), inspector header "Grade…" and the
  Picture / Grade tab's "Open grade workspace…" (`Controls/SourcesWorkspaceControl.Inspector.cs:24,142-150`).
- With no source: "Select a source before editing its color grade". At most three editors: "Close
  one of the three grade editors before opening another" (`StudioViewModel.Grading.cs:66-76`). The
  core enforces the same capacity (`GradePreviewController.h:19`).
- The remote-control library has no grade action (grep of `CoreVideoPro.Control` finds none), so
  this spec adds none.

### 3.2 Controls and ranges

- Basic panel: Built-in look (`none`, `neutral`, `warm-film`, `cool-broadcast`, `punch`;
  `VM.cs:12-13`, `CGE:51`) and Exposure, Contrast, Saturation, Temperature, each −50…+50 in whole
  "points" (`CGE:52-55`). The core accepts ±100 (`MediaCore.cpp:163-165,1247-1249`) and rejects an
  unknown look (`:1250-1251`).
- "Expand / collapse grade" switches to the advanced panel (`CGE:48`). Expanding turns live editing
  off and shows "Draft — Apply Live sends this grade to the show" (`VM.Advanced.cs:73-77`).
- Advanced document: Bypass, overall intensity 0-100 %, up to eight adjustments, "+ Primaries",
  "+ Curves", Up / Down / Duplicate / Remove (`CGE:60-73`; limit `VM.Advanced.cs:119`).
- Primaries ranges (`Controls/GradeAdjustmentControl.cs:29-32`; the same bounds are enforced in
  `CoreVideoPro.MediaCore/Models/AdvancedGradeDocument.cs:80-83` and in the core,
  `native/src/compositor/AdvancedGrade.h:52-56`): exposure −8…8 stops, contrast 0…4, pivot 0…1,
  saturation 0…4, temperature −1…1, tint −1…1, lift −1…1, gamma 0.1…4, gain 0…4; per-adjustment
  enabled and intensity.
- Curves: Master, Red, Green, Blue; 2-16 points each, strictly increasing x, fixed endpoints at
  x = 0 and x = 1, linear interpolation; click adds, drag shapes, Delete removes an interior point,
  arrows nudge 0.1 % (Shift 1 %), numeric point input/output (`GradeAdjustmentControl.cs:20-22,36`;
  `Controls/GradeCurveControl.cs`; validation `AdvancedGradeDocument.cs:90-103`).
- Import LUT: a `.cube` file up to 2 MB becomes a `cube` adjustment carrying the file text and its
  SHA-256 (`Views/ColorGradeEditorWindow.Advanced.cs:94-100`; `VM.Advanced.cs:181-190`). Parser
  rules: `LUT_3D_SIZE` 2-33, optional `DOMAIN_MIN/MAX`, exact sample count
  (`native/src/compositor/CubeLut.h:9-41`). A grade embeds at most 1.5 MB of LUT text
  (`AdvancedGradeDocument.cs:74`).
- Undo / Redo (100 deep, one entry per slider gesture), Reset (`VM.Advanced.cs:16-23,144-154`).
- Copy / Paste as JSON `{presetVersion: 1, grade}`; Save / Load preset as a `.cvgrade` file with
  the same content, 16.5 MB cap (`VM.Advanced.cs:156-180`; `ColorGradeEditorWindow.Advanced.cs:73-93`).
  A paste is one undo step and cannot leak intermediate values to air (`VM.Advanced.cs:169-172`).
- There is **no looks library** beyond the five built-ins and preset files, and **no A/B** beyond
  "Compare original (monitor only)" (`CGE:47`). The brief's "looks library, A/B compare" map to
  these.

### 3.3 Live, draft, apply, discard

- "Apply edits live" (default on in Basic) sends each change; off makes edits a draft
  (`CGE:46`; `VM.cs:57-63`).
- "Apply Live" sends the draft and stays open; "Apply and Done" sends and closes; "Close / discard
  draft" closes without sending (`CGE:104,109-110`; `VM.cs:129-134`).
- One apply is in flight at a time; newer edits replace the pending one (`VM.Apply.cs:33-60`). The
  window cannot close mid-apply (`Views/ColorGradeEditorWindow.xaml.cs:58`).
- Refusals keep the previous live grade and say why: "Grade was not applied: {reason}. Previous
  live grade retained." (`VM.Apply.cs:50`). An epoch change disables apply: "Source identity
  changed. Close and reopen its grade editor before applying." (`:25-27`). A revision moved by
  another editor: "Live grade changed since this draft opened…" (`:30`).
- Tests: `CoreVideoPro.WinUI.Tests/AdvancedGradeEditorTests.cs:13,28,39,49,57,63,72`;
  `ColorGradeEditorViewModelTests.cs:14,68,81`.

### 3.4 Wire

- `register-grade-document {documentRef}` asks whether a document is cached; if not, the same
  command with `documentJson` uploads it. `documentRef` is the SHA-256 of the exact JSON text
  (`CoreVideoPro.MediaCore/Services/MediaCoreSupervisor.GradeDocuments.cs:27-48`;
  `JsonRpcServer.cpp:278-290`). The cache is weak plus the 16 most recent
  (`AdvancedGrade.h:13-29`), so Windows registers before every apply.
- `set-source-grade {sourceId, sourceEpoch, expectedRevision, grade:{lut, exposure, contrast,
  saturation, temperature, advanced:{documentRef}}}` answers `{accepted, revision, sourceEpoch}`
  or `{accepted:false, reason[, revision]}` (`MediaCoreSupervisor.GradeDocuments.cs:11-25`).
  Reasons: `native-source-grade-not-built`, `missing-grade`, `invalid-basic-grade`,
  `unsupported-look`, `invalid-advanced-grade` (`MediaCore.cpp:1245-1253`), `invalid-source-grade`,
  `source-identity-changed`, `grade-revision-conflict`, `grade-capacity` (256 sources)
  (`SourceGradeState.h:39-47`).
- The core applies an accepted grade to every plan layer whose source matches, on Program and on
  the multiview plan (`SourceGradeState.h:50-59`; `MediaCore.cpp:4087,5249`). If the live frame's
  epoch no longer matches, the layer is forced neutral (`SourceGradeState.h:58`). Test:
  `native/tests/AdvancedGradeTest.cpp:38`.
- Windows also attaches the grade to matching scene routes (`StudioViewModel.Grading.cs:213-243`),
  which is how a grade survives a core respawn there.

### 3.5 Persistence

- Saved per **stable identity**: `capture:<deviceId>` or `zoom-person:<persistentId>`; at most 64,
  oldest retired with a notice (`StudioViewModel.Grading.cs:16-17,24,149-153`); stored in
  `SourceGrades` and `ScopeRegions` (`Services/ProductionOutputPreferencesStore.cs:222-223`);
  saved 750 ms after the last change (`StudioViewModel.Grading.cs:38-47`).
- A session grade is bound to `persistentId:sourceGeneration` and is dropped when that binding
  changes (`:18-19,205-211`).
- After a relaunch a saved grade is **not** applied automatically. Opening the editor loads it as a
  draft: "Saved grade loaded as a draft. Apply Live confirms this source binding." (`:87-90`).
  Whether a scene-persisted route grade re-applies at launch is UNVERIFIED.

### 3.6 Preview, compare and scopes: where the work runs

- The shell sends `set-grade-preview {instanceId, sourceId, revision, enabled, grade, scopesEnabled,
  scopesOriginal, compareOriginal, histogramMode 0-1, waveformMode 0-2, scopeView 0-3, scopeRoi}`
  and renews it every 500 ms (`Services/GradePreviewCoordinator.cs:41-65`); the lease is 3 s
  (`GradePreviewController.h:21`), so a crashed editor cannot leave demand alive.
- On the render tick the core only builds a request at most every 100 ms and hands it to the
  compositor (`GradePreviewController.h:22,126-146`; `MediaCore.cpp:6219`). The request holds the
  demanded sources' frames by reference.
- D3D11 renders it on a **separate worker thread with its own D3D device**, pending capacity 1,
  created on first demand and released on the worker when demand ends
  (`D3D11CompositorAdapter.cpp:223-240,270-280`). Program's device context is never used.
- Preview: the source drawn with the draft grade (or neutral for compare) into an exported
  texture (`:1042-1061`). Compare never touches Program
  (test `ColorGradeEditorViewModelTests.cs:68`).
- Scopes: the source is drawn, graded or original, cropped to the ROI, into a 256x144 target
  (`:1197-1212`); a compute shader bins it (4 histograms, 4 waveforms of 256x256, one 256x256
  vectorscope; circle mask for a circular ROI), a second finds the histogram peak, and a pixel
  shader draws one 1536x512 image of three panels with graticules and 75 % colour targets
  (`D3DGradeScopes.h:9-72,83-113`). No CPU readback anywhere.
- Modes: histogram Luma / RGB overlay; waveform Luma / RGB overlay / RGB parade; Expand scope: All /
  Histogram / Waveform / Vectorscope; Analyze original (`CGE:37,88-89,102-103`).
- ROI: draw rectangle or circle, select / move, clear, four precision sliders, arrow keys move and
  Shift+arrows resize, circle stays round in pixels, guide is editor-only, "The grade applies to
  the whole source" (`CGE:14-19,90-100`; `Views/ColorGradeEditorWindow.Roi.cs`;
  `Controls/ScopeRoiControl.cs`; validation `native/src/compositor/ScopeRoi.h:13-16`).
- Status the operator sees: ready / "Source is not advancing — last native image held" (after 1 s,
  `GradePreviewController.h:160,169`) / "Source unavailable" / preparing / unavailable with the
  reason (`VM.cs:103-111`), and a scope line naming graded or original, the region in pixels, the
  sample count and "Rec.709 SDR assumed" (`VM.Advanced.cs:56`).
- Tests: `native/tests/GradePreviewTest.cpp` (control, platform-neutral, runs on Mac);
  `native/tests/IsolatedMonitorPixelTest.cpp:230-234,263,309,332,464` (pixels, D3D-only).

## 4. Mac today

| Item | State | Evidence |
|---|---|---|
| Programme-wide grade, four sliders ±10 step 0.1, Reset, always live, 60 ms coalesce | Exists | `ColorGrade.swift:27-61`; `AppModel.swift:2001-2033` |
| Persisted and re-asserted on each core generation | Exists | `ShellPrefs.swift:134`; `AppModel.swift:608,681-685,1496-1498` |
| Entry points | Studio "Grade" chip and control `open grade` only | `ShellApp.swift:146-147,187,791` |
| Stale comment: says the core clamps to ±10 | Wrong; the core clamps to ±100 | `ColorGrade.swift:46-48` vs `MediaCore.cpp:163-165` |
| Per-source grade, looks, advanced stack, curves, LUT import, presets, undo, draft | Missing in the shell | no `set-source-grade` or `register-grade-document` in `mac-shell` (grep) |
| Zoom `persistentId` | Published by the core, not parsed by the shell | `native/src/modules/ZoomEngineState.cpp:405`; `native/zoom-engine/engine/main-macos.mm:498-501,558`; no match in `mac-shell` (grep) |
| Private preview, compare, scopes, ROI | Missing natively | section 2 |
| Snapshot `gradePreview` diagnostics node and `source-grade-preview` capability | Shared, published on Mac with `supported:false` | `native/src/core/CoreStateCapture.cpp:258,941` |
| Metal pixel test for the basic axes and named looks | None found (advanced only) | `MetalCompositorTest.cpp:222` |

Shared and already working on macOS: grade parsing and validation, the document cache, the cube
parser, `SourceGradeState`, `GradePreviewController`, per-layer shading on Metal. Windows-only:
the grade worker, the preview export, `D3DGradeScopes`, and the appearance-preview branch that
shares the same channel for the Graphics Designer.

## 5. Design

### 5.1 Components

Shell (Swift, new files in `mac-shell/Sources/CoreVideoProShell/`):

- `SourceGrade.swift`: Codable twins of the Windows `ColorGrade` and `AdvancedGradeDocument`
  (version 2, `rec709-sdr`), with the same validation, plus a `.cube` validator. JSON field names
  are exactly those in `AdvancedGradeDocument.cs`, so presets and clipboard text move between
  platforms unchanged.
- `SourceGradeStore.swift`: owns saved grades by stable key, session grades by live source id,
  the applied revision and epoch per source, and the apply queue. It is the only sender of
  `set-source-grade`.
- `GradeEditorModel.swift`: one per open editor (maximum three). Draft, live toggle, undo / redo,
  adjustment stack, preview lease, scope options, ROI.
- `GradeEditorWindow.swift`, `GradeCurveView.swift`, `ScopeRoiOverlay.swift`: the views.
- `GradeInspectorSection.swift`: the block the Sources inspector hosts (section 5.8).

Native:

- Shared core: grade identity on the wire (section 5.2).
- `native/src/compositor/MetalGradePreview.mm` (new): a worker that renders preview and scope
  surfaces off the render thread (sections 5.5-5.6).

### 5.2 Grade identity without a preview (shared core, new)

Two additions, both platform-neutral and both read-only:

1. New request `query-source-grade {sourceId}` answering
   `{present: bool, sourceEpoch: number, revision: number}`. `present` is false when the core has
   no frame for that id. It reads the epochs `SourceGradeState::observe` already keeps
   (`SourceGradeState.h:31-34`).
2. New snapshot node `sourceGrades`, published unconditionally (empty array when there are none):
   `[{sourceId, revision, appliedEpoch, liveEpoch, active}]` for sources that have an applied
   grade. `active` is false when the layer is being forced neutral by an epoch mismatch.

The first lets the shell make its first apply. The second makes an applied grade observable (to the
shell and to `/snapshot`-driven QA) and tells the store when a grade has gone neutral under it.
Both names are new; adding the request means updating the protocol lists in
`native/src/core/Protocol.h` and whatever `native/tests/ContractParityTest.cpp` compares them with.

### 5.3 Apply flow

1. Editor asks the store to apply grade G to source S.
2. The store verifies S's stable identity is the one the grade was authored for (capture device id,
   or Zoom `persistentId`). This is the shell's half of the fence: the epoch only proves the stream
   did not change, the stable id proves the person did not.
3. If the store holds no epoch for S, it sends `query-source-grade`. `present:false` ends with
   "Source has no video yet; grade not applied."
4. If G has an advanced document: `register-grade-document` (query, then upload if absent).
5. `set-source-grade` with the held epoch and revision. On acceptance the store records the new
   revision. On `source-identity-changed` or `grade-revision-conflict` it re-queries once,
   re-checks step 2, and retries once; a second refusal is shown to the operator verbatim in the
   Windows wording.
6. One apply per source is in flight; a newer request replaces the pending one. No timer debounce
   is needed beyond that, and no apply rides the repeating sync channel.

Re-assertion. Windows relies on scene routes to carry a grade across a core respawn; the Mac shell
does not send per-route grades, so the store re-applies every session grade when the bridge reports
a new core generation (the existing hook at `AppModel.swift:1496-1498`), and when `sourceGrades`
reports `active:false` for a source whose stable identity is unchanged. The second case is a
deliberate improvement on what the Windows code appears to do (see risk R3).

### 5.4 Editor behaviour

Parity with section 3.2-3.3, with these Mac specifics:

- Basic sliders become −50…+50 step 1, matching Windows. Units are unchanged, so existing saved
  values stay valid.
- Until the native preview exists (slice G5), there is no private picture. The editor says so:
  "No private preview on this Mac build yet. Applied edits go to air." Draft mode still works
  (edit, then Apply Live) but is blind; Compare, scopes and ROI controls are hidden, not disabled.
- File dialogs use `NSSavePanel` / `NSOpenPanel` with the `.cvgrade` and `.cube` extensions and the
  Windows size caps. Clipboard uses plain text.
- Curve editor keyboard map follows Windows, with Command in place of Control.
- Every control has an accessibility label.

### 5.5 Native preview and compare on Metal

`MetalCompositor` overrides `supportsGradePreview`, `submitGradePreviews`, `latestGradePreviews`
and `gradePreviewDiagnostics`, delegating to a `MetalGradePreview` object that reuses the shared
`MonitorRenderWorker` (pending capacity 1, superseded jobs counted) if it runs cleanly on macOS,
or an equivalent single-slot thread if not (UNVERIFIED: the worker header includes
`core/ComApartmentLifetime.h` and scheduling helpers; its test is in the common test list).

Per job, for each demand (at most three):

- Find the demanded frame in the request. Mac frames are CPU buffers (I420 or BGRA,
  `MetalCompositorAdapter.mm:234,1340`), immutable and reference-counted, so the worker uploads
  them into **its own** textures. It never reads the render thread's source-texture cache.
- Draw the source through the same fragment functions Program uses, with the draft grade, or
  neutral when `compareOriginal`, into an IOSurface-backed target at the source's size capped at
  1920x1080.
- Publish a `GradePreviewSurface` stamped with the frame's `sourceEpoch`, `frameId` and capture
  time. Status vocabulary is the existing one: `ready`, `held`, `preparing`, `unavailable`.

Wire change (new, additive): the `texture` objects in the `grade-preview` event gain
`iosurfaceId`, as `program-shared-texture` already carries on Mac
(`MetalCompositorTest.cpp:530`; shell use at `AppModel.swift:1908-1913`). `sharedHandleHex` stays
empty on Mac. The shell shows the surface with the existing IOSurface view
(`ShellApp.swift:1306-1337`).

A demand carrying `lowerThirdPreview` (the Graphics Designer's authoring canvas) answers
`unavailable` with the new reason `native-appearance-preview-not-built`. That belongs to the
graphics workstream.

Once this lands, the shell takes its epoch from the first `grade-preview` observation exactly as
Windows does, and `query-source-grade` remains the path for applying with no editor open
(re-assertion).

### 5.6 Scopes on Metal

A 1:1 Metal Shading Language port of the three HLSL programs in `D3DGradeScopes.h`, keeping the
bin layout (328,708 32-bit counters), the 256x144 analysis size, the 1536x512 output and the mode
and view semantics, so the published `sampleWidth`, `sampleHeight`, `sampleCount`, `roiPixel*` and
`units` fields mean the same thing on both platforms.

1. Draw the source (graded, or original when `scopesOriginal`), cropped to the ROI, into a private
   256x144 texture.
2. Compute kernel: clear, then accumulate with atomic adds; circle mask when the ROI is a circle.
3. Peak kernel: maximum of the four histograms.
4. Draw the 1536x512 scope image into an IOSurface-backed target; publish it in
   `GradePreviewSurface::Scopes` with the ROI, its pixel rectangle and the revision.

The shell renders the scope surface in one IOSurface view under the preview and draws the three
panel captions ("HISTOGRAM · 0–255", "WAVEFORM · 0–100%", "VECTORSCOPE · 75%") itself, as Windows
does.

### 5.7 Threading and performance contract

The owner's rule: analysis never runs on the render tick and never costs Program frames.

- **Render thread.** Unchanged shared code. At most every 100 ms it copies up to three frame
  references into a request and calls `submitGradePreviews`, which must only swap a pointer under a
  leaf mutex. No Metal call, no allocation proportional to pixels, no wait.
- **Worker thread.** One thread, utility quality-of-service, created on first demand and torn down
  when demand ends (matching D3D's release-on-owner rule). It owns its own `MTLCommandQueue`,
  textures, pipelines and IOSurfaces. It never calls `waitUntilCompleted` on anything the render
  thread owns and takes no lock the render thread takes except the single-slot hand-off.
- **Cadence.** 10 Hz ceiling from the controller. Results are published from the command buffer's
  completion handler; a job arriving while one is in flight supersedes the pending slot.
- **GPU sharing.** Metal has one GPU; a separate queue isolates ordering, not capacity. The work is
  bounded by construction: per editor one draw at up to 1080p, one 256x144 draw, 36,864 compute
  threads and one 1536x512 draw, ten times a second. The worker measures each job's GPU time; if
  the last job exceeded 4 ms it skips the next request (5 Hz), and reports `lastWorkMs` through the
  existing `gradePreview` diagnostics node.
- **Giving way.** When the core is shedding monitors (`realtimeEvidence.monitorShed.divisor > 1`),
  grade previews should slow by the same divisor. That is a small shared-core change in
  `GradePreviewController::tick` and therefore also changes Windows; if the owner does not want
  Windows touched, the Mac worker applies its own skip rule only (risk R2).
- **Memory.** Three editors hold at most three 1080p preview surfaces, three scope surfaces and
  three upload textures: about 40 MB.
- **Applying a grade** costs Program nothing new: the constants, the 16x32 curve texture and the
  3D LUT are already bound on every layer draw; a new document builds its textures once on first
  use (`MetalCompositorAdapter.mm:1627-1637,1645-1655`). A 33-point LUT is a 2.3 MB upload on the
  render thread, once per LUT (risk R4).

### 5.8 Where this plugs into the Sources inspector

The Sources spec owns the table, selection and the three-tab inspector. This spec supplies
`GradeInspectorSection` for the Picture / Grade tab: the source's current grade in one line (look,
the four basic values, "Advanced adjustments active" or "Grade bypassed" using the Windows
indicator wording at `VM.Advanced.cs:62-64`), a Reset, and "Open grade workspace…". The editor
itself is a separate window, as on Windows.

Needed from the Sources spec:

1. For a slot: `gradeSourceId` (Zoom participant id, or `capture:<deviceId>`; nil when unassigned)
   and the display name.
2. For Zoom slots: the participant's `persistentId` parsed from the snapshot roster.
3. Three call sites that invoke `openGradeEditor(sourceId:)`: the row "Grade" button, the toolbar
   "Grade selected…" button (disabled with no selection), and the inspector header "Grade…".
4. Optionally a small "graded" mark on the row, fed by `SourceGradeStore`.

Until the Sources rebuild lands, slice G1 adds a "Grade" button to the existing `SlotRow` so the
feature has a consumer on day one. Live thumbnails (row 2.6) are not required.

### 5.9 Persistence

- New file `source-grades.json` in the app's Application Support folder (new), separate from
  `ShellPrefs`, because a grade can embed 1.5 MB of LUT text and prefs are rewritten often.
  Content: `{version: 1, grades: {<stableKey>: ColorGrade}, scopeRegions: {<stableKey>: Roi}}`,
  keys as on Windows, capped at 64 with oldest-first retirement and the same notice.
- Written 750 ms after the last change and on quit; decoded tolerantly (a bad entry is skipped with
  "Saved grade unavailable: {reason}", `StudioViewModel.Grading.cs:27`).
- Relaunch behaviour is owner decision D2.

### 5.10 What the operator sees when things go wrong

| Situation | Shown |
|---|---|
| Core refuses an apply | "Grade was not applied: {reason}. Previous live grade retained." |
| Source has no frames | "Source has no video yet; grade not applied." (new) |
| Zoom guest's stream changed, same person | Grade re-applied silently; logged |
| Different person on a reused id | Grade not applied; "Source identity changed. Close and reopen its grade editor before applying." |
| Core restarted | Editors show "Native preview reconnecting."; session grades re-applied |
| Preview worker failed | Preview and scopes show the core's `failureReason`; applying still works |
| Core is shedding monitors | Preview and scopes update slowly; status shows "held" or "stale" |
| Fourth editor | "Close one of the three grade editors before opening another" |
| Bad `.cube` or preset | Parser message in the editor status line; draft unchanged |

## 6. Slices

| # | What lands | Lane | Size | Depends on | Proof |
|---|---|---|---|---|---|
| G0 | `query-source-grade` and the `sourceGrades` snapshot node. Consumer: G1's Zoom apply | native (shared) | S | — | New cases in `AdvancedGradeTest.cpp`; protocol parity test; Windows suite unchanged |
| G1 | `SourceGradeStore`, per-source editor with built-in looks and the four basic sliders, Reset, live toggle, Apply / Done / Discard, three-editor limit, persistence, re-assertion, "Grade" button on the current slot row | shell | M | G0 for Zoom (capture works without it) | Shell tests; Metal pixel test for basic axes and looks; snapshot-harness proof that only the graded source changes |
| G2 | Advanced document: primaries stack, bypass, intensity, reorder / duplicate / remove, undo / redo, copy / paste, `.cvgrade` presets, `register-grade-document` flow | shell | M | G1 | Shared grade fixtures; cross-platform preset round trip |
| G3 | Curve editor and `.cube` import | shell | M | G2 | Shared cube fixtures; pixel proof with a channel-swap LUT |
| G4 | Hook into the Sources inspector and toolbar; retire the interim slot-row button | shell | S | G1, Sources spec | Shell tests |
| G5 | Metal grade preview worker; `iosurfaceId` in `grade-preview`; shell preview surface, Compare original, epoch from observation, status lines | native + shell | L | G1 | Metal port of `GradePreviewPixels.*Draft*`; drill with editors open |
| G6 | Metal scopes; shell scope panel, modes, expand, analyze original | native + shell | L | G5 | Metal port of the histogram / waveform / vectorscope patch test; drill |
| G7 | ROI: draw, move, resize, precision sliders, keys, circle, per-source persistence | shell (+ native check) | M | G6 | Metal port of the ROI and circle-mask tests; shell geometry tests |

G1-G4 give the operator everything that changes pixels on air. G5-G7 give the instruments.

## 7. Acceptance and test strategy

**Shared fixtures (new).** A folder of grade JSON documents and `.cube` texts, each with its
expected verdict (valid, or the failing rule), read by a C# test, the native
`AdvancedGradeTest.cpp` and a Swift test, so the three validators cannot drift. Location follows
the existing contract-fixture convention (UNVERIFIED where that lives; `contracts/README.md` is
referenced by the repo guide).

**Shell unit tests** (in the `COREVIDEO_SHELL_TESTS` harness), each written to fail on today's Mac
behaviour first, ported from the named Windows tests:

- Apply waits for the acknowledgement; a conflict keeps the draft and saves nothing
  (`AdvancedGradeEditorTests.cs:39`).
- An acknowledgement updates the expected revision; another editor's observation cannot (`:49`).
- A core restart during a pending apply cannot save a stale acknowledgement (`:63`).
- Expanding advanced makes edits a draft; undo restores a whole preset as one step (`:13,28`).
- Draft and compare never send `set-source-grade` (`ColorGradeEditorViewModelTests.cs:68`).
- New: a Zoom grade is not applied to a different `persistentId` on the same participant id; a
  grade is re-applied after a core generation change and after `active:false`; a slider drag
  produces one in-flight apply and one pending.

**Native tests.**

- G0: `SourceGradeState` reports epoch and revision; the node lists `active:false` after an epoch
  bump.
- G1: a Metal readback test that each basic axis and each named look moves a known patch to the
  value the shared `colorGradeParams` predicts, and that a per-layer grade leaves a second layer
  untouched.
- G5-G7: Metal ports of `IsolatedMonitorPixelTest.cpp:230-234` (draft preview matches Program
  pixels for BGRA and I420), `:263` (known neutral, RGB and skin patches land in the expected
  histogram, waveform and vectorscope bins), `:309` (ROI samples only the selection), `:332`
  (circle mask excludes corners and leaves Program unchanged). Scope images are read back from the
  IOSurface and judged on pixels.
- A test that `submitGradePreviews` returns in bounded time while the worker is blocked (the
  existing vcam-tap test at `MetalCompositorTest.cpp:1052` is the model).

**Pixel proofs through the app.** With the `COREVIDEO_SHELL_SNAPSHOT` harness: two synthetic
sources side by side, grade one, capture Program; the graded half changes by the predicted amount
and the other half is identical to the ungraded capture. Repeat with an imported channel-swap
`.cube`.

**Performance gate (blocking for G5 and G6).** `python scripts/mac-show-drill.py --seconds 40
--load 8` on the owner's Apple Silicon machine, three runs each: no editor; three editors with
preview; three editors with preview and scopes. Pass means zero additional underruns or missed
output deadlines, delivery and sustained fps equal to the no-editor run, and no monitor shedding
caused by the editors. The drill needs a way to open editors headlessly: it sends
`set-grade-preview` itself over the core's stdio (it already drives the core that way). Record
hardware, load and duration with the result.

**Live owner checks.**

1. Two real cameras: match one to the other using the waveform; confirm Program, the recording and
   the stream all show the match.
2. A real Zoom guest: grade them, have them leave and rejoin, and confirm the grade follows the
   person and never lands on someone else.
3. Kill the core mid-show: grades return without operator action.
4. Save a preset on the Mac, load it on Windows, and the reverse.
5. Scopes side by side with the Windows build on the same test pattern.

## 8. Risks and unknowns

- **R1. How resource-lane events reach the Swift bridge.** `grade-preview` events are drained on
  the core's resource lane (`JsonRpcServer.cpp:933-943,1460,1495`). Whether the Mac bridge already
  receives that lane, or only the events inside sync replies, is UNVERIFIED. Resolve with a
  half-day spike at the start of G5.
- **R2. GPU contention on one GPU.** A separate queue does not reserve GPU time. Resolved by the
  performance gate; if it fails, lower the preview size, then the cadence, never Program.
- **R3. Grades going neutral on a Zoom stream change.** From the code, an applied grade is forced
  neutral whenever the Zoom epoch changes (a size change during the join ramp, or a re-subscribe),
  and nothing on Windows re-applies it without the operator reopening the editor. This is inferred
  from `SourceGradeState.h:50-59` and `ZoomEngineRuntime.cpp:1578-1582` and is UNVERIFIED at run
  time. The Mac design re-applies automatically (section 5.3). Resolve by a live check on Windows;
  if confirmed, file a Windows issue.
- **R4. Large LUT upload on the render thread.** A first-use 33-point LUT upload happens inside the
  layer draw. Measure it in G3; if it shows as a render overrun, move texture creation to a
  prepare step off the tick (native, S).
- **R5. Metal atomics.** The port needs atomic add and atomic max on a buffer of unsigned integers
  from a compute kernel. Atomic add is standard; if atomic max is not available on the minimum OS,
  the peak pass becomes a single-thread reduction over 1,024 bins. Resolve at the start of G6.
- **R6. `MonitorRenderWorker` on macOS.** Reuse is assumed; if its scheduling helpers are
  Windows-shaped, write a small single-slot worker instead. No behaviour change either way.
- **R7. Protocol list change.** Adding a request touches the shared protocol lists and their parity
  test, and so touches the Windows build. Kept minimal and read-only; run the Windows suite.
- **R8. Scope appearance.** Line widths in the scope shader are fractions of a 1536x512 image; on a
  Retina display scaled down they may read thin. Owner check 5 decides whether a Mac-only output
  size is wanted.

## 9. Owner decisions needed

- **D1. The existing programme-wide grade window.** Windows has no such control in its UI. A source
  grade replaces it on that source rather than stacking. Options: keep it as "Program look" beside
  the per-source editors, or remove it. Recommendation: keep it, relabelled "Program look (sources
  without their own grade)", because saved values exist in the field and it is the only way to
  tint graphics-free content globally.
- **D2. After a relaunch.** Windows brings a saved grade back only as a draft the operator must
  confirm. Recommendation: match that for Zoom guests (lighting and identity can change between
  meetings), but re-apply automatically for capture devices, whose identity is the hardware.
- **D3. Editing before the private preview exists.** G1-G3 ship before G5, so advanced edits are
  made while watching Program or the multiview. Recommendation: ship them anyway with the plain
  warning in section 5.4; the alternative is holding all advanced grading until the native work is
  done.
- **D4. Re-applying a grade after a Zoom stream change** (R3). Recommendation: yes on Mac, keyed on
  `persistentId`, and raise the same change for Windows.

## 10. Out of scope

- The Sources table, selection model and inspector shell (row 2.4), and live thumbnails (row 2.6).
- The Graphics Designer's appearance preview that shares `set-grade-preview`.
- Remote-control actions for grading (Windows has none).
- HDR or any colour space other than Rec.709 SDR; the document format says `rec709-sdr`.
- New looks, a looks browser, split-screen A/B or scope types Windows does not have.
- Per-scene or per-route grades on the Mac wire (the store's re-assertion replaces them).
- Windows changes other than the read-only additions in G0 and, if approved, the shedding rule.
