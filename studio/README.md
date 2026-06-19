# CoreVideo Studio Native Shell

This is the native desktop test shell for CoreVideo Pro. It is intentionally
separate from the deprecated Electron prototype.

Current scope:

- Win32 native window.
- Launches `native/build/corevideo-native.exe`.
- Talks line-delimited JSON-RPC over stdio.
- Shows native media-core handshake, health, snapshot, and command responses.
- Sends a basic scene graph and starts stub program output.

Build from the repository root on Windows:

```powershell
cmake -S studio -B studio/build
cmake --build studio/build
```

Run:

```powershell
studio/build/Debug/CoreVideoStudio.exe
```

This is a first native test harness, not the final production UI.

