# macOS parity Phase 4: remaining control actions, telemetry, crash upload, leftovers (RC7, R4-R7) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

Status: DRAFT for owner review, 2026-11-22. Lane: Shell B (control, settings, health). Written in worktree
`mac-phase4-plans` at `origin/main` `e117a8fa`. Nothing here has been built or run; every code claim below was
read at that commit.

> `docs/superpowers/plan-authoring-rules.md` is binding. Every task lists the mutations it must turn red, and its
> report records whether each one did. When a supplied test contradicts a reasonable implementation, suspect the
> test first (rule 4) and say so in the report.

**Goal:** A Mac operator gets every remote action whose feature exists on the Mac, opt-in telemetry and a
consented crash report that behave like Windows 1.0 at every boundary (endpoint, consent file, payload, headers,
UI), release symbols that make those reports readable, and the 22 still-open leftover audit rows.

**Architecture:** Four batches, each closed by an independent review. RC7 flips rows in
`contracts/control.platforms.json` and adds dispatch cases exactly as Phase 1/2 did, with per-action vectors
exported from the Windows code instead of retyped. Telemetry and crash upload are Swift ports of the
MediaCore/WinUI classes, held to a new contract fixture that a Windows MediaCore test writes; crash input is the
macOS `.ips` report (the Mac twin of a WER dump), sanitized before it leaves the machine. Leftovers are small
shell items, each matched to the Windows XAML layout and copy.

**Tech Stack:** Swift 5.9 / SwiftUI / AppKit, `URLSession`, `/usr/bin/ditto`, the in-binary harness
(`COREVIDEO_SHELL_TESTS=1`); C# xUnit on dotnet (`~/.dotnet/dotnet`, MediaCore.Tests and Control.Tests run on
this Mac; WinUI.Tests run only on the Windows CI job, `.github/workflows/ci.yml:378`); bash release scripts.

**Specs:** `docs/superpowers/specs/2026-11-15-mac-remote-control-parity-design.md` (slice RC7, sections 4-5) and
`docs/superpowers/specs/2026-11-15-mac-comms-social-remaining-design.md` (Part C: C3, C5, C6 slices R4-R7).
Master plan Phase 4 rows. Read both specs, then the corrections below, which override them.

Path shorthand: `W/` = `native-shell/CoreVideoPro.WinUI/`, `WT/` = `native-shell/CoreVideoPro.WinUI.Tests/`,
`MC/` = `native-shell/CoreVideoPro.MediaCore/`, `MCT/` = `native-shell/CoreVideoPro.MediaCore.Tests/`,
`C/` = `native-shell/CoreVideoPro.Control/`, `CT/` = `native-shell/CoreVideoPro.Control.Tests/`,
`M/` = `mac-shell/Sources/CoreVideoProShell/`, `SVM` = `W/ViewModels/StudioViewModel.cs`,
`SCS` = `W/Services/StudioControlSurface.cs`.

## Global Constraints

- Windows 1.0 is the reference. Copy is quoted verbatim from the Windows file named in the task; layout follows
  the Windows XAML element order named in the task.
- Telemetry endpoint: `https://telemetry.corevideo.io`; override `COREVIDEO_TELEMETRY_ENDPOINT` (non-blank,
  trimmed, wins); key only from `COREVIDEO_TELEMETRY_API_KEY`, never baked, never logged; no key = quietly
  disabled (`MC/Services/CrashReportUploader.cs:13-41`). Enabled needs a key and an absolute `http`/`https` URL.
- Events: `POST {endpoint}/v1/events`, `Authorization: Bearer <key>`, `Content-Type: application/json`, body
  < 64 KiB. Crashes: `POST {endpoint}/v1/crashes`, `Content-Type: application/zip`, headers
  `X-CoreVideo-Version`, `X-CoreVideo-Machine-Class`, `X-CoreVideo-Reason`, body cap 24 MiB uncompressed content.
- Outcome classes, both clients: 2xx = Accepted (read `reportId`); 429 or >= 500 or a network error = RetryLater
  (read `Retry-After` delta or date); any other status = Rejected. Error bodies trimmed to 300 characters.
- Consent: opt-in, **absent file or absent value = OFF**; a corrupt or unreadable file reads as OFF; revoking
  always lands. Nothing is sent without consent **and** a key. Crash reports send only on an explicit
  "Send report" click (telemetry consent is not required for that; Windows gates it on the key only,
  `W/Views/StudioWorkspace.xaml.cs:137-153`).
- Privacy: no names, meeting ids, participant ids, stream keys, URLs, file paths, usernames or stable machine
  identifiers ever leave the machine (Windows copy, `W/ViewModels/SettingsViewModel.cs:140-144`). Where Windows
  sends less-filtered data than this rule allows, the Mac filters more and the gap is filed for Windows (D-T4, D-X2).
- Injected clocks only: no `Date()` / `Timer` / `Task.sleep` read directly in a tested policy; every timeout and
  interval is a parameter.
- A row in `contracts/control.platforms.json` flips to `supported` in the same commit that adds its dispatch
  case, its state field, `npm run contract:generate`, the updated counts in `control/closure` (42 today) and
  `control/unavailable-422` (68 today), and a re-recorded `companion-module-corevideopro/src/fixtures/mac-control.json`
  with the count in `companion-module-corevideopro/src/mac.test.ts:29` (42 today).
- New Swift test files are `M/ShellTests*.swift` or `*Tests*.swift` (ContractParity skips them). New cases
  register in their own static list, the pattern of `M/ShellTestsQuickWins.swift:12-14`.
- No file under `native/` changes except Task 13 (`native/CMakeLists.txt`, compile flags only). Nothing here
  touches the render or audio worker, so `mac-show-drill.py --load 8` is not a gate except after Task 13.
- One commit per task, message ending `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

1. **A session-end send must neither be lost nor hold the quit.** An idle quit returns `.terminateNow` today
   (`M/AppModel.swift:5589-5594`), which kills an in-flight `URLSession` task. Expect: with consent and a key,
   quit waits at most 2 s for the send; without either, quit is exactly as fast as today. Pinned in Task 8.
2. **A crash report must not carry the operator's identity.** A real `.ips` carries `crashReporterKey` (a stable
   per-machine id), `bootSessionUUID`, `userID`, `incident_id`, `vmSummary` and `trialInfo`
   (checked on `~/Library/Logs/DiagnosticReports/CoreVideoProShell-2026-11-22-032121.ips`). Expect: none of those
   keys, and no `/Users/<name>` string, in the zip. Pinned in Task 10.
3. **A consent file that cannot be read must never become "on", and a revoke must land even then.** Pinned in
   Task 7 with a file that is locked, corrupt, and a directory.
4. **A remote action must refuse with Windows' exact sentence and change nothing.** Each RC7 refusal is a vector
   exported from the Windows helper (Task 1), so a retyped sentence cannot drift. Pinned in Tasks 2-5.
5. **A crash in the core or engine, not only the shell, must be offered once and only once,** including two
   crashes before the next launch and a relaunch after dismissing. Pinned in Task 10.

---

## Where things stand (verified 2026-11-22 at `e117a8fa`)

### RC6 (Volt rundown) landed

`a2b46a93` on `mac-control-audio-phase2` (merged as `73332da7` via `e9352859` / PR #1223, in `origin/main`):
`media.rundown.load/.preview/.take` are `supported` in `contracts/control.platforms.json`, dispatched at
`M/ControlSurface.swift` (the `// ── Volt rundown (RC6 ...)` cases), listed in `supportedActionIds`, and proven by
the volt cases in `M/ControlApiTestsPhase2.swift`. Nothing to carry.

### Action accounting

| Group | Count | Where |
|---|---|---|
| Windows registry (`contracts/control.manifest.json`) | **110** | |
| Mac serves (`supported`) | **42** | `M/ControlSurface.swift:221` `supportedActionIds`; `control/closure` asserts 42 |
| Not served | **68** | 66 `blocked` + 2 `notApplicable` (`transport.ndi.graphicsKey.set`, `showEngine.restart`) |
| Owned by other lanes, **excluded here** | **54** | GFX 43 → graphics shell **G11**; TIL 8 → Tiles **C1**; BRW 3 → outputs **B2** |
| This lane (RC7 remainder) | **12** | STU 9 + OUT 3, below |

The Phase 2 outcomes say 40/110; settings quick wins added `audio.zoomMode.set` and
`automation.autoAssignInputs.set` (42). Note for G11's owner: two of the 43 GFX rows are not `graphics.*` ids
the G11 slice text names — `graphics.caption.set` (the remote spec put it in RC3; the platform file says GFX)
and `automation.lowerThirds.set`. Both stay with G11 because their `blockedOn` is GFX; this plan does not
touch them.

| Action | Batch | Status for this plan |
|---|---|---|
| `transport.stream.srt.configure` | C | **Do now** (Task 2). The Mac SRT destination exists (`M/StreamDestinations.swift:15-24,64`). |
| `multiview.showLabels.set`, `.showTally.set`, `.showMeters.set`, `.showClock.set` | C + R7 | **Do now** with leftover B24/E5/G21 (Task 3). |
| `scene.cueWithRoutes`, `scene.route.set` | C | **Do now** under D-C2 (Task 4). |
| `scene.restoreStarters` | C | **Do now** (Task 5): needs a Mac starter catalog and removed set. |
| `transport.ndi.toggle`, `transport.ndi.set` (+ feedback `ndi`) | C | **Blocked on outputs N1** (Phase 4 outputs lane; no N1 outcomes exist). Task 6 is written and gated. |
| `settings.programBuffer.set` | — | **Not planned.** The Metal compositor has no program buffer (`MetalCompositorAdapter.mm` does not override `configureProgramBuffer`; the `Interfaces.h` default is a no-op). Needs a native slice no lane owns: D-C4. |
| `view.setMode` (+ feedback `viewMode`) | — | **Not planned.** Owner decision 3 of the remote spec (leave blocked until Mac view modes are decided). |

After Tasks 2-5 the Mac serves **50/110**; after Task 6, 52.

### Leftover rows (spec C5) at HEAD

Done, removed from the batches: **A3** (`AutoAssignInputsToggle`, `M/AutomationExtras.swift:393`, Phase 1),
**E4** (drag-swap in the shared `MultiviewerCanvas` is not gated by `isPopout`, `M/ShellApp.swift:840-854`),
**G14 header chip** (`M/ShellApp.swift:773`), **2.13a/2.13b** (settings quick wins Z1/Z2). Closed with no work:
**ZoomOAuth timeout** (D-R4). Everything else is open or partial and is in Tasks 14-23.

---

## Owner decisions (defaults taken; the owner may override)

