# CoreVideo Pro — WinUI native shell

Lightweight Windows demo shell that hosts the same media-core JSON bridge as the Electron desktop app, without bundling Chromium.

## Prerequisites

| Requirement | Notes |
|-------------|-------|
| **Windows 10/11 (x64)** | Target platform `10.0.19041.0`, min `10.0.17763.0` |
| **[Windows App Runtime 1.6](https://learn.microsoft.com/windows/apps/windows-app-sdk/downloads)** | Required for unpackaged runs (`WindowsAppSDKSelfContained=false`) |
| **[.NET 9 SDK](https://dotnet.microsoft.com/download/dotnet/9.0)** | Build and framework-dependent publish |
| **Node.js** (optional) | Only when no `corevideo-native.exe` is staged; runs `desktop/coreStub.cjs` |
| **Visual Studio 2022** or **Build Tools** | WinUI / Windows SDK build chain (via `Microsoft.Windows.SDK.BuildTools`) |
| **CMake 3.20+** (optional) | Portable C++ media-core stub (`COREVIDEO_STUB=ON`) for real `corevideo-native.exe` |

## Build the C++ media core (stub)

The WinUI shell prefers a real `corevideo-native.exe` when it can find one under `native/build*`; otherwise it falls back to the Node stub.

From a **x64 Native Tools** or **Developer PowerShell for VS** prompt:

```powershell
# From repo root
cmake -S native -B native/build -DCOREVIDEO_STUB=ON -DCOREVIDEO_ENABLE_DEV_ADAPTERS=OFF -DBUILD_TESTING=ON
cmake --build native/build --config Release --target corevideo-native corevideo-native-tests
ctest -C Release --test-dir native/build --output-on-failure
```

CI gate (skips gracefully when `cmake` or MSVC is unavailable):

```powershell
npm run test:native-media-core
```

The script stages `corevideo-native.exe` to:

- `native/build/`
- `native/build-dev/`
- `native/build-dev/Release/`

`MediaCorePaths.ResolveNativeCoreExecutable()` probes those locations (plus the app directory after `npm run pack:native`).

For a full dev-machine build with Zoom SDK, D3D11 compositor, and hardware adapters, use `scripts/build-native-dev.ps1` instead (`COREVIDEO_STUB=OFF`).

## Quick start (dev)

```powershell
# From repo root
npm run launch:native
# or double-click: Launch CoreVideo Pro (Native).bat
```

## Package for demos

### Unpackaged folder (portable)

```powershell
npm run pack:native
```

Output: `artifacts/native/win-unpacked/CoreVideo Pro.exe` plus runtime DLLs and optional `corevideo-native.exe`.

Double-click **Launch CoreVideo Pro (Native).bat** to run the packaged build when present; otherwise it builds and launches from source.

### MSIX (single-project, dual mode)

The WinUI project stays **unpackaged** for day-to-day dev (`WindowsPackageType=None`). MSIX is produced only when you run the MSIX pack script, which overrides `WindowsPackageType=MSIX` at publish time.

```powershell
npm run pack:native:msix
```

Output:

| Artifact | Purpose |
|----------|---------|
| `artifacts/native/CoreVideoPro.msix` | Unsigned demo package (primary) |
| `artifacts/native/install-msix.ps1` | Local install helper |
| `artifacts/native/msix-layout/` | Fallback folder layout if MSIX tooling/signing is blocked in CI |
| `artifacts/native/install-msix-layout.ps1` | Register layout with `Add-AppxPackage -AllowUnsigned` |

The pack script stages `corevideo-native.exe` (and sibling DLLs when present) into the MSIX payload, or falls back to `desktop/coreStub.cjs` like `pack:native`.

**Signing (local dev):** packages are built with `AppxPackageSigningEnabled=false` — no Authenticode cert required. Install on your machine with:

```powershell
Add-AppxPackage -Path artifacts/native/CoreVideoPro.msix -AllowUnsigned
# or
powershell -ExecutionPolicy Bypass -File artifacts/native/install-msix.ps1
```

For repeatable installs on a dev PC you can create a self-signed cert (`New-SelfSignedCertificate -Type Custom -Subject "CN=CoreVideo Pro Dev" ...`), sign the `.msix` with `SignTool`, and trust the cert under **Local Computer → Trusted People**. Production distribution needs a real code-signing certificate and signed MSIX; the unsigned path is intentional for demo/CI.

**CI without signing:** if `dotnet publish` cannot emit an `.msix`, the script publishes an unpackaged layout, writes `AppxManifest.xml`, and optionally runs `makeappx.exe pack` when the Windows SDK is available. Use `install-msix-layout.ps1` to register the layout unsigned.

## Project layout

```
native-shell/
  CoreVideoPro.WinUI/   # WinUI 3 app (.NET 9, Windows App SDK 1.6)
  README.md
```

Bootstrap version in `Program.cs` (`0x00010006`) matches Windows App SDK **1.6** for unpackaged launch.