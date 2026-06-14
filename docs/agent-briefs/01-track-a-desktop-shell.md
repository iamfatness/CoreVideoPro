# Track A (Agent A / Claude) — Desktop Shell + Real IPC Bridge

**Repo:** `iamfatness/CoreVideoPro`, branch `claude/wonderful-darwin-34tjph`.
Monorepo; add a new top-level `desktop/` directory.

**Mission:** Turn the existing React/Vite mock skeleton into a real Electron
desktop app with a genuine main↔renderer↔native-core process boundary.
Everything you build must run end-to-end in a plain Linux CI container using
stubs — no GPU, Zoom SDK, or hardware required.

## Read first

- `src/main.tsx` — engine selection (native bridge vs mock).
- `src/engine/nativeHostBridge.ts` — `NativeHostBridge` type + the
  `window.coreVideoNative` seam.
- `src/engine/nativeBridgeProtocol.ts` — `NativeBridgeCommand` /
  `NativeBridgeResponse` RPC envelope with `id` correlation.
- `src/engine/engineBundle.ts` — `createNativeZoomEngineBundle` vs
  `createMockEngineBundle`.
- `src/engine/nativeMediaCoreCommands.ts` — `buildNativeMediaCoreCommands`
  (currently wired to nothing — you will close this seam).
- `src/engine/nativeMediaCoreProtocol.ts` + `src/engine/runtimeEnvironment.ts` —
  capability handshake → UI status.

## Tasks

1. **Electron shell** in `desktop/`: `main.ts`, `preload.ts`, electron-builder
   config, an `npm run desktop` script. Load the existing Vite renderer. No
   `nodeIntegration`; use `contextBridge` + `ipcRenderer.invoke`.
2. **Preload injects `window.coreVideoNative`** implementing `NativeHostBridge`
   (`request`, `platform`, `host: "electron"`, `mediaCoreProfile`).
3. **Main-process IPC router** (`desktop/ipcRouter.ts`): dispatch
   `NativeBridgeCommand` by `type`, return `NativeBridgeResponse` with the
   matching `id`. Back handlers initially with the existing simulated engines
   ported to Node, so the shell runs with zero native code.
4. **Close the dangling seam:** extend `nativeBridgeProtocol.ts` with a
   `media-core` command variant carrying `NativeMediaCoreCommand[]`, and wire
   `buildNativeMediaCoreCommands()` from the renderer through the bridge.
   *(Protocol edits land first, in their own commit — Agent B mirrors them.)*
5. **Capture-device + audio/caption bridge commands:** define the command
   shapes now (back with stubs); keeps the door open to delegate these to native
   later.
6. **Child-process supervisor** (`desktop/mediaCoreClient.ts`): spawn/monitor
   Agent B's native binary over stdio JSON-RPC, with crash isolation, restart,
   and the handshake that populates `mediaCoreProfile`. Until the binary exists,
   talk to a Node stub returning a synthetic profile.

## Done = the integration gate

`npm run desktop` launches; renderer detects the bridge (status is *not*
"Mock studio"); main spawns the core stub; handshake yields a profile so
`describeRuntimeEnvironment` reports `"ready"`/`"degraded"`; a scripted Magic
Scene take round-trips `NativeMediaCoreCommand`s to the stub which acks with
synthetic health.

## Guardrails

- `npm run typecheck && npm run test` stays green.
- You own the TS protocol files; coordinate any change with Agent B and land it
  first in its own commit.
- Keep AI/scene-recommendation, presets, and (for now) audio/captions in the
  renderer per the existing hybrid model.

## Representative files

`desktop/main.ts`, `desktop/preload.ts`, `desktop/ipcRouter.ts`,
`desktop/mediaCoreClient.ts`; edits to `src/engine/nativeBridgeProtocol.ts`,
`src/engine/nativeMediaCoreCommands.ts`, `src/main.tsx`, `package.json`, new
electron-builder config.
</content>
