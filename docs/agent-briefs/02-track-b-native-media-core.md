# Track B (Agent B / Codex) — Native Media Core Skeleton

**Repo:** `iamfatness/CoreVideoPro`, branch `claude/wonderful-darwin-34tjph`.
Monorepo; add a new top-level `native/` directory.

**Mission:** Build a C++20 native media-core process that talks JSON-RPC over
stdio, matching the TypeScript bridge protocol. Scaffold + stub everything so
the default build is green in a plain Linux container; isolate all real
SDK/GPU/hardware code behind interfaces flagged for a Mac/Windows dev machine.

## Contract source of truth (read, mirror, do NOT redefine)

- `src/engine/nativeBridgeProtocol.ts` — command/response envelope + `id`
  correlation.
- `src/engine/nativeMediaCoreProtocol.ts` — `NativeMediaCoreProfile`,
  `NativeMediaCoreCapability`, `requiredMvpMediaCoreCapabilities`,
  `validateNativeMediaCoreProfile`.
- `src/engine/nativeMediaCoreCommands.ts` — `NativeMediaCoreCommand`:
  `load-scene-graph`, `set-participant-transform`, `set-overlay-asset`,
  `start-program-output`.

## Tasks

1. **CMake project** (`native/CMakeLists.txt`, `native/src/main.cpp`,
   `native/src/rpc/*`): cross-platform, process entry, JSON-RPC loop over stdio
   with `id` correlation exactly matching the TS envelope. Emit the
   `NativeMediaCoreProfile` handshake on startup.
2. **Pure-virtual module interfaces + stub impls** (`native/src/modules/*`), one
   per capability so the compositor/mixer never know the vendor:
   - `IZoomCaptureSource` — stub returns synthetic frames.
   - `ICompositor` — stub is CPU/no-op.
   - `IAudioMixer` — stub is passthrough.
   - `IEncoderSink` — stub counts frames.
   - `ICaptureDevice` — stub enumerates fake devices.
3. **Command application:** consume `NativeMediaCoreCommand`s, drive the stubs,
   return health/session state shaped to the TS types.
4. **Per-platform SDK adapters behind the interfaces**, marked
   `// REQUIRES DEV MACHINE` and config-gated (default `-DCOREVIDEO_STUB=ON`):
   Zoom Meeting SDK, Blackmagic DeckLink, AJA NTV2, D3D11/Metal compositor,
   NVENC/QuickSync/AMF/VideoToolbox encoders. These link only where
   SDKs/hardware exist.
5. **GoogleTest** for JSON-RPC framing, command application, and the profile
   handshake — all runnable in-container.

## Done = builds and tests green in container

```
cmake -S native -B native/build -DCOREVIDEO_STUB=ON
cmake --build native/build
ctest --test-dir native/build
```

…and the binary completes the handshake + acks a `start-program-output` command
from Agent A's supervisor with synthetic health.

## Guardrails

- Never edit the TS protocol files — Agent A owns them; you mirror.
- Add a **contract-parity test** asserting the command/response `type` strings
  match the TS side so the two of you can't silently drift.

## Representative files

`native/CMakeLists.txt`, `native/src/main.cpp`, `native/src/rpc/*.cpp`,
`native/src/modules/*.{h,cpp}`, `native/src/platform/{win,mac}/*` (guarded),
`native/tests/*`.
</content>
