# CoreVideo Pro — Native Build Work Plan (for Claude + Codex agents)

## Context

The CoreVideoPro repo today is a TypeScript/React/CSS skeleton: complete domain
models, typed engine contracts, mock/simulated engines, and a runnable operator
console. **None of the real product — the native media core — exists.** All the
hard, high-value work (Zoom raw ingest, GPU compositing, hardware encoding,
streaming, capture-card SDKs) is unstarted. This plan splits the foundational
and native work into two parallelizable tracks to hand to two coding agents.

**Hard environment constraint:** this Linux container has no GPU, no Zoom SDK
credentials/entitlements, no Mac/Windows toolchains, and no Blackmagic/AJA
hardware. So real native pieces can be *scaffolded, stubbed, and unit-tested*
here, but compile-against-real-SDK and on-hardware validation must happen on a
proper Mac/Windows dev machine. The plan is structured so everything buildable
here is front-loaded, and SDK-bound work is isolated behind interfaces.

## The architecture seam (already in place — do not redesign)

- Renderer entry `src/main.tsx` picks engines: if `window.coreVideoNative`
  (type `NativeHostBridge`, in `src/engine/nativeHostBridge.ts`) exists, it uses
  `createNativeZoomEngineBundle(...)`; otherwise `createMockEngineBundle()`
  (`src/engine/engineBundle.ts`).
- The native bridge is a typed RPC: `NativeBridgeCommand` → `NativeBridgeResponse`
  with `id` correlation (`src/engine/nativeBridgeProtocol.ts`). Today it covers
  only **zoom** (`join`/`leave`/`snapshot`) and **output** commands.
- `buildNativeMediaCoreCommands(state)` (`src/engine/nativeMediaCoreCommands.ts`)
  already serializes scene graph / transforms / overlays / output into a command
  list — but it is **not wired to the bridge** yet. This is a key dangling seam.
- Capability handshake: `NativeMediaCoreProfile` +
  `validateNativeMediaCoreProfile` (`src/engine/nativeMediaCoreProtocol.ts`);
  surfaced to UI via `describeRuntimeEnvironment` (`src/engine/runtimeEnvironment.ts`).
- There is **no** Electron/Tauri config and **no** native/C++ directory yet.

## Track split

### Track A — Desktop shell + real IPC bridge (Claude agent)
**Fully buildable and runnable in this container.** Turns the mock skeleton into
a real desktop app with a genuine process boundary. This unblocks Track B.

1. **Electron shell** (`shell/` or `desktop/` dir): main process, preload,
   packaging config (electron-builder). Load the existing Vite renderer.
2. **Preload injects `window.coreVideoNative`** implementing `NativeHostBridge`
   (`request`, `platform`, `host: "electron"`, `mediaCoreProfile`). Use
   `contextBridge` + `ipcRenderer.invoke`; no `nodeIntegration`.
3. **Main-process IPC router**: receives `NativeBridgeCommand`, dispatches by
   `type`, returns `NativeBridgeResponse` with matching `id`. Initially backs
   handlers with the existing simulated engines ported to Node, so the real
   shell runs end-to-end with zero native code.
4. **Media-core command channel**: extend `nativeBridgeProtocol.ts` with a
   `media-core` command variant carrying `NativeMediaCoreCommand[]`, and wire
   `buildNativeMediaCoreCommands()` from the renderer through the bridge. Close
   the dangling seam.
5. **Capture-device + audio/captions bridge commands**: extend the protocol so
   `CaptureDeviceEngine` and (eventually) audio/caption ops can be delegated to
   native too (currently browser-only). Define commands now; back with stubs.
6. **Child-process supervisor**: main process spawns/monitors the native media
   core (Track B binary) over stdio JSON-RPC, with crash isolation, restart, and
   the capability handshake that populates `mediaCoreProfile`. Until the binary
   exists, talk to a Node stub that returns the synthetic profile.

**Representative files:** new `desktop/main.ts`, `desktop/preload.ts`,
`desktop/ipcRouter.ts`, `desktop/mediaCoreClient.ts`; edits to
`src/engine/nativeBridgeProtocol.ts`, `src/engine/nativeMediaCoreCommands.ts`,
`src/main.tsx`, `package.json`, new `electron-builder` config.

### Track B — Native media core skeleton (Codex agent)
**Scaffold + stub here; real SDK work flagged for the dev machine.** A C++20
process behind a stdio JSON-RPC matching the bridge protocol, with one module
per capability in `NativeMediaCoreCapability`.