| # | Default | Why |
|---|---|---|
| **D-T1** Key delivery | Mac reads `COREVIDEO_TELEMETRY_API_KEY` / `COREVIDEO_TELEMETRY_ENDPOINT` from the process environment only, as Windows. | Parity. **Consequence the owner must accept:** a Finder- or Dock-launched app does not inherit a shell environment, so no Mac customer uploads anything until a key channel exists (Windows can at least use a machine env var). Test rigs use `launchctl setenv`. A Mac-only channel (for example a root-owned file the .pkg could install) is a separate decision, not taken here. |
| **D-T2** Machine class | Label `mac-arm64-cpu<N>-ram<N>gb` (fallback `mac-arm64`), the Windows shape (`MC/Services/MachineClassProbe.cs:44-56`). `cpuCores` = `ProcessInfo.processorCount`; RAM GB = physical memory rounded **half to even** (C# `Math.Round`), banded by the Windows table. `gpuTier` = `apple-m<gen>[-pro|-max|-ultra]` parsed from `machdep.cpu.brand_string`, nil when unparsable. | Same server index (`services/telemetry-ingest/lib/handlers.mjs:218` accepts any string, capped at 200 chars, `:94`), so the spec's risk "the worker may reject an unknown machine class" does not exist. Windows' GPU band is vendor + VRAM band (`W/Services/GpuTierProbe.cs:46`); Apple has unified memory, so the chip family is the coarse equivalent. No model code, no serial. |
| **D-T3** Card placement | "Diagnostics & telemetry" card at the bottom of the Mac **Zoom** pane, after "Zoom status". | Windows puts it on its Zoom-tab page (`W/Views/SettingsPage.xaml:184-223`, hosted by `IsSettingsTab`, `W/Views/StudioWorkspace.xaml:705-708`) after "Health and support" and "Optional workspaces"; the Mac has neither (Health is the Diagnose tab, OHG is out of scope), so it follows "Zoom status". |
| **D-T4** Idle quit and session-end | With consent and a key, an idle quit returns `.terminateLater` and replies when the send finishes or 2 s pass, whichever is first. Otherwise unchanged. The stop-and-quit path starts the send first and runs it concurrently with output teardown. | Windows fires it first and lets it race teardown with a 2 s cap (`W/MainWindow.xaml.cs:842-855`, `W/Services/TelemetryEventService.cs:25`); a Mac `.terminateNow` would kill it every time. |
| **D-X1** Crash input | macOS `.ips` reports from `~/Library/Logs/DiagnosticReports` (and its `Retired/` folder) for `CoreVideoProShell`, `corevideo-native`, `corevideo-zoom-engine`, `corevideo-plugin-host`, `corevideo-browser-host`. Not the camera extension. No in-process signal handler, no MetricKit. | `.ips` is the OS-level twin of a WER LocalDumps file, which is what Windows uses (`MC/Services/CrashDumpScanner.cs:20-28`). MetricKit delivers only the subscribing process's own crashes, so it cannot see the core or engine (spec owner decision 4 wants all three). An in-process handler (Crashpad/PLCrashReporter) would be new native code in three processes with no Windows twin. The camera extension runs as `_cmiodalassistants` and reports elsewhere; Windows does not monitor its virtual camera either. |
| **D-X2** Sanitize before upload | The Mac zip carries a **sanitized** copy of each `.ips` (allow-list in Task 10) and **redacted** log tails (`Redactor.redactLines`, `M/Diagnostics.swift:153`). | The privacy rule. Windows uploads raw minidumps and **raw, unredacted** log tails (`MC/Services/CrashReportArchiveBuilder.cs:77-81` reads tails with no redaction) while its bar promises "secrets redacted" (`W/Views/StudioWorkspace.xaml.cs:167-170`). That is a Windows defect: file it as a `backlog` issue (owner approval first). |
| **D-X3** Symbols | Release builds of the core, engine and helpers on Apple get `-g`; the release script runs `dsymutil` on every product Mach-O, then `strip -S`, then signs, and writes `CoreVideoPro-symbols-<version>-arm64.zip` plus each binary's UUID into `manifest.json`. A missing dSYM fails the release. | Windows refuses to release without its PDB bundle (`.github/workflows/release.yml:190-216`). Today the Mac release core is `-DCMAKE_BUILD_TYPE=Release` with no `-g` (`scripts/release-macos.sh:179-188`), so a core `.ips` cannot be symbolicated. `strip -S` keeps build paths (with the builder's username) out of the shipped binaries once `-g` is on. |
| **D-C1** Action vectors | Per-action vectors are exported by the Windows tests from the Windows helpers into `contracts/control.action-vectors.json`; the Mac replays them. | "Fixtures from real Windows output." There is no fake view model for `StudioControlSurface` (it takes a concrete `StudioViewModel`, `SCS:104,117`), so vectors come from the pure helpers it calls. |
| **D-C2** Scene route ids | `routeId` in `scene.cueWithRoutes` / `scene.route.set` is a Mac `SceneLayer.id` (`"<scene>-layer-<n>"`), the D9 rule Volt already uses (`M/VoltRundown.swift:43-55`). | The Mac has no Windows-named routes (`ohg-host` etc.) and its starter scenes differ. A Companion button made on Windows with a Windows route id answers `Unknown route ...` on a Mac, which is the truthful refusal. |
| **D-C3** Starter scenes on Mac | The Mac starter catalog is the Mac's built-in scene list (`M/AppModel.swift:499-503`); a removed starter id is persisted (`ShellPrefs.removedStarterSceneIds`) so restore can re-insert it. | Windows: `SceneCatalogPolicy.MissingCatalogScenes` plus `RemovedCatalogSceneIds` (`docs/reference/scenes.md`). The Mac replaces built-ins with persisted scenes on load (`M/AppModel.swift:962`) and keeps no removed set today. |
| **D-C4** Program buffer | Stays `blocked`. File a native backlog issue "Metal compositor program buffer (2/3 frames)". | The core feature is missing on Metal, and the frame-delivery rule in `CLAUDE.md` makes it a native perf item, not a shell flip. |
| **D-R1** Update banner feed | Built but dormant: reads `COREVIDEO_UPDATE_FEED_URL` like Windows (`MC/Services/UpdateCheckService.cs:205`), accepts a new optional `pkgUrl` field in the same feed JSON, and is a link only (spec decision 6). | No feed is hosted for either platform. The feed schema gains one optional field; Windows ignores unknown fields (verify in Task 23). |
| **D-R2** Menu bar (F6) | Add `Settings {}` (⌘,), and Take / FTB / Pop out multiview commands that call the same model entry points as the footer. | No Windows counterpart (custom title bar); macOS users expect ⌘,. Owner may drop the row. |
| **D-R3** G25 role and health | Port only what Windows actually derives. Every Windows mapping site that builds a `Participant` from a roster, media asset or capture device sets `Role = ParticipantRole.Guest` (`SVM:11345,11424,13123,14051,14169`); `ParticipantMapper.ParseRole` reads a `RoleLabel` whose source is circular in the code read. Task 16 step 1 settles the source; if no roster field carries a role, the Mac shows GUEST like Windows and G25 shrinks to health + selection. | Avoids inventing a role the core never reports (honest-reporting rule). |
| **D-R4** ZoomOAuth timeout row | Close with no change. | Windows never expires the pending authorization (`MC/Services/ZoomOAuthService.cs:157,186,191`) and polls the UI for 2 min; the Mac times its loopback out at 5 min and keeps `pendingState`, so a late callback still works. No security gap; only copy and timing differ. |

## Boundary findings (Windows vs Mac)

1. **Telemetry endpoint and key:** same constants and same "quietly disabled" rule are portable; the environment
   is not (D-T1).
2. **Consent storage:** Windows `%LOCALAPPDATA%\CoreVideoPro\telemetry-consent.json`, camelCase
   `{"enabled": bool, "lastSentUtc": ISO-8601?}`, temp-file + atomic move, bounded retry (20 × 25 ms)
   (`MC/Services/TelemetryConsentStore.cs`). Mac: `~/Library/Application Support/CoreVideoPro/telemetry-consent.json`,
   same JSON. Crash watermark: `crash-watermark.json`, `{"lastSeenUtc", "reports":[{"reportId","sentAtUtc","dumpFiles"}]}`,
   first-run lookback 3 days (`MC/Services/CrashReportWatermarkStore.cs:43`).
3. **Payload schema:** exactly `name, version, sessionLengthSeconds, outputConfigShape{recordingEnabled,
   streamingEnabled, vcamEnabled, isoSourceCount, captureSourceCount, zoomParticipantCount},
   crashCountSinceLastSend, machineClass, machine{label, cpuCores, ramBand, gpuTier?}`; null omitted
   (`MC/Services/TelemetryEvent.cs:44-150`). Events: `heartbeat` every 24 h (first after 24 h, only if
   configured at start), `session-end` once at shutdown with a 2 s cap; only the heartbeat honours
   `Retry-After` (`W/Services/TelemetryEventService.cs`). The exact wire JSON is logged before each send.
   There is **no platform field**; the label prefix is the only platform signal.
4. **Crash mechanism:** Windows = WER LocalDumps minidumps, per-user folder plus the installer folder, offered
   once by watermark, uploaded on click. Mac = `.ips` (D-X1). An `.ips` has backtraces, registers and binary
   images with UUIDs, but no memory: less than a minidump, enough with dSYMs. macOS already rewrites home paths
   to `/Users/USER/*/` in `procPath` and `usedImages` (observed), but leaves `crashReporterKey` and the others in
   Review Focus 2.
5. **Symbols in the notarized flow:** none today (D-X3). `dsymutil` and `strip -S` must run **before**
   `codesign`; codesigning does not change a Mach-O UUID, so the dSYM still matches the notarized binary.
6. **Control vectors:** WinUI.Tests cannot run on the Mac; Control.Tests can. Vectors from `C/` helpers
   regenerate locally; vectors from `W/` helpers regenerate only on the Windows CI job (Task 1).

## Spec and master-plan corrections

1. **RC7 is 12 actions here, not 71.** 68 are unserved; 54 of those are owned by G11, C1 and B2. STU fell from
   12 to 9 (A3 auto-assign shipped as quick-wins C1) and AUD's 2 shipped (A1, A3).
2. **`graphics.caption.set` is GFX in the platform file**, not RC3 as the remote spec's table says.
3. **R5 does not depend on R4's consent.** Windows sends a crash report on the click with only the key
   configured; the dependency is shared code (config, machine class), not consent.
4. **The ingest worker accepts any machine class** (spec C6 risk row is void).
5. **Windows crash zips carry unredacted logs** (D-X2); the spec's "same manifest shape" is kept, the content is
   filtered.
6. **Spec C5 C1 is wrong:** Windows locks the join fields only **in a meeting** (`W/ViewModels/SettingsViewModel.cs:231`),
   not while joining. **C5 is wrong:** Windows never disables Join for a reason; Join is hidden only in a meeting
   (`:239`) and the reason lives in a hint line (`:243-266`).
