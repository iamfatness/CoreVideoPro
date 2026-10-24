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
| `-SkipStream`, `-SkipVirtualCamera` | off | see below: skipping the camera means the run cannot PASS |

Without `-RemoteIds` the drill uses local sources only: webcam and screen
capture in Program (scene `speaker-slides`), the webcam as the only ISO, and
the verdict says so. Screen capture carries no QA ISO identity stamp (only Zoom
and UVC sources call `traceQaIsoArrival`), so it is never ISO-recorded here.
Media sources are not part of the workload yet.

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
   automation off, assigns inputs, cues and takes the scene, and starts record
   and stream. It then waits until every check has held for 30 s (300 s cap).
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
   - `hardwareEncodersThroughout`: the Program encoder and every ISO on hardware.
   - `remotes1080Throughout`: every remote live at 1920x1080.
   - `captureProgressThroughout`: every capture at 1920x1080 or larger, with frames advancing within 2 s.
   - `programSourcesAdmittedThroughout`.
   - `streamHealthyThroughout`.
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
interval) **and** every throughout-assertion held. NOT JUDGED and UNBOUNDED
never count as PASS. With `-SkipVirtualCamera`, Program and stream can only be
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
  webcam, a 720p remote, software encoders and a slow stream each fail.
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
