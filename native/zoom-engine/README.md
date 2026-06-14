# Vendored Zoom capture engine

This is the proven, **OBS-free** Zoom Meeting SDK capture engine vendored from the
CoreVideo OBS plugin. It is a standalone child process (`ZoomObsEngine` in the
plugin, built here as `corevideo-zoom-engine`) that:

- initializes the Zoom Meeting SDK and authenticates (JWT or public app key),
- joins a meeting,
- subscribes to **per-participant raw I420 video** (and audio / screen share),
- writes frames to shared memory via a lock-free double-buffer
  (`shared/engine-ipc.h` `ShmFrameHeader`: `sequence/width/height/y_len`,
  followed by Y + U + V planes),
- speaks a line-delimited JSON IPC protocol (`init` / `join` / `subscribe` /
  `leave` …) over a pipe/socket.

## Provenance

Copied verbatim from `iamfatness/corevideo` (the OBS plugin) with **one** change:
`engine/main.cpp`'s `#include "../../src/engine-ipc.h"` was rewritten to
`#include "engine-ipc.h"` to match this flattened layout. The engine has no
`libobs`/Qt/`blog` dependencies, so no other edits were needed.

Source files:
- `engine/` ← plugin `engine/src/` (`main`, `engine-video`, `engine-audio`,
  `engine-share`, `engine-writer.h`)
- `shared/engine-ipc.h` ← plugin `src/engine-ipc.h`

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

## Keeping it in sync

This is a vendored copy, not a submodule. If the plugin engine changes, re-copy
`engine/src/*` and `src/engine-ipc.h` and re-apply the single include-path edit
above. Keep this engine OBS-free so it stays portable to this app.
