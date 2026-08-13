# Operator validation runbook - WinUI + PKCE + native media proof

Use this checklist on a **Windows 10/11 x64 dev machine** before demoing CoreVideo Pro with real Zoom media. Automated gates (`npm run test:native-shell`, CI `native-shell-windows`) cover protocol and build health; this runbook proves operator-facing flows and the native media capabilities that are currently available.

Current native proof surface:

- Deterministic compositor signatures and `program-frame-preview` BGRA payloads are available in the portable stub and dev-adapter builds.
- Native audio DSP metrics are available through `audioMixSession` (participant gain, noise suppression, limiter, master level, loudness, mixed-frame count).
- Dev builds include D3D11 compositor, Media Foundation MP4, and RTMP send-proof adapters when `build-native-dev` succeeds.
- RTMP reports runtime diagnostics even when FFmpeg/libavformat is missing.
- FFmpeg runtime packaging is optional. Missing FFmpeg should not block recording, preview, shell, or Zoom validation; it only limits live RTMP runtime proof.

## 1. Prerequisites

| Item | Verify |
|------|--------|
| Windows App Runtime **2.x** (2.2+) | Installed from [Windows App SDK downloads](https://learn.microsoft.com/windows/apps/windows-app-sdk/downloads) |
| .NET 9 SDK | `dotnet --version` |
| Node.js 22+ | `node --version` |
| Visual Studio 2022+ or 2026+ (C++ workload) | `scripts/test-native.ps1` finds `VsDevCmd.bat` |
| CMake | `cmake --version` |
| Zoom Meeting SDK **7.0.5** x64 | Archive with `x64/h/zoom_sdk.h` and `x64/lib/sdk.lib` |
| FFmpeg runtime for RTMP (optional) | `ffmpeg.exe` on `PATH`, or `COREVIDEO_FFMPEG_BIN_DIR` points to a `bin` folder with `avformat*.dll` |

## 2. Stage Zoom SDK

```powershell
cd C:\path\to\CoreVideoPro
# Repo-local SDK (auto-discovered when present):
#   ZoomSDK\zoom-sdk-windows-7.0.5.39292\x64
.\scripts\stage-zoom-sdk.ps1
# Or override: $env:ZOOM_SDK_DIR = "C:\path\to\zoom-sdk-windows-7.0.5.39292\x64"
```

Staged target: `native-core/zoom-runtime/windows/x64`. Settings -> Zoom readiness should show SDK runtime checks passing after a dev build.

## 3. Build native media core (dev adapters)

From **x64 Native Tools** or **Developer PowerShell for VS**:

```powershell
npm run build:native-dev
# or: .\scripts\build-native-dev.ps1 -ZoomSdkDir $env:ZOOM_SDK_DIR
```

Expect artifacts under `native/build-dev/`:

- `corevideo-native.exe`
- `corevideo-zoom-engine.exe`
- `corevideo-native-tests.exe`

The dev build enables Zoom, D3D11 compositor, Media Foundation MP4, and RTMP send-proof adapters. DeckLink/AJA probes remain opt-in script switches.

## 4. Automated gates (no GUI)

```powershell
npm run test:native-media-core   # portable C++ stub build + unit tests + stdio preview smoke
npm run test:native-shell        # MediaCore unit tests + bridge smoke
npm run test:native-shell-smoke  # WinUI publish + brief launch smoke
```

Expected evidence:

- `test:native-media-core` reports `program-frame-preview smoke ok` for both `corevideo-native.exe` and `corevideo-native.exe`.
- Native unit tests cover deterministic render-plan IDs, program preview events, shared texture handle shape, audio DSP session metrics, encoder metadata, RTMP send-proof artifacts, and dev-adapter gates.
- Bridge smoke should report `Using packaged native core: ...\native\build-dev\corevideo-native.exe` when the dev build is present.
- Native shell smoke should either launch briefly or skip with an explicit Windows App Runtime / GUI-session reason.

## 5. Validate deterministic compositor previews

The compositor proof is not just "a window showed video." The operator/dev evidence is the native snapshot and event stream:

- `programFrame.renderPlanId` / `renderPlan.renderPlanId` is stable for the same scene graph.
- `programFrame.programPixelSignature` is non-zero in the native frame.
- `programFramePreview.pixelFormat` is `bgra`.
- `programFramePreview.bgraBase64` is present and downscaled to the configured preview max size.
- `program-frame-preview` unsolicited events are emitted after `media-core-sync`.
- D3D11 dev builds may also emit `programSharedTexture` / `program-shared-texture` with a `B8G8R8A8_UNORM` handle shape.

Fast proof:

```powershell
npm run test:native-media-core
```

End-to-end proof:

```powershell
npm run validate:record-stream -- --destinations recording --timeout-ms 30000
```

The validation report should include `programPreviewSeen: true`, a `sceneId`, active outputs, encoder metadata, and at least the configured minimum program frames.

## 6. Validate native audio DSP foundation

Native audio DSP is present as a deterministic media-core foundation. It is not yet a paid-show proof of live Zoom raw-audio quality, but it does produce native `audioMixSession` telemetry from audio frames and audio mix commands.

Evidence to look for in snapshots or mapped WinUI diagnostics:

- `audioMixSession.status`: `live` or `warning`
- `audioMixSession.summary`: contains native DSP wording or explicit gain/limiter activity
- `audioMixSession.masterLevel` greater than zero when audio frames are mixed
- `audioMixSession.loudnessLufs`
- `audioMixSession.limiterActive`
- `audioMixSession.mixedFrameCount` greater than zero after output starts
- per-participant metrics: `inputLevel`, `outputLevel`, `gainDb`, `noiseSuppression`, `limiterActive`, `muted`

Fast proof:

```powershell
npm run test:native-media-core
```

Operator proof:

1. Launch WinUI.
2. Open the media-core diagnostics / output area.
3. Start program output or a record/stream validation.
4. Confirm the Audio mix, Mix level, and Mix loudness readouts update from `audioMixSession`.

## 7. Launch WinUI operator console

```powershell
npm run launch:native
# or double-click: Launch CoreVideo Pro (Native).bat
```

If the app fails with a side-by-side / configuration error, install **Windows App Runtime 2.x** (match the NuGet pin in `CoreVideoPro.WinUI.csproj`) and retry.

## 8. PKCE sign-in (Settings)

Embedded OAuth identity (same broker as [CoreVideo plugin](https://github.com/iamfatness/CoreVideo)):

| Field | Value |
|-------|-------|
| Broker start | `https://corevideo.iamfatness.us/oauth/start` |
| Zoom redirect (broker) | `https://corevideo.iamfatness.us/oauth/callback` |
| App return URI | `corevideo://oauth/callback` (broker allowlists exactly this; `corevideopro://` is a legacy alias the app still accepts) |
| Public Client ID | `y6sIWSwiTZe1JygMx4C9EQ` |

Operator steps:

1. Open **Settings** -> **Zoom account (Public Client OAuth + PKCE)**.
2. Confirm readiness shows **OAuth PKCE broker** embedded (not blocked).
3. Click **Sign in with Zoom**; browser opens the broker start URL.
4. Complete Zoom consent; browser redirects to `corevideo://oauth/callback`.
5. WinUI should show **Signed in** and mint broker JWT + ZAK on join.

Troubleshooting:

- Deep link does not return to app: confirm `corevideo` protocol is registered (WinUI manifest / packaged install; unpackaged runs self-register HKCU on launch).
- `Zoom rejected the OAuth client`: Marketplace app must be **Public Client OAuth (PKCE)**.
- Override broker URL only when testing staging: `$env:COREVIDEO_ZOOM_OAUTH_BROKER_START_URL`.

## 9. Join a real meeting

1. Ensure sign-in from step 8 (required for external-account meetings).
2. Enter meeting URL (and passcode if needed) in the join UI.
3. After join, verify:
   - **Participants** roster populates (not empty / not stale stub).
   - **Program** tile shows video (GPU/full-res if D3D11 interop works, else CPU/BGRA preview).
   - **Breakout room** name updates when you move rooms.
   - Diagnostics show live Zoom capabilities when the dev SDK path is active (`zoom-raw-video`, `zoom-raw-audio`).
4. **Leave** meeting; roster clears, meeting state returns to idle.

## 10. Headless live Zoom harness (optional)

For stdio-level proof without WinUI GUI:

```powershell
$env:COREVIDEO_TEST_MEETING_URL = "https://zoom.us/j/XXXXXXXXXX"
# Optional: $env:COREVIDEO_ZOOM_MEETING_PASSCODE = "..."
npm run validate:live-zoom
```

Pass criteria (defaults): at least 3 participants, at least 3 video feeds, first frame <= 1000 ms. Requires `build-native-dev` output and staged Zoom runtime.

## 11. Headless record/stream harness (optional)

For stdio-level proof of recording + RTMP output without WinUI GUI:

```powershell
npm run validate:record-stream
# Optional: node ./scripts/validate-record-stream.mjs --timeout-ms 30000 --destinations recording,rtmp
```

The harness launches `native/build-dev/corevideo-native.exe`, arms `load-scene-graph` + `start-program-output` + `start-recording-session`, then polls snapshot/`get-output-health` until evidence is present.

Pass criteria (defaults): recording bytes >= 1, program frames >= 3, and RTMP proof via artifact or acceptable warning/live status. Requires `build-native-dev` output.

Recording evidence:

- MP4 artifact path on disk, or `recordingBytesWritten` / `totalBytesWritten` > 0.
- Encoder metadata reports `media-foundation`, `h264`, and hardware acceleration when the dev adapter is active.

RTMP evidence:

- `rtmp.sendArtifactPath` points to JSONL send-proof output, or status is `live`.
- `rtmp.runtimeDetail` / `sendProofRuntimeDetail` reports availability such as `available:avformat-61.dll`, or a missing DLL list.
- `warning` is acceptable when the RTMP adapter is present but libavformat is unavailable and `--disallow-rtmp-warning` was not set.

FFmpeg missing-runtime warning:

```text
RTMP runtime is missing. Install FFmpeg or set COREVIDEO_FFMPEG_BIN_DIR to a bin folder containing avformat*.dll before packaging or validation.
```

This warning means RTMP runtime proof is limited. It does not invalidate compositor preview, recording, native shell, Zoom join, or audio DSP validation.

## 11b. ISO recording (Program only vs Program + ISOs, Demo E)

ISO records a self-contained per-source MP4 (own video + own audio) for each selected
source, time-aligned to the program — the podcast/interview deliverable (Demo E: open
Program + ≥2 ISOs in an NLE/DAW; they sync within a few frames).

Operator flow:

1. **Program only vs Program + ISOs.** Open the Record caret (recording-output flyout) →
   the **"Program + ISOs"** switch. Default is **Program only** (records `Program.mp4`
   only — byte-identical to a no-ISO recording). Turn it on to arm ISO writers.
2. **Pick per-source ISOs.** In **Sources → Inputs**, tick the **ISO** checkbox on each
   eligible row (Zoom guests + capture devices; media assets are not ISO-eligible). A
   pure camera → video-only ISO; a camera with a paired microphone → muxed A+V.
3. **Arm.** Press Record. A **disk pre-flight** runs first: genuinely-insufficient space
   BLOCKS the start with a message; low space shows a persistent warning but lets you
   proceed; ample space arms silently. The flyout shows **"Program + N ISOs"** and any
   per-stream ISO warning.
4. **Where files land (spec §5 folder scheme).** Each session writes one subfolder under
   the recording target folder (default `%USERPROFILE%\Videos\CoreVideo Pro`):

   ```text
   <RecordingFolder>/<prefix>-<yyyymmdd-hhmmss>/
     Program.mp4
     ISO-01-<SafeName>.mp4     # roster/display name, sanitized; 01.. in selection order
     ISO-02-<SafeName>.mp4
     manifest.json             # session id, epoch, program + ISO entries
   ```

5. **Diagnose.** Diagnostics → Export support bundle lists every ISO path + encode health
   (frames, audio samples, per-stream warning) so a failed ISO is visible from the bundle.

The selection + switch persist across launches (ProductionOutputPreferences v8). Headless
Demo E proof (Zoom): `node scripts/validate-iso-record.mjs`.

## 12. Package for demo

```powershell
npm run pack:native
```

Output: `artifacts/native/win-unpacked/CoreVideo Pro.exe`. Pack prefers `native/build-dev/` over CI stub. Confirm `corevideo-native.exe` is beside the app (not stub-only).

FFmpeg runtime packaging is best-effort:

- `npm run pack:native` and `npm run pack:native:msix` run `scripts/sync-ffmpeg-runtime-to-app.ps1`.
- If FFmpeg is found through `COREVIDEO_FFMPEG_BIN_DIR`, `FFMPEG_BIN_DIR`, `ffmpeg.exe` on `PATH`, or WinGet package discovery, the package should include `corevideo-ffmpeg-runtime.json` plus `avformat*.dll`, `avcodec*.dll`, and related DLLs.
- If FFmpeg is not found, packaging should continue with a warning. RTMP will report missing runtime diagnostics at validation/runtime.

For MSIX:

```powershell
npm run pack:native:msix
Add-AppxPackage -Path artifacts/native/CoreVideoPro.msix -AllowUnsigned
```

## 13. Remaining dev-machine-only limitations

These are still not general release guarantees:

- Real Zoom SDK capture requires a Windows dev machine, staged Zoom runtime, valid PKCE/ZAK flow, and a meeting/account that permits raw media.
- D3D11 compositor, Media Foundation recording, RTMP sender, DeckLink, and AJA adapters are gated behind `COREVIDEO_ENABLE_DEV_ADAPTERS` and local runtime/hardware availability.
- RTMP send-proof can validate adapter wiring without FFmpeg; real live RTMP requires libavformat runtime DLLs and a valid ingest target.
- Native audio DSP telemetry is implemented and gated by tests, but live Zoom raw-audio quality still needs show-condition validation.
- CI/stub gates prove deterministic protocol, preview, and telemetry behavior; they do not prove GPU driver, encoder, Zoom SDK, FFmpeg, or capture-card behavior.

## 14. Sign-off checklist

- [ ] `npm run test:native-media-core` green locally and reports preview smoke OK
- [ ] `npm run test:native-shell` green locally
- [ ] `npm run test:native-shell-smoke` green locally or explicitly skipped for missing GUI/App Runtime
- [ ] `npm run build:native-dev` produced `native/build-dev/corevideo-native.exe`
- [ ] Deterministic program preview evidence present (`programPreviewSeen`, `programFramePreview`, stable `renderPlanId`)
- [ ] Native audio DSP evidence present (`audioMixSession`, `mixedFrameCount`, master/loudness/limiter metrics)
- [ ] WinUI launches (App Runtime 2.x)
- [ ] PKCE sign-in completes via `corevideo://oauth/callback`
- [ ] Live join shows roster + program preview
- [ ] `pack:native` bundles `corevideo-native.exe` from `build-dev`
- [ ] FFmpeg packaging state is known: runtime manifest/DLLs present, or missing-runtime warning accepted
- [ ] (Optional) `validate:live-zoom` report `status: "passed"`
- [ ] (Optional) `validate:record-stream` report `status: "passed"` or RTMP warning explicitly accepted
