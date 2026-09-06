# Command-thread render stall follow-up

## Starting point

Reliability PR [406](https://github.com/iamfatness/CoreVideoPro/pull/406) merged as
`4ab599bd6832bb09ea76177e3868af7e8454a8a4`. Its final head passed 18 GitHub checks.
The sole failed check was the macOS recording-rate drill, which fails the same
assertion on the unchanged base. Final-head timing and command-response gates
passed. The PR records both CI job links and the exception.

## Diagnosed work

In the server, the display worker already renders on its own cadence.
`MediaCore::applyCommands` nevertheless renders up to two additional synthetic
frames for each nonempty sync while the command thread holds `coreMutex`.
These renders include work that the video-only display path avoids. The offline
September 6 baseline captured a batch with 17 ms of commands followed by 243 ms
of redundant rendering; the next display tick waited 264 ms for the lock.

The bounded fix assigns frame production to the existing workers in live server
mode. Command state still applies in order before its response, while rendered
frame metadata remains the last published frame until the next display tick.
Direct callers without workers retain synchronous synthetic rendering.

## Baseline limits

`artifacts/render-stall/baseline/native-soak-results.json` records 90 successful
native-confirmed operator cycles, 16:37:43–16:43:06 Eastern, with frame counter
707,895 to 720,043. Zoom was offline, so these checks establish command state and
frame-counter advancement, not fresh meeting video. A brief managed publish
overlapped part of the run. It is diagnostic evidence, not a controlled benchmark.

## Validation

- Native Release core and test build passed; all 632 native cases passed. New
  regressions cover applied versus rendered scene state, large elapsed values,
  lower-third animation advancing on display ticks, and direct synchronous use.
- All 829 WinUI cases passed after adding rendered-frame control evidence and
  lower-third rejection tests. The final shell publish succeeded.
- The control API now reports `nativeRenderPlanId` directly from the last rendered
  frame. `nativeRenderedSceneId` is nullable and is not inferred when the native
  protocol omits it. The focused scene test checks rendered-plan identity and a
  newer frame, rather than treating an acknowledged scene as fresh output.

- Final live session `820dcfd06afb4572839fc9f9aec63dc3`, shell PID 5600/native
  PID 37184, passed all 60 focused scene changes from 17:06:17 to 17:07:47 Eastern.
  Eight test guests were joined and ingest was live. The test required capture,
  Zoom Live, the requested rendered plan, an advancing frame count, and native
  Preview convergence. No builds ran during this interval.
- Bounded logs: 45 render windows, median 60 fps, minimum 56.2 fps, worst frame
  129.3 ms, and 70 reported drops. There were 29 command-lock warnings, median
  32 ms and maximum 65 ms. No thresholded `applyCommands` samples or logged
  errors/timeouts appeared. Detailed metrics, copied logs and candidate hashes
  are under `artifacts/render-stall/candidate`.

This live Take/Preview workload differs from the offline broad baseline. No
controlled speedup is claimed. The redundant command-thread renders are removed,
but the remaining worst frame shows that not all stalls are resolved. The
existing macOS recording-rate failure also remains outside this change.

## Additional lower-third finding

The initial broad operator test stopped at its lower-third assertion. The live
UI rejected the request with `Lower third needs a program source`, but the API
returned success. Native overlay state reads the current asset directly, so this
was not an old rendered frame rolling back the key. No build-in command started.
The API now returns failure when the view model rejects the requested intent.

Once an eligible source appeared, the same native candidate successfully built
the lower third to on-air. Automatic source attribution still needs a separate
fix: an automatic route can show a native positional fallback while the managed
lower-third resolver finds no active speaker, or selects a source that differs
from that fallback. At 17:04:28 the Panel lower third named Susan while the visible
six-person grid did not contain her. This branch does not change routing policy
or claim that this identity mismatch is resolved. The aborted broad test remains
in `artifacts/render-stall/candidate/native-soak-results.json`.

On the final candidate at 17:08:51, the same unavailable-source request correctly
returned HTTP 422 with `ok: false` and the source error. The response is retained
in `artifacts/render-stall/candidate/lower-third-rejection.json`.
