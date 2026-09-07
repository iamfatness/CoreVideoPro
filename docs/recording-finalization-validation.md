# Recording finalization and decode proof

Run the strict local recording harness with an explicit freshly built core:

```powershell
node scripts/validate-recording-finalization.mjs --native-core C:/path/to/build-dev/corevideo-native.exe
```

It requires a real Media Foundation or AVFoundation encoder plus `ffmpeg` and
`ffprobe`. Missing tools, a stub encoder, absent lifecycle fields, failed writer
finalization, invalid streams, and decode errors fail the run.

The test generates its own 1920×1080 color-bar image, configures 60fps output, and
records six seconds after the writer becomes live. It routes no camera, starts no
Zoom engine or meeting, and configures no network destination. Silent audio is
expected. After Stop, it waits for the same session to report both `completed` and
`finalized: true` before probing and fully decoding the artifact. Pixel checks
ensure the decoded recording contains varied color content.

Artifacts, core stderr, binary/file hashes, lifecycle transitions, probe output,
effective video rate, and encoder queue drops remain in a unique
`recording-finalization-proof-*` directory beside the executable. The test's
`passed` verdict covers finalization and decoding only. Performance warnings are
reported separately; this short synthetic scene cannot establish live Zoom,
multisource load, continuous 60fps, A/V content synchronization, or soak acceptance.

The initial Windows validation and an isolated rerun both finalized and decoded
successfully. The isolated rerun delivered approximately 51.13 effective video
frames/second with 65 encoder queue drops despite a steady 60fps renderer. This is
an outstanding writer-throughput finding, not passing 60fps evidence. A detached
build of baseline `660f626` with identical adapter flags/compiler/SDK produced
50.72 effective fps and 62 drops on the same generated scene and duration. The
baseline needed no source changes. This comparison identifies an existing
throughput ceiling; it does not establish statistical performance equivalence.

A detached build of the reviewed baseline (`660f626`) was tested with the same
compiler, SDK, build flags, generated scene, recording settings, and duration.
The baseline was left unchanged; its comparator waited for actual written frames,
sent Stop, then required a readable container and full decode because baseline
lifecycle fields do not exist.

| Build | Effective video fps | Encoder queue drops | Final artifact decode |
| --- | ---: | ---: | --- |
| Reviewed baseline `660f626` | 50.72 | 62 | Passed |
| Remediation build, isolated run | 51.13 | 65 | Passed |

This short comparison reproduces the throughput limitation on the baseline. It
does not indicate a new throughput regression from the lifecycle changes; a
controlled longer benchmark is still needed before claiming 60fps acceptance.
