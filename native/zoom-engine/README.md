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
- `engine/main-macos.mm` — the **macOS engine**, re-forked 2026-08-02 from the
  CoreVideo OBS plugin's mac port (its `engine/src/main-macos.mm`, live-verified
  there against real meetings). The macOS Meeting SDK is Objective-C-only, so
  this is a self-contained ObjC++ rewrite of all four Windows files speaking
  the SAME wire protocol. Adapted to Pro's protocol on the way in: 128-slot
  audio SHM ring (`ShmAudioRingHeader`), beacon-cadence frame/audio pipe
  events, `--ipc-token` instance-spliced socket/SHM names, `raw_media_status`
  events, `{stage,msg,code,reason}` join failures, mono raw audio, 32-source
  caps. Its header comment lists the deltas; keep it in sync with the Windows
  engine when the protocol moves.

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

### macOS

On macOS, `ZOOM_SDK_DIR` is the directory **containing** `ZoomSDK.framework`
and the target builds `main-macos.mm`. The SDK is not self-contained — it loads
its runtime bundles through the main bundle's `Contents/Frameworks`, so a bare
binary auths `Failed(1)` synchronously. Wrap it with:

```
scripts/make-macos-engine-bundle.sh --build-dir native/build-engine --link-sdk
```

(`--link-sdk` symlinks the ~600MB SDK for local iteration; omit for a
distributable copy. Never commit any Zoom SDK file.) Point the core at the
bundled binary with `COREVIDEO_ZOOM_ENGINE_PATH=<...>.app/Contents/MacOS/corevideo-zoom-engine`.

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

## Comms (Zoom talkback) — shared verbatim with CoreVideo Edge

Since 2026-11-09 the engine is also the **Comms** talkback station (design:
CoreVideo Edge `docs/superpowers/specs/2026-11-09-comms-design.md`; Pro's
contract: `docs/reference/comms-talkback.md`). Talkback is sent by a meeting
participant that is host or co-host and the SDK is one participant per
process, so it has to be this process.

**Keep these files byte-identical with CoreVideo Edge's `native/zoom-engine/`**
(ported from Edge `comms-engine` @ `28c06bc`): `engine/engine-comms.{h,cpp}`,
`engine/comms/**` (ZComms @ `168a572`, see `engine/comms/PORTED.md`),
`fake/fake-comms.{h,cpp}`, `third_party/miniaudio/miniaudio.h`. Fix them in
one repo and copy to the other; the full delta description (Edge "delta 7"
and "delta 8") lives in Edge's engine README and applies here unchanged.
`main.cpp` and `fake/fake-engine.cpp` are NOT identical between the apps
(Edge carries other deltas); the Comms hooks in them are:

- `main.cpp`: `#include "engine-comms.h"`; `EngineComms::instance().start()`
  before the command loop (100 ms timer on the SDK thread);
  `handle_line()` FIRST for every line (the dispatch below matches
  `quit`/`join`/`leave` as substrings, a device name may hold one);
  `on_meeting_status()` as the first line of `onMeetingStatusChanged`;
  `set_capture_only(line has "capture_only":true)` in the join handler;
  `shutdown()` after the heartbeat stops and before `Leave`/`CleanUPSDK()`.
- `fake/fake-engine.cpp`: the same core over a scripted talkback SDK
  (`fakecomms::Start/HandleLine/SetStationName/Stop`), env knobs
  `COREVIDEO_FAKE_WEB_CLIENT_ID`, `COREVIDEO_FAKE_COMMS_ROLE=attendee`,
  `COREVIDEO_FAKE_COMMS_UNSUPPORTED=1`.

**Pro difference that matters:** Pro's joins are never capture-only (the
operator may speak in the meeting). `comms/mic_guard` installs the silent
virtual mic only for a capture-only join OR when the operator turned on
**Private comms** (`comms_private {"on":true}`, owner ruling 2026-11-10): then
it mutes the operator first and runs the capture-only path unchanged (licence,
unmute only after `onMicInitialize`, 5 s timeout, drop = revoke). Private OFF
removes the virtual mic (`setExternalAudioSource(nullptr)`) and leaves the
operator MUTED, never auto-unmuted (`MicGuard::Act::Uninstall`). Without
private the operator's meeting mic is left exactly as they set it; talkback is
silent while it is muted (ZComms law 1). `comms_state.private` reports
capture_only || requested. Pro has no `enforce_capture_only()` (that is an Edge
delta), so `EngineComms::owns_mic()` is unused here. The fake engine runs the
real MicGuard over a scripted SDK (`COREVIDEO_FAKE_SILENT_MIC=never`,
`COREVIDEO_FAKE_OPERATOR_MUTED=1`; `fakecomms::SetCaptureOnly` from the join).

New P2E lines: `comms_key {"slot":0..15|"all","on",["lease_ms"]}`,
`comms_latch`, `comms_assign {"slot","participant_id","on"}`, `comms_mic`,
`comms_sidetone_out`, `comms_sidetone`, `comms_tone`, `comms_gain`,
`comms_release_all`, `comms_private {"on"}`. New E2P: `comms_state` (<= 10 Hz, only on change;
meter-only changes at most every 250 ms). The media core relays them
(`src/modules/ZoomCommsRelay`, `ZoomEngineRuntime::commsCommand`). Nothing in
Comms calls into EngineVideo/EngineAudio/EngineShare; every tick is
exception-fenced.

Build: `COREVIDEO_COMMS_DIR` targets in `native/CMakeLists.txt`
(`corevideo_comms_core` shared by real engine, fake and tests;
`corevideo_comms_audio` = miniaudio mic chain, real engine only). The comms
translation units compile with `NOMINMAX` (Edge builds with it globally).

**Meeting SDK 7.1.5.43953** (was 7.0.5.39292): the talkback controller needs
it. Pro's engine compiled and linked against 7.1.5 with no source change.
Raw video/ISO on 7.1.5 is proven only against the fake validator and unit
tests in this worktree; a live 7.1.5 take is still owed.
