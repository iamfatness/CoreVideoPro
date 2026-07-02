# CoreVideo Pro Alpha Preflight

- Generated: 2026-07-02 17:21:38 -04:00
- Branch: claude/alpha-validation-evidence
- Commit: 452b639
- Overall: **passed**
- Report artifacts: C:\Users\walla\CoreVideoPro\artifacts\alpha-preflight\20260702-172118

## Automated Evidence

| Check | Status | Notes | Log |
|---|---:|---|---|
| npm audit | passed |  | C:\Users\walla\CoreVideoPro\artifacts\alpha-preflight\20260702-172118\logs\npm-audit.log |
| TypeScript typecheck | passed |  | C:\Users\walla\CoreVideoPro\artifacts\alpha-preflight\20260702-172118\logs\typescript-typecheck.log |
| MediaCore tests | passed |  | C:\Users\walla\CoreVideoPro\artifacts\alpha-preflight\20260702-172118\logs\mediacore-tests.log |
| WinUI view-model tests | passed |  | C:\Users\walla\CoreVideoPro\artifacts\alpha-preflight\20260702-172118\logs\winui-view-model-tests.log |
| Native media-core stub build and smoke | passed |  | C:\Users\walla\CoreVideoPro\artifacts\alpha-preflight\20260702-172118\logs\native-media-core-stub-build-and-smoke.log |
| WinUI build | passed |  | C:\Users\walla\CoreVideoPro\artifacts\alpha-preflight\20260702-172118\logs\winui-build.log |
| Dev native core artifact | passed | Found native/build-dev/corevideo-native.exe. |  |
| Zoom helper artifact | passed | Found native/build-dev/corevideo-zoom-engine.exe. |  |
| Staged Zoom runtime | passed | Found staged Zoom runtime at C:\Users\walla\CoreVideoPro\native-core\zoom-runtime\windows\x64. |  |
| Native shell dev readiness | passed |  | C:\Users\walla\CoreVideoPro\artifacts\alpha-preflight\20260702-172118\logs\native-shell-dev-readiness.log |
| Record stream harness | passed |  | C:\Users\walla\CoreVideoPro\artifacts\alpha-preflight\20260702-172118\logs\record-stream-harness.log |

## Manual Home Alpha Pass

Run these when you can interact with the app and a real Zoom meeting:

1. Launch: npm run run:studio or npm run launch:native.
2. Confirm Program/Preview show first-frame/live/degraded/error status instead of stale compositor waiting text.
3. Settings: select a recent meeting if available, join Zoom, then turn capture on.
4. Sources: assign a Zoom participant and a UVC webcam; confirm source dropdowns update.
5. Routing: route ISO A and ISO 1, switch to another source, then click again/remove to prove one-source isolation.
6. Media: import an asset, select it, play/pause it, and assign it as Brand Kit logo from Overlays.
7. Record: start a short MP4 recording and confirm output health plus file bytes on disk.
8. Leave/rejoin Zoom and confirm recent meeting memory, roster reset, and warnings remain readable.

## Decision

Offline alpha preflight is green. The remaining risk is live Zoom/device validation.
