# RC beta acceptance soak: stamps-on outputs drill plus installed acceptance — design

**Date:** 2026-10-23
**Status:** approved direction (owner, 2026-10-23: "cut a beta so I can test current state"; "as close to zero issues as possible")
**Gate:** per-frame identity of every output over 30 minutes (owner ruling 2026-10-22), amended 2026-10-23: held frames during a source stall pass when counted and labelled (#1038)
**Related:** #443 (`scripts/qa/outputs-drill.ps1`, merged in #1024, never run live), #1030/#1035/#1038 (live acceptance pending), #433/#1033, #1009/#1025, #439/#1032, #434/#436/#1034

## Why

Two betas have shipped without the gate being exercised. `beta-2026-10-23-1072a50`
was published after a test meeting with no remote feeds, and the owner's live soak
that morning ran with a clean picture (no identity stamps), so it produced evidence of
deadlines, drops and the #1030 wedge but no per-frame verdict. Everything merged since
(the wedge fix, the installer dump keys, offline recording, GPU-tier defaults, the
production endpoints) has unit and hosted coverage only. The next beta is the first one
that can be judged, and the judgement has to cover both the gate and the installed
acceptance items each PR left open.

## Two runs, not one

A stamps-on run burns a counter into Program and ISO pixels and is visible on the
virtual camera in Zoom. It cannot be the owner's real show. So:

1. **Drill run (gate):** `outputs-drill.ps1` against the installed beta, in the test
   meeting, with the owner plus helpers as remote feeds, `-Seconds 1800`, one helper
   leaving at about minute 12 and rejoining at about minute 15 (the #1030 acceptance),
   one helper turning the camera off for two minutes (the held-frame path and the #1038
   judge rule). Zero missing identities on every output; held frames only where the
   roster shows a stall; recording still armed and a fresh take possible after the
   departure.
2. **Show run (the owner's "current state" test):** the owner's real meeting on the same
   beta, clean picture, with the 1 Hz sampler and delivery trace from
   `D:\CoreVideo-QA\soak-20261023-01\` reused. Evidence only: deadlines, drops,
   encoder evidence, stream retries, warnings. No identity verdict is claimed.

## Installed acceptance folded into the drill day

Each row is a PR that said "installed acceptance pending". Each gets one checkable
observation on the installed beta, recorded in the drill folder.

| PR | Observation that closes it |
|---|---|
| #1033 (#433) | After the silent install: the three `HKLM\...\LocalDumps\<exe>` keys in the 64-bit view with `DumpFolder` REG_EXPAND_SZ, `DumpType` 2, `DumpCount` 5; the recording folder exists at the app default; a deliberate `corevideo-native` fault lands a `.dmp` in `%LOCALAPPDATA%\CoreVideoPro\crashes`. |
| #1025 (#1009) | Studio Record start and stop with Zoom disconnected; the file finalizes and plays. |
| #1032 (#439) | Settings shows the measured tier line for the RTX 4090 ("16 GB+ ... tiles up to 10 (product limit)"); the saved canvas and tile count are unchanged after launch; `launch.log` has the `gpu-tier-defaults` line with `switch=` and `canvasAbove=` fields. |
| #1034 (#434) | `launch.log` has `crash-report: telemetry endpoint host=telemetry.corevideo.io apiKeyPresent=false`; nothing is uploaded (no key). Sign in with Zoom is NOT exercised until the owner has activated `auth.corevideo.io`. |
| #1035/#1038 (#1030) | Covered by the drill run: departure at minute 12, held frames on the camera-off guest, judge row reads `held=N, source stalled`, recording survives. |
| #1024 (#443) | The drill itself completes end to end on an installed build for the first time; the preferences backup and restore are verified by hash afterwards. |

## Out of scope

Signing, the Zoom SDK redistribution answer, reference machines (#423 to #425).
Streaming to the public RTMP destination (never). Any evidence on C:.

## Acceptance

The drill verdict is PASS with the roster events above present in the evidence, every
row in the table has its observation recorded, and the write-up lands in `docs/qa/`
with the beta tag. Any FAIL becomes a new issue with a BACKLOG row before the next beta
is cut.

## Risks

- `outputs-drill.ps1` has never run live; its first live run will probably stop on a
  harness defect before the 30 minutes. Task 2 of the plan is a 90-second local-sources
  shakeout for that reason.
- The drill needs `corevideo-shared-camera-receiver.exe` from `native\build-dev`, which
  is not in the installer; the receiver must be built from the beta commit or shown to be
  commit-independent.
- The helpers must stay for 30 minutes and follow a timed script; a departure that does
  not happen leaves #1030 unaccepted, and the run is still a valid gate run.