7. **F5:** Shift on the arrow keys is a 10 px step, not an aspect lock; Shift during a resize keeps the aspect
   (`W/Controls/SceneCanvasEditorControl.xaml.cs:587,724-742`).
8. **G14 is a deletion:** Windows has no scene-rail summary and forbids it (`WT/OperatorUiTelemetryPlacementTests.cs:39-41`);
   the Mac line at `M/ShellApp.swift:984` and the hint at `:997` go, they are not computed.
9. **Multiview overlays are drawn by the shell** on Windows (`W/Controls/ShowMultiviewHost.xaml.cs:341-358`); the
   core only stores and echoes the four flags (`native/src/core/MediaCore.cpp:3627-3637`). No core work.
10. **D6 needs no core work:** `set-verbose-diagnostics` is in the shared core (`native/src/core/Protocol.h:121`,
    `MediaCore.cpp:1411-1415`).
11. **The update feed has no URL on Windows either** (empty `COREVIDEO_UPDATE_FEED_URL` = off).

## Before you start

- [ ] Record the baseline: `cd mac-shell && swift build -c release && COREVIDEO_SHELL_TESTS=1 ./.build/release/CoreVideoProShell`
  (expect `N shell checks passed, 0 failed`; record N), `COREVIDEO_SHELL_SELFCHECK=1 mac-shell/.build/release/CoreVideoProShell`
  (`self-check: PASS`), `bash scripts/lint-mac-design.sh`, `npm run contract:check`,
  `~/.dotnet/dotnet test native-shell/CoreVideoPro.Control.Tests/CoreVideoPro.Control.Tests.csproj`,
  `~/.dotnet/dotnet test native-shell/CoreVideoPro.MediaCore.Tests/CoreVideoPro.MediaCore.Tests.csproj`.
- [ ] Ask the owner before filing the backlog issues named in D-C4 and D-X2.
- [ ] Do not send a request to `127.0.0.1:8011`, launch a test app, or call the real telemetry host. All HTTP in
  tests goes to an in-process stub (`URLProtocol` subclass registered on an ephemeral `URLSessionConfiguration`).

## Batches and order

| Batch | Tasks | Independent review after | Depends on |
|---|---|---|---|
| C (control) | 1 → 2, 3, 4, 5; 6 gated | Task 5 (and again after 6) | Task 6: outputs N1 merged |
| T (telemetry) | 7 → 8 → 9 | Task 9 | none |
| X (crash) | 10 → 11 → 12; 13 | Task 13 | 7 (config, machine class) |
| R6 | 14, 15, 16, 17 | Task 17 | none |
| R7 | 18, 19, 20, 21, 22, 23 | Task 23 | Task 3 for E5/G21 meters; 16 for D15 health |

Each review is a fresh reviewer given the batch diff, this plan's task text, and the Windows files cited; the
reviewer breaks the named mutations and reports any that stay green. Findings are fixed test-first, one commit each.

---

## Batch C: remaining control actions (RC7)

### Task 1: Windows-exported action vectors (M)

**Files:**
- Create: `contracts/control.action-vectors.json`
- Create: `CT/ControlActionVectorExportTests.cs` (scene routes, view mode helpers in `C/`)
- Create: `WT/ControlActionVectorExportTests.cs` (SRT configure, NDI set, multiview setters, restore starters)
- Create: `M/ControlApiTestsRC7.swift` (the replay harness and the RC7 case list)
- Modify: `M/ShellTests.swift` (register `controlRC7Cases`)

**Interfaces:**
- Produces: the file shape every later Batch C task appends to:
  `{"version":1, "sections": {"<sectionName>": {"source": "<Windows file:line>", "writer": "CT|WT", "cases": [Case]}}}`,
  `Case = {"name", "action", "args": [...], "given": {object}, "expect": {"ok": bool, "error"?: string, "effects"?: {object}}}`.
  `given` and `effects` keys are named per section (stated in each task). Every string in `error` is produced by
  running the Windows helper, never typed.
- Produces (Swift): `struct ActionVectorFile: Decodable` and `func loadActionVectors(section: String) -> [ActionVector]`
  in `M/ControlApiTestsRC7.swift`.

- [ ] **Step 1: Write the CT exporter.** `ControlActionVectorExportTests.SceneRoutesSectionIsCurrent` runs every
  `scene-routes` input below through `SceneCueWithRoutesArgs.TryParseRoutes` (`C/SceneCueWithRoutesArgs.cs`) and
  compares the produced cases with the file's `scene-routes` section; `COREVIDEO_REWRITE_ACTION_VECTORS=1` rewrites
  that section only. Inputs (fixture constants): `{"cam":2}`, `{"cam":null}`, `[1]`, `"x"`, `{"":1}`,
  `{"cam":"2"}`, `{"cam":1.5}`, `not json`.
- [ ] **Step 2: Write the WT exporter** `ControlActionVectorExportTests` with one `[Fact]` per section
  (`srt-configure`, `ndi-set`, `multiview-overlays`, `restore-starters`), each writing or comparing its section the
  same way. Its content is specified in Tasks 2, 3, 5, 6. It runs on the Windows CI job only. **First version of
  a WT section:** run the WT test on the Windows CI job without the file section; the failure message prints the
  section JSON; paste it into the file. A section is never hand-typed.
- [ ] **Step 3: Write the Mac drift guard** `control/action-vectors-shape`: the file parses, every section has
  `source`, `writer` and at least one case, every `action` is an id in `contracts/control.manifest.json`, and no
  case name repeats. Run it: FAIL (file missing).
- [ ] **Step 4: Create the file** with the CT section generated by Step 1 (`COREVIDEO_REWRITE_ACTION_VECTORS=1 ~/.dotnet/dotnet test ... --filter ControlActionVectorExportTests`).
- [ ] **Step 5: Run** the CT test (PASS) and `control/action-vectors-shape` (PASS).
- [ ] **Step 6: Commit** `contracts: Windows-exported control action vectors (RC7)`.

**Mutations:** edit one `error` string in the file by hand (CT test red); add a case naming `scene.bogus`
(shape test red); delete the `writer` key (shape test red).

### Task 2: `transport.stream.srt.configure` (S)

**Files:** Modify `M/AppModel.swift` (new `configureSrtForControl`), `M/StreamDestinations.swift`,
`M/ControlSurface.swift` (+ `supportedActionIds`), `contracts/control.platforms.json`, regenerate,
`M/ControlApiTestsRC7.swift`, `contracts/control.action-vectors.json` (WT section `srt-configure`), the Companion
fixture and count.

**Interfaces:**
- Consumes: `loadActionVectors(section: "srt-configure")`; `StreamDestinationPolicy.validateSrt` (`M/StreamDestinations.swift:64`);
  the Keychain `srtPassphrase` (`M/AppModel.swift:608`); the RTMP template `configureRtmpForControl` (`M/AppModel.swift:2774`).
- Produces: `func configureSrtForControl(_ args: [Any?]) -> ControlInvokeResult` on `AppModel`.

**Behaviour** (Windows `W/Services/StreamDestinationControl.cs:9-61` with `StudioStreamOutputValidation.cs:206-250`):
busy (streaming desired, streaming observed live, or destinations locked) refuses first and saves nothing; then
the schema check; mode trimmed and lower-cased and must be `caller|listener|rendezvous`; host without scheme or
query; then `validateSrt` only when `enabled`; an **omitted** passphrase keeps the stored one, an **empty** one
clears it; save, then apply; never starts the stream. No refusal text contains an argument value.

**WT section `srt-configure`** (writer drives `StreamDestinationControl.Configure(true, args, busy, current, save, apply)`;
`given = {busy, storedPassphrase}`, `effects = {saved, applied, mode, host, passphrase: "kept"|"cleared"|"set"}`).
Cases: valid caller with passphrase (16-byte key, `test-passphrase-sentinel`); busy; `[]`; mode
`invalid-mode-test-secret`; host `srt://x`; host `h?x=1`; port `0`; port `65536`; latency `19`; stream id of 513
`a`s; key length `8`; key length 16 with no passphrase; passphrase of 9 and of 80 characters; key 0 with a
passphrase; omitted passphrase with one stored; `""` passphrase with one stored; `enabled=false` with an empty host.

- [ ] **Step 1:** Generate the WT section (Task 1 Step 2 procedure).
- [ ] **Step 2: Write `control/rc7-srt-vectors`** replaying every case against a real `AppModel` with the test
  bridge and the in-memory Keychain seam: assert `ok`, the exact `error`, and each `effects` key against model
  state; plus `invalid-mode-test-secret` and `test-passphrase-sentinel` never appear in any reply.
- [ ] **Step 3: Run it:** FAIL (`Unknown action` / 422).
- [ ] **Step 4: Implement** `configureSrtForControl`, add the Mac twins of the two Windows texts the Mac lacks
  (`"SRT mode must be caller, listener, or rendezvous."`, `"SRT host must be a hostname or IP address without a URL scheme or query."`),
  dispatch, flip the row, regenerate, update counts (43 / 67), re-record the Companion fixture.
- [ ] **Step 5: Run** `control/rc7-srt-vectors`, `control/closure`, `control/dispatch-drive`,
  `control/unavailable-422`, `npm run contract:check`, the Companion vitest in a scratch copy: all PASS.
- [ ] **Step 6: Commit** `mac control RC7: transport.stream.srt.configure`.

**Mutations:** validate before the busy check (busy case red: `effects.saved`); treat an omitted passphrase as
empty (kept case red); apply before save (storage order assertion red); interpolate the mode into the refusal
(secret-leak assertion red).

### Task 3: multiview overlays, the master meter and four actions (M) — RC7 + leftover B24/E5/G21

**Files:** `M/ShellPrefs.swift`, `M/ProductionSettings.swift`, `M/CoreCommands.swift:70-73`,
`M/SettingsPane.swift:79-100`, `M/ShellApp.swift` (`MultiviewTileChrome` `:883-930`, header clock `:777`, pop-out),
`M/ControlSurface+Production.swift`, `M/ControlSurface.swift`, platform file + regenerate, `M/ControlApiTestsRC7.swift`,
new `M/ShellTestsLeftovers.swift`, vectors section `multiview-overlays`, Companion fixture.

**Interfaces:**
- Produces: `struct MultiviewOverlays: Codable, Equatable { var showLabels = true, showTally = true, showMeters = true, showClock = false }`
  (Windows defaults, `W/Services/ProductionOutputPreferencesStore.cs:160-163`); `AppModel.multiviewOverlays`;
  `func setMultiviewOverlay(_ keyPath: WritableKeyPath<MultiviewOverlays, Bool>, _ on: Bool)` (persist + one
  `configure-multiviewer`); `CoreCommands.configureMultiviewer(layoutMode:tileCount:overlays:) -> JSONObject`
  carrying `showLabels, showTally, showMeters, showClock`.
