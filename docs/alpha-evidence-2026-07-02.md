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

**Result: RAN FULL WINDOW, NO CRASH — but the audio-glitch-freedom gate FAILS
under adversarial GPU load.** Window: 21:45:29 (`start-recording-session`) →
21:56:04 (10m35s). During the window the operator was playing **Overwatch**
fullscreen (GPU saturated) — treat this as a worst-case co-load run, not a
clean-rig gate.

- **Render thread (video): healthy and decoupled.** 133 `[render]` samples over
  the window: fps min/avg/max **30.1 / 39.2 / 44.2**, lockWait min/avg/max
  **0 / 0.65 / 10.7 ms**, render-ms min/avg/max **21.2 / 24.9 / 28.2**. The fps
  ceiling is pure GPU contention with the game (render span ~25ms); the near-zero
  lockWait proves the Phase 2 render/audio lock decoupling behaves as designed.
- **Zero crash events.** `Get-WinEvent` Application Id 1000 from 21:20 onward:
  none for CoreVideoPro/corevideo-native (or anything else). No CoreMessagingXP
  0xc000027b fail-fast despite 10+ min of roster 3↔4 churn + active-speaker
  rotation + occluded-window operation.
- **Audio/output worker collapse (gate FAIL):** `[audioOut]` cadence degraded
  from 47.3 ticks/s @ 3.0ms work (idle, 21:26) → 9.3/104.6ms (21:43, capture+zoom
  audio) → 3.7/271ms (21:45, recording armed) → **0.7/1411.6ms (21:48)** →
  **0.6/1666.3ms (21:51)**. Monitor underruns reached **14,473** (launch.log
  audio telemetry). At 0.6 ticks/s the monitor device is fed every ~1.6s —
  continuous audible glitching is certain; "ears on the monitor" is moot at this
  load. The long work span lives in the worker's DSP/device/encoder/output span
  (`renderAudioOutputTick` work phase), i.e. `encoder->submit` + monitor render
  under total-system CPU/GPU starvation.
- **Bridge TIMEOUT storm (gate FAIL):** from 21:49:28 every request type
  (`ping`, `media-core-sync`, `zoom-media-spine-sync`) timed out at 4000ms
  continuously; `[req]` telemetry shows **queueWait growing to ~174,000ms**
  (~3min of queued commands) with small lockWait (≤1s) and small handle times —
  the single command thread's drain rate collapsed below the ~7 req/s arrival
  rate under CPU starvation. Empty-poll de-serialization (Phase 2 increment 2)
  can't help because ALL requests serialize through the one `inQ` behind
  lock-taking pings.
- **OPERATIONAL P0: the operator could not stop the recording.** The Record
  toggle was clicked at 21:56:05; the `stop-recording-session` command was still
  stuck behind the queue 3+ minutes later. Compounding it,
  `MediaCoreSupervisor.TeardownChild` closes stdin then immediately
  `Kill(entireProcessTree)` — so quitting the app would hard-kill the core with
  no finalize grace, orphaning the (already-starved) recording. Under overload
  there is NO path for the operator to cleanly land a recording.
- **Recording artifact starved:** the active MP4 stayed at 12,161 bytes on disk
  for the whole window (encoder submits ~0.6fps on the collapsed worker; MF sink
  writes buffered). ffprobe results appended below once the stop lands.
- **CPU samples** (`soak-cpu-samples.csv`): total CPU 30–45% across the window —
  total CPU was NOT saturated; the collapse is GPU contention (encode + game) +
  scheduling starvation of the core's worker/command threads, not raw CPU
  exhaustion.

**Verdict for `docs/phase2-threading-plan.md` §6:** the soak executed end-to-end
with zero fail-fast/crash (churn-crash class stays closed), but the gate's
"audio meters live + glitch-free, 0 TIMEOUTs" criteria FAIL under a fullscreen
game co-load. Bottleneck ranking for the fix: (1) `encoder->submit`/monitor span
must be budgeted/shed (drop-to-latest already exists for output; encode needs
it too), (2) command-loop drain needs priority/fairness independent of system
load (Phase 2 increments 3+6 remain open), (3) recording stop must be a
priority-lane control command, and app quit needs a bounded finalize grace
instead of stdin-close-then-kill. A clean-rig re-run is required for the formal
gate PASS — logged under Needs Human (game session was outside the agent's
control).

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

