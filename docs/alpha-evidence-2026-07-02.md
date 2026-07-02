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

## Track C — Operator Workflow Polish

_(pending)_

## Track D — Record And Stream Proof

_(pending)_

## Track E — Diagnostics And Recovery

_(pending)_

## Track F — Packaging

_(pending)_

## Needs Human

_(pending)_