- Produces for R7: `MultiviewOverlayFormatting.programMeter(left:right:showMeters:isProgramTile:) -> (Double, Double)?`.

**Behaviour:**
- Settings > Multiview gets an **"Overlays"** card after the layout card, toggles in Windows order and text:
  "Source labels", "Tally borders", "Audio meters", "Clock" (`W/Views/ProductionSettingsWindow.xaml:850-869`).
- Labels and tally borders draw only when on. The master L/R meter draws only on the Program tile (PGM or
  program tally) and only when meters are on (`W/Services/MultiviewOverlayFormatting.cs:91-92`), from the
  footer's master levels. Main view: the header clock shows only when Clock is on (Windows `StudioWorkspace.xaml:429-433`,
  host `ShowClock="False"`); the pop-out draws all four on the canvas (`MultiviewPopoutWindow.xaml:39-48`).
- The four actions set the flag and always succeed (`SCS:679-690`). State fields: none (Windows publishes none).
- Restored prefs apply before the first `configure-multiviewer`.

**WT section `multiview-overlays`:** writer builds the Windows `configure-multiviewer` command
(`MC/Services/MediaCoreCommandBuilder.cs:360-375`) for four fixtures (all default; all off; labels off only;
clock on only) with `layoutMode "pgmPvwTop"`, `tileCount 10`, and records the command JSON as `effects.command`.

- [ ] **Step 1:** Generate the section.
- [ ] **Step 2: Write tests** `leftovers/multiview-overlay-defaults` (fresh prefs = true,true,true,false; the Mac
  header clock is hidden by default, which changes today's always-on clock), `leftovers/multiview-overlay-command`
  (Mac command for each fixture equals `effects.command` key for key), `leftovers/multiview-program-meter`
  (meter only on the program tile and only with meters on; left 0.4 / right 0.7 pass through),
  `control/rc7-multiview-actions` (each action flips exactly its flag, persists, sends one command; `false` then
  `false` sends one, not two), and a tab snapshot of the Overlays card via `COREVIDEO_SHELL_TAB_SNAPSHOT`.
- [ ] **Step 3: Run:** FAIL.
- [ ] **Step 4: Implement** as above; flip four rows; counts 47 / 63 after Task 2.
- [ ] **Step 5: Run** the new cases, closure, dispatch-drive, unavailable-422, self-check (prefs round trip
  covers `multiviewOverlays`), contract check, design lint: PASS. Review the snapshot pixels against the XAML order.
- [ ] **Step 6: Commit** `mac RC7 + B24/E5/G21: multiview overlays, program meter, multiview.show*.set`.

**Mutations:** default `showClock = true` (defaults red); send the command without the flags (command red);
draw the meter on every tile (meter red); write the flag without persisting (self-check / round-trip red).

### Task 4: `scene.cueWithRoutes` and `scene.route.set` (S-M)

**Files:** `M/ControlSurface.swift`, new `M/ControlSurface+Scenes.swift`, `M/VoltRundown.swift` (extract the layer
resolver), platform file + regenerate, `M/ControlApiTestsRC7.swift`, Companion fixture.

**Interfaces:**
- Consumes: CT section `scene-routes` (parse outcomes); `voltCandidateLayers`-style layer lookup by id.
- Produces: `static func parseSceneRoutes(_ json: String) -> Result<[String: Int?], String>` (the twin of
  `TryParseRoutes`) and `@MainActor static func cueWithRoutes(_ model: AppModel, sceneId: String, routes: [String: Int?]) -> ControlInvokeResult`.

**Behaviour** (`SCS:316-324,845-873`, D-C2): parse first; then, with nothing changed yet, refuse
`"Unknown scene '<id>'."`, `"Unknown route '<routeId>' on scene '<sceneId>'."` (route = a layer id of that scene),
`"Unknown Show Input slot <n> for route '<routeId>'."`; only when all routes resolve, select the scene on Preview,
write each layer's `slotId` (null = active speaker, the Mac's nil), and answer `appliedPreview(sceneId)` or
`"Scene '<id>' was not queued on Preview."`. `scene.route.set` is a one-entry map and has no parse step.

- [ ] **Step 1: Write** `control/rc7-scene-route-parse` replaying the CT section through `parseSceneRoutes`
  (exact strings) and `control/rc7-scene-routes` against a fixture model with scene `two-up` whose layers are
  `two-up-layer-0`, `two-up-layer-1` and slots 1-10. Fixture invariant: the layer ids must exist in the fixture
  scene, slot 11 must not exist. Cases: success sets both layers and Preview; an unknown second route changes
  **neither** layer nor Preview (all-or-nothing); unknown slot; unknown scene; `null` clears to active speaker.
- [ ] **Step 2: Run:** FAIL. **Step 3: Implement**, flip two rows (49 / 61). **Step 4: Run:** PASS (plus closure set).
- [ ] **Step 5: Commit** `mac RC7: scene.cueWithRoutes and scene.route.set (layer ids)`.

**Mutations:** apply routes one by one, refusing midway (all-or-nothing red); accept `1.5` as slot 1 (parse
vector red); select Preview before validating (unknown-route case red: Preview moved).

### Task 5: `scene.restoreStarters` (M)

**Files:** `M/AppModel.swift` (scene load at `:962`, `deleteScene` at `:3487`), `M/ShellPrefs.swift`,
`M/ControlSurface+Scenes.swift`, Scenes tab UI (the restore button), platform file, `M/ControlApiTestsRC7.swift`,
vectors section `restore-starters`, Companion fixture.

**Interfaces:**
- Produces: `ShellPrefs.removedStarterSceneIds: [String]?`; `AppModel.restoreStarterScenes() -> String` returning
  the status sentence; `static let starterSceneIds: [String]` (the Mac built-ins, D-C3).

**Behaviour** (`SVM:4141-4166`): deleting a starter scene records its id; restore re-inserts each missing starter
with its starter layers, clears the removed set, saves prefs, and sets the command status to exactly
`"All starter scenes are already present"`, `"<name> restored"` (one) or `"<n> starter scenes restored"`; the action
always succeeds. A starter id the operator re-created by hand is not duplicated. The Scenes tab gets a
"Restore starter scenes" button where `docs/reference/scenes.md` places it on Windows (verify the XAML element
first and cite it in the commit).

**WT section `restore-starters`:** writer drives the Windows status-sentence builder for 0, 1 (`name "Two-up"`)
and 3 missing scenes and records the three sentences.

- [ ] **Step 1: Write** `control/rc7-restore-starters` (delete one starter → restore → it is back with its layers,
  status = the one-scene vector; delete none → status = the zero vector; delete all three → three-scene vector;
  prefs after restore have an empty removed set) and `leftovers/starter-removed-set-persists` (relaunch prefs
  keep the removed id until restore).
- [ ] **Step 2-4:** FAIL → implement, flip one row (50 / 60) → PASS.
- [ ] **Step 5: Commit** `mac RC7: scene.restoreStarters and the starter catalog`.

**Mutations:** forget to record the removed id on delete (restore case red after relaunch); restore duplicates a
re-created starter (duplicate assertion red); pluralise "1 starter scenes restored" (vector red).

**Batch C review (after Task 5).** Fresh reviewer; additionally confirms the Companion fixture re-record and
that `GET /manifest` lists exactly 50 actions in Windows order.

### Task 6: `transport.ndi.toggle` / `transport.ndi.set` and feedback `ndi` (S) — GATED on outputs N1

Do not start until the outputs lane has merged N1 (NDI output build, loader paths, the independent NDI toggle on
the Mac). If N1 shipped a different model entry point, use it and note it.

**Behaviour** (`SVM:1998-2010`, `SCS:58,274-277`, `W/Services/StudioStreamOutputValidation.cs:262-273`): only turning
on is validated; a refusal leaves state unchanged and its sentence is the reply; off always succeeds; same value is
idempotent; never touches Stream or Record. State field `ndi` = N1's observed NDI-on, not intent.

**WT section `ndi-set`:** writer runs the Windows NDI validation for: no program name; capability missing
(`ndi-output` absent); graphics key name equal to program name; valid.

- [ ] Steps: generate the section; write `control/rc7-ndi-vectors` against the model; FAIL; implement; flip two
  rows and the `ndi` feedback row (52 / 58); PASS; commit `mac RC7: transport.ndi.* and ndi feedback`.

**Mutations:** validate the turn-off path (off-while-unnamed case red); publish intent as `ndi` (feedback case
red, with a fixture where intent is on and the sender is not live).

---

## Batch T: telemetry consent and events (R4)

### Task 7: Windows telemetry fixture; consent store, machine class, payload builder (M)

**Files:**
- Create: `contracts/telemetry.fixtures.json`
- Create: `MCT/TelemetryFixtureExportTests.cs`
- Create: `M/TelemetryConsent.swift`, `M/TelemetryPayload.swift`, `M/ShellTestsTelemetry.swift`

**Interfaces:**
- Produces: `struct TelemetryConfig { endpoint: String?; apiKey: String?; var enabled: Bool; var endpointHost: String; static let defaultEndpoint = "https://telemetry.corevideo.io"; static func resolve(endpointOverride: String?, apiKey: String?) -> TelemetryConfig; static func fromEnvironment(_ env: [String: String]) -> TelemetryConfig }`.
- `struct TelemetryConsentState: Codable { var enabled = false; var lastSentUtc: Date? }` and
  `final class TelemetryConsentStore { init(path: String, fileIO: TelemetryFileIO = .real, retry: (attempts: Int, backoff: TimeInterval) = (20, 0.025)); func load() -> TelemetryConsentState; func setEnabled(_: Bool) throws; func markSent(_: Date) throws; static func defaultPath() -> String }`.
- `struct TelemetryMachineClass: Codable { label: String; cpuCores: Int; ramBand: String; gpuTier: String? }` and
  `enum MachineClassProbe { static func describe(cpu: Int, physicalMemoryBytes: UInt64) -> String; static func ramBand(_ gb: Int) -> String; static func ramGigabytes(_ bytes: UInt64) -> Int /* half to even */; static func gpuTier(brandString: String?) -> String?; static func probe() -> TelemetryMachineClass }`.
- `enum TelemetryPayloadBuilder { static func outputConfigShape(snapshot: JSONObject?) -> JSONObject; static func build(name: String, version: String, sessionLengthSeconds: Int64, snapshot: JSONObject?, crashCountSinceLastSend: Int, machine: TelemetryMachineClass) -> JSONObject; static func serializeWire(_: JSONObject) -> String; static func serializePreview(_: JSONObject) -> String }`
  — keys in the Windows declaration order (boundary finding 3), nil `gpuTier` omitted.
- `enum TelemetryCrashCount { static func countSince(reports: [CrashReportRecord], lastSentUtc: Date?) -> Int }`
  (`CrashReportRecord` is defined here as `Codable { reportId: String; sentAtUtc: Date; dumpFiles: [String] }`;
  Task 10 reuses it).

- [ ] **Step 1: Write the MCT exporter** `TelemetryFixtureExportTests.FixtureIsCurrent` (runs on this Mac).
  It writes/compares `contracts/telemetry.fixtures.json` (`COREVIDEO_REWRITE_TELEMETRY_FIXTURES=1`) with:
  - `payloads`: input = a **core `media-core-sync` response** JSON (`{"snapshot":{...}}`) parsed with the same
    parser the shell's `LastSnapshot` comes from (`MC/Services/CoreProtocolParser.cs:560`; confirm that is the path,
    and if `Participants` is filled by a Zoom merger rather than the parser, put the participant list in the
    fixture's input under the field that merger reads and say so in the file's `notes`), plus `version "1.2.3"`,
    `sessionLengthSeconds 3600`, `crashCount 2`, machine `{label "mac-arm64-cpu12-ram36gb", cpuCores 12,
    ramBand "32-64GB", gpuTier "apple-m3-pro"}`; output = `SerializeWire` and `SerializePreview` strings. Three
    inputs: the secret-laden snapshot from `MCT/TelemetryPayloadBuilderTests.cs:23-81` re-expressed as core JSON
    (stream key `live_SUPERSECRETSTREAMKEY_9f3a2b`, guest `Alice Q. Attendee`, a recording folder, a meeting URL);
    an engine-off null snapshot; a snapshot with one NDI sender live and nothing else (NDI is not streaming,
    `MCT/...:109`). Fixture invariants: capture devices include a duplicate id and a `detected` device so the count
    (2) differs from the raw list length (4); `gpuTier` null in one input to prove omission.
  - `ramBands`: the Windows `RamBand` table for 0, 4, 7, 8, 15, 16, 31, 32, 63, 64, 127, 128, 256.
  - `ramRounding`: `Math.Round(x / 2^30)` for byte counts equal to 7.5, 8.5, 16.4999, 35.9 GiB (expect 8, 8, 16, 36:
    half to even).
  - `consentFiles`: the JSON `TelemetryConsentStore.Save` writes for `{enabled:true, lastSentUtc: 2026-11-22T10:00:00Z}`
    and for the default.
  - `config`: `Resolve` for (null, null), ("", "k"), ("  https://staging.example/  ", " k "), ("ftp://x", "k"),
    ("not a url", "k") with `Enabled` and `EndpointHost`.
- [ ] **Step 2: Run** the exporter with the rewrite switch, then without: PASS.
- [ ] **Step 3: Write Swift tests** in `M/ShellTestsTelemetry.swift`:
  `telemetry/payload-matches-windows` (each `payloads` input: Mac wire JSON parses to an object equal to Windows'
  parsed wire JSON, and its top-level and nested key **order** equals Windows'; the secret strings from Step 1 and
  `pid-secret`, `rtmp://`, `zoom.us` are absent), `telemetry/ram-bands`, `telemetry/ram-rounding-half-even`,
  `telemetry/machine-label` (`describe(cpu: 12, physicalMemoryBytes: 36 GiB) == "mac-arm64-cpu12-ram36gb"`; brand
  `"Apple M3 Pro"` → `apple-m3-pro`, `"Apple M2"` → `apple-m2`, `"Intel(R) Core(TM) i9"` → nil, nil → nil),
  `telemetry/config-matches-windows`, `telemetry/consent-defaults-off` (no file, empty file, `{}`, `{"enabled":"yes"}`,
  a directory at the path, a file the `TelemetryFileIO` fake reports as locked for every attempt: all `enabled == false`),
  `telemetry/consent-file-shape` (Mac writes JSON equal to `consentFiles`), `telemetry/revoke-lands-when-unreadable`
  (fake read throws every attempt: `setEnabled(false)` writes the disabled default; `setEnabled(true)` throws and
  writes nothing), `telemetry/marksent-never-revokes` (read throws: `markSent` throws, file still `enabled:true`),
  `telemetry/crash-count` (reports at t1 < t2 < t3, lastSent between t1 and t2 → 2; nil → 3).
- [ ] **Step 4: Run:** FAIL (types missing).
- [ ] **Step 5: Implement.** File writes go temp-then-`rename(2)`; the retry loop takes its sleep from
  `TelemetryFileIO` so tests never wait.
- [ ] **Step 6: Run:** PASS; self-check PASS.
- [ ] **Step 7: Commit** `mac R4 (1/3): telemetry fixture from Windows, consent store, payload, machine class`.

**Mutations:** round RAM half away from zero (rounding red at 8.5); emit `gpuTier: null` (omission red); read
`streaming` from intent instead of sender status (NDI-only input red); make a corrupt file default to the last
in-memory value (consent red); swap two payload keys (order red).

### Task 8: event client and service: heartbeat, session-end, audit log (M)

**Files:** Create `M/TelemetryService.swift`; modify `M/AppModel.swift` (`handleQuitRequest` `:5589`,
`confirmQuit`), `M/ShellTestsTelemetry.swift`.

**Interfaces:**
- Consumes: Task 7 types.
- Produces: `enum TelemetrySendStatus { accepted, retryLater, rejected }`,
  `struct TelemetrySendResult { status; reportId: String?; httpStatus: Int?; message: String?; retryAfter: TimeInterval? }`,
  `typealias TelemetryTransport = (_ url: URL, _ apiKey: String, _ body: String, _ timeout: TimeInterval) async -> TelemetrySendResult`,
  `enum TelemetryEventClient { static func classify(status: Int, headers: [String: String], body: Data, now: Date) -> TelemetrySendResult; static func urlSessionTransport(_ session: URLSession) -> TelemetryTransport }`,
  `@MainActor final class TelemetryService { init(config:, consent:, snapshot: @escaping () -> JSONObject?, crashReports: @escaping () -> [CrashReportRecord], version: @escaping () -> String, machine: @escaping () -> TelemetryMachineClass, transport:, clock: TelemetryClock, log: @escaping (String) -> Void); func start(); func previewJSON() -> String; func fireSessionEnd() -> Task<Void, Never>?; var isConfigured: Bool }`,
  `protocol TelemetryClock { var now: Date { get }; func schedule(after: TimeInterval, _ fire: @escaping @MainActor () -> Void) -> TelemetryTimerToken }`.

**Behaviour** (`W/Services/TelemetryEventService.cs`, `MC/Services/TelemetryEventClient.cs`):
`start()` starts the session clock; schedules the heartbeat at 24 h, repeating, **only if configured at start**;
each fire sends only if consent is on at fire time, with a 15 s timeout; a `retryLater` with `retryAfter > 0`
reschedules the next fire to that delay. `fireSessionEnd()` sends once per process (idempotent) with a 2 s
timeout, never honours `Retry-After`, returns the task or nil when consent or config is missing. Before every POST
the log gets `telemetry: sending '<name>' (<bytes> bytes): <wire>`; after, `accepted (reportId=...)` or
`not accepted (<status>, http=<n|n/a>): <message>`. An accepted send calls `markSent(clock.now)`. Quit
(D-T4): idle quit with a non-nil session-end task → `.terminateLater`, reply when the task ends or 2 s on the
clock, whichever first; confirm-quit starts the task first.

- [ ] **Step 1: Write tests:** `telemetry/client-classify` (202 + `{"reportId":"r1"}` → accepted r1; 429 +
  `Retry-After: 30` → retryLater 30 s; 503 with an HTTP-date 90 s after the injected `now` → 90 s; 400 → rejected; a
  400 body of 400 `x` → message carries exactly 300 `x`); `telemetry/client-request-shape` (URLProtocol stub records
  `POST /v1/events`, `Authorization: Bearer k`, `Content-Type: application/json`, body = wire JSON; endpoint
  `https://t.example/base` still posts to `/v1/events`, the Windows `new Uri(endpoint, "/v1/events")` rule);
  `telemetry/no-send-without-consent`, `telemetry/no-send-without-key` (transport fake asserts never called; the
  heartbeat timer is not even scheduled without a key); `telemetry/session-end-once`;
  `telemetry/heartbeat-cadence` (manual clock: nothing before 24 h; one at 24 h; consent turned off at 30 h → the
  48 h fire sends nothing; retryLater 600 s → next fire at +600 s); `telemetry/audit-log-precedes-send` (log line
  with the exact wire captured before the transport call); `telemetry/marks-sent-only-on-accept`;
  `quit/idle-waits-for-session-end` (consent+key, a transport that **never returns**: `.terminateLater`, reply
  `true` after exactly 2 s of manual clock, not before); `quit/idle-unchanged-without-consent` (`.terminateNow`);
  `quit/idle-replies-when-send-finishes` (transport returns at 0.3 s → reply at 0.3 s). Rule 9: one transport fake
  honours its `timeout` argument by returning `retryLater` at the deadline, one ignores it; the quit test must pass
  against both.
- [ ] **Step 2-4:** FAIL → implement → PASS (plus every existing `quit/*` case).
- [ ] **Step 5: Commit** `mac R4 (2/3): telemetry events, heartbeat, session-end on quit`.

**Mutations:** honour `Retry-After` on session-end (session-end assertion red); schedule the heartbeat without a
key (no-key red); reply to quit only when the send finishes (never-returning transport red); log after the POST
(audit red).

### Task 9: "Diagnostics & telemetry" card (S)

**Files:** `M/ShellApp.swift` (`ZoomPane`, after the "Zoom status" card ending at `:1510`), `M/AppModel.swift`
(owns the `TelemetryService`), `M/ShellTestsTelemetry.swift`; correct the header promise in `M/Diagnostics.swift:20-21`
to "nothing in this file sends a byte off the machine; uploads live in Telemetry*.swift and CrashReport*.swift and
need consent".

**Layout and copy** (`W/Views/SettingsPage.xaml:187-221`, strings `W/ViewModels/SettingsViewModel.cs:140-152`), in
order inside a card styled like "Zoom status": title **"Diagnostics & telemetry"**; toggle **"Share anonymous health
data"** (Off/On); body text **"Send anonymous health data on app close and once a day: app version, session length,
output counts (recording / streaming / virtual camera on-off, ISO / capture / participant counts), crashes since the
last send, and a hardware class (CPU cores, RAM band, GPU tier). No names, meeting IDs, stream keys, URLs, or file
paths are ever included."**; status line, 12 pt, accent colour: configured → **"Telemetry endpoint and API key
configured."**, otherwise **"Telemetry API key is missing in this build (endpoint is configured) — nothing is sent even
when enabled."**; ghost button **"Preview what's sent"**; when a preview exists, a bordered monospaced, selectable,
scrolling block (max height 320) with the indented session-end payload. The preview works with consent off. The toggle
persists on change (never on load) and logs `telemetry: consent ENABLED by operator` / `disabled`.

