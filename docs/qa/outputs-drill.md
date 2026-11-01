# Outputs drill (release gate, #443)

`scripts/qa/outputs-drill.ps1` runs the release-gate workload against an
already-installed CoreVideo Pro and writes a per-output, per-frame verdict.
Owner ruling 2026-10-22: the outputs are the release gate (Program recording,
every ISO, the registered OS virtual camera, the stream), judged by pixel
identity with zero tolerance. On-screen Studio cells are a UX metric with a
tolerance and are **not** part of this command.

**Amendment, owner ruling 2026-10-23 23:27 EDT (#1030, #1038):** a held frame passes the per-frame identity gate as a labelled, counted, source-caused repeat. A held frame is the last picture that an ISO writer re-submits while that source's video is stalled. It passes only when all three conditions hold: (1) it repeats the immediately preceding picture; (2) it lies at least 1 s, minus one frame period, after that picture; (3) the number of such repeats in the file equals the writer's `heldFrames` in the finalized recording snapshot. The close-time tail hold is excused the same way, only as the file's last frame when `tailFrames` is 1. Zero tolerance remains for anything the pipeline itself loses or duplicates: a missing arrival, any other repeat, a reorder, an invalid picture, or a hold count that disagrees with the writer still fails. A passing row with holds must read `held=N, source stalled`, so the stall is never a silent pass.

```powershell
# 30-minute local-sources drill (webcam + screen capture), all outputs:
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\qa\outputs-drill.ps1

# Full workload with real remote feeds in the test meeting:
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\qa\outputs-drill.ps1 `
  -RemoteIds 16778240,16783360 -MeetingUrl '<full test-meeting link with pwd>'
```

| Parameter | Default | Notes |
|---|---|---|
| `-InstallRoot` | target of the Start-menu "CoreVideo Pro" shortcut | folder holding `StartCoreVideo.cmd` |
| `-Seconds` | 1800 | measured interval, 60 to 7200 |
| `-Out` | `D:\CoreVideo-QA\drills\<timestamp>` | must be on D: and empty |
| `-RemoteIds` | none | Zoom participant ids (up to 8, slots 1-5,7-9), recorded as ISOs; needs `-MeetingUrl` |
| `-WebcamSourceId` | the one `capture:<id>` in the profile | refuses if the profile names zero or several |
| `-NoWebcam` | off | run without the webcam (camera rig off the bench): Program is the single-route `intro` scene with the staged motion clip (`-MediaClip`), no screen is captured, a local-only run has no ISO, and the summary carries `Webcam source  NOT EXERCISED`; excludes `-WebcamSourceId` |
| `-MediaClip` | none (required with `-NoWebcam`) | a 1920x1080 H.264 clip at exactly 60 fps that outlasts `45 + Seconds + 55 + 360` s; `scripts\qa\outputs-drill-clip.ps1` makes one. Staged as `%LOCALAPPDATA%\CoreVideoPro\media\qa\outputs-drill-motion.mp4` before launch and removed after |
| `-CameraReceiver` | `native\build-dev\corevideo-shared-camera-receiver.exe` | build target `corevideo-shared-camera-receiver`; it is not in the installer |
| `-SkipStream`, `-SkipVirtualCamera` | off | see below: skipping the camera means the run cannot PASS |
| `-ProbeMaxSessions` | unset | 1..16; sets `COREVIDEO_ENCODER_PROBE_MAX_SESSIONS` for the launch (discovery depth only) to force a software-ISO spill; recorded in `drill.json` and the workload line |

Local-only runs never call `transport.engine.set`: the capture engine is the
Zoom media engine and the control is refused (HTTP 422) outside a meeting, while
webcam and screen capture route to Program without it. The drill also refuses,
before any output starts, when the profile's webcam id is not in the app's
capture-device list (unplugged or renamed), naming the UVC devices it did see.
A refused `/invoke` is reported with the action, its arguments and the router's
reason text.

Without `-RemoteIds` the drill uses local sources only: webcam and screen
capture in Program (scene `speaker-slides`), the webcam as the only ISO, and
the verdict says so. With `-NoWebcam` (added 2026-10-26, when the owner's
camera rig was off the bench and the profile's HD60 S+ had no device) Program
is a synthetic 1080p60 motion clip alone, cued through the catalog's
single-route `intro` scene (layout host-focus, route `intro-1`), the device
check is skipped, no screen is captured, and a local-only run records no ISO at
all, since a media clip carries no QA ISO identity; the preferences patch then
turns ISO recording off rather than arming an empty list.

**Why a clip and not the screen (owner ruling 2026-10-30).** The first
complete stamps-on run (2026-10-30, 30 min) routed `capture:screen:0`, which
is the owner's portrait secondary monitor. Screen capture is event-driven: the
display was dark and static, the core marked the source `stalled` after one
frame, Program carried that held black frame for 32 minutes (the three output
rows still passed, 108034/108034 each, as counted holds), and the drill's
progress rule failed from the first sample. Earlier runs had captured the live
desktop into the evidence. The media bin (`MediaBinClassifier.cs`) is a folder
scan of `%LOCALAPPDATA%\CoreVideoPro\media` at app start, its asset id is
`media-` + the first 12 hex of SHA-256 over the forward-slash relative path, and
there is no control-API import; so the drill copies the clip to
`media\qa\outputs-drill-motion.mp4` before launch (asset `media-63a585ec7312`),
assigns `media:media-63a585ec7312` to slot 6, and deletes the copy after the app
exits. A bin clip plays once from Take and then holds its last frame as
`ended` (only a scene background loops), so the clip must outlast the run; the
drill refuses a short, non-1080p60 or non-H.264 clip with ffprobe before it
touches anything, readiness requires the clip's `mediaSources[]` transport to
be `live` on Program, and an `ended` transport aborts the collection. The
show-input roster (`show-input-roster.json`) is backed up and restored with the
preferences, since `input.assign` persists into it. Cueing `speaker-slides` with its webcam route null is
accepted, but the shell sends that route with mode `none`, the core logs
"unsupported mode none" and marks Program health degraded, and readiness
("Program source admissions not ready") never passes; the 2026-10-28 live run
spent its 300 s on that, so the empty-route form is not used.

The camera serving proof (`camera-serving-identity.json`: the installed DLL's
"Serving module path" line and a later `[vcam-delivery-v2]` line of that pid
with `historyReads > 0`) is built by `Get-DrillCameraServingProof` from plain
strings. The 2026-10-29 and 2026-10-30 live runs wedged right after "Outputs
ready" with outputs left live and one core at 100 %: the proof record held the
lines as `Get-Content` returned them, with their provider note properties
(`PSDrive`, `PSProvider`, ...), and `ConvertTo-Json -Depth 8` walked that object
graph without returning. The script now casts the tail to `[string[]]` and the
function returns strings; a unit check serializes a proof built from
`Get-Content` lines in under 5 s. Found with `Set-PSDebug -Trace 1` scoped to
the loop in a diagnostic copy of the script; a non-invasive `cdb -pv` stack of
the host had not reached the pipeline thread. `drill.json` carries `webcamExercised: false` and a null
`webcamSourceId`, the verdict gets `notExercised: ["Webcam source"]`, and the
summary prints `Webcam source  NOT EXERCISED` under the output rows. The
outputs are still judged with zero tolerance; the line says only that UVC
capture was not on the bench, so such a run is not evidence about the webcam. Screen capture carries no QA ISO identity stamp (only Zoom
and UVC sources call `traceQaIsoArrival`), so it is never ISO-recorded here.
With a webcam, media sources are not part of the workload.

## What it does

1. Refuses if `CoreVideoPro.WinUI`, `corevideo-native` or `corevideo-zoom-engine`
   is running, or anything answers on port 8011. Refuses to stream when the
   stored SRT passphrase is DPAPI-protected (`dpapi:`), since the sink cannot use it.
2. Copies `%LOCALAPPDATA%\CoreVideoPro\production-output-preferences.json` **and
   its `.bak` sidecar** (the app's `AtomicJsonFile` keeps the previous primary
   there) to `%LOCALAPPDATA%\CoreVideoPro\outputs-drill-backups\...`, outside
   the evidence folder because the files carry the DPAPI-protected RTMP key.
3. Patches the primary: `RecordingTargetFolder` = `<Out>\recordings`, the ISO
   selection, RTMP **off**, SRT caller to `127.0.0.1:<free UDP port>`. Every
   value is written with the app model's JSON type (`StreamSrtPort` is a
   *string*). A patch that changes the JSON kind of any existing value is
   refused, because the app would reject the whole file and silently fall back
   to its `.bak`. The stored RTMP key and URL are never changed.
4. Starts the loopback SRT listener (ffmpeg, `stream-received.ts`) right after
   choosing the port. Then it launches `StartCoreVideo.cmd`. The drill runs at
   Normal priority for the whole live phase, so the app, the sink and the
   camera receiver all start at Normal; each one's priority is checked and
   recorded. The launch sets `COREVIDEO_QA_ISO_COUNTER=1`,
   `COREVIDEO_QA_PROGRAM_COUNTER=1` and a delivery trace (`all` stages) at
   `<Out>\arrival-trace.bin`. The trace path is fixed at launch. These
   counters change output pixels: use them for QA runs only, never a show.
5. Once the control API answers, re-reads the preferences file and **refuses to
   start any output** unless the patch is still there with the same types
   (`preferences-applied.json`). Readiness also fails hard if the recording
   writes outside `<Out>\recordings` or any RTMP sender appears.
6. Through the control API only: joins the meeting (remote mode), turns
   automation off, assigns inputs, verifies the planned scene is on the rail,
   cues and takes the scene, and starts record and stream. Both Program scenes
   the drill cues (`speaker-slides`, and `intro` with `-NoWebcam`) are catalog
   scenes, and since #1070 the operator can delete any catalog scene and the
   deletion persists in the profile. So before `scene.cueWithRoutes` the drill
   reads `scene.list`; when the planned scene is missing it invokes
   `scene.restoreStarters` once (re-seeds the missing catalog scenes at their
   catalog position with starter routes, so the `<scene>-1` / `-2` route ids
   the plan names exist again), re-reads the rail, and refuses to start any
   output if the scene is still absent. A restore is logged and recorded in the
   `notes` of `drill.json`; the restored scene is left in the profile (it is a
   catalog scene the operator can delete again). It then waits until every
   check has held for 30 s (300 s cap).
   The checks are remotes at 1920x1080, captures ingesting, every ISO writing,
   Program source admissions consistent, and one stream sender delivering at
   55 fps or more with no retry and no warnings. The sink must also have
   received bytes.
7. Enables the virtual camera and the independent OS camera receiver, and
   proves that the registered camera serves the installed DLL
   (`vcam-serve.log`). It settles for 45 s, then measures for `-Seconds`. Every
   second it checks invariants: a core restart, stopped recording, changed
   Program sources, an RTMP sender, or a dead sink or receiver aborts the run
   as FAIL. It also accumulates the throughout-assertions of the reference
   `Collect-Identity.ps1` into `measurement.json`:
   - `programEncoderHardwareThroughout`: Program's MEASURED
     `recording.encoder.encoderPath` is `hardware` on every sample (gating; owner
     ruling D8: Program on software is a FAIL for RC). The path is read back from
     the encoder MFT the Program writer instantiated, never inferred from the
     hardware request (`hardwareRequested`); absent fails.
   - `hardwareEncodersThroughout`: Program and every ISO on hardware. **Recorded
     as a fact only, never gating** (#935, D8): an ISO on the software encoder
     is a note. `encoderPlacement` lists each ISO's MEASURED `encoderPath`
     values (`not reported` until its writer began) and whether it changed.
   - `remotes1080Throughout`: every remote live at 1920x1080.
   - `sourceProgressThroughout`: every capture at 1920x1080 or larger, with frames advancing within 2 s.
   - `programSourcesAdmittedThroughout`.
   - `streamHealthyThroughout`.

   It also measures, once per second and supplementary only (it explains a
   FAIL, never causes one), `encoderCost`: the installed core process CPU %
   normalized to logical processors (mean and peak), and for each software ISO
   its `isoIndex`, mean and peak-interval encode work per frame
   (`videoWorkUs / muxVideoFrameCount`), `frameRate`, and `droppedFrames` at
   start and end. A value that could not be measured is null, never 0.
8. **Shutdown, in a `finally` that also runs on Ctrl-C:**
   - Stops in `Collect-Identity` order: stream, then record. It waits up to
     90 s for the recording to finalize.
   - Then the trace stop event, the virtual camera off, the engine off, and the
     trace finalize. It leaves the meeting.
   - It waits until the shell's close guard would have nothing to ask: not
     recording, not streaming, engine off, recording lifecycle settled, no
     live sender.
   - Only then it sends WM_CLOSE. If outputs are still live it does **not**
     send WM_CLOSE, because that would raise the "Stop outputs and close?"
     prompt and activate the window.
   - Preferences are restored only after the app has exited, waiting up to
     10 min. Otherwise it writes `preferences-restore-pending.json` (backup
     paths and instructions), prints a red warning and exits 3. The app
     re-saves preferences while it runs, so an earlier restore would be lost.
9. Judges offline with `scripts/qa/outputs-drill-judge.py`.

## Verdict

`<Out>\verdict.json` and `<Out>\summary.txt` have one line per output, with
identities expected / decoded / missing / repeated and one of these statuses:

- **PASS** or **FAIL**.
- **UNBOUNDED**: clean over the whole file, but no OS camera interval bounded it.
- **NOT JUDGED**: the judge could not run; the reason is given.

The overall verdict is PASS only when every enabled output is PASS (zero
missing, repeated, reordered or invalid identities inside the camera-bounded
interval) **and** every gating throughout-assertion held. NOT JUDGED and UNBOUNDED
never count as PASS. **Encoder placement is reported, never gated; a run fails
only on measured loss** (#935): an ISO on the software encoder keeps its row's
PASS/FAIL from `iso-pixel-verdict.py` alone, its summary row gets
` encoder=software`, and a note gives its measured cost ("ISO-n <source> on
software encoder: mean X ms of Y ms per frame, peak interval Z ms, dropped
frames a -> b, core CPU mean m % / peak p %", with "not measured" for anything
absent). Program on the software encoder fails through
`programEncoderHardwareThroughout`. A required flag missing from
`measurement.json` (`programEncoderHardwareThroughout`, `remotes1080Throughout`,
`sourceProgressThroughout`, `programSourcesAdmittedThroughout`) fails as
"(not recorded)". The verdict schema is `outputs-drill-verdict-v3`; evidence
judged as v2 failed on `hardwareEncodersThroughout` and lacks the Program flag,
so never compare the two. With `-SkipVirtualCamera`, Program and stream can only be
UNBOUNDED, so such a run reports counts but can never PASS. The reference
harness refused to judge Program without the camera bound.

| Output | Judge (unchanged) | Expected identities |
|---|---|---|
| Program recording | `recording-pixel-verdict.py`, gated on `program-source-admission-capture.mjs` | Program counter range seen by the OS camera inside the interval |
| each ISO | `iso-pixel-verdict.py` | source arrivals offered in the trace (stage 11) inside the interval |
| OS virtual camera | `camera-pixel-receiver.mjs` | receiver samples bracketing the interval |
| Stream | `recording-pixel-verdict.py` on `stream-received.ts` | same range as Program |

Program GPU deadlines (`delivery-trace.py --require-program-deadlines`) are
printed as supplementary evidence only, outside the overall verdict.
`iso-pixel-verdict.py` gained additive `missing`/`repeated` counts for the
summary (#443), and #1030 extended its pass rule as follows.
**Held frames (#1030):** a repeat is excused only when it repeats the immediately preceding picture at least 1 s (minus one frame period, the median packet interval clamped to 1/frameRate) after it AND the excused count equals that ISO's `heldFrames` in the finalized recording snapshot; the close-time tail hold is excused only as the file's last frame repeating the one before it when that ISO's `tailFrames` is 1 (a stall that runs through Stop); any other repeat fails, and an excused stall is reported on the row as `held=N, source stalled` with the longest arrival gap. The summary's `repeated=` column excludes excused repeats by design. Accepted narrow hole: a stale-stamped duplicate from upstream that lands in a hold's slot is pixel-identical to the hold, so it passes as that hold. A source stall by itself never counted as missing: the ISO judge expects only arrivals the trace OFFERED, so a stall (no arrivals) passed before held frames existed and still does (pinned by `test_iso_pixels.test_a_source_stall_without_holds_loses_no_arrival`); the stall shows in `coverage.maximumSourceArrivalGapMs`.

## Tests

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\qa\outputs-drill.test.ps1
python scripts\qa\test_outputs_drill.py
```

The PowerShell test covers:
- Parameters and the workload plan.
- The preferences patch on a v14-shaped profile. The raw JSON must contain
  `"StreamSrtPort": "50999"`, every value must keep its JSON kind, and the
  original numeric-port bug must be caught.
- The applied-check, including the `.bak` recovery case.
- Backup and byte-exact restore of the primary and the `.bak` sidecar,
  including removing a sidecar the drill created.
- Readiness, invariants and throughout flags on sample snapshots: a frozen
  webcam, a 720p remote, a software Program encoder and a slow stream each fail;
  a software ISO is recorded as a fact and in `encoderPlacement` without failing
  any gating flag; an encoder-path change is reported; `Get-DrillEncoderCost`
  and the cost record give null, not 0, for anything unmeasured.
- The close-guard predicate.
- When dotnet and an installed app are present, the installed app's own
  `ProductionOutputPreferencesSerializer`, loaded by reflection through
  `scripts/qa/outputs-drill-prefs-probe`. It must load the patched profile and
  reject the numeric-port variant.

It then runs the Python tests: verdict aggregation, plus the offline
orchestration on synthetic 1080p60 videos, a synthetic trace, synthetic OS
camera NDJSON and admission samples, decoded by the real judges.

## Not yet proven

The live part has not been run end to end from this script. That covers the
launch, the API driving, the preferences applied-check against the running
app, the sink, the receiver, the stop and close sequence, and the restore
against the real files. It is a transcription of the 2026-10-22 harness
(`D:\CoreVideo-QA\beta-20261021-build\artifacts\qualification-1008-25`),
which passed a 30-minute soak. Tested on Windows PowerShell 5.1 only.

**#935 software-ISO run: not run yet.** The 30-minute drill with one ISO forced
onto the software encoder (`-ProbeMaxSessions 8`) is the measurement #935 Task 8
reports: per-output identity, the software ISO's mean and peak ms/frame, its
dropped frames, core CPU mean and peak, Program `programMissingFrames`,
`programBuffer.underruns` and the GPU deadlines line, with hardware, workload and
duration. Its numbers go here when it has run. A software-ISO identity FAIL there
is a finding (the software budget admitted a stream this machine cannot carry at
1080p60), not something to waive.
