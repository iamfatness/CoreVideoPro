# CoreVideo Pro - WinUI Native Shell

**Primary desktop shell** for Windows demos and packaging. Use the WinUI host for day-to-day operator work, `npm run pack:native`, and demo distribution. The WinUI app talks directly to the native C++ media-core JSON bridge.

**Electron has been removed.** Do not use `npm run launch`, `dev:desktop`, or `pack:desktop`; the native shell and `studio/` C++ app are the active product paths.

| Launch path | Role |
|-------------|------|
| `npm run launch:native` | Default WinUI demo path |
| `npm run run:studio` | Native C++ Studio shell |
| `Launch CoreVideo Pro (Native).bat` | Double-click entry for packaged or dev native shell |

## Prerequisites

| Requirement | Notes |
|-------------|-------|
| Windows 10/11 x64 | Target platform `10.0.19041.0`, min `10.0.17763.0` |
| Windows App Runtime 2.x | Required for unpackaged WinUI runs |
| .NET 9 SDK | Build and framework-dependent publish |
| Node.js | Repository scripts and TypeScript tests |
| Visual Studio 2022 or Build Tools | WinUI / Windows SDK build chain |
| CMake 3.20+ | Portable C++ media-core stub (`COREVIDEO_STUB=ON`) |

## Build the C++ media core

The WinUI shell requires `corevideo-native.exe`. Build it under `native/build*` before launching the app.

For operator testing on Windows, use the Studio build script so local audio capture and monitor output are real WASAPI adapters instead of stub-only PCM:

```powershell
.\scripts\build-studio.ps1 -Config Release
```

The portable stub build stays useful for CI and protocol tests:

```powershell
cmake -S native -B native/build -DCOREVIDEO_STUB=ON -DCOREVIDEO_ENABLE_DEV_ADAPTERS=OFF -DBUILD_TESTING=ON
cmake --build native/build --config Release --target corevideo-native corevideo-native-tests
ctest -C Release --test-dir native/build --output-on-failure
```

CI gates:

```powershell
npm run test:native-media-core
npm run test:native-shell-smoke
npm run test:native-shell-dev-readiness
```

For a full dev-machine build with Zoom SDK, D3D11 compositor, RTMP, and hardware adapters, use `scripts/build-native-dev.ps1` instead (`COREVIDEO_STUB=OFF`).

## Quick Start

```powershell
npm run launch:native
npm run run:studio
```

## Package for Demos

```powershell
npm run pack:native
npm run pack:native:msix
```

The package scripts stage `corevideo-native.exe` and sibling DLLs when present. Packaging fails if the native core is missing.

## Project Layout

```text
native-shell/
  CoreVideoPro.WinUI/   # WinUI 3 app (.NET 9, Windows App SDK 2.2)
  README.md
```

`Program.cs` bootstraps Windows App SDK 2.2 before starting the WinUI loop. Launch via `CoreVideoPro.WinUI.exe`, `npm run launch:native`, or `scripts/launch-native.ps1`.