- [ ] **Step 1: Write** `telemetry/card-copy` (the four strings exactly), `telemetry/toggle-persists-not-on-load`
  (loading `enabled:true` writes nothing; flipping writes once), `telemetry/preview-without-consent`, and a
  `COREVIDEO_SHELL_TAB_SNAPSHOT` capture of the Zoom tab with the preview open.
- [ ] **Step 2-4:** FAIL → implement → PASS; review the capture against the XAML order.
- [ ] **Step 5: Commit** `mac R4 (3/3): Diagnostics & telemetry card`.

**Mutations:** persist during the initial load (load assertion red); preview gated on consent (preview red).

**Batch T review.** Fresh reviewer; additionally greps the diff for any `print`/log of the API key.

---

## Batch X: crash upload (R5) and release symbols

### Task 10: `.ips` scanner, watermark, sanitizer (S-M)

**Files:** Create `M/CrashReports.swift`, `M/ShellTestsCrash.swift`, fixture
`mac-shell/Tests/Fixtures/crash/CoreVideoProShell-fixture.ips` (a sanitized copy of a real report; see invariants).

**Interfaces:**
- Produces: `struct CrashReportFile { path: String; processName: String; lastWrite: Date; length: Int64 }`;
  `enum CrashReportScanner { static let monitoredProcesses = ["CoreVideoProShell","corevideo-native","corevideo-zoom-engine","corevideo-plugin-host","corevideo-browser-host"]; static func processName(forFile: String) -> String?; static func scan(directories: [String], after: Date, fs: CrashFS) -> [CrashReportFile]; static func defaultDirectories() -> [String] }`;
  `struct CrashReportWatermark: Codable { var lastSeenUtc: Date; var reports: [CrashReportRecord] }`;
  `final class CrashReportWatermarkStore { init(path:); func load(now: Date) -> CrashReportWatermark; func save(_:) throws; static let firstRunLookback: TimeInterval = 3 * 86400 }`;
  `enum IpsSanitizer { static func sanitize(_ ips: Data) -> Data? }`.

**Behaviour:** file name `<process>-YYYY-MM-DD-HHMMSS.ips` (optionally `.<n>.ips`); only exact monitored process
names match (`corevideo-native-tests-...` must **not** match — a real file of that name sits in this Mac's folder);
newer than the watermark only; oldest first; a missing or unreadable directory is skipped without hiding the others;
duplicates listed once. Watermark JSON and first-run lookback as Windows (boundary finding 2); a corrupt file is the
fresh default. **Sanitizer allow-list:** header keys `app_name, app_version, build_version, bug_type, os_version,
platform, slice_uuid, timestamp, name`; body keys `procName, cpuType, osVersion, version, captureTime, exception,
termination, faultingThread, threads, usedImages, legacyInfo, asi, ktriageinfo, sharedCache` — with `usedImages[]`
reduced to `uuid, base, size, arch, name, source` (no `path`), `threads[]` to `triggered, id, threadState, queue,
frames`, and any remaining string anywhere containing `/Users/` that is not `/Users/USER/` replaced by
`/Users/USER/`. Everything else is dropped. Output = header line + `\n` + body JSON, like the input. Unparsable
input → nil (the report is then listed in the manifest as skipped, never sent raw).

- [ ] **Step 1: Make the fixture.** Copy one real `.ips` (`ls ~/Library/Logs/DiagnosticReports | grep CoreVideoProShell`),
  replace every value of `crashReporterKey`, `bootSessionUUID`, `sleepWakeUUID`, `incident`, `incident_id`,
  `userID` with obviously fake sentinels (`SENTINEL-KEY-0001` etc.), insert one `"/Users/jdoe/Secret/x.dylib"`
  string inside an `asi` message, set `usedImages[1].path` to `"/Users/jdoe/Library/x.dylib"` (unredacted, so
  the keep-path mutation bites), keep the structure. Invariants the tests rely on: the faulting thread index
  points at a thread whose frame 0 has `imageIndex` 0; `usedImages[0].uuid` is a fixed value the test names.
- [ ] **Step 2: Write tests:** `crash/name-match` (table incl. `corevideo-native-tests-2026-11-22-025659.ips` → nil,
  `CoreVideoProShell-2026-11-22-032121.ips` → shell, `corevideo-zoom-engine-2026-11-22-010101.1.ips` → engine,
  `Safari-...ips` → nil); `crash/scan-order-and-watermark` (fake FS with three directories, one unreadable);
  `crash/offer-once` (scan, advance watermark to max, rescan → empty; two crashes of different processes before the
  next launch → both offered, oldest first); `crash/watermark-first-run` (missing file → lastSeen = now − 3 days on
  the injected clock); `crash/sanitize-drops-identity` (none of the sentinels, no `vmSummary`, `trialInfo`,
  `coalitionName`, `parentProc`, no `/Users/jdoe`); `crash/sanitize-keeps-symbolication` (fault thread frames,
  `usedImages[0].uuid` and `base` equal the fixture constants); `crash/sanitize-garbage-is-nil`.
- [ ] **Step 3-5:** FAIL → implement → PASS.
- [ ] **Step 6: Commit** `mac R5 (1/3): .ips scanner, offer-once watermark, sanitizer`.

**Mutations:** prefix-match process names (`-tests` row red); keep `usedImages.path` (identity red if the fixture
path is not `/Users/USER`, so the fixture keeps one unredacted path to make this bite); drop `usedImages.uuid`
(symbolication red); advance the watermark only after Send (offer-once red).

### Task 11: archive builder, uploader, coordinator (M)

**Files:** Create `M/CrashReportArchive.swift`, `M/CrashReportUploader.swift`; modify `M/Diagnostics.swift`
(expose the existing bundle export for the coordinator), `M/ShellTestsCrash.swift`.

**Interfaces:**
- Consumes: Task 7 `TelemetryConfig`, `MachineClassProbe.describe`; Task 10 types; `Redactor.redactLines`.
- Produces: `struct CrashArchiveRequest { reports: [CrashReportFile]; logFiles: [String]; supportBundlePath: String?; maxTotalBytes: Int64 = 24 << 20; logTailBytes: Int64 = 2 << 20; appVersion: String }`,
  `struct CrashArchiveResult { zipPath; zipLength; included: [String]; skipped: [String]; manifestText: String }`,
  `enum CrashReportArchiveBuilder { static func build(zipPath: String, request:, now: Date) throws -> CrashArchiveResult }`,
  `enum CrashReportUploader { static func upload(endpoint: URL, apiKey: String, version: String?, machineClass: String?, reason: String?, zipPath: String, transport: CrashTransport) async -> TelemetrySendResult }`,
  `@MainActor final class CrashReportCoordinator { static func make(config:, store:, directories:, clock:) -> CrashReportCoordinator?; func scanAndAdvance() -> [CrashReportFile]; func send(_: [CrashReportFile], writeSupportBundle: (String) async -> Bool) async -> CrashSendOutcome }`,
  `struct CrashSendOutcome { success: Bool; canRetry: Bool; message: String; reportId: String?; zipPath: String? }`.

**Behaviour** (`MC/Services/CrashReportArchiveBuilder.cs`, `MC/Services/CrashReportUploader.cs`,
`W/Services/CrashReportCoordinator.cs`): zip entries at the root — `manifest.txt`, `dumps/<name>.ips` (sanitized),
`logs/CoreVideoPro-shell.log` and `logs/CoreVideoPro-core.log` (redacted tails, last 2 MiB), `support-bundle.json`
(the existing redacted Mac export); manifest lines exactly as Windows: `CoreVideo Pro crash report`, `Created: <ISO>`,
`App version: <v>`, `Content budget: <n> bytes (uncompressed)`, blank, `Included: ...` lines in Windows wording,
blank, `Skipped: ...` lines. Largest-first greedy fit of reports into the budget left after logs and bundle and
64 KiB slack; the included reports ordered oldest first. Zip with `/usr/bin/ditto -c -k` (no `--keepParent`) from a
staging folder. Files go to `~/Library/Application Support/CoreVideoPro/support-bundles/crash-report-<yyyyMMddHHmmss>.zip`
and stay there after upload. Upload: request shape in Global Constraints, 120 s timeout, outcome classes as
events. Reason header `wer-dump: <processes>` becomes **`ips: <processes>`** (distinct, comma-joined, case-insensitive
dedupe); `wer-dump` is a Windows word. Accepted → append `CrashReportRecord` to the watermark. Messages verbatim from
`W/Services/CrashReportCoordinator.cs:155-178`: `"Thanks — the report was sent. Reference ID: <id>. A copy stays at <zip>."`
(without the id sentence when none), `"<server message> The report is saved at <zip> — nothing was lost."`,
`"Could not assemble the crash report: <error>"`. `make` returns nil without a key.

- [ ] **Step 1: Write tests:** `crash/archive-entries-and-manifest` (two reports, two logs, a bundle: entries and the
  manifest text exactly, against a manual clock); `crash/archive-log-tail-redacted` (a log whose last 2 MiB contain
  `rtmp://a.rtmp.youtube.com/live2/live_SUPERSECRETSTREAMKEY_9f3a2b`: the key is absent, the tail keeps the last
  line); `crash/archive-budget-largest-first` (budget 1 MiB, reports of 700, 500, 200 KiB → 700 and 200 included,
  500 skipped with the Windows sentence); `crash/archive-single-oversized`; `crash/upload-request-shape` (URLProtocol
  stub: `POST /v1/crashes`, Bearer, `Content-Type: application/zip`, the three headers, body bytes = zip bytes);
  `crash/upload-outcomes` (413 → rejected, no retry; 429 `Retry-After: 30` → retryLater; network error → retryLater;
  missing zip → rejected with `Could not read the report archive: ...`); `crash/coordinator-disabled-without-key`;
  `crash/coordinator-records-report-id`.
- [ ] **Step 2-4:** FAIL → implement → PASS.
- [ ] **Step 5: Commit** `mac R5 (2/3): crash archive, uploader, coordinator`.

**Mutations:** smallest-first fit (budget red); raw log tail (redaction red); `--keepParent` (entry names red);
record a report on retryLater (records red).

### Task 12: the crash bar (S)

**Files:** `M/ShellApp.swift` (`RootView` `:268-292`), `M/AppModel.swift` (launch scan off the main actor, once per
process), `M/ShellTestsCrash.swift`.

**Layout and copy** (`W/Views/StudioWorkspace.xaml:1337-1356`, `W/Views/StudioWorkspace.xaml.cs:137-226`): a warning
bar overlaying the top of the content area under the header and readiness strip (Windows `Grid.Row="2"`, top,
margin 16/10/16/0, above every tab), closable, title **"CoreVideo Pro crashed last session"**, message
**"A crash dump from <process> was found (1 dump|<n> dumps). Send a report to help fix it? The report (dump + recent
logs, secrets redacted) is saved locally either way — nothing is sent unless you click Send."** (Mac wording: "crash
report" for "crash dump", D-X1; the rest verbatim), action button **"Send report"**. On click: button disabled,
message "Assembling and sending the crash report…"; then title "Crash report sent" (success style) or "Crash report
not sent" (warning), the outcome message; button hidden on success or a non-retryable failure, else relabelled
**"Try again"**. A thrown error → error style, "Sending failed: <message>". No scan and no bar without a key. The
launch scan never delays first paint.

