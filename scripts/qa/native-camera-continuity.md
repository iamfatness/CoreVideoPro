# Native OS-camera continuity reference

`native-camera-continuity.py` starts one owned headless core and the independent
Media Foundation camera receiver. It does not register or install a camera.
Supply a core at the path owned by the existing machine camera registration;
another development path can correctly fail with an ownership conflict.

```powershell
python scripts/qa/native-camera-continuity.py `
  --core C:/path/to/installed/corevideo-native.exe `
  --receiver C:/path/to/corevideo-camera-receiver.exe `
  --source-commit EXACT_COMMIT `
  --output artifacts/camera-reference --duration 75
```

The workload is a synthetic counter-only 1920x1080 Program at 60/1, with the
approved two-frame buffer. CPU source preparation and monitor isolation are
explicitly enabled; GPU capture remains disabled. Ambient COREVIDEO variables
are removed by the shared owned-process driver. No real meeting, streaming or
operator settings are used. Preserve and identify any temporary installed-file
transaction separately; this script never performs one.

Activation must succeed before measurement. A fixed two-second setup settle
allows the OS endpoint notification to follow camera Start, then the receiver
runs its full fixed 30-second warmup. There is no retry that discards a failed
capture. Duration must be 65..7200 seconds. Only processes created by this
harness are terminated during cleanup.

The output retains setup, native snapshots, stderr, raw per-sample pixel
identities and a JSON verdict. PASS requires receiver exit success, advancing
native delivery without buffer losses, and the independent OS-camera judge:
verified 60/1 cadence, no invalid/torn/duplicate/missing/reordered identities,
at least 30 measured seconds and no unexplained arrival interval above 33.4 ms.
Activation, startup, timeout, malformed evidence or cleanup failures are INVALID;
accounted delivery failures are FAIL. Failed/invalid directories remain intact.

This reference does not qualify mixed-source load, physical display, source
content latency, decoded audio/video synchronization, a packaged candidate,
the camera DLL actually loaded by the service, or another machine. A PASS does
not authorize an isolation/preparation default change. Run the broader #517
qualification separately and retain all missing boundaries explicitly.

The receiver judge's negative controls are maintained in
`camera-pixel-receiver.test.mjs`; run them with `node --test`.
