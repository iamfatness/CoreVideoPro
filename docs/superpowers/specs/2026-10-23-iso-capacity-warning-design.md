# ISO software-encoder capacity notice: report it, never cancel on it (#935) — design

**Date:** 2026-10-23
**Status:** approved (owner rulings D8 and D9, 2026-10-23 23:50 EDT, see "Decisions")
**Issue:** [#935](https://github.com/iamfatness/CoreVideoPro/issues/935) (OPEN)
**Related:** #930 / PR #932 (full-workload validation where it was found), #1013 (probe cap raised to 9, CLOSED, `8f2e9928`), #1030 (writer-loss note on the same warning channel), #443 (outputs drill), PR #1025 (`RecordingControlPolicy`, #1009)

## Why

### What happened

During #930 full-workload validation (2026-10-17, ~10:51 and ~10:53 America/New_York), a
recording start with Program plus eight Zoom ISOs was cancelled before any writer had
received a sample. The native core had admitted the workload and said so on
`recording.warning`: "1 ISO source will record on the CPU software encoder, not the GPU
(7 on hardware): ...". The shell read that notice as a failed start and rolled the
recording back. The empty MP4s then failed finalization with `0xC00D4A44`.

Tonight (2026-10-23) the same notice still fails the headless gate. On the dev box
(RTX 4090; the probe log reads `sessions<=9 (creation-proof only)`),
`node scripts/validate-iso-record.mjs --sources 8 --seconds 20` fails only on that
note, and passes with `--allow-capacity-warning`. The live show the same day recorded
Program and 6 ISOs, all on hardware, so it never reached the spill.

### The code path that turned the notice into a cancel

1. **The notice is created and is not fatal.**
   `native/src/modules/MediaFoundationEncoderAdapter.cpp:2226-2257` looks up the probe
   (`EncoderCapacityCache::lookup`), plans placement (`planIsoEncoders`, Program reserves one
   hardware session), and runs `evaluateIsoAdmission`
   (`native/src/modules/IsoEncoderAdmission.h:137-245`). A spill within the software budget
   returns `AdmitWithWarning` with code `iso-encoder-software-spill` (or `-unprobed`). ISO
   still arms (`admission.armIso()` is true). The message goes through `raiseIsoWarning`
   (`:2371-2376`) into the single string `session_.recordingWarning`.
2. **The notice becomes a "warning" status.** `native/src/core/CoreStateCapture.cpp:1829-1833`
   sets the encoder state `status` to `"warning"` whenever `recordingWarning` is non-empty.
   `:2071-2072` publishes it as `recording.warning`. `AsyncEncoderSink.cpp:569` and `:817` set
   `degradedWarning` from any non-empty warning, so lifecycle health reads `degraded`
   (`:832`, `OutputLifecyclePolicy.h:90`).
3. **The shell cancelled on "warning".** Before commit `935b2800` (2026-10-17),
   `TransportStatusFormatter.TryFormatRecordingStartHealthFailure`
   (`native-shell/CoreVideoPro.WinUI/ViewModels/Transport/TransportStatusFormatter.cs:116`)
   matched recording `OutputHealth` with `Status is "failed" or "warning"` (now `:125`) and
   `Recording is { Active: true, Status: "failed" or "warning" }` (now `:133`).
   `TransportCoordinator.SetRecordingAsync` calls it right after the start sync
   (`TransportCoordinator.cs:391`), restores `Recording = previousRecording`, and fires
   `SyncFailedRecordingStartRollbackAsync` (`:400`, body `:472-503`). That resync carries
   Recording=false to the core, which stops writers that have no samples. The retry path
   does the same (`:514`, `:523`).

**In one sentence:** the core published its nonfatal software-spill admission note as
`recording.warning` with status `"warning"`, and `TryFormatRecordingStartHealthFailure`
treated every `"warning"` as a failed start, so `TransportCoordinator.cs:391-400` rolled
the take back through `SyncFailedRecordingStartRollbackAsync`.

### What is already fixed, and what is not

Commit `935b2800` ("Keep recording capacity warnings from cancelling startup (#930, #935)")
is on `main` (an ancestor of `1879ccb7`). It narrows the predicate to `"failed"` only and
adds `TryFormatRecordingStartHealthFailure_DoesNotCancelForSoftwareIsoCapacityNotice`
(`StudioViewModelAudioStatusTests.cs:1164`). The issue comment records a hotfix shell
(SHA256 `ABC993F1...FFD64`) that recorded Program plus eight ISOs for about 41.85 s each,
with 2,511 consecutive Program identities and zero invalid, repeated, missing or reordered.
That was a startup smoke check, not a qualification.

These parts are still open, and #935 stays open until they ship:

| Gap | Where | Effect |
|---|---|---|
| The headless gate fails on the notice | `scripts/validate-iso-record.mjs:262-263` pushes `recording.warning surfaced` for any warning unless `--allow-capacity-warning` is passed, and then only when the warning *starts with* the admission sentence | A correct 8-ISO run fails by default. With the flag, a queue-setting warning prefixed at `CoreStateCapture.cpp:2067-2069` would still fail it. The check runs every 100 ms poll, so one note becomes about 200 duplicate failures |
| The release drill fails on placement, not loss | `scripts/qa/outputs-drill-lib.ps1:371-372` `hardwareEncodersThroughout` requires every ISO `encoderPath == 'hardware'`. `outputs-drill-judge.py:40,152-162` puts that flag in `THROUGHOUT_FLAGS`, and any false flag fails the run | A run in which every ISO frame is delivered exactly once still FAILs because one ISO used the software MFT |
| The notice is one overwritable string | `raiseIsoWarning` (`MediaFoundationEncoderAdapter.cpp:2371-2376`) replaces any earlier warning except a program-audio drop. `AsyncEncoderSink.cpp:814-815` prefixes the #1030 loss sentence | A later ISO note (for example a manifest write failure, `:2453`) erases the placement fact. A reader that parses text cannot tell placement from loss |
| The operator cannot see which ISO, or what it costs | Per-stream `encoderPath` / `fallbackReason` are serialized (`CoreStateCapture.cpp:1949-1950`) and mapped (`NativeMediaCoreProtocol.cs:400-401`), but no view reads them. `IsoHealthWarning` (`StudioViewModel.cs:234`) shows only per-stream `Warning`, and the software ISO's is empty. `Recording.Warning` is read only by the start predicate (`TransportStatusFormatter.cs:134`) | Since `935b2800` the note no longer cancels the take, and it is also invisible. The operator sees a Warning-coloured recording pill and nothing else |
| No coordinator-level pin | `TransportCoordinatorTests.cs:327` pins rollback on `"failed"`. No test drives `ToggleRecordingAsync` with a warning snapshot | A future predicate edit could bring the cancel back with only the formatter test failing |

### Owner rules this design obeys

- Honest reporting: every value shown is measured, or shown as absent. None is estimated.
- A warning that does not prevent the recording must never stop it.
- The operator must see which ISO is on the software encoder, and what that costs.
- The per-frame identity gate (`iso-pixel-verdict.py`, `recording-pixel-verdict.py`) is
  unchanged.
- The hardware-session probe semantics are unchanged without new measurement. Ten NVENC
  sessions were measured feasible on 2026-10-22 (`artifacts/qualification-1008-25`, 30-min
  soak, Program + 8 ISO + HEVC SRT stream). The probe still caps discovery at 9 (#1013).

## Desired behavior

### 1. Start proceeds on a notice; only a real inability fails

| Condition | Severity | Start | Source of truth |
|---|---|---|---|
| ISO admitted with a software spill within budget (`iso-encoder-software-spill`, `-unprobed`) | notice | proceeds | `recording.isoAdmission` (new), per-stream `encoderPath` |
| Program encoder is the software MFT (`recording.encoder.hardwareAccelerated == false`) | notice at start; **FAIL for RC qualification** (D8) | proceeds | `recording.encoder.hardwareAccelerated` |
| Disk pre-flight low-space warning | notice | proceeds (unchanged) | `RecordingDiskWarning` |
| ISO refused before the show (`iso-encoder-unplaceable`, `iso-encoder-oversubscribed`, unwritable folder) | ISO disabled, Program records | proceeds for Program (unchanged) | `recording.isoAdmission.decision == "refuse-iso"` |
| **No encoder at all for Program**: the stub/counting encoder (`MediaCoreOnAirPolicy.RecordBlockReason`, `TransportCoordinator.cs:338`), or `program_.open` / `ensureAudioStream` / `beginWriting` failing (`setRecordingFailure`, `MediaFoundationEncoderAdapter.cpp:2266-2318`) | **fatal** | refused / rolled back (unchanged) | `recording.lifecycle.state == "failed"`, `OutputHealth[recording].status == "failed"` |
| Writer lost media after it started (#1030 loss, abandoned writer, preparation timeout) | **fatal for qualification**, visible live | not cancelled by the shell. Loss is judged by the drill | `recording.proof.*Dropped*`, `streams[].droppedFrames`, `lifecycle.error` |

The shell's start predicate stays as `935b2800` left it: reject `lifecycle.state ==
"failed"`, recording `OutputHealth.status == "failed"`, or `Recording.Status == "failed"`.
This design adds tests that keep it that way (Plan Task 4). No new cancel path is added.

### 2. The notice is structured, not parsed

The core adds a `recording.isoAdmission` node, built once at arm from the
`IsoAdmissionVerdict` and the `IsoCapacitySource` already computed at
`MediaFoundationEncoderAdapter.cpp:2239-2247`:

```json
"isoAdmission": {
  "decision": "admit-with-warning",
  "code": "iso-encoder-software-spill",
  "message": "1 ISO source will record on the CPU software encoder, not the GPU (7 on hardware): ...",
  "hardwareCount": 7, "softwareCount": 1, "unplacedCount": 0,
  "admissibleIsoCount": 10,
  "probed": true, "probeStatus": "ready",
  "capacity": "hw=yes sessions<=8 (creation-proof only) ... sw=yes",
  "workload": "h264 1920x1080@60"
}
```

`decision` is one of `admit`, `admit-with-warning`, `refuse-iso`. The node is absent when
no ISO was selected. Every field is copied from the verdict or the probe; none is derived
in the shell.

Each ISO stream gains `isoIndex`, the 1-based number already used in its file name
(`ISO-NN-<name>.mp4`, `:2295`), so the operator label and the file match without parsing
paths.

`recording.warning` keeps the admission sentence, so text readers keep working. Machine
readers (validator, drill, shell) use `isoAdmission` and `encoderPath` instead. A later
`raiseIsoWarning` can still overwrite the text. The structured node is never overwritten.

### 3. The operator sees which ISO, and what it costs

A pure shell readout (`IsoEncoderPlacementReadout`) builds one line per software ISO from
`recording.streams[]` and `recording.isoAdmission`:

> ISO-8 Guest 8 is on the CPU software encoder (GPU sessions used up) · 9.4 ms per frame of a 16.7 ms budget · 0 frames dropped

- "which": `isoIndex` + `displayName` (fallback `sourceId`), only for streams whose
  `encoderPath == "software"`. The reason comes from `fallbackReason`
  (`hardware-capacity-exhausted` -> "GPU sessions used up", `hardware-unavailable` ->
  "no GPU encoder", `operator-selected` -> "set to software").
- "cost": the mean encode work per frame for that writer, `videoWorkUs / muxVideoFrameCount`
  (both cumulative and already in the snapshot, `CoreStateCapture.cpp:1939,1955`; see
  `docs/reference/iso-recording.md` "Telling the two ISO loss sites apart"), against the
  frame budget `1000 / recording fps`. Also that writer's `droppedFrames`. If
  `muxVideoFrameCount == 0` or `videoWorkUs` is absent, the readout says "cost not measured
  yet". It never shows an estimate.
- Before the take (armed, not recording): the placement is not known until arm, so nothing
  is shown. The core decides placement at arm. The shell does not predict it.
- Severity: Informational while the writer's mean work is under its budget and it has not
  dropped. Warning once the mean exceeds the budget or `droppedFrames` is above 0. The
  InfoBar sits beside the existing `IsoHealthWarning` one (`StudioWorkspace.xaml:1161-1163`)
  and never replaces it.
- If the Program encoder is software, the readout says "Program is on the CPU software
  encoder" with the same cost fields from the Program stream.
- The support bundle already carries the snapshot. The manifest keeps `encoderCapacity` and
  `isoAdmission` (`:2469-2471`).

Not in scope: process-level CPU percentage in the UI. The shell has no measured per-encoder
CPU figure. Showing a whole-process number as "what the software ISO costs" would
misattribute it. The drill measures process CPU (section 4).

### 4. Drill rule change: pass with the note, fail only on actual loss

**`scripts/validate-iso-record.mjs`**

- A `recording.warning` equal to `recording.isoAdmission.message` (when
  `decision == "admit-with-warning"`) is a **note**, not a failure. Any other warning text,
  including the admission sentence with anything prepended or appended, is still a failure.
  Each distinct warning is reported once, not once per poll.
- An `isoAdmission.decision` of `refuse-iso` while ISOs were requested is a failure, because
  the validator asked for N ISO streams.
- Every ISO, hardware or software, must still pass every existing check: framesWritten > 0,
  dedup bound, audio stem, ffprobe decoded count == native mux count, 1080p geometry,
  head-clap under 50 ms. The steady-state loss checks (`programMissingFrames`,
  `encoderQueueDroppedVideoFrames`, `encoderQueueDroppedAudioPackets`) are also unchanged.
- New per-ISO check: `streams[].droppedFrames` and `writeQueue.dropped` must not increase
  between the steady-state baseline and the stop snapshot. This is the "actual drop"
  measure for a software writer that cannot keep up.
- The summary prints a `Note :` line per software ISO, in the same words as the shell
  readout, with the measured ms/frame. The evidence JSON gains `notes[]`, `isoAdmission`
  and per-ISO `{isoIndex, encoderPath, fallbackReason, meanVideoWorkUs, droppedFrames}`.
- `--allow-capacity-warning` is still accepted, and prints that it is now the default. It
  waives nothing more than before.

**`scripts/qa/outputs-drill-lib.ps1` + `outputs-drill-judge.py`**

- `hardwareEncodersThroughout` leaves the gating `THROUGHOUT_FLAGS`. It is still recorded
  in `measurement.json` as a fact, and a new `encoderPlacement` record lists, per ISO, the
  `encoderPath` seen on every sample and whether it ever changed. Placement is fixed at arm,
  so a change would itself be reported.
- The judge adds a note per software ISO ("ISO-n <source> on software encoder: mean X ms of
  Y ms per frame, peak core CPU Z %"). It adds `encoder=software` to that ISO's summary row.
  The row's PASS/FAIL still comes only from `iso-pixel-verdict.py` (zero missing, repeated or
  invalid identities).
- `THROUGHOUT_FLAGS[:4]` at `outputs-drill-judge.py:265` (missing-flag detection) is
  rewritten to name its flags explicitly, so removing one flag does not shift the list.
- New measurement: once per second the collector samples the core process's
  `TotalProcessorTime` and computes CPU % normalized to logical processors, plus each
  software ISO's `videoWorkUs / muxVideoFrameCount` and `droppedFrames`. These go to
  `measurement.json` under `encoderCost`, and are supplementary. They explain a FAIL; they
  never cause one.
- Gating stays: every ISO writing (`Test-DrillInvariant`), recording producing, the
  per-frame identity verdicts, and the other four throughout flags. Per D8, Program's half of
  the old flag becomes its own gating flag, `programEncoderHardwareThroughout`.

### 5. What is fatal, restated

The recording start is refused or rolled back only when Program cannot record: no real
encoder (stub/counting encoder), a Program writer that failed to open or begin, or a
lifecycle `failed`. A qualification run fails only on measured loss: missing, repeated,
reordered or invalid identities, rising drop counters, a writer that disappeared, or a
recording that stopped producing. ISO encoder placement is never fatal by itself. Program on
the software encoder is a qualification FAIL for RC (D8).

## Acceptance

1. `node scripts/validate-iso-record.mjs --sources 8 --seconds 20 --evidence <file>` on a
   Release `build-dev` core prints `ISO-RECORD VALIDATION PASS` **without**
   `--allow-capacity-warning`. When a spill happens it prints one `Note :` line naming the
   ISO, its index and its measured ms/frame. To force the spill on the 4090, the run uses
   the existing override `COREVIDEO_ENCODER_PROBE_MAX_SESSIONS=8`. That limits discovery
   depth only; it does not change probe semantics.
2. The same run with an injected writer stall (`--stall-source zoom:<id-of-the-software-ISO>
   --stall-ms 6000`, past the 4 s join bound) still FAILS, on loss or abandonment, not on the
   note.
3. A unit test drives `TransportCoordinator.ToggleRecordingAsync` with a snapshot that
   carries the admission note (status `warning`, health `degraded`, `isoAdmission.decision =
   admit-with-warning`). Recording stays requested, no rollback sync is sent, and
   `OutputStatus` is "Recording start requested.". The existing `"failed"` test
   (`TransportCoordinatorTests.cs:327`) still rolls back.
4. The Windows MF test `MediaFoundationEightZoomIsoWritersSurviveEncoderCapacity` asserts
   `session.isoAdmission` (decision, counts, code) and that the software stream's `isoIndex`
   is 8. A stub-sink MediaCore test asserts both fields reach the snapshot JSON.
5. The shell readout tests cover: one software ISO, under budget -> Informational line with
   measured ms; over budget or dropped -> Warning; zero frames -> "cost not measured yet";
   all hardware -> no line; Program software -> Program line.
6. Outputs drill: `outputs-drill.test.ps1` / `test_outputs_drill.py` show a software-ISO
   sample no longer fails the run, while a missing ISO identity still does. One 30-minute
   `outputs-drill.ps1 -RemoteIds ...` run with `-ProbeMaxSessions 8` (one software ISO)
   reports, per output, the identity verdict plus the software-ISO cost note, with the
   measured CPU. Whether that ISO PASSes is the result being measured. A FAIL there is a
   finding to report, not something the design waives.
7. Docs updated in the same change: `docs/reference/iso-recording.md` (validator paragraph,
   `:97-100`), `docs/qa/outputs-drill.md` (throughout list and verdict rule),
   `docs/reference/encoder-capacity-probe.md` (structured node).

## Risks

- **CPU load from a software ISO at 1080p60.** The software budget
  (`softwareSessionBudget`, `EncoderCapacityProbe.cpp:115-130`) is an assumed formula
  (logical CPUs / 4 per 1080p30 stream, scaled by pixel rate). It was never measured. On the
  dev box's 28 logical CPUs it admits 3 software ISOs at 1080p60. A software H.264 encode
  competes with the core's CPU source preparation and audio. If it cannot hold 60 fps, that
  writer drops (`streams[].droppedFrames`), and the identity judge fails that ISO, which is
  the correct outcome. The risk is collateral damage: CPU starvation stalling Program or the
  audio worker. **The drill must therefore measure**: core process CPU % (per second, peak
  and mean), each software ISO's mean and peak ms/frame against 16.7 ms, its
  `droppedFrames` and `writeQueue.highWater` / `oldestQueuedAgeMs`, Program
  `programMissingFrames`, `programBuffer.underruns` and the Program GPU deadlines
  (supplementary today). All of them are reported per run, with the hardware, the workload
  and the duration.
- **Turning a gate into a note hides a real problem.** The notes are reported on every run
  and in the operator UI, never dropped, and the per-frame identity and loss counters still
  gate. A software ISO that drops a frame fails.
- **Why does the 4090 spill at all, if the probe says 9?** With `sessions<=9`,
  `toIsoEncoderCapacity` gives 9 - 1 (Program) = 8 hardware ISO sessions, so 8 ISOs should
  not spill. A 7+1 split means the arm-time `lookup()` saw either the unprobed fallback (8,
  "pending": the 1080p60 prewarm in `configureRecording`, `:1549`, may still be running when
  `start` arms 2 s later in the validator) or a smaller measured ceiling. This design does
  not change the probe. Plan Task 1 records `isoAdmission.code`, `probed` and `probeStatus`
  from the run, so the cause is measured. If it is the prewarm race, that is a separate
  issue to file. It is not fixed here.
- **Old evidence re-judged.** Drill folders judged before this change were failed on
  `hardwareEncodersThroughout`. The new judge would pass the same evidence if identities were
  clean. The verdict schema bumps to `outputs-drill-verdict-v3` so the two are never
  confused.
- **Text compatibility.** Anything outside the repo that reads `recording.warning` sees the
  same sentence as before. The new JSON fields are additive.

## Decisions (owner, 2026-10-23 23:50 EDT)

- **D8 — what fails a run.** An ISO on the software encoder is a **note**: its row passes or
  fails on frame identity alone (`iso-pixel-verdict.py`, plus the loss counters). **Program on
  the software encoder stays a FAIL for RC.** The outputs drill therefore keeps a gating
  `programEncoderHardwareThroughout` flag (Program `recording.encoder.hardwareAccelerated`
  true on every sample), and `validate-iso-record.mjs` fails when Program reports
  `hardwareAccelerated == false` or does not report it. Only the ISO half of the old combined
  `hardwareEncodersThroughout` flag is demoted to a recorded fact. At record start Program on
  software is still not a cancel (section 1): the take proceeds, and the qualification fails.
- **D8 is judged on a MEASURED value (review B1, 2026-10-24).** `recording.encoder.hardwareAccelerated`
  used to be a constant set in the sink constructor; Media Foundation silently falls back to the
  software MFT when hardware is only requested. The core now reads back the encoder MFT each writer
  instantiated and publishes `recording.encoder.encoderPath`, per-stream `encoderPath` (Program and
  every ISO) and `hardwareRequested` separately. Both gates read the measured path; absent fails.
- **D9 — UI severity.** The shell shows a software ISO as **Informational**, with its measured
  per-frame cost. It turns **Warning** only when that writer drops frames (or its measured mean
  work exceeds the frame budget). Program on the software encoder gets its own line in the same
  readout.

These answer the two open questions this design was waiting on: (1) Program may **not** be
judged by identity alone; (2) an under-budget software ISO is Informational.

## Out of scope

- Any change to `EncoderCapacityProbe` discovery, its cap (9), its fallback (8), or the
  software budget formula.
- Reserving a session for the stream encoder in ISO placement (the 10-session layout).
- A per-encoder CPU meter in the shell.
- Fixing a probe-pending race at arm, if Task 1 shows one. That gets its own issue.
