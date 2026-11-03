# Handoff: Graphics Designer Phase 2, Slice 3 (per-layer motion)

**For:** Codex. **From:** Claude Code, 2026-11-03. **Owner:** the CoreVideo Pro owner (they/them).

## 1. What you are building

Each layer of a graphic gets its own way in and out: a start offset, a duration, easing, and one of a small set
of moves. The Designer gets a motion strip, a per-layer Motion tab, presets and Play preview, and the motion has
to reach air from Operate. This is spec §5 and Acceptance 3:

> Give the plate "Wipe from left", and the name "Fade + rise" 0.2 s later. Play preview. Show on air: the bar
> leads the words.

## 2. Where everything is

| What | Where |
|---|---|
| Branch (start here) | `codex/graphics-designer-slice3`, up to date with main `81bbb0de` |
| **The plan (your requirements)** | `docs/superpowers/plans/2026-11-02-graphics-designer-slice3-motion.md` (8 tasks) |
| Spec (binding when the plan is unclear) | `docs/superpowers/specs/2026-11-01-graphics-designer-phase2-design.md` §5 |
| Owner-reviewed mockup D4 (motion) | `D:\CoreVideo-QA\tmp\gfx-slice2\DesignerAnimate.dc.html`; D1 (where the strip sits): `DesignerMain.dc.html` |
| Previous slice's plan (house style) | `docs/superpowers/plans/2026-11-02-graphics-designer-slice2-surface.md` |
| Previous slice's review record and lessons | `D:\CoreVideo-QA\triage\gfx-slice2-sdd\` (`progress.md`, `final-review.md`) |
| Operator docs to update | `docs/reference/graphics-playout.md` (section "The Designer"), `CLAUDE.md` graphics row, `docs/BACKLOG.md` |
| Repo rules | `AGENTS.md` and `CLAUDE.md` (read both first) |

