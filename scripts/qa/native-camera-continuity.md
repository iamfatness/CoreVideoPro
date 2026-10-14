# Native OS-camera continuity reference

`native-camera-continuity.py` starts one owned headless core and the independent
Media Foundation camera receiver. It does not register or install a camera.
Supply a core at the path owned by the existing machine camera registration;
another development path can correctly fail with an ownership conflict.
The candidate DLL defaults to `corevideo-virtualcam.dll` beside the core; use
`--expected-camera-dll` when the installed payload keeps it elsewhere.

```powershell
python scripts/qa/native-camera-continuity.py `
  --core C:/path/to/installed/corevideo-native.exe `
  --receiver C:/path/to/corevideo-camera-receiver.exe `
  --source-commit EXACT_COMMIT `
  --output artifacts/camera-reference --duration 75
```

The workload is a synthetic counter-only 1920x1080 Program at 60/1, with the
approved two-frame buffer. CPU source preparation and monitor isolation are
explicitly enabled; GPU capture remains disabled. `COREVIDEO_DELIVERY_TRACE=1`
is explicitly enabled. Sparse stable
correlation sidecar observations retain Program identity and delivery/publication
timestamps beside snapshots; they are not a complete per-frame trace. Ambient COREVIDEO variables
are removed by the shared owned-process driver. No real meeting, streaming or
operator settings are used. Preserve and identify any temporary installed-file
transaction separately; this script never performs one.

Activation must succeed before measurement. A fixed two-second setup settle
allows the OS endpoint notification to follow camera Start, then the receiver
runs its full fixed 30-second warmup. There is no retry that discards a failed
capture. Duration must be 65..7200 seconds. Only processes created by this
harness are terminated during cleanup.

The output retains setup, native snapshots, stderr, raw per-sample pixel
identities, trial-appended `serve.log`, correlation observations and a JSON verdict.
The serve-log capture begins before activation at the existing file's byte
offset. It retains at most 8 MiB and records file identity, offsets, boundary and
capture SHA256s. Sampled truncation, replacement, changed preceding bytes,
partial records or excess appends invalidate serving evidence; old log history
cannot supply a matching module record.

After activation and again before teardown, a read-only `Win32_Service` query
identifies the actual `FrameServer` service PID and process creation identity.
Only that PID's trial module-path records qualify; the activating core's matching
DLL is insufficient. The observed module's on-disk SHA256 must equal the expected
installed DLL digest, the candidate must remain unchanged, and the service
instance/capture must remain stable. Missing, stale or unobserved serving identity
prevents an overall PASS even when `cameraPixels` passes. The pixel verdict is
retained independently. This does not measure a loaded memory-image digest;
module paths are self-reported by the serving DLL. No service, registry or
registration changes are made by the harness.

With `--monitors`, Program/Preview multiview is
enabled and the verdict also requires progressing display-copy observations with
no measured reuse/refusal/skip/reorder increments. These sparse observations do
not replace the independent multiview GPU pixel phase tests.

PASS requires receiver exit success, advancing
native delivery without buffer losses, verified serving identity, and the independent OS-camera judge:
verified 60/1 cadence, no invalid/torn/duplicate/missing/reordered identities,
at least 30 measured seconds and no unexplained arrival interval above 33.4 ms.
Activation, startup, timeout, malformed evidence or cleanup failures are INVALID;
accounted delivery failures are FAIL. Failed/invalid directories remain intact.

This reference does not qualify mixed-source load, physical display, source
content latency, decoded audio/video synchronization, a complete installed upgrade,
a loaded memory-image digest, or another machine. A PASS does
not authorize an isolation/preparation default change. Run the broader #517
qualification separately and retain all missing boundaries explicitly.

The receiver judge's negative controls are maintained in
`camera-pixel-receiver.test.mjs`; run them with `node --test`.
Serving-identity/log-capture controls, including a perfect-pixels/wrong-DLL harness
negative, run with `python scripts/qa/test_camera_serving_evidence.py` or
`node --test scripts/tests/camera-serving-evidence.test.mjs`.
