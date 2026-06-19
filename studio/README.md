# CoreVideo Studio Native Shell

This is the native desktop test shell for CoreVideo Pro. It is intentionally
separate from the earlier web prototype.

Current scope:

- Win32 native window.
- Launches `native/build/corevideo-native.exe`.
- Talks line-delimited JSON-RPC over stdio.
- Shows native media-core handshake, health, snapshot, and command responses.
- Sends a basic scene graph and starts stub program output.

Build the native media core and native Studio shell from the repository root on
Windows:

```powershell
.\scripts\build-studio.ps1
```

Run Studio from the repository root:

```powershell
.\scripts\run-studio.ps1
```

The build script creates:

- `native/build/corevideo-native.exe`
- `studio/build-clean/Debug/CoreVideoStudio.exe`

`run-studio.ps1` performs an incremental build first, then launches
`studio/build-clean/Debug/CoreVideoStudio.exe` with the repository root as the
working directory so the shell can find `native/build/corevideo-native.exe`.

This is a first native test harness, not the final production UI.