- [x] **`npm run validate:record-stream -- --destinations recording`** — PASS
  (both inside preflight and standalone with `--ffprobe`:
  `artifacts/alpha-evidence/trackD-record-stream-harness*.txt`). Running it
  surfaced and fixed THREE harness/product bugs:
  1. **BUG (product, fixed on this branch): recordings were never finalized on
     stop.** `IEncoderSink` had no stop seam; the MF sink only wrote the moov box
     in its destructor (process exit) or on the next `start()`, so a stopped
     recording stayed unplayable while the app ran. Fixed:
     `IEncoderSink::stopRecording()` + `MediaFoundationEncoderSink::stopRecording()`
     (closeWriters → Finalize) called from `MediaCore::stopRecordingSession`
     under `audioOutputMutex_`; stub sink mirrors the semantics; 2 new native
     tests (7/7 EncoderRecordingSession pass).
  2. **BUG (harness, fixed): `child.kill()` without stop** left a 79-byte stub
     on disk while the report claimed 16.5MB "written" (`bytesWritten` counts
     submitted raw bytes, not disk bytes). The harness now stops the session,
     waits for the artifact to settle, and resolves the relative artifact path
     against the native-core cwd (its `--ffprobe` previously always reported
     `file-missing`).
  3. **Harness ffprobe audio criterion made honest:** it now requires an AAC
     stream exactly when audio packets were muxed (headless runs have no audio
     source; that gap stays surfaced via the existing recommendation).
- [x] **Record a short manual MP4 (in-app)** — **FAILED as shipped; root cause
  found and FIXED on this branch; re-proof below.**
  **BUG (P0, alpha exit-bar blocker): every in-app recording with real program
  audio died ~1 second in.** `Mp4Writer` adds the AAC stream lazily on the first
  `submitAudio`, but `openRecordingWriters` calls `BeginWriting()` up front —
  and the MF sink writer rejects `AddStream` after `BeginWriting` with
  **`MF_E_INVALIDREQUEST` (0xC00D36B2)** → `setRecordingFailure` finalizes after
  ~1 video frame. Evidence: the 10-min soak "recording" is a valid 1-frame
  0.017s MP4 (`corevideo-recording-program-1783043129614.mp4`, 12,161 bytes) and
  the diagnostics window showed "Media Foundation could not add program AAC
  stream: add AAC stream: 0xC00D36B2." The offline harness never caught it
  because headless runs mux no audio. **Fix:** configure the 48kHz-stereo AAC
  stream before `BeginWriting`; `submitAudio` drops format-mismatched buffers
  with a warning instead of killing the session
  (`native/src/modules/MediaFoundationEncoderAdapter.cpp`).
- [ ] 30-minute recording soak — NOT RUN (blocked first by the AAC bug, then by
  rig conditions; run after the short proof is green on a quiet rig).
- [ ] RTMP push with real program audio — **Needs Human** (no live RTMP ingest
  endpoint/credentials available to the agent; FFmpeg runtime IS staged —
  diagnostics shows `FFmpeg runtime found: C:\ffmpeg\bin\ffmpeg.exe`).
- [x] NDI treated as optional — not promoted into the Alpha promise.

## Track E — Diagnostics And Recovery

- [x] **Support bundle after a normal run** — PASS.
  Diagnostics window → "Export support bundle" →
  `%LOCALAPPDATA%\CoreVideoPro\support-bundles\support-20260703020446.json`
  (copy: `artifacts/alpha-evidence/trackE-support-bundle-normal-run.json`).
  Contains app/summary/triage/outputs/mediaCore/runtime/warnings sections.
- [x] **Simulated native-core crash → crash event, recovery, warnings** — PASS.
  Killed `corevideo-native` (pid 37068's predecessor) at 22:05:37; the
  supervisor restarted it within the same second; UI showed
  **"Media core recovering (restart 1)"**; audio chain re-established (capture
  frame counters resumed). Post-failure bundle exported
  (`trackE-support-bundle-after-crash.json`) records `restartCount: 1`,
  `recovering: false`, full warning set, and triage lines ("Media core
  restarts: 1"). Note: `Stop-Process -Force` kills do not write Application
  Event 1000 (that class is reserved for real faults; none occurred all night —
  see soak). **Wrinkle worth a decision:** after the restart the shell re-armed
  the (operator-stopped-but-stuck) recording automatically — resilience for a
  crash mid-show, but it resurrected a session the operator had tried to stop.
- [x] **Failures remain visible until recovered** — PASS: "Recording stop
  failed: Media core failed while starting recording… request core-3783 timed
  out" stayed pinned in the diagnostics Output/encoder panel until the core
  restart cleared it.
- [x] **Secrets redaction** — PASS for the exercised config: both `streamKey`
  fields in the bundle render as `"absent"`; no tokens/passphrases present.
  (No real stream key was configured on this rig, so redaction-of-a-value is
  unproven — noted under Needs Human.)

## Track E — Diagnostics And Recovery

_(pending)_

## Track F — Packaging

_(pending)_

## Needs Human

_(pending)_
