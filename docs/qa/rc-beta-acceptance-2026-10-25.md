# RC beta acceptance soak — 2026-10-25, `beta-2026-10-25-1f697ca`

Plan: [`docs/superpowers/plans/2026-10-23-rc-beta-acceptance-soak.md`](../superpowers/plans/2026-10-23-rc-beta-acceptance-soak.md), Task 5 (clean-picture show run). Raw evidence is local in `D:\CoreVideo-QA\soak-20261025-02\` (`summary.txt`, `samples.ndjson`, end-of-window and finalized snapshots, final logs, the received stream); it is not in git.

## What was run

| Item | Value |
|---|---|
| Build | `beta-2026-10-25-1f697ca` (main `1f697caf`), installed at `%LOCALAPPDATA%\Programs\CoreVideoPro\beta-2026-10-25-1f697ca`; hosted qualification run 40248720508 passed |
| Session | the owner's own session (Start-menu shortcut, no QA environment variables); the soak attached over the control API on `127.0.0.1:8011` and never relaunched the app |
| Meeting | the owner's test meeting, 10 participants, constant for the whole window (no departure, no camera toggle) |
| Workload | Program + 8 Zoom ISOs recording (Media Foundation hardware H.264, fragmented MP4, 1080p60), SRT caller stream (HEVC NVENC, 1080p60, 10 Mbps) to a loopback listener on `127.0.0.1:50413`, OS virtual camera at 1080p60; RTMP off |
| Window | 21:21:30 to 21:51:30 EDT; outputs stopped at 21:53:05 |
| Stamps | none (clean picture): **no per-frame identity verdict**; the #443 pixel-identity gate was not exercised |

Preferences were not edited. The ISO set (8 of the 10 roster members) and the recording folder came from the stored preferences, because the control API has no ISO-selection action and a preference change needs a relaunch.

## Results

| Output | Observation | Outputs-only rule |
|---|---|---|
| Program recording | 1,894.187 s, 113,650 video packets at 60 fps: **one frame short** of the 113,651 scheduled. Missed at 21:36:04 inside a 15,127 µs `coreMutex` hold at `render.display-tick` (budget 8,000 µs). Writer drained clean: received = encoded = processed = 113,650, 0 encoder-queue drops, 0 failures. | **FAIL** on one frame, filed as [#1056](https://github.com/iamfatness/CoreVideoPro/issues/1056) |
| 8 ISO recordings | 1,894.2 s each; every writer accepted = received, `dropped_video` 0, `dropped_audio` 0. Held frames (source stalls, #1038): ISO-03 1, ISO-04 1, ISO-06 2, ISO-07 2, ISO-08 1, others 0; writer `heldFrames` equals the drain `held_frames` for every file. | continuity clean; identity not judged |
| SRT stream | sender live for the whole window, 0 retries, 0 sender warnings, 2.47 GB sent. Received file: HEVC 1920x1080, 113,526 video packets over 1,892.100 s = 60.00 fps. | continuity clean |
| OS virtual camera | live at 1080p60, `framesAccepted` 113,365, `framesPublished` 113,348, `pendingFramesReplaced` 17 (all within the first 20 s, before any consumer), `publicationExceptions` 0. No receiver was attached, so this row is attested by the publisher counters only. | not judged |
| Stop / Start after the window | stop: every writer drained, joins at 37 ms (bound 4,000 ms), session `completed` and finalized within about 1 s, `finalizeTimeouts` 0; start: the new take was `producing` 2 s after the request (bound 15 s) and finalized `completed` on its stop. | pass (the #1030 Stop/Start check, without a departure) |

Sampler: 1,865 one-second `/snapshot` samples from 21:20:34 to 21:53:32, 0 unreachable, snapshot age median 122 ms and maximum 324 ms. Core working set 982 MB at T+10 and 1,001 MB at T+20; commit level `ok` throughout.

Head-of-show note: the async encoder sink reports `startupDroppedVideo` 38 (31 of them attributed per ISO) from shedding behind the writer's synchronous open during the arming second; steady-state `droppedVideo` stayed 0 for the whole session. This is the clipped head the sink reports separately on purpose, and it lies outside any declared interval.

## The one failure, in detail

Over-budget `render.display-tick` holds happened about once a minute for the whole run (25 logged, 63 of roughly 193,000 holds counting suppressed ones, most between 8 and 12 ms). Only the two longest cost a frame: 16,584 µs at 21:21:30.698, the arming tick before the session's first frame, and 15,127 µs at 21:36:04.513, the one inside the window. This is the #1037 family; its open follow-up [#1049](https://github.com/iamfatness/CoreVideoPro/issues/1049) keeps three engine-mutex calls inside the render tick on purpose, and [#1056](https://github.com/iamfatness/CoreVideoPro/issues/1056) carries this run's evidence that the remaining holds are long enough to lose a Program frame about once per half hour at 10 participants. No stack for the long tick was captured.

## Not exercised

- Per-frame identity of any output (no stamps). The stamps-on drill (plan Task 3) still has not run on a shipped beta; it relaunches the app, so it needs a window when the owner's session is not live.
- A participant departure during the window (#1030 live acceptance).
- A receiver on the virtual camera.
- Graceful close (the owner's app stayed open) and the installed-acceptance rows (plan Task 4).

## Open items

- [#1056](https://github.com/iamfatness/CoreVideoPro/issues/1056) Program missed one frame on a 15 ms render-tick lock hold (new).
- The stamps-on 30-minute drill with the roster script (plan Task 3) and the installed-acceptance observations (plan Task 4) remain owner-scheduled.
