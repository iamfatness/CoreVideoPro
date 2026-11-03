# RC beta acceptance, stamps-on outputs drill — 2026-10-30, `beta-2026-10-26-f969935`

Plan: [`docs/superpowers/plans/2026-10-23-rc-beta-acceptance-soak.md`](../superpowers/plans/2026-10-23-rc-beta-acceptance-soak.md), Task 3 (the stamps-on drill, [#443](https://github.com/iamfatness/CoreVideoPro/issues/443)). Raw evidence is local in `D:\CoreVideo-QA\drills\20261030-210909-stamps-mediaclip` (verdict.json, summary.txt, measurement.json, samples.ndjson, judges/, recordings/, stream-received.ts, os-camera/). This is the first per-frame identity verdict on a shipped beta; the 2026-10-25 soak ([`rc-beta-acceptance-2026-10-25.md`](rc-beta-acceptance-2026-10-25.md)) judged continuity only.

## What was run

| Item | Value |
|---|---|
| Build | `beta-2026-10-26-f969935` (main `f9699351`), installed; `corevideo-native.exe` sha256 `6F6C4AC9…49C5` recorded in `launch.json` |
| Drill | `scripts/qa/outputs-drill.ps1 -NoWebcam -MediaClip D:\CoreVideo-QA\media\drill-motion-2400s.mp4 -Seconds 1800` from the `codex/drill-nowebcam-scene` worktree at `c5a9fbc5` (PR #1066), launched detached, app closed beforehand, no game on the GPU |
| Session | the drill's own launch of the installed app with the QA stamps (`COREVIDEO_QA_PROGRAM_COUNTER`, delivery trace), preferences patched and restored byte for byte, show-input roster restored byte for byte |
| Program source | one synthetic 1080p60 H.264 motion clip staged into the media bin as `media:media-63a585ec7312`, routed through the single-route `intro` scene (owner ruling 2026-10-30; no screen capture, no webcam: the camera rig is off the bench) |
| Outputs | Program recording (Media Foundation hardware H.264, 1080p60), SRT caller stream to a loopback listener, registered OS virtual camera read by `corevideo-shared-camera-receiver` at 1080p60; no ISO (a media clip carries no QA ISO identity) |
| Window | measured interval 21:10:38 to 21:40:39 EDT (1,800 s); readiness in 43 s; outputs stopped and finalized at 21:41:40; verdict at 21:47:20 |

## Results

| Output | Expected | Decoded | Missing | Repeated | Reordered | Invalid | Verdict |
|---|---|---|---|---|---|---|---|
| Program recording | 108,036 | 108,036 | 0 | 0 | 0 | 0 | **PASS** |
| OS virtual camera | 108,038 | 108,038 | 0 | 0 | 0 | 0 | **PASS** |
| Stream (SRT loopback) | 108,036 | 108,036 | 0 | 0 | 0 | 0 | **PASS** |
| Webcam source | | | | | | | NOT EXERCISED |
| Program GPU deadlines (supplementary) | | | | | | | PASS |

**Overall: PASS.** Every workload assertion held for the whole interval: Program on the hardware encoder, the clip's frame counter advancing within 2 s at every sample, Program source admissions ready, stream healthy (`measurement.json`, `firstFailures` empty). A 60 s smoke run of the same configuration at 21:04 also passed (3,653 / 3,655 / 3,653 frames, zero defects).

## Lock-hold attribution (#1062, the #1056 question)

The build carries the `[lock-hold-attribution]` line for every over-budget `coreMutex` hold. In the 40 minutes from launch to verdict the core logged 11 over-budget holds out of 7,788 (worst 2,578 µs), none suppressed:

| Site | Count | Worst | Attribution |
|---|---|---|---|
| `cmd.handle` (command lane) | 5 | 1,727 µs, budget 1,000 µs | all `media-core-sync`; `pre-read-engine-wait` not involved |
| `render.display-tick` | 3 | 11,564 µs, budget 8,000 µs | `program` phase 8,469 / 7,903 / 11,423 µs; `frame_group_engine_wait_us=0`, `lock-wait=0`, `pre-read-engine-wait=0` on all three |

The three render-tick holds were at 21:09:16 (warmup, before the interval), 21:17:26 and 21:26:27 (inside the interval). Each was the Program composite itself, not a wait on the Zoom engine mutex, and none cost an output frame: the three outputs decoded every expected identity. Fifteen `[program-buffer-miss]` lines exist in the window, all at 21:41:40, after the interval, while the outputs were being stopped. So on this build, with one media source and no Zoom feeds, the over-budget render-tick holds are GPU composite time and are absorbed; the 15 ms hold that cost a frame on 2026-10-25 had 10 Zoom participants and 8 ISOs, which this run does not reproduce. #1056's acceptance (a 10-participant soak on a post-#1062 build with zero in-interval misses) is still owed.

Idle-log observation: the same 40 minutes wrote 1,935 `[monitor-worker]` heartbeat lines on this build; #1064 (merged 2026-10-30 as PR #1068, not in this beta) changes that cadence to 60 s.

## What this run proves and does not

- Proves: per-frame identity of Program recording, stream and the OS virtual camera over 30 minutes at 1080p60 on the installed beta, with the drill's preferences patch and restore, close guard, and media-bin staging all working end to end.
- Does not prove: anything about UVC capture (webcam not on the bench), Zoom participant video, ISO recording (none in a local-only plan), audio, or a participant departure (#1030). Those need the roster script with remote feeds (`-RemoteIds`), which is the next drill.

## Open items

- [#1056](https://github.com/iamfatness/CoreVideoPro/issues/1056): acceptance soak with 10 participants still owed; this run adds the attribution evidence that render-tick overruns here are composite time, not engine waits.
- [#443](https://github.com/iamfatness/CoreVideoPro/issues/443): the remote-feed drill (ISOs, Zoom sources) and the webcam row remain to be exercised on a shipped beta.
- PR #1066 (the drill fixes this run depended on: intro scene, log writer, serving-proof serialization, media-clip source, roster restore) awaits merge.