- [ ] **Step 1: Write** `crash/bar-copy-and-states` (model-level states for: 1 report, 3 reports, success,
  retryable, non-retryable, throw), `crash/no-bar-without-key`, `crash/scan-once-per-process`, and a
  `COREVIDEO_SHELL_SNAPSHOT` capture with the bar open over the Studio tab.
- [ ] **Step 2-4:** FAIL → implement → PASS; review the capture.
- [ ] **Step 5: Commit** `mac R5 (3/3): crash report bar`.

**Mutations:** keep "Try again" after a 413 (non-retryable state red); scan on every tab appearance (once red).

### Task 13: release symbols (S-M) — release lane

**Files:** `native/CMakeLists.txt` (Apple + Release: add `-g` to C, C++ and Objective-C++ compile options),
`scripts/release-macos.sh`, `scripts/make-macos-shell-bundle.sh` (strip before sign), new
`scripts/tests/test-release-macos-symbols.sh`. Coordinate with the outputs lane, which also edits these scripts.

**Behaviour (D-X3):** after the clean builds and **before** any `codesign`: for each product Mach-O
(`CoreVideoProShell`, `CoreVideoProCamera`, `corevideo-native`, `corevideo-plugin-host`, `corevideo-child-guard`,
`corevideo-zoom-engine`, `corevideo-browser-host` when present) run `dsymutil <bin> -o <stage>/<bin>.dSYM`, verify
with `dwarfdump --uuid` that the dSYM UUID equals the binary's, then `strip -S`. Zip the dSYMs into
`dist/macos/<version>/CoreVideoPro-symbols-<version>-arm64.zip`; add `"symbols": {"<bin>": "<uuid>"}` to
`manifest.json`. Fail the release if any product dSYM is missing, empty (no `__DWARF` section), or mismatched.
Vendor binaries (Zoom SDK, ffmpeg) are skipped by name. Whether `swift build -c release` already emits DWARF is
UNVERIFIED: the check proves it either way.

- [ ] **Step 1: Write** `scripts/tests/test-release-macos-symbols.sh`: builds a two-file C program with and
  without `-g`, runs the script's symbol function on it; expects success and a matching UUID with `-g`, and a
  non-zero exit naming the binary without it.
- [ ] **Step 2: Run:** FAIL (function missing). **Step 3: Implement** the function and wire it in.
- [ ] **Step 4: Run** the test (PASS), then `scripts/release-macos.sh --skip-notarize` and confirm: symbols zip
  present, every UUID in the manifest equals `dwarfdump --uuid` of the signed binary in the app, `strings` on the
  signed `corevideo-native` finds no `/Users/`.
- [ ] **Step 5: Run** `python scripts/mac-show-drill.py --seconds 40 --load 8` on the `--skip-notarize` core and
  compare per-frame delivery with the last recorded run (a `-g` build must not change codegen; any new missed
  deadline stops the task).
- [ ] **Step 6: Commit** `release(mac): dSYMs, strip -S, symbols zip and UUIDs in the manifest`.

**Mutations:** run `strip -S` before `dsymutil` (empty-dSYM check red); drop `-g` (missing-DWARF check red); sign
before strip (codesign verify in the script red, because stripping after signing invalidates the signature).

**Batch X review.** Fresh reviewer; additionally symbolicates one sanitized fixture-style report from a
`--skip-notarize` build with `atos -o <dSYM> -l <base> <address>` and confirms a function name comes back.

---

## Batch R6: operator-facing leftovers

### Task 14: Zoom page join rules (C1, C3, C5) (M)

**Files:** `M/ShellApp.swift` (`ZoomPane` `:1364-1510`), `M/ShellPrefs.swift` (`recentMeetings` `:135`),
`M/AppModel.swift` (`:2710`), new `M/ZoomJoinPolicy.swift`, `M/ShellTestsLeftovers.swift`.

**Interfaces:** `struct RecentZoomMeeting: Codable, Equatable { meetingNumber: String; displayName: String; webinar: Bool; label: String /* "<meetingNumber> - <displayName>" */ }`;
`enum ZoomJoinPolicy { static func canEditJoinFields(inMeeting: Bool) -> Bool; static func joinActionHint(sdkBlocked: Bool, sdkUnavailable: Bool, oauthAvailable: Bool, signedIn: Bool) -> String }`;
`ShellPrefs.recentMeetingEntries: [RecentZoomMeeting]?` with migration from the old `[String]` (each string becomes
`meetingNumber` = the parsed id, `displayName` = the current display name, `webinar` false).

**Behaviour:** C1 — URL, display name, Webinar and Recent meetings are disabled **only while in a meeting**
(`W/ViewModels/SettingsViewModel.cs:231`); the Mac passcode field follows the same rule. C3 — Recent meetings is a
picker headed "Recent meetings" with placeholder "Select a recent meeting", hidden when empty, after the status
block and before "Zoom account" (`W/Views/SettingsPage.xaml:76-90`); rows read `"<number> - <name>"`
(`MC/Services/RecentZoomMeetingStore.cs:12`); picking fills URL, name and Webinar, sets the status to
"Ready to rejoin <number>", is ignored in a meeting, and the selection clears. C5 — Join is hidden in a meeting
and otherwise never disabled for a reason; the hint (tooltip **and** a permanent line under the button row,
`SettingsPage.xaml:122,142-144`) is, in order: "Zoom is unavailable. Open Health for details." / "Zoom needs
attention. Open Health for details." when the SDK blocks; "Sign in with Zoom before joining this meeting in
CoreVideo." when OAuth is available and not signed in; else "Join with CoreVideo, or open this meeting in the Zoom app."
An empty meeting field at Join gives the Windows join error (read `SettingsViewModel` join path; quote it).

- [ ] **Step 1: Write** `leftovers/c1-fields-lock-only-in-meeting` (joining → editable; in meeting → locked),
  `leftovers/c3-recent-migration` (old prefs `["https://zoom.us/j/123", "456"]` → two entries), `leftovers/c3-pick`
  (fills, status text, ignored in meeting), `leftovers/c3-hidden-when-empty`, `leftovers/c5-hint-order` (all 8
  combinations of the four booleans; each expected sentence a constant), `leftovers/c5-join-not-disabled`, and a
  Zoom tab snapshot.
- [ ] **Step 2-4:** FAIL → implement → PASS; self-check (migration round trip).
- [ ] **Step 5: Commit** `mac R6: Zoom join rules (C1, C3, C5)`.

**Mutations:** lock while joining (C1 red); keep the empty-field disable (C5 red); swap hint priority (order red).

### Task 15: Studio header and rail (G4, G6, G14, G23) (S-M)

**Files:** `M/ShellApp.swift` (`:475-477` pills, `ReadinessStrip` `:705-743`, rail `:984,997`, Room chip `:800`,
rail visibility `:752`), `M/StudioFooter.swift:44` (reuse `setupSummary`), `M/ShellTestsLeftovers.swift`.

**Behaviour:** G4 — an always-visible **ZOOM** pill right of CAPTURE, green when Zoom is live, red otherwise
(`W/Views/StudioWorkspace.xaml:263-275`, `SVM:2774-2797`). G6 — the strip is [icon] **"SHOW"** | status | setup
summary (`StudioWorkspace.xaml:277-303`); status: no slot assigned → "Add sources"; none in the show → "Choose
inputs for the show"; recording or streaming → "On air"; else "Ready" (`SVM:2870-2886`). The words "Show readiness"
and the long guidance go. G14 — delete the rail summary line and the "Tap a scene to queue" hint. G23 — "Room" is a
toggle that shows or hides the room rail, default **hidden**, not persisted (`SVM:129`), tooltip "Show or hide the
Video-in-room panel (the Sources tab covers this)"; the rail header gets an ✕ with tooltip "Hide the room panel
(Sources covers this)" (`StudioWorkspace.xaml:481-490,593-597`).

- [ ] **Step 1: Write** `leftovers/g6-show-status` (4 states from fixture slot sets), `leftovers/g4-zoom-pill`
  (offline → red, live → green, the pill exists offline), `leftovers/g23-room-default-hidden`, and a **text guard**
  `leftovers/studio-forbidden-strings` that scans `M/ShellApp.swift` for "Show readiness", "Tap a scene to queue"
  and the `1920x1080 · 60 fps` literal (the Mac twin of `WT/OperatorUiTelemetryPlacementTests.cs:36-41`); Studio
  snapshot.
- [ ] **Step 2-4:** FAIL → implement → PASS.
- [ ] **Step 5: Commit** `mac R6: ZOOM pill, SHOW strip, rail cleanup, Room toggle`.

**Mutations:** "On air" from intent only when observed is off (state red with a fixture where recording is desired
but not live — Windows uses the requested flags, so verify which and pin it); Room default visible (default red).

### Task 16: room participants (G25) and feed health (shared with D15) (M)

**Files:** new `M/RoomParticipants.swift`, `M/ShellApp.swift` (`:1253` and the rail rows), `M/AppModel.swift`
(roster → rows, selection → audio-processing target), `M/ShellTestsLeftovers.swift`.

- [ ] **Step 1: Settle the source (D-R3).** Read where Windows' room-rail `Participant.RoleLabel`/`Health` come from
  (`W/Services/ParticipantMapper.cs:8-83` parses labels `host|presenter|speaker|panelist|panel` and
  `video-off|low-resolution|recovering`; follow `LiveProductionParticipantContext` back to the roster). Record the
  answer and the Mac snapshot field that carries the same fact, or "none", in the commit message. No role is
  shown that the roster does not report.
- [ ] **Step 2: Write** `leftovers/g25-rows` (header "Video in room (N)"; row = initials, name,
  `"<Role> · <HealthLabel>"`, "Talking" when talking, badge HOST/SPEAKER/GUEST/PANEL; health labels Live, Low res,
  Recovering, Video off from fixture roster rows), `leftovers/g25-select-sets-processing-target`, and
  `leftovers/d15-feed-summary` ("No Zoom feeds — join a meeting", "<n> feeds · all healthy",
  "<n> feeds · <k> need attention"; row states per `W/Models/ProductionModels.cs:977-1030`).
- [ ] **Step 3-5:** FAIL → implement `RoomParticipants.rows(roster:selectedId:)` and
  `FeedHealthSummary.make(rows:)` → PASS.
- [ ] **Step 6: Commit** `mac R6: room participants and feed health`.

