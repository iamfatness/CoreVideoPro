# Alpha Validation Evidence — 2026-07-02

_Executed by the automated alpha validation pass on the Windows rig (branch
`claude/alpha-validation-evidence`). Machine context: other agents were building
code in separate worktrees during this run, so CPU load fluctuated; baseline at
17:10 local was ~36% CPU with OBS, Discord, Zoom client, and a Claude agent
running. Timestamps are local (UTC-4)._

Evidence artifacts live in `artifacts/alpha-evidence/` (screenshots, ffprobe
dumps, log excerpts).

## Track A — Alpha Evidence And Build Hygiene

- [x] **Run `npm run alpha:preflight` on latest baseline** — PASS (second run, 17:21 local).
  First run at 17:17 reported `overall: failed`; both failures were diagnosed and fixed
  on this branch (see bugs below), rerun is fully green: 11/11 checks passed
  (npm audit, typecheck, MediaCore tests, WinUI VM tests, native stub build+smoke,
  WinUI build, dev native core / Zoom helper / staged runtime artifacts, dev
  readiness, record-stream harness).
- [x] **Attach generated preflight report** — copied to
  `artifacts/alpha-evidence/trackA-alpha-preflight.md` / `.json`
  (source: `artifacts/alpha-preflight/20260702-172118/`). First (failing) run console:
  `artifacts/alpha-evidence/trackA-preflight-console.txt`.