Slices 1 and 2 are merged: designs in the show file, the Designer surface, and the Operate variable boxes.
Main also has "Use for Zoom lower thirds" (#1090) and the duplicate F-key fix (#1091).

## 3. Owner decisions still open

The plan's "Decisions" section has items marked ⚑. The owner has **not** ruled on them yet. Build them as the
plan says unless the owner says otherwise:

- **Row Motion wins.** When a playlist row has its own Motion, the whole graphic moves as one and per-layer
  motion is ignored. When it is unset, a design row plays the design's motion, and built-in rows keep the
  250/200 ms fade.
- **Reversal.** Show during the way out plays backwards from where it is, and Hide during the way in does the
  same, so no layer jumps.
- **"Out mirrors In"** is on by default, and the Out view is read-only while it is on.
- **Masking.** Moves are masked by the parent's area, so a slide or wipe enters from the graphic's own edge.
- **Pop and grow.** Pop scales 0.5 to about 1.05 and settles at 1. Grow scales from one edge with no overshoot.
- **Play preview** runs at 30 fps and stops on any edit; Loop restarts on each change.
- **Limits.** At most 16 layers with their own motion; a 50 ms drag grid (10 ms with Ctrl).
- **Strip order and layout.** The motion strip is ordered back-to-front, as in mockup D4. It starts collapsed
  at 1360×768.
- **Renderer scope.** Only the Windows D3D11 compositor draws motion.

## 4. How to run the work

- **Order.** Do the tasks in order, one at a time; the native tasks (1–5) come first. Each task is test-first:
  record the failing run, then implement and make it pass. Commit each task on the branch with its plan
  commit message.
- **Review.** Get each task reviewed against its plan text before starting the next. At the end, review the
  whole branch.
- **Walk the acceptance path.** Before calling the slice done, trace Acceptance 3 end to end through the
  code: Designer, then Save to playlist, then Operate Show, then on air.
  - Slice 2's whole-branch review found a **critical** gap that no task owned: Operate had no box to type the
    new variable. Task 8 of this plan already owns the known Slice 3 gap. Re-showing a row whose instance
    exists only patches fields today, so a new design version and its motion would never reach air.
- **Open a PR** to `main` with a verification table. **Do not merge** without the owner's word, and do not
  publish a release.

## 5. Build and test (this Windows box)

**Shell suites.** Run from `native-shell\`:

```powershell
dotnet test CoreVideoPro.MediaCore.Tests
dotnet test CoreVideoPro.Control.Tests
dotnet test CoreVideoPro.WinUI.Tests
```

- The only allowed failure is `AdapterConformanceTests...GoldenFacadeSequence` (#1088). It fails whenever
  `show-engine/dist` is built locally.
- If a WinUI test build reports missing asset files, rebuild with `--no-incremental`.

**WinUI build:**

```powershell
dotnet build native-shell\CoreVideoPro.WinUI -c Release -p:Platform=x64
```

**Native build.** Run from PowerShell:

```powershell
$env:ZOOM_SDK_DIR='C:\Users\walla\Downloads\zoom-sdk-windows-7.0.5.39292\zoom-sdk-windows-7.0.5.39292\x64'
cmd /c "npm run build:native-dev > D:\CoreVideo-QA\tmp\gfx-slice3\native-build.log 2>&1"
```

**Native tests.**

- Run from `native\build-dev` (not `Release\`, which has no DLLs: exit 0xC0000135).
- Always set `$env:COREVIDEO_VCAM_SHM_DIR='D:\CoreVideo-QA\tmp\gfx-slice3\vcam-shm'`.
- Use an exact `--gtest_filter=Suite.Name`. The shim does not accept `:`-joined filters.
- Run the full native suite only once, at the end, and only when the app is closed (`tasklist` shows no
  `CoreVideoPro.WinUI` or `corevideo-native`). Never set `COREVIDEO_CAPTURE_TESTS`.

**Layout probe.**

```powershell
powershell -File scripts\test-studio-layout.ps1 -ReportDirectory D:\CoreVideo-QA\tmp\gfx-slice3-probe-<n>
```

- Use a **new** directory each run; the script refuses to reuse one.
- Run it only when the app is closed.
- Read the screenshots yourself and check 1360×768 and 1600×900.

**Native tests and the clock.** Native timing tests must use the injected or tick clock. Wall-clock waits
flake under TSan in CI.

## 6. Known traps (each one has bitten this repo)

- **Temporary `shared_ptr` in a range-for.** `for (x : registry.poll()->member)` lets the temporary die before
  the loop body runs (TSan race, PR #1085). Hold the snapshot in a local first.
- **Contract parity scan.** `native/tests/ContractParityTest.cpp` treats any C# `["type"] = "<literal>"` as a
  request sent to the core. For JSON field kinds, use a named constant (PR #1089).
- **Cross-thread test recorders.** A test recorder that background tasks write to (for example auto-out) must
  be locked.
- **Unhandled exceptions in WinUI.** An exception escaping a WinUI event or async handler crashes the whole app
  (0xc000027b). Live example, 2026-11-02: duplicate F3 hotkeys made a `ToDictionary` throw from a
  library-changed event. Surface errors in the status line instead.
- **Combo boxes that rebuild their own inspector.** When a ComboBox's SelectionChanged rebuilds the inspector
  holding it, defer the rebuild with `DispatcherQueue.TryEnqueue`.
- **Auto-merge before CI.** A merge loop can fire before CI registers its checks. Never merge on an empty
  check rollup.
- **PRs touching `.github/workflows`.** The `gh` token cannot merge them; flag such a PR to the owner.
- **Line endings.** Files are CRLF; keep diffs free of line-ending churn.

## 7. Hard rules from the owner

- **The installed app.** Never launch, kill or attach to it. It lives in
  `%LOCALAPPDATA%\Programs\CoreVideoPro\<release>`. The owner may be live in a show.
- **No input injection.** No SendKeys and no focus stealing. Use the control API, WM_ messages or UIA Invoke
  only.
- **Evidence and logs** go under `D:\CoreVideo-QA\tmp\` only. The owner's personal screen content must never
  appear in evidence.
- **Never write the Zoom test meeting link** into the repo, issues or PRs.
- **The OAuth broker host is `corevideo.iamfatness.us`** and never moves.
- **Docs are part of done.** Update `CLAUDE.md` and `docs/reference/graphics-playout.md` in the same change.
- **Commit trailer:** `Co-Authored-By: <your agent identity>`. PR bodies say plainly what was and was not
  verified live.

## 8. Current state you inherit

- **Installed on the owner's box:** beta `beta-2026-11-02-61d3b8e`. Main has moved on to `81bbb0de`, adding
  the duplicate F-key fix, which is not yet installed. The owner's show file was repaired by hand; the backup
  is in `D:\CoreVideo-QA\tmp\beta-1102b\`.
- **Parked minor findings** from Slice 2 are listed in `D:\CoreVideo-QA\triage\gfx-slice2-sdd\progress.md`
  (lines starting with "Ruling:"). Examples:
  - The "Empty title" sample label is the owner's call.
  - A full-library eviction race.
  - A rare second double-click needed to rename.
  - Group opacity is not carried when a layer moves between groups.

  None of them blocks Slice 3.
- **Not built yet:**
  - Playlist row delete and reorder in the UI. The control API already has `graphics.playlist.row.remove`.
  - "Use for Zoom lower thirds" from the Your designs list (it is in the Designer header only).
- **Open issues:**
  - #1087: Slice 1 follow-ups. Fixed except item (b), which belongs to Slice 4.
  - #1088: the golden conformance failure.
- **Never live-verified:** the whole Designer (Slices 1–2) has only been verified by tests and the probe. The
  owner's Acceptance 1 run on the installed beta is the first live use.

## 9. After Slice 3

The spec lists two more slices:

- **Slice 4, content:** ellipse and line shapes, stroke, outline, drop shadow, size to content, text shrink,
  and PNG/SVG import into a design. The import must also fix the logo picker's `.svg` filter and add asset-pool
  eviction.
- **Slice 5:** extra design variables mapped to Sources columns and names-list CSV columns.
