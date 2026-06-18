# Operator validation runbook — WinUI + PKCE + live Zoom

Use this checklist on a **Windows 10/11 x64 dev machine** before demoing CoreVideo Pro with real Zoom media. Automated gates (`npm run test:native-shell`, CI `native-shell-windows`) cover protocol and build health; this runbook proves operator-facing flows.

## 1. Prerequisites

| Item | Verify |
|------|--------|
| Windows App Runtime **2.x** (2.2+) | Installed from [Windows App SDK downloads](https://learn.microsoft.com/windows/apps/windows-app-sdk/downloads) |
| .NET 9 SDK | `dotnet --version` |
| Node.js 22+ | `node --version` |
| Visual Studio 2022+ (C++ workload) | `scripts/test-native.ps1` finds `VsDevCmd.bat` |
| Zoom Meeting SDK **7.0.5** x64 | Archive with `x64/h/zoom_sdk.h` and `x64/lib/sdk.lib` |

## 2. Stage Zoom SDK

```powershell
cd C:\path\to\CoreVideoPro
# Repo-local SDK (auto-discovered when present):
#   ZoomSDK\zoom-sdk-windows-7.0.5.39292\x64
.\scripts\stage-zoom-sdk.ps1
# Or override: $env:ZOOM_SDK_DIR = "C:\path\to\zoom-sdk-windows-7.0.5.39292\x64"
```

Staged target: `native-core/zoom-runtime/windows/x64`. Settings → Zoom readiness should show SDK runtime checks passing after a dev build.

## 3. Build native media core (dev adapters + D3D11)

From **x64 Native Tools** or **Developer PowerShell for VS**:

```powershell
npm run build:native-dev
# or: .\scripts\build-native-dev.ps1 -ZoomSdkDir $env:ZOOM_SDK_DIR
```

Expect artifacts under `native/build-dev/`:

- `corevideo-native.exe`
- `corevideo-zoom-engine.exe`

## 4. Automated gates (no GUI)

```powershell
npm run test:native-media-core   # C++ stub build + stdio smoke
npm run test:native-shell        # 75 MediaCore unit tests + bridge smoke
npm run test:native-shell-smoke  # WinUI publish + brief launch smoke
```

Bridge smoke should report `Using packaged native core: ...\native\build-dev\corevideo-native.exe` when the dev build is present.
Native shell smoke should either launch briefly or skip with an explicit Windows App Runtime / GUI-session reason.

## 5. Launch WinUI operator console

```powershell
npm run launch:native
# or double-click: Launch CoreVideo Pro (Native).bat
```

If the app fails with a side-by-side / configuration error, install **Windows App Runtime 2.x** (match the NuGet pin in `CoreVideoPro.WinUI.csproj`) and retry.

## 6. PKCE sign-in (Settings)

Embedded OAuth identity (same broker as [CoreVideo plugin](https://github.com/iamfatness/CoreVideo)):

| Field | Value |
|-------|-------|
| Broker start | `https://corevideo.iamfatness.us/oauth/start` |
| Zoom redirect (broker) | `https://corevideo.iamfatness.us/oauth/callback` |
| App return URI | `corevideopro://oauth/callback` |
| Public Client ID | `y6sIWSwiTZe1JygMx4C9EQ` |

**Operator steps:**

1. Open **Settings** → **Zoom account (Public Client OAuth + PKCE)**.
2. Confirm readiness shows **OAuth PKCE broker** embedded (not blocked).
3. Click **Sign in with Zoom** — browser opens the broker start URL.
4. Complete Zoom consent; browser redirects to `corevideopro://oauth/callback`.
5. WinUI should show **Signed in** and mint broker JWT + ZAK on join.

**Troubleshooting**

- Deep link does not return to app: confirm `corevideopro` protocol is registered (WinUI manifest / packaged install).
- `Zoom rejected the OAuth client`: Marketplace app must be **Public Client OAuth (PKCE)**.
- Override broker URL only when testing staging: `$env:COREVIDEO_ZOOM_OAUTH_BROKER_START_URL`.

## 7. Join a real meeting

1. Ensure sign-in from step 6 (required for external-account meetings).
2. Enter meeting URL (and passcode if needed) in the join UI.
3. After join, verify:
   - **Participants** roster populates (not empty / not stale stub).
   - **Program** tile shows video (GPU · full-res if D3D11 interop works, else CPU · BGRA preview).
   - **Breakout room** name updates when you move rooms.
4. **Leave** meeting — roster clears, meeting state returns to idle.

## 8. Headless live Zoom harness (optional)

For stdio-level proof without WinUI GUI:

```powershell
$env:COREVIDEO_TEST_MEETING_URL = "https://zoom.us/j/XXXXXXXXXX"
# Optional: $env:COREVIDEO_ZOOM_MEETING_PASSCODE = "..."
npm run validate:live-zoom
```

Pass criteria (defaults): ≥3 participants, ≥3 video feeds, first frame ≤1000 ms. Requires `build-native-dev` output and staged Zoom runtime.

## 8b. Headless record/stream harness (optional)

For stdio-level proof of recording + RTMP output without WinUI GUI:

```powershell
npm run validate:record-stream
# Optional: node ./scripts/validate-record-stream.mjs --timeout-ms 30000 --destinations recording,rtmp
```

The harness launches `native/build-dev/corevideo-native.exe`, arms `load-scene-graph` + `start-program-output` + `start-recording-session`, then polls snapshot/`get-output-health` until:

- **Recording:** MP4 artifact path on disk, or `recordingBytesWritten` / `totalBytesWritten` > 0 (Media Foundation dev adapter or stub counters).
- **RTMP:** send-proof JSONL artifact, or `warning` status from the dev adapter when RTMP runtime is unavailable, or `live` with frames sent.

Pass criteria (defaults): recording bytes ≥1, program frames ≥3, RTMP proof via artifact or acceptable warning/live status. Requires `build-native-dev` output (D3D11 + MF encoder + RTMP send-proof adapters).

## 9. Package for demo

```powershell
npm run pack:native
```

Output: `artifacts/native/win-unpacked/CoreVideo Pro.exe`. Pack prefers `native/build-dev/` over CI stub. Confirm `corevideo-native.exe` is beside the app (not stub-only).

For MSIX: `npm run pack:native:msix` then `Add-AppxPackage -Path artifacts/native/CoreVideoPro.msix -AllowUnsigned`.

## 10. Sign-off checklist

- [ ] `test:native-shell` green locally
- [ ] `test:native-shell-smoke` green locally or explicitly skipped for missing GUI/App Runtime
- [ ] WinUI launches (App Runtime 2.x)
- [ ] PKCE sign-in completes via `corevideopro://oauth/callback`
- [ ] Live join shows roster + program preview
- [ ] `pack:native` bundles `corevideo-native.exe` from `build-dev`
- [ ] (Optional) `validate:live-zoom` report `status: "passed"`
- [ ] (Optional) `validate:record-stream` report `status: "passed"`