- [x] **Fix any failed offline gates before live validation** — two real bugs found and fixed:
  1. **Test hermeticity bug** — `MediaCorePathsTests.ResolveZoomSdkArchitectureRoot_PrefersRepoZoomSdkBeforeFlatNativeBuildDrop`
     fails on any machine with ambient `ZOOM_SDK_DIR` exported (which CLAUDE.md
     instructs): the env var intentionally outranks the repo `ZoomSDK\` candidates in
     `MediaCorePaths.BuildZoomSdkArchitectureRootCandidates`, and the test never cleared
     it. Reproduced deterministically (pass without env var, fail with it). Fixed by
     clearing `ZOOM_SDK_DIR`/`COREVIDEO_ZOOM_RUNTIME_DIR` inside the two env-sensitive
     tests (`native-shell/CoreVideoPro.MediaCore.Tests/MediaCorePathsTests.cs`).
  2. **Preflight harness false failures** — `scripts/alpha-preflight.ps1` ran child
     steps with `$ErrorActionPreference = "Stop"` + `2>&1`; under Windows PowerShell 5.1
     the FIRST child stderr line becomes a terminating ErrorRecord, so steps that exit 0
     were reported failed/warning (native stub gate failed on VsDevCmd's harmless
     `'vswhere.exe' is not recognized` probe line; record-stream harness warned on the
     core's `[cmd] 'start-program-output' 159ms` stderr log). Verified the stub gate
     passes standalone (exit 0, `artifacts/alpha-evidence/trackA-native-stub-gate.txt`).
     Fixed by scoping `$ErrorActionPreference = "Continue"` around the child invocation —
     the child exit code is the authoritative signal.
- [x] **Keep missing-dependency warnings visible** — no warnings suppressed; rerun shows
  zero warnings because Zoom SDK, staged runtime, FFmpeg (winget Gyan.FFmpeg 8.1.1) and
  the dev native build are all staged on this rig.

## Track B — Live Zoom And Source Proof (synthetic where meaningful)

Two blockers were found and root-caused while bringing the fake-engine meeting up
(~21:25–21:36 local):

1. **BUG (alpha-blocking on rigs with the OBS Zoom plugin): engine IPC pipe-name
   collision.** `corevideo-zoom-engine` and the core hard-code
   `\\.\pipe\ZoomObsPlugin_P2E` / `_E2P` (`native/zoom-engine/shared/engine-ipc.h:46`).
   The user's OBS Studio (32.1.2) had `obs-zoom-plugin.dll` loaded, which owns those
   exact pipe names, so every CoreVideo join failed with "Timed out connecting to Zoom
   engine IPC" — the spawned engine exits 1 from `ipc_setup` because
   `CreateNamedPipe` can't claim the names. Reproduced standalone (engine exits
   code 1 while OBS runs; pipes visible via `\\.\pipe\` listing). Cleared by closing
   OBS for this run. **Fix needed:** per-instance pipe names (e.g. suffix with the
   parent PID, passed to the engine on its command line).
2. **BUG (UX): stale Zoom OAuth refresh token hard-fails join with no fallback.**
   With a stored-but-expired token, `ZoomOAuthService.EnsureJoinCredentialsAsync`
   throws ("Zoom broker token refresh failed") and join aborts; only an EMPTY token
   store falls back to the public-app-key path (`ZoomOAuthService.cs:195-222`).
   An operator with an old sign-in cannot join at all until they explicitly sign
   out. Worked around by backing up + removing
   `%LOCALAPPDATA%\CoreVideoPro\zoom-oauth.json` (restored after the run).
3. **Stale fake-engine artifact note:** `native/build-dev/corevideo-zoom-engine-fake.exe`
   (86528 bytes, 6/29 08:23) predates the d095cde roster-fidelity fix and never emits
   `joined`; rebuilt from current source with cl.exe (88064 bytes) for this run.

- [ ] Join a real meeting (capture, roster, active speaker, mute/unmute, screen
      share, leave/rejoin, churn) — **Needs Human** (see section below); synthetic
      churn/roster evidence collected instead (see soak below).
- [x] Inputs 1-10 persistence across restart — PASS: the shell restored
      "10/10 inputs assigned - 2 in show" from `show-input-roster.json` across
      multiple app restarts during this pass (observed at 21:24, 21:33, and the
      soak launch).
- [x] Non-Zoom input path — PASS (WASAPI loopback local audio source live from
      launch: `local-machine-audio:wasapi-loopback ... streaming=True frames=320640`
      in launch.log audio telemetry; synthetic/test-pattern video path exercised by
      the record-stream harness).

## Phase 2 §6 Gate — ≥10-min audio-routed + recording soak (fake engine)

**Setup (21:37–21:45 local):** fresh app launch (`npm run app -- -StubOnly`,
fixed core from `native/build-dev` staged by the launcher), fake engine
(88064-byte rebuild) at all three paths. UIA-driven: Join `8123456789` →
**Zoom Live, Video in room (4)** (mv1/mv2/Producer/mv4, roster 3↔4 churn +
active-speaker rotation) → **Capture On** ("Capture live — CoreVideo Pro Native
Media Core") → per-participant audio subscriptions
(`participant-audio-101..104-mix`) → scene **Panel** queued on PVW → **Take** →
**Record** (MP4 8.2 Mbps). Recording session `start-recording-session` at
21:45:29, artifact `publish/Recordings/CoreVideo Pro/corevideo-recording-program-0.mp4`.
Program carries a REAL capture device (Elgato Game Capture HD60 S+ 1080p60) and
the audio mix carries real WASAPI loopback + capture audio (master ≈ -16.0 LUFS,
peak -8.3 dBFS — live meters). Screenshots:
`soak-01-capture-on.png`, `soak-02-panel-on-program.png`, `soak-03-take-settled.png`.

**Machine context during soak:** the operator was actively PLAYING A FULLSCREEN
GAME on this rig (plus Discord + two agent build jobs in worktrees), so this is a
worst-case ambient-load soak; CPU samples in
`artifacts/alpha-evidence/soak-cpu-samples.csv`. The app window is occluded the
whole time (screenshots via PrintWindow; UIA driven without stealing focus).

_(results appended after the window closes)_

## Track C — Operator Workflow Polish

- [x] Sources tab as canonical Inputs 1-10 mapping — smoked: compact routing
  table present (IN/SHOW/TYPE/SOURCE/AUDIO per-row editors, 10 rows); KindCombo
  offers Unassigned / Zoom participant / Blackmagic / AJA / UVC webcam /
  SRT ingest / Media asset; setting rows 3-4 to "Zoom participant" exposed the
  live fake roster (mv1/mv2/Producer) in SourceCombo. **BUG (automation/UX):**
  after a Kind change the row's SourceCombo elements intermittently vanish from
  the UIA tree (ItemsRepeater virtualization/rebuild), which also implies visible
  dropdown flicker for operators — the id-set keying fix (CLAUDE.md) covers
  participant Health churn but not Kind-change rebuilds.
- [x] Scenes flow (queue on PVW, Take to PGM) — functionally works: Take swapped
  PGM/PVW labels and scene badges correctly (Panel shows PGM badge; transport
  header updates). **BUG (P1, operator-facing): after Take, the PROGRAM and
  PREVIEW panes' video CONTENT did not swap with their labels.** PROGRAM header
  says "Panel" but the pane still renders the HD60 S+ camera; PREVIEW header says
  "Manual one-up: Game Capture HD60 S+" but renders the Panel participant grid
  (animated fake-Zoom tiles). Persisted across ≥5s and two screenshots
  (`soak-02`, `soak-03`). The recorded program MP4 (below) is the ground truth
  for which surface is really PGM.
- [ ] Routing matrix ISO isolation + gain — partially smoked only (Routing page
  reachable; matrix behavior not exercised end-to-end this run) — **Needs Human**
  for the click-through.
- [x] Empty/loading/error states — exercised implicitly: "Join failed — Timed out
  connecting to Zoom engine IPC" surfaced clearly in Settings and Show-readiness
  strip and persisted until recovery (good visibility, though the stale error
  stayed visible after a later successful join — minor).
- [ ] Full 16:9 canvas at common window sizes — **Needs Human** (window occluded
  by the operator's game; resize testing is also the known crash-adjacent area,
  not safe to automate mid-soak).

## Track D — Record And Stream Proof

_(pending)_

## Track E — Diagnostics And Recovery

_(pending)_

## Track F — Packaging

_(pending)_

## Needs Human

_(pending)_