1. **CMake project** (`native/` dir): cross-platform, process entry, JSON-RPC
   loop over stdio, request/response `id` correlation matching
   `nativeBridgeProtocol.ts`. Emits the `NativeMediaCoreProfile` handshake.
2. **Module interfaces (pure virtual) + stub impls**, one per subsystem so the
   compositor/mixer never know the vendor:
   - `IZoomCaptureSource` — Zoom Meeting SDK raw video/audio (stub returns synthetic frames).
   - `ICompositor` — D3D11/Metal scene-graph renderer (stub = CPU/no-op).
   - `IAudioMixer` — per-participant gain/limiter/noise-suppress (stub = passthrough).
   - `IEncoderSink` — NVENC/QuickSync/AMF/VideoToolbox + RTMP/file (stub = counts frames).
   - `ICaptureDevice` — Blackmagic DeckLink / AJA NTV2 (stub = enumerates fake devices).
3. **Command application**: consume `NativeMediaCoreCommand` (`load-scene-graph`,
   `set-participant-transform`, `set-overlay-asset`, `start-program-output`) and
   drive the module stubs; return health/session state shaped to the TS types.
4. **Per-platform SDK adapters behind the interfaces**, clearly marked
   `// REQUIRES DEV MACHINE`: Zoom SDK, DeckLink SDK, NTV2 SDK, GPU + HW encoder
   paths. These compile-link only where SDKs/hardware exist; CI here builds the
   stub configuration.
5. **Native unit tests** (GoogleTest) for JSON-RPC framing, command application,
   and profile handshake — all runnable in-container.

**Representative files:** new `native/CMakeLists.txt`, `native/src/main.cpp`,
`native/src/rpc/*.cpp`, `native/src/modules/*.{h,cpp}`,
`native/src/platform/{win,mac}/*` (guarded), `native/tests/*`.

## Coordination between the two agents

- **The TypeScript protocol files are the single source of truth.** Any change
  to `nativeBridgeProtocol.ts` / `nativeMediaCoreProtocol.ts` /
  `nativeMediaCoreCommands.ts` is a contract change: Claude owns editing them,
  Codex mirrors them on the C++ side. Land protocol changes first, in their own
  commits, before either side builds against them.
- **Stub-first, real-later.** Both tracks must run end-to-end with stubs in this
  container before any SDK-bound code is added. Real-SDK code goes behind the
  interfaces and is `#ifdef`/config-gated so the default build stays green here.
- **Milestone gate (the integration target both tracks aim at):** launch the
  Electron app → renderer detects `window.coreVideoNative` → main spawns the C++
  stub core → handshake announces a profile → `describeRuntimeEnvironment`
  reports `"ready"` (or `"degraded"` if capabilities withheld) → a Magic Scene
  take pushes `NativeMediaCoreCommand`s through the bridge to the C++ stub, which
  acks and returns synthetic health. No real video yet — but the whole pipe is
  real and typed.
- Keep AI/scene-recommendation, presets, and (initially) audio/captions in the
  renderer per the existing hybrid model; only migrate to native once the core
  pipe is proven.

## Suggested sequencing

1. Claude: protocol extensions (media-core + capture-device commands) — land first.
2. Parallel: Claude builds Electron shell + IPC router on simulated backends;
   Codex builds CMake + JSON-RPC loop + stub modules.
3. Claude wires `mediaCoreClient` to spawn Codex's stub binary; prove handshake.
4. Both: drive `buildNativeMediaCoreCommands` output through to the C++ stub.
5. Flag and hand off SDK-bound work (Zoom/GPU/encoder/capture) for the dev machine.

## Verification (all runnable in this container with stubs)

- `npm run typecheck && npm run test` — existing TS suite stays green after
  protocol edits.
- New native build: `cmake -S native -B native/build -DCOREVIDEO_STUB=ON &&
  cmake --build native/build && ctest --test-dir native/build` — stub core
  builds and unit tests pass.
- Shell smoke test: `npm run desktop` (new script) launches Electron, renderer
  shows native runtime status (not "Mock studio"), and a scripted Magic Scene
  take round-trips commands to the stub core (assert via a logging handler /
  Playwright-Electron or a headless main-process test).
- Contract-parity check: a small test (TS + C++) asserting the command/response
  `type` strings match on both sides, so the two agents can't silently drift.
