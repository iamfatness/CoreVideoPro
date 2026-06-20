# Vendored Zoom capture engine

This is the proven standalone Zoom Meeting SDK capture engine vendored from prior
CoreVideo work. It is a standalone child process built here as
`corevideo-zoom-engine` that:

- initializes the Zoom Meeting SDK and authenticates (JWT or public app key),
- joins a meeting,
- subscribes to **per-participant raw I420 video** (and audio / screen share),
- writes frames to shared memory via a lock-free double-buffer
  (`shared/engine-ipc.h` `ShmFrameHeader`: `sequence/width/height/y_len`,
  followed by Y + U + V planes),
- speaks a line-delimited JSON IPC protocol (`init` / `join` / `subscribe` /
  `leave` …) over a pipe/socket.

## Provenance

Copied from `iamfatness/corevideo` with only include-path rewrites from
`../../src/engine-ipc.h` to `engine-ipc.h` so the flattened `shared/` header can
be used. The engine has no external switcher UI dependencies, so no other edits
were needed.

Source files:
- `engine/` from the prior capture engine (`main`, `engine-video`,
  `engine-audio`, `engine-share`, `engine-writer.h`)
- `shared/engine-ipc.h` from the prior capture IPC header

## Building (dev machine only)

Requires the Zoom Meeting SDK. Excluded from the default in-container stub build.

```
cmake -S native -B native/build \
  -DCOREVIDEO_ENABLE_DEV_ADAPTERS=ON \
  -DCOREVIDEO_BUILD_ZOOM_ENGINE=ON \
  -DZOOM_SDK_DIR=/path/to/zoom-sdk
cmake --build native/build --target corevideo-zoom-engine
```

`COREVIDEO_BUILD_ZOOM_ENGINE` is intentionally separate from the other native
dev-adapter gates. Media Foundation, RTMP, D3D11, DeckLink, and AJA builds must
still configure without a Zoom SDK unless they explicitly enable either this
helper target or `COREVIDEO_WITH_ZOOM`.

## Dev runtime wiring

The native Studio and WinUI shells use the C++ media core. To exercise the native media
core and have that core supervise the vendored Zoom engine, set both layers:

```
COREVIDEO_MEDIA_CORE_COMMAND=...\native\build\corevideo-native.exe
COREVIDEO_ZOOM_ENGINE_PATH=...\native\build-zoom-engine\corevideo-zoom-engine.exe
COREVIDEO_ZOOM_PUBLIC_APP_KEY=...
```

Use `COREVIDEO_ZOOM_SDK_JWT` instead of `COREVIDEO_ZOOM_PUBLIC_APP_KEY` when
testing the JWT auth path. Optional join tokens are read from
`COREVIDEO_ZOOM_ON_BEHALF_TOKEN`, `COREVIDEO_ZOOM_USER_ZAK`, and
`COREVIDEO_ZOOM_APP_PRIVILEGE_TOKEN`. These values are only sent to the helper
process; do not commit or log them.

## Keeping it in sync

This is a vendored copy, not a submodule. If the upstream capture engine changes,
re-copy `engine/src/*` and `src/engine-ipc.h` and re-apply the include-path
rewrites above. Keep this engine standalone so it stays portable to this app.