**Mutations:** hard-code GUEST when the roster reports a role (only if Step 1 found one; else note "n/a");
count video-off as healthy (summary red).

### Task 17: macOS menu bar (F6) (S)

**Files:** `M/ShellApp.swift:110-113`, `M/ShellTestsLeftovers.swift`.

**Behaviour (D-R2):** a `Settings {}` scene hosting the existing Settings pane (⌘,); a "Production" menu with Take
(the footer's Take entry), Fade to Black, Pop Out Multiview; each item disabled exactly when its footer control is.
Key equivalents must not collide with the transport hotkeys (`model.syncTransportHotkeys`); list them in the commit.

- [ ] Steps: write `leftovers/f6-menu-commands-call-the-model` (each command routes to the same entry point as the
  footer, through the test seam the `keys/handler-real-commands` case uses); FAIL; implement; PASS; commit
  `mac R6: Settings scene and Production menu`.

**Mutations:** Take menu item bypasses `canTake` (case red).

**Batch R6 review.** Fresh reviewer with the four Studio/Zoom snapshots beside the Windows XAML.

---

## Batch R7: support and polish leftovers

### Task 18: Health rows D4, D11, D13 (S)

**Files:** `M/Diagnostics.swift` (audio section `:1282-1310`, a Monitor tile, a Lower third tile),
`M/AudioObservationPolicy.swift` (add `describe`), `M/ShellTestsLeftovers.swift`.

**Behaviour:** D4 — "Audio telemetry unobserved" or "Audio telemetry <state> · <age> ms old · revision <n>"
(`MC/Services/AudioObservationPolicy.cs:45-51`; port `AudioObservationPolicyTests` vectors). D13 — tile "Lower
third": the native lower-third status or "Native lower-third state waiting for media core.", then
"<preset> · in <N> ms / out <N> ms" (`W/Views/DiagnosticsView.xaml:166-175`, `SVM:1206-1212`; values at
`M/AppModel.swift:5237`). D11 — the Monitor tile's recovery line prints only counters the Mac actually has, in the
Windows order and `name=value` form (`W/ViewModels/StudioViewModel.AudioMonitorControl.cs:11-28`); counters the Mac
lacks are omitted, not zero (honest-reporting rule). List the omitted names in the commit.

- [ ] Steps: write `leftovers/d4-describe` (Windows vectors), `leftovers/d13-tile`, `leftovers/d11-only-observed-counters`;
  FAIL; implement; PASS; Diagnose tab snapshot; commit `mac R7: Health audio observation, monitor recovery, lower third`.

**Mutations:** print an unobserved counter as `0` (D11 red); age in seconds (D4 red).

### Task 19: Extra logs (D6) (S)

**Files:** `M/Diagnostics.swift`, `M/AppModel.swift` (`onConnected`), `M/CoreCommands.swift`, `M/ShellTestsLeftovers.swift`.

**Behaviour** (`W/Views/DiagnosticsView.xaml:57-71`, `W/ViewModels/SettingsViewModel.cs:205-207,343-400`): card
"Detailed diagnostics", status line left, toggle "Extra logs" right; copy on: "Detailed frame, timing, and routing
traces are being captured for this session. Turn this off during normal shows."; off: "Production logging is active.
Detailed frame and timing traces are off."; sends `{"type":"set-verbose-diagnostics","enabled":bool}`; session only,
never persisted; re-sent on each core connect **only when on**.

- [ ] Steps: write `leftovers/d6-command-and-reassert` (on → one command; reconnect → re-sent; off + reconnect →
  nothing) and `leftovers/d6-not-persisted`; FAIL; implement; PASS; commit `mac R7: Extra logs`.

**Mutations:** re-send `false` on reconnect (red); persist the flag (red).

### Task 20: Settings deep links (B1) and GPU tier (B4) (M)

**Files:** `M/SettingsPane.swift`, `M/StudioFooter.swift:524,555`, new `M/GpuTierPolicy.swift`, `M/ShellPrefs.swift`,
`M/ShellTestsLeftovers.swift`.

**Behaviour:** B1 — the footer's recording and streaming "More" open Settings scrolled to that section
(`ScrollViewReader` anchors `recording`, `streaming`; `W/Views/StudioWorkspace.xaml:1002-1006,1110-1114`).
B4 — port `W/Services/GpuTierPolicy.cs` (bands, ceilings, the status sentence and the unknown notice verbatim,
`:40,134-151`) with the Mac memory measure `MTLDevice.recommendedMaxWorkingSetSize` (owner may prefer physical
memory; record the choice); in Settings > Canvas under the size/FPS grid, in Windows order: toggle "Allow settings
above this GPU's recommended limits", status line, notice (only when present), canvas profile summary
(`W/Views/ProductionSettingsWindow.xaml:158-166`); pref `gpuTierLimitsOverride`.

- [ ] Steps: write `leftovers/b1-deep-link-anchor`, `leftovers/b4-policy-vectors` (port every
  `WT/GpuTierPolicyTests.cs` case with its constants), `leftovers/b4-unknown-memory-notice`; FAIL; implement; PASS;
  Settings snapshot; commit `mac R7: Settings deep links and GPU tier`.

**Mutations:** 1440p counted inside the 1080p tier (vector red); override not persisted (self-check red).

### Task 21: pop-out header (E6) and scene canvas keys (F5) (M)

**Files:** `M/ShellApp.swift:771-808` (header in pop-out mode), `M/SceneCanvas.swift`,
`M/StudioSceneEditing.swift:233-234` (remove the "not yet" comment), `M/ShellTestsLeftovers.swift`.

**Behaviour:** E6 — pop-out header is exactly "MULTIVIEWER" | "<n> live inputs" | "Pop in"
(`W/Views/MultiviewPopoutWindow.xaml:21-33`); the canvas chip, clock, Scene builder, LT, Room and Grade go from the
pop-out. F5 (`W/Controls/SceneCanvasEditorControl.xaml.cs:587,656-680,724-742`): Esc cancels a drag in progress
(layer returns to its start frame); arrows nudge 1 px, Shift+arrows 10 px; Shift during a resize keeps the aspect;
snapping to canvas 0, ½, 1 and to other layers' edges and centres within 0.015 of the canvas size, gated by a
"Snap" checkbox, default on.

- [ ] Steps: write `leftovers/e6-popout-header`, `leftovers/f5-nudge` (1 and 10 px on a 1920 canvas),
  `leftovers/f5-esc-cancels`, `leftovers/f5-shift-resize-aspect`, `leftovers/f5-snap` (edge at 0.014 snaps, 0.016
  does not; Snap off never snaps); FAIL; implement pure `SceneCanvasKeys`/`SceneSnap` helpers; PASS; commit
  `mac R7: pop-out header, canvas keys and snapping`.

**Mutations:** snap threshold `<=` vs `<` at exactly 0.015 (pin which Windows uses and test the boundary);
Shift on arrows locks aspect (nudge red).

### Task 22: copy and dead code (A2, A4) (S)

**Files:** `M/ShellApp.swift:1711`, `M/AutomationExtras.swift:284-292`.

**Behaviour:** A2 — "Use CoreVideo Tiles to compose your show, then cue scenes and Take manually from Studio, your
keyboard, or Stream Deck. Scene automation is unavailable in this release." (`W/Views/AutomationPage.xaml:7`).
A4 — delete `AutomationExtrasSection` and the Magic Scene run path; keep `AutoAssignInputsToggle` (`:391`) and the
reset path calling `setAutoAssignInputs(true)` (`:160`).

- [ ] Steps: extend `leftovers/studio-forbidden-strings` with "Run Magic Scene" and assert the A2 sentence;
  FAIL; edit; PASS (and `quickwins/*` auto-assign cases stay green); commit `mac R7: automation copy, dead code removed`.

### Task 23: update banner (G5) (M) — dormant until a feed exists

**Files:** new `M/UpdateCheck.swift`, `M/ShellApp.swift` (banner), `M/ShellTestsLeftovers.swift`.

**Behaviour** (`MC/Services/UpdateCheckService.cs:26-67,205`, `W/Services/UpdateNotificationService.cs:25-83`,
`W/MainWindow.xaml:12-29`, D-R1): one check per launch, 5 s after start (injected clock), feed URL from
`COREVIDEO_UPDATE_FEED_URL` (empty = off), 10 s timeout; numeric version compare of 2-4 parts with optional `v`;
quiet when no `pkgUrl`, when not newer, or when that version was dismissed; dismiss writes
`~/Library/Application Support/CoreVideoPro/update-dismissed.json` (Windows shape). Banner bottom-right, max width
520, closable: title "Update available — v<new>", message "You are on v<cur>. Dismiss to be reminded when the next
version ships.", button "Get update" opens `pkgUrl` in the browser (no in-app install).

- [ ] **Step 1:** Confirm the Windows feed parser ignores an unknown `pkgUrl` field (read `UpdateCheckService`
  deserialization); if it does not, stop and raise it with the owner.
- [ ] Steps: port every `MCT/UpdateCheckServiceTests.cs` version-compare vector; write `leftovers/g5-quiet-cases`
  and `leftovers/g5-dismiss-persists`; FAIL; implement; PASS; commit `mac R7: update banner (link only, off without a feed)`.

**Mutations:** string compare (`1.10` vs `1.9` red); show without `pkgUrl` (quiet red).

**Batch R7 review.** Fresh reviewer; Diagnose and Settings snapshots beside the Windows XAML.

---

## Owner live checks (after all batches, on a `--skip-notarize` or notarized build)

1. Companion on a Stream Deck: an SRT configure button, the four multiview overlay buttons, a route button using a
   Mac layer id, Restore starters.
2. With `launchctl setenv COREVIDEO_TELEMETRY_API_KEY <staging key>` and
   `COREVIDEO_TELEMETRY_ENDPOINT` = the workers.dev staging Worker: consent on, quit; one `session-end` arrives in
   KV with machine class `mac-arm64-…`.
3. `kill -SEGV` the core; relaunch; the bar offers one report from `corevideo-native`; Send; the zip in R2 contains
   no `crashReporterKey`; symbolicate it with the release symbols zip.
4. Zoom tab, Studio header and Settings read like the Windows screens side by side.

## Self-review notes

- Spec coverage: RC7 (Tasks 1-6; programBuffer and view.setMode explicitly not planned with reasons), R4 (7-9),
  R5 (10-12, plus 13 for readable reports), R6 (14-17; A3 done), R7 (18-23; E4 done, OAuth closed).
- Review Focus lines are pinned in Tasks 8 (quit), 10 (identity), 7 (consent), 1-5 (refusals), 10 (offer once).
- Type names shared across tasks: `CrashReportRecord` (7, 10, 11), `TelemetryConfig` and
  `MachineClassProbe.describe` (7, 11), `TelemetrySendResult` (8, 11), `loadActionVectors` (1-6),
  `MultiviewOverlays` (3, 20 untouched).
