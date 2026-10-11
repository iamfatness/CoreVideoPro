# Idle recording state — #859

Windows validation on October 10 against `main` `04dc017` plus the focused
stop-state fix. Native build: Release, D3D11 and Media Foundation encoder enabled;
Zoom, camera and network output adapters were not enabled in this isolated rig.
The installed app and production settings were preserved.

## Cause and change

Disabled recording synchronization sends Stop before any recording has started.
Previously `MediaCore::stopRecordingSession` unconditionally set the internal
state to stopping/finalizing. An idle sink has no recording lifecycle, so snapshot
fallback retained that invented state indefinitely. The internal stopping gate
also prevented a later non-recording output from starting the encoder.

Idle Stop now leaves state untouched when both the core and sink have no
recording. A synchronous sink's returned stopped status releases the fallback
state. Actual asynchronous recordings retain the existing FIFO Stop barrier and
writer-owned lifecycle; the command does not claim finalization.

## Evidence

The new idle/stub regression checks failed against the pre-fix library, and the
real finalization validator failed against the previous beta's native executable
on its first idle Stop, before recording anything. Private baseline logs are
retained under ignored `artifacts` and the previous build's proof directory.

The fixed real-MF validator passed three idle Stops, one generated 1080p60 MP4
recording, and three repeated terminal Stops. It observed requested, preparing,
producing, stopping and completed. Stop acknowledgement remained stopping with
`finalized=false`; the completed result retained the same session identity and
`finalized=true`. Full container decode and generated color-bar inspection passed
while the core remained alive. Audio/video duration difference was 0.009783 seconds.

Native executable SHA-256:
`284e7fec3cfc6137c808a011d1b32ddcb351d37f091ce67be5561ae31ae387e7`.
Local proof: `native/build-dev/recording-finalization-proof-1791689293157/evidence.json`.

Focused regression coverage passed 100 checks: 30 EncoderRecordingSession,
41 AsyncEncoderSink, eight ProgramBufferIntegration and 21 OutputLifecyclePolicy.
The first fixed-build test run had two test assertions expecting a present null
node; direct command snapshots omit idle recording instead. Those assertions
were corrected to accept absent/null (both exclude a phantom recording) and both
passed on rerun. The other 28 recording checks passed on the first fixed run;
their runtime implementation did not change afterward. Failed logs are retained.

This run reported zero steady encoder video queue drops and 13 startup drops.
It is finalization/state evidence, not zero-miss or real-meeting qualification.
`writeQueueDepth=10` in the original idle snapshot was configured capacity, not
ten pending writes. The symptom does not establish a damaged recording.
