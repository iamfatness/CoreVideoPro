# CoreVideo Pro — Desktop shell (Track A)

Turns the mock React/Vite skeleton into a real Electron desktop app with a
genuine **renderer ↔ main ↔ native-core** process boundary. Everything here runs
end-to-end in a plain Linux container using a Node stub — no GPU, Zoom SDK, or
hardware required.

## Process map

```
┌────────────┐  window.coreVideoNative   ┌──────────────┐  stdio JSON-RPC   ┌──────────────┐
│  Renderer  │ ───── contextBridge ────► │ Electron main │ ───── spawn ────► │  media core  │
│ (Vite app) │ ◄──── ipcRenderer ─────── │  ipcRouter    │ ◄──── stdout ──── │ stub / native │
└────────────┘     invoke / sendSync     └──────────────┘   crash-isolated   └──────────────┘
```

- **`preload.ts`** exposes `window.coreVideoNative` (a `NativeHostBridge`):
  `request`, `platform`, `host: "electron"`, `mediaCoreProfile`, `syncMediaCore`.
  `contextIsolation` on, `nodeIntegration` off. Every command rides one
  `corevideo:request` invoke channel; the capability profile is fetched
  synchronously so the renderer detects the native runtime on first paint.
- **`main.ts`** owns the window, the IPC handlers, and the media-core supervisor.
- **`ipcRouter.ts`** dispatches each `NativeBridgeCommand` by `type` and returns a
  `NativeBridgeResponse` with the matching `id`. Zoom / output / capture / audio /
  captions are backed by the existing simulated engines ported to Node;
  media-core traffic is delegated to the supervised child process.
- **`mediaCoreClient.ts`** (`MediaCoreSupervisor`) spawns and monitors the core
  over stdio JSON-RPC, with crash isolation, automatic restart, and the
  capability handshake that populates `mediaCoreProfile`.
- **`coreStub.ts`** is the Node stand-in for Track B's native binary; it speaks
  the handshake + `media-core-sync` protocol and returns synthetic health via
  **`syntheticMediaCore.ts`**. Runs under plain `node` (Node ≥ 22 type-stripping).

## The protocol seam

`buildNativeMediaCoreCommands()` now flows through the bridge:
`nativeBridgeProtocol.ts` carries a `media-core-handshake` / `media-core-sync`
command (with `NativeMediaCoreCommand[]`), plus reserved `audio` / `caption`
command shapes. Track B mirrors these on the native side. Protocol changes land
first, in their own commit.

## Run the integration gate (headless, no Electron)

```
npm run test -- desktop/integration.test.ts
```

This spawns the stub core through the supervisor, runs the handshake → profile →
`describeRuntimeEnvironment` "ready", and round-trips a scripted Magic Scene take
to the core, which acks with synthetic health.

## Launch the full shell (dev machine)

Electron is intentionally **not** a default dependency (keeps CI light). Install
it on demand, then launch:

```
npm install --no-save electron@latest tsx
npm run desktop          # builds the renderer, then starts Electron
```

For a live renderer with HMR, point at the dev server instead of a build:

```
npm run dev              # in one terminal (Vite on 127.0.0.1:5173)
COREVIDEO_RENDERER_URL=http://127.0.0.1:5173 npm run desktop
```

On Windows, if Vite prints a different port (e.g. `5174`), match it in
`COREVIDEO_RENDERER_URL`. `launch.mjs` bundles `preload.cjs` automatically and
loads the Electron main process through `tsx`.

## Checks

```
npm run typecheck          # renderer/src (unchanged gate)
npm run typecheck:desktop  # desktop/ via ambient Node+Electron decls (no installs)
npm run test               # full suite incl. desktop/*.test.ts
```
