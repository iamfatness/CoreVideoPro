# Outputs drill (release gate, #443)

`scripts/qa/outputs-drill.ps1` runs the release-gate workload against an
already-installed CoreVideo Pro and writes a per-output, per-frame verdict.
Owner ruling 2026-10-22: the outputs are the release gate (Program recording,
every ISO, the registered OS virtual camera, the stream), judged by pixel
identity with zero tolerance. On-screen Studio cells are a UX metric with a
tolerance and are **not** part of this command.

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
| `-CameraReceiver` | `native\build-dev\corevideo-shared-camera-receiver.exe` | build target `corevideo-shared-camera-receiver`; it is not in the installer |
| `-SkipStream`, `-SkipVirtualCamera` | off | a skipped output is listed as SKIPPED, never PASS |

Without `-RemoteIds` the drill uses local sources only: webcam and screen
capture in Program (scene `speaker-slides`), the webcam as the only ISO, and
the verdict says so. Screen capture carries no QA ISO identity stamp (only Zoom
and UVC sources call `traceQaIsoArrival`), so it is never ISO-recorded here.
Media sources are not part of the workload yet.

## What it does

1. Refuses if the app is already running or anything answers on port 8011.
2. Backs up `%LOCALAPPDATA%\CoreVideoPro\production-output-preferences.json`,
   then points `RecordingTargetFolder` at `<Out>\recordings`, selects the ISO
   sources, forces RTMP **off**, and points SRT (caller) at `127.0.0.1:<free UDP port>`.
   The stored RTMP key and URL are never changed.
3. Launches `StartCoreVideo.cmd` at Normal priority with
   `COREVIDEO_QA_ISO_COUNTER=1`, `COREVIDEO_QA_PROGRAM_COUNTER=1` and a delivery
   trace (`all` stages) at `<Out>\arrival-trace.bin`. The trace path is fixed at
   launch. These counters change output pixels: QA runs only, never a show.
4. Through the control API only: joins the meeting (remote mode), turns
   automation off, assigns inputs, cues and takes the scene, starts a loopback
   SRT listener (ffmpeg, `stream-received.ts`), starts record and stream, and
   waits until every output is healthy for 30 s (max 300 s).
5. Enables the virtual camera and an independent OS camera receiver, proves the
   registered camera serves the installed DLL (`vcam-serve.log`), settles 45 s,
   then measures for `-Seconds`. A core restart, stopped recording, changed
   Program sources, dead sink or receiver aborts the run as FAIL.
6. Stops stream, record (waits for finalize), trace, camera, engine; stops the
   sink; leaves the meeting; closes the app with WM_CLOSE; restores the
   preferences file byte for byte (also on failure).
7. Judges offline with `scripts/qa/outputs-drill-judge.py`.

## Verdict

`<Out>\verdict.json` and `<Out>\summary.txt`, one line per output with
identities expected / decoded / missing / repeated and PASS, FAIL or NOT
JUDGED. Overall PASS only when every enabled output passes with zero missing,
repeated, reordered or invalid identities. A judge that cannot run marks its
output NOT JUDGED with the reason; that never counts as PASS.

| Output | Judge (unchanged) | Expected identities |
|---|---|---|
| Program recording | `recording-pixel-verdict.py` | Program counter range seen by the OS camera inside the interval |
| each ISO | `iso-pixel-verdict.py` | source arrivals offered in the trace (stage 11) inside the interval |
| OS virtual camera | `camera-pixel-receiver.mjs` | receiver samples bracketing the interval |
| Stream | `recording-pixel-verdict.py` on `stream-received.ts` | same range as Program |

With the camera skipped or unjudged, Program and stream are judged over their
whole files and the summary says so. Program GPU deadlines
(`delivery-trace.py --require-program-deadlines`) are printed as supplementary
evidence only, outside the overall verdict.

`iso-pixel-verdict.py` gained additive `missing`/`repeated` counts for this
summary; its pass rule is unchanged.

## Tests

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\qa\outputs-drill.test.ps1
python scripts\qa\test_outputs_drill.py
```

The PowerShell test covers parameter validation, the workload plan, the
preferences patch/restore round trip on a temp file, the sink URL, readiness
and collection invariants on sample snapshots, and runs the Python tests. Those
cover verdict aggregation from sample judge outputs and the offline judge
orchestration on synthetic 60-frame videos and a synthetic trace decoded by the
real judges (clean run PASS; one dropped frame FAIL on Program, ISO and stream).

## Not yet proven

The live part (launch, API driving, sink, receiver, stop order, app close) has
not been run end to end from this script; it is a transcription of the
2026-10-22 harness (`D:\CoreVideo-QA\beta-20261021-build\artifacts\qualification-1008-25`),
which passed a 30-minute soak. Tested on Windows PowerShell 5.1 only.
