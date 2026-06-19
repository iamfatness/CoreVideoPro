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
