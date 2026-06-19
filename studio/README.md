# CoreVideo Studio Native Shell

This is the native desktop test shell for CoreVideo Pro. It is intentionally
separate from the earlier web prototype.

Current scope:

- Win32 native window.
- Launches `native/build/corevideo-native.exe`.
- Talks line-delimited JSON-RPC over stdio.
- Joins a Zoom meeting through the native core contract.
- Shows the current Zoom roster, active speaker, native-core health, output state, and recording artifact path.
- Builds a Magic Scene from the current roster and starts program recording/output through the C++ media core.
- Owner-draw program preview surface with frame metadata overlay when `program-frame-preview` events arrive.
- Dynamic scene/output panel (scene id, routes, overlays, destinations, encoder lifecycle).
- Status bar and health panel surface first-frame arrival and recording health (frames, duration, metadata).
- `run-studio.ps1 -UseDevNativeCore` (or `npm run run:studio:dev`) prefers `native/build-dev/corevideo-native.exe`.

Build the native media core and native Studio shell from the repository root on
Windows:

```powershell
.\scripts\build-studio.ps1
```

Run Studio from the repository root:

```powershell
.\scripts\run-studio.ps1
```

Or through npm:

```powershell
npm run build:studio
npm run run:studio
```

Use the real Zoom SDK dev build after staging the SDK and building dev adapters:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\stage-zoom-sdk.ps1
powershell -ExecutionPolicy Bypass -File scripts\build-native-dev.ps1
npm run run:studio:dev
```

The build script creates:

- `native/build/corevideo-native.exe`
- `studio/build-clean/Debug/CoreVideoStudio.exe`

`run-studio.ps1` performs an incremental build first, then launches
`studio/build-clean/Debug/CoreVideoStudio.exe` with the repository root as the
working directory so the shell can find `native/build/corevideo-native.exe`.

Smoke-test the working operator flow:

```powershell
npm run test:studio-workflow
```

That launches the real Win32 app, clicks Join Zoom, Magic Scene, Record Program,
Health, and Snapshot, then verifies the roster and live recording state.
