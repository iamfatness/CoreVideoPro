# Track A — Next Milestones (Agent A / Claude): Desktop + Renderer Integration

**Status baseline (already built — do not redo):** `desktop/` has a working
Electron shell (`main.ts`, `preload.ts` injecting `window.coreVideoNative`,
`ipcRouter.ts`, `mediaCoreClient.ts` `MediaCoreSupervisor` with crash-restart,
`coreStub.ts` Node fallback). Bridge protocol covers 6 command families
(zoom, output, capture-device, media-core, audio, caption). The Zoom media spine
is fully typed (`src/engine/zoomMediaSpine*.ts`, `zoomSdkReadiness.ts`,
`zoomWindowsSdkPackage.ts`).

**Theme:** connect the typed Zoom spine + media-core sync to the *real* native
process path, and surface readiness/capabilities in the operator UI. Everything
stays green in-container with stubs; real validation is on a dev machine.

- **A1 — Wire the Zoom spine through the bridge (the key open seam).** Today
  `desktop/ipcRouter.ts` answers Zoom via the in-memory `SimulatedZoom`; the real
  seam is `NativeHostBridge.syncZoomMediaSpine(payload)`
  (`src/engine/nativeHostBridge.ts`, `zoomMediaSpineNativeSync.ts`). Add a
  bridge command + IPC route that forwards the `ZoomMediaSpineSyncPayload` to the
  `MediaCoreSupervisor` child (new `zoom-media-spine-sync` request in
  `desktop/coreProtocol.ts`), and have `NativeHostZoomMediaSpineSyncEngine` use
  it instead of `buildFallbackZoomMediaSpineSnapshot()`. *Gate:* with the Node
  stub core, a join/sync round-trips through Electron→main→core and the renderer
  shows spine-sourced roster, not the renderer fallback.
- **A2 — Renderer integration of the spine controller.** Drive
  `ZoomMediaSpineSessionController.joinProduction/syncProduction/leave`
  (`zoomMediaSpineSessionController.ts`) from the app on join + on tick. Add a
  pre-flight panel rendering `assessZoomSdkReadiness()` and (Windows)
  `inspectZoomWindowsSdkPackage()` so blockers/warnings show before a join.
  *Gate:* readiness report renders; blocked states disable Join.
- **A3 — Supervisor hardening + status.** Surface `MediaCoreSupervisor` health
  (handshake timeout, restart count, `__crash`) into `describeRuntimeEnvironment`
  status so the UI banner reflects degraded/recovering core states. Add a
  handshake timeout and bounded sync backpressure. *Gate:* killing the stub core
  shows a recovering state and auto-restart in the UI.
- **A4 — Capability-gated UI.** Read `profile.capabilities`
  (`nativeMediaCoreProtocol.ts`) and gate Phase-2 outputs in the UI
  (NDI/SRT/WebRTC/virtual camera) so they only enable when the core announces
  them. *Gate:* a stub profile without `srt-output` hides/disables SRT arming.
- **A5 — Packaging + e2e smoke (dev machine).** Finish electron-builder targets
  via `desktop/scripts/launch.mjs` path; add a Playwright-Electron smoke
  (`playwright` is already a devDep) asserting the join→Magic Scene→take→core-ack
  round-trip. *Gate:* `npm run desktop` launches; smoke passes locally.

**Ownership:** Agent A owns the TS protocol files
(`nativeBridgeProtocol.ts`, `nativeMediaCoreProtocol.ts`,
`nativeMediaCoreCommands.ts`) and `desktop/coreProtocol.ts`. Land any new
command in its own commit; Agent B mirrors on the C++ side before building.

**Verification:** `npm run typecheck && npm run typecheck:desktop && npm run test`
green; `npm run desktop` manual smoke.
</content>
