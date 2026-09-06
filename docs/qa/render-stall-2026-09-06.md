# Command-thread stalls and rendered lower-third identity

## Final implementation

PR [407](https://github.com/iamfatness/CoreVideoPro/pull/407) removes two sources
of work under the command lock: redundant live rendering, and full session
snapshots built and discarded after each command in a batch. Commands still
apply in order; each batch captures one final snapshot. Direct callers without
live workers retain synchronous rendering.

Lower thirds now resolve from the successfully composed Program frame's ordered
video-source bindings, rather than independently resolving automatic routes.
The mapper reads the nested native frame's scene ID; it no longer substitutes
the acknowledged scene ID. A source edit waits for its sync acknowledgment and
a newer frame. The compositor suppresses a source-bound key on the first frame
that no longer contains that source. Failed composition invalidates attribution;
visibility confirmation matches the rendered source, content, and phase.
Explicit missing sources remain missing, with no arbitrary roster substitution.
Binding attribution does not itself prove fresh source pixels.

The native log now includes the stage breakdown of the slowest individual tick
in each 120-tick window. This supplements averages and excludes core-lock wait;
it does not change frame budgets or rendering quality.

## Final regression and live checks

- Native Release core and tests rebuilt at `2825ea0`; **637/637 tests passed**.
- **505/505 MediaCore and 834/834 WinUI tests passed**; shell publish succeeded.
- Native regressions cover one snapshot per ordered batch, display-worker frame
  ownership, stale intent versus rendered scene, immediate key suppression,
  failed composition, same-ID key rebinding, and hidden-to-visible proof.
- Managed regressions cover native wire mapping, exact current source metadata,
  sticky selection only while present, same-scene freshness, and API evidence.
- The first combined live candidate passed **90 operator cycles and 146 visible
  lower-third identity observations**, 23:09:20.739–23:12:40.673 UTC. Frames
  advanced from 3,328 to 15,328. Tests exercised Take/Preview convergence, keys
  carried across cuts, Magic cueing, and manual automation override.
- After the Clang compatibility correction, the final candidate passed automatic
  Take, manual lower-third off, and manual Take holding. A real scene-builder
  check preserved the source assignment on opening the picker; choosing Luis
  changed the draft preview while Program retained Anika. Update scene then
  rendered Luis and his matching lower third. This exercised the real view-model
  freshness path, which cannot be constructed safely in the current unit harness.

Local evidence is in `artifacts/render-stall-final`: candidate hashes, build/test
logs, `operator/native-soak-results.json`, `automation/automatic-take-results.json`,
and `scene-source-ui-results.json`. The UI test assigned Speaker + Slides source
2 to Input 03 (Luis); this is a test-show configuration change.

The final extended run uses `COREVIDEO_TEST_MINUTES=30` with
`scripts/qa/operator-api-stress.cjs`, writing to `artifacts/render-stall-final/soak`.
The harness requires Zoom Live and capture, verifies newer rendered frames, and
rejects any observed visible lower third whose source is absent from rendered
Program. It retains failures and timestamps. See PR 407 for the completed
extended-run result, bounded performance metrics, and final merge disposition.

## CI exception

Clang initially rejected a nested deduced-return function used before its
definition; moving that definition before its callers fixed the Mac builds.
This was a new compile failure and was corrected rather than waived.

The remaining macOS recording-rate assertion already fails on the exact base.
At `2825ea0`, the [Mac drill](https://github.com/iamfatness/CoreVideoPro/actions/runs/34066221905/job/101575327338)
measured 29.1 fps (166 frames / 5.71 seconds), with lock-budget and command-response
checks passing. The [unchanged base drill](https://github.com/iamfatness/CoreVideoPro/actions/runs/34059147233/job/101556417182)
measured 21.0 fps (129 frames / 6.15 seconds), failing the same sole assertion.
This is not a claim that recording performance is fixed.

## Earlier diagnostic evidence

The sections below record the earlier candidate and why the additional changes
above were needed. Their limitations and failed broad run remain part of the
evidence; they are not final-candidate results.

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

## Earlier candidate validation

- Native Release core and test build passed; all 632 native cases passed. New
  regressions cover applied versus rendered scene state, large elapsed values,
  lower-third animation advancing on display ticks, and direct synchronous use.
- All 829 WinUI cases passed after adding rendered-frame control evidence and
  lower-third rejection tests. The final shell publish succeeded.
- The control API now reports `nativeRenderPlanId` directly from the last rendered
  frame. The later review found and removed a mapper substitution of acknowledged
  scene identity. The focused scene test checks rendered-plan identity and a
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

Once an eligible source appeared, that native candidate successfully built
the lower third to on-air. Automatic source attribution required another
fix: an automatic route could show a native positional fallback while the managed
lower-third resolver finds no active speaker, or selects a source that differs
from that fallback. At 17:04:28 the Panel lower third named Susan while the visible
six-person grid did not contain her. The final implementation above addresses
this identity mismatch using rendered bindings. The aborted broad test remains
in `artifacts/render-stall/candidate/native-soak-results.json`.

On the final candidate at 17:08:51, the same unavailable-source request correctly
returned HTTP 422 with `ok: false` and the source error. The response is retained
in `artifacts/render-stall/candidate/lower-third-rejection.json`.
