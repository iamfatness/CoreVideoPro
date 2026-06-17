# CoreVideo Pro — WinUI native shell

**Primary desktop shell** for Windows demos and packaging. Use the WinUI host for day-to-day operator work, `npm run pack:native`, and demo distribution. The WinUI app loads the same media-core JSON bridge as the legacy Electron supervisor, without bundling Chromium.

**Electron is legacy.** Keep `npm run launch` / `pack:desktop` only for Chromium-based fallback when WinUI or the Windows App Runtime is unavailable (older CI agents, cross-platform dev).

| Launch path | Role |
|-------------|------|
| `npm run launch:native` | **Default / primary demo path** — build/publish WinUI and run the native shell |
| `npm run launch` | **Legacy Electron fallback** — Chromium + desktop supervisor |
| `Launch CoreVideo Pro (Native).bat` | Double-click entry for packaged or dev native shell |
| `Launch CoreVideo Pro.bat` | Electron fallback launcher |

## Prerequisites

| Requirement | Notes |
|-------------|-------|
| **Windows 10/11 (x64)** | Target platform `10.0.19041.0`, min `10.0.17763.0` |
| **[Windows App Runtime 2.x](https://learn.microsoft.com/windows/apps/windows-app-sdk/downloads)** | Required for unpackaged runs (`WindowsAppSDKSelfContained=false`); project targets **2.2** |
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

CI gates (skip gracefully when toolchain or GUI session is unavailable):

```powershell
npm run test:native-media-core      # C++ stub build + stdio smoke
npm run test:native-shell-smoke     # WinUI Release publish + brief process launch
```

The CI stub gate stages `corevideo-native.exe` to:

- `native/build/`
- `native/build/Release/`

`scripts/build-native-dev.ps1` owns `native/build-dev/` (and stages copies to `native/build-dev/Release/`).

`MediaCorePaths.ResolveNativeCoreExecutable()` probes those locations (plus the app directory after `npm run pack:native`).

For a full dev-machine build with Zoom SDK, D3D11 compositor, and hardware adapters, use `scripts/build-native-dev.ps1` instead (`COREVIDEO_STUB=OFF`).

### D3D11 GPU compositor + shared-texture export (dev machine)

The WinUI program surface opens DXGI shared handles from the native D3D11 compositor when a real adapter build is available. The portable stub (`COREVIDEO_STUB=ON`) still emits synthetic `program-shared-texture` events for protocol validation.

From a **x64 Native Tools** or **Developer PowerShell for VS** prompt:

```powershell
# From repo root — real D3D11 compositor (no portable stub)
cmake -S native -B native/build-dev `
  -DCOREVIDEO_STUB=OFF `
  -DCOREVIDEO_ENABLE_DEV_ADAPTERS=ON `
  -DCOREVIDEO_WITH_D3D11=ON `
  -DBUILD_TESTING=ON
cmake --build native/build-dev --config Release --target corevideo-native corevideo-native-tests
ctest -C Release --test-dir native/build-dev --output-on-failure
```

When `cmake` or MSVC is unavailable, `npm run test:native-media-core` still runs the portable stub gate and skips gracefully.

After a successful D3D11 build, stage `corevideo-native.exe` under `native/build-dev/` (or run `scripts/build-native-dev.ps1`) so `MediaCorePaths.ResolveNativeCoreExecutable()` picks up the GPU compositor. The WinUI program tile shows **GPU · full-res** when shared-handle presentation succeeds and falls back to **CPU · BGRA preview** when interop is unavailable or a handle is invalid.

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

For repeatable installs on a dev PC you can run the optional signing stub:

```powershell
npm run pack:native:msix
powershell -ExecutionPolicy Bypass -File scripts/sign-native-msix.ps1
```

The script creates a self-signed dev cert when `SignTool` is available, or prints manual `New-SelfSignedCertificate` / `SignTool` steps when signing tools are missing. Trust the cert under **Local Computer → Trusted People**. Production distribution needs a real code-signing certificate and signed MSIX; the unsigned path is intentional for demo/CI.

**CI without signing:** if `dotnet publish` cannot emit an `.msix`, the script publishes an unpackaged layout, writes `AppxManifest.xml`, and optionally runs `makeappx.exe pack` when the Windows SDK is available. Use `install-msix-layout.ps1` to register the layout unsigned.

## Project layout

```
native-shell/
  CoreVideoPro.WinUI/   # WinUI 3 app (.NET 9, Windows App SDK 2.2)
  README.md
```

`Program.cs` bootstraps Windows App SDK **2.2** (`0x00020002`) before starting the WinUI loop. Publish uses `UseAppHost=false`; launch via `dotnet CoreVideoPro.WinUI.dll` (see `scripts/launch-native.ps1`).