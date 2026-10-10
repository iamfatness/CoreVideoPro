# Lower-third fallback casing: #592

The owner's reproduction was confirmed in the designated Zoom test meeting on
the installed `34b0afc306f6c6949131ef261a7ffe2354672039` beta with lower thirds enabled.
The same source's native `key:lower-third.title` changed from `guest` to `Guest`
and back while its raw Zoom role remained `Guest` and its authored title was absent.
The initial 100-sample trace captured eight uppercase pulses. The first was
02:40:10.859 UTC, followed by lowercase at 02:40:11.177 UTC on the same source.

## Cause and change

`ZoomMediaSpinePayloadBuilder.MapRole` serializes lowercase wire role labels.
`LiveProductionSync.MapRawParticipants` formerly preserved that casing and copied
it into an absent participant title. SDK/native roster snapshots carry display
casing. Both mappings feed the shell's source-bound key; a content comparison
therefore sees a different title on refresh and sends a new overlay asset even
though the participant's role did not change. Native output received the different
complete strings; this is not solely a WinUI readout discrepancy.

Normalize known role labels once at the shared roster mapping boundary. Both
paths now generate `Guest` for an absent title. Authored titles and custom role
labels retain their casing. No animation, native raster, or render-rate change
is included in this patch.

## Validation

- Nine new regression cases cover casing variants, missing role, other known
  roles, equal resolved key content across refresh, and authored text preservation.
- Release MediaCore suite: 2,357 passed. One initial full-suite run failed the
  unrelated process-exit descendant sweep test; its focused rerun (44 tests,
  including mapping tests) and the complete rerun passed.
- Live comparison uses the installed beta's exact MediaCore source plus only
  this mapping change, built separately. Other assemblies and native binaries
  remained those of the installed beta. The original library was backed up and
  restored with a hash comparison after validation.
- Patched installed run: 243 observations over 50.125 seconds, all `Guest`.
  Concurrent OS webcam capture: 2,700 frames in 45 seconds; an extracted output
  frame visibly renders `Guest`.
- Restored original run: 171 observations over 35 seconds, 150 `guest` and 21
  `Guest`, reproducing the case changes again. A concurrent 30-second webcam
  capture retains the original output frame sequence.

Local evidence is under `artifacts/`: `lower-third-live-baseline-trace.json`,
`lower-third-baseline-transitions.json`, `lower-third-installed-fixed.jsonl`,
`lower-third-installed-restored.jsonl`, their summary JSON files, the matching
camera MP4s, test logs, library hashes, and cleanup report. Meeting credentials
are excluded from these traces. `scripts/qa/lower-third-casing-trace.py` provides
a read-only repeatable observation harness.

The temporary validation copy could not publish the camera because registration
belongs to the installed path; installed-path validation above supersedes it.
Failed capture setup attempts (missing encoder / pixel format) are retained in
separate logs and are not counted as captures.

## Limits

This confirms and fixes the reported casing change on one machine. Snapshots are
sampled, and the retained videos have not been judged frame by frame for every
possible partial glyph or clipping artifact. Recording and streaming outputs
were idle. This is not a long bake, a public beta, or closure of #592's broader
output acceptance criteria. The app left the test meeting and closed afterward;
the original installed library remains in place.
