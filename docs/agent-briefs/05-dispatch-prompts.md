# Dispatch Prompts

Ready-to-paste kickoff prompts for each agent. These point at the round-2
next-milestone plans (`03-...`, `04-...`) and inline the key constraints so each
agent can start without further context.

---

## Agent A (Claude) — Desktop + Renderer

> You are working in the `iamfatness/CoreVideoPro` repo. Check out branch
> `claude/wonderful-darwin-34tjph` (or branch from latest `main`) and read
> **`docs/agent-briefs/03-track-a-next-milestones.md`** — that is your task list.
>
> Context: the Electron shell (`desktop/`), IPC bridge, `MediaCoreSupervisor`,
> the native core skeleton (`native/`), the 6-family bridge protocol, and the
> typed Zoom media spine (`src/engine/zoomMediaSpine*.ts`) are already built. Do
> not re-scaffold them.
>
> Work milestones A1→A5 in order. Start with **A1**: wire the Zoom media spine
> through the real native bridge path — today `desktop/ipcRouter.ts` answers Zoom
> via the in-memory `SimulatedZoom`; route a new `zoom-media-spine-sync` request
> (add to `desktop/coreProtocol.ts`) to the `MediaCoreSupervisor` child and have
> `NativeHostZoomMediaSpineSyncEngine` (`src/engine/zoomMediaSpineNativeSync.ts`)
> use it instead of `buildFallbackZoomMediaSpineSnapshot()`.
>
> Rules: you OWN the TS protocol files (`nativeBridgeProtocol.ts`,
> `nativeMediaCoreProtocol.ts`, `nativeMediaCoreCommands.ts`) and
> `desktop/coreProtocol.ts` — land any new command in its own commit so Agent B
> can mirror it. Everything must keep `npm run typecheck && npm run
> typecheck:desktop && npm run test` green in-container with stubs. Commit per
> milestone; open a draft PR.

---

## Agent B (Codex) — Native Implementations

> You are working in the `iamfatness/CoreVideoPro` repo. Check out branch
> `claude/wonderful-darwin-34tjph` (or branch from latest `main`) and read
> **`docs/agent-briefs/04-track-b-next-milestones.md`** — that is your task list.
>
> Context: the C++ native core skeleton exists at `native/` (`CMakeLists.txt`
> with `COREVIDEO_STUB` default ON and `COREVIDEO_ENABLE_DEV_ADAPTERS` default
> OFF, `src/main.cpp` JSON-RPC loop, `src/modules/Interfaces.h`,
> `StubModules.cpp`, `ContractParityTest`). A Node mirror lives at
> `native-core/`. Do not re-scaffold them.
>
> Work milestones B1→B6 in order. Start with **B1**: implement the
> `zoom-media-spine-sync` request (which Agent A adds to
> `desktop/coreProtocol.ts`) in both the C++ core and
> `native-core/src/zoomMediaSpine.ts` — consume `ZoomMediaSpineSyncPayload`,
> return `ZoomMediaSpineNativeSnapshot`; stub path returns synthetic, dev path is
> gated.
>
> Rules: NEVER edit the TS protocol files — mirror them in
> `native/src/core/Protocol.h` and `native-core/src/protocol.ts`. All real
> SDK/GPU/encoder code goes behind `COREVIDEO_ENABLE_DEV_ADAPTERS` so the default
> build stays stub-only and green. Keep `ContractParityTest` green and extend it
> for every new request type. Verify with `cmake -S native -B native/build
> -DCOREVIDEO_STUB=ON && cmake --build native/build && ctest --test-dir
> native/build` and `npm run test:native-core`. Commit per milestone; open a
> draft PR.
</content>
