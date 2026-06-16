# Zoom Windows SDK Packaging

CoreVideo Pro's Windows helper must package the Zoom Meeting SDK beside the native Zoom media engine before real meeting join and raw media capture can work.

Verified local SDK archive:

- Source: `C:\Users\walla\Downloads\zoom-sdk-windows-7.0.5.39292.zip`
- Version: `7.0.5.39292`
- Target architecture: `x64`
- Runtime source: `zoom-sdk-windows-7.0.5.39292/x64/bin/*`
- Headers: `zoom-sdk-windows-7.0.5.39292/x64/h/**/*`
- Import library: `zoom-sdk-windows-7.0.5.39292/x64/lib/sdk.lib`
- Helper runtime target: `native-core/zoom-runtime/windows/x64`

Required files for the first real media-path spike:

- `x64/bin/sdk.dll`
- `x64/lib/sdk.lib`
- `x64/h/zoom_sdk.h`
- `x64/h/meeting_service_interface.h`
- `x64/h/rawdata/zoom_rawdata_api.h`
- `x64/h/rawdata/rawdata_renderer_interface.h`
- `x64/h/rawdata/rawdata_audio_helper_interface.h`

Important implementation constraints:

- Keep Zoom SDK binaries out of source control unless licensing explicitly allows redistribution in the final installer artifact.
- The helper process must run with a Windows message loop, or SDK auth/join/raw-media callbacks will not fire.
- Raw participant video and screen share require the raw-data headers and runtime DLLs above.
- Lower delivered resolution must be surfaced as a warning, not treated as success.
- Leave/rejoin must clear roster and raw subscription state before resubscribing.

The app-side verifier lives in `src/engine/zoomWindowsSdkPackage.ts` and should be used by packaging/preflight code before enabling a real Windows Zoom media helper.

## Staging for WinUI native shell

Discovery env vars (checked in order for runtime resolution):

- `COREVIDEO_ZOOM_RUNTIME_DIR` — staged runtime folder override
- `ZOOM_SDK_DIR` — source SDK x64 folder (or package root containing `x64/`)
- `COREVIDEO_ZOOM_OAUTH_BROKER_START_URL` — optional override for embedded PKCE broker (`src/config/zoomOAuth.json`)

Embedded OAuth identity (same broker as [CoreVideo plugin](https://github.com/iamfatness/CoreVideo)):

| Field | Value |
|-------|-------|
| Broker start | `https://corevideo.iamfatness.us/oauth/start` |
| Zoom redirect (broker) | `https://corevideo.iamfatness.us/oauth/callback` |
| App return URI | `corevideopro://oauth/callback` |
| Public Client ID | `y6sIWSwiTZe1JygMx4C9EQ` |

Stage the SDK beside the helper target:

```powershell
$env:ZOOM_SDK_DIR = "C:\path\to\zoom-sdk-windows-7.0.5.39292\x64"
.\scripts\stage-zoom-sdk.ps1
```

Default runtime target: `native-core/zoom-runtime/windows/x64`
