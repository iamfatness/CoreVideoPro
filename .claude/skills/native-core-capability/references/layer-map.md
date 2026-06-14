# Layer map — per-file detail

Read this when you need the exact shape of a layer. Layers are listed
backend-first (the order you should implement in). File paths are relative to
the repo root.

## 1. `native-core/src/protocol.ts` — the wire types

Defines every command and the snapshot shape. Commands are a discriminated union
keyed on `type`:

```ts
export type MediaCoreCommand =
  | { type: "load-scene-graph"; sceneId: string; routes: Array<{
        routeId: string; mode: MediaCoreRouteMode;
        participantId?: string; audioRole: MediaCoreAudioRole;
      }> }
  | { type: "set-participant-transform"; participantId: string;
      crop: { x: number; y: number; width: number; height: number };
      scale: number; chromaKey?: { enabled: boolean; color: string; spillSuppression: number } }
  | { type: "set-overlay-asset"; overlayId: string; /* ... */ }
  | ({ type: "set-output-profile" } & MediaCoreOutputProfile)
  | { type: "start-program-output"; destinations: MediaCoreDestination[]; isoParticipantIds: string[] }
  | ({ type: "set-recording-targets" } & MediaCoreRecordingTargets)
  | { type: "start-recording-session"; sessionId?: string; startedAtMs?: number }
  | { type: "stop-recording-session"; reason?: string }
  | { type: "fail-recording-session"; message: string };
```

The snapshot is the read model the UI consumes:

```ts
export type MediaCoreStateSnapshot = {
  sceneId?: string;
  routeCount: number;
  frameCount: number;
  frames: MediaCoreFrame[];
  participantTransformCount: number;
  overlayCount: number;
  outputs: MediaCoreDestination[];
  isoParticipantIds: string[];
  outputProfile: MediaCoreOutputProfile;
  outputHealth: MediaCoreOutputHealth[];
  recording?: MediaCoreRecordingSession;
  diagnostics: MediaCoreDiagnosticsSnapshot;
  lastCommandTypes: string[];
  warnings: string[];
};
```

Statuses are small unions: `MediaCoreOutputHealthStatus = "idle" | "live" | "warning" | "failed"`,
`MediaCoreRecordingStatus = "recording" | "warning" | "stopped" | "failed"`,
`MediaCoreFrameHealth = "live" | "stale" | "dropped" | "low-resolution"`.

**Add:** a new union member for your command, plus any snapshot fields it
produces. Keep payloads transport-neutral (plain JSON-serializable data).

## 2. `native-core/src/mediaCore.ts` — the backend state machine

`MediaCoreRuntime` holds state, applies commands, and emits snapshots:

```ts
export class MediaCoreRuntime {
  private sceneGraph?: SceneGraphState;
  private readonly transforms = new Map<string, TransformState>();
  private readonly overlays = new Map<string, OverlayState>();
  // ... more state

  handle(request: MediaCoreRequest): MediaCoreResponse {
    if (request.type === "snapshot") return { id: request.id, ok: true, state: this.snapshot() };
    const warnings = this.apply(request.commands);
    this.tick(this.elapsedMs);
    return { id: request.id, ok: true, appliedCommandCount: request.commands.length,
             state: this.snapshot(warnings) };
  }

  apply(commands: MediaCoreCommand[]) {
    const warnings: string[] = [];
    commands.forEach((command) => {
      if (command.type === "load-scene-graph") {
        this.sceneGraph = command;
        if (command.routes.length === 0) warnings.push("Scene graph loaded without routes.");
        return;
      }
      // ... one branch per command type
    });
    return warnings;
  }

  snapshot(warnings: string[] = []): MediaCoreStateSnapshot { /* read state into snapshot */ }
}
```

**Add:** a state field, a branch in `apply()` (mutate state + push warnings on bad
input), and the field(s) in `snapshot()`. Validate payloads here (e.g. crop
within 0–1) and prefer a warning over throwing — the renderer surfaces warnings.

## 3. `native-core/src/service.ts` — usually no change

A thin JSON-line server: parses a line, validates `id`/`type`, delegates to the
runtime. Only touch it if you add a new top-level request *type* (rare — most
work rides on `sync`).

## 4. `native-core/src/client.ts` — usually no change

`MediaCoreServiceClient` spawns the service and correlates JSON-line responses by
`id`. `client.sync(commands)` accepts any `MediaCoreCommand[]`, so a new command
flows through with no change. Add a test, not code.

## 5. `native-core/src/index.ts` — exports

Re-exports runtime, client, and the protocol types. **Add** your new public type
to the `export type { ... } from "./protocol.js"` list.

## 6. `src/engine/nativeMediaCoreProtocol.ts` — the renderer mirror

A *parallel* copy of the protocol types, intentionally duplicated so the renderer
imports nothing from `native-core`. Mirror your command/snapshot additions here
exactly (same discriminator strings, same field names/types/order).

It also holds capability metadata:

```ts
export type NativeMediaCoreCapability =
  | "zoom-raw-video" | "gpu-compositor" | "scene-graph-rendering"
  | "dynamic-overlays" | "chroma-key" | "audio-mixer"
  | "program-recording" | "iso-recording"
  | "rtmp-output" | "ndi-output" | "srt-output" | "webrtc-output";
```

If your feature is a distinct capability, add it here, extend the validation
helper (`validateNativeMediaCoreProfile`), and add it to
`requiredMvpMediaCoreCapabilities` if MVP-critical.

## 7. `src/engine/nativeMediaCoreCommands.ts` — the command builder

`buildNativeMediaCoreCommands(state: ProductionState): NativeMediaCoreCommand[]`
serializes the renderer's immutable production state into commands:

```ts
export function buildNativeMediaCoreCommands(state: ProductionState): NativeMediaCoreCommand[] {
  const activeScene = state.scenes.find((s) => s.id === state.activeSceneId) ?? state.scenes[0];
  const commands: NativeMediaCoreCommand[] = [
    { type: "load-scene-graph", sceneId: activeScene.id, routes: /* ... */ },
    ...state.videoEffects.map(buildTransformCommand),
    ...state.graphics.filter((g) => g.enabled).map(buildOverlayCommand),
    buildOutputProfileCommand(state),
  ];
  const outputCommand = buildOutputCommand(state);
  if (outputCommand) commands.push(outputCommand);
  commands.push(...buildRecordingCommands(state));
  return commands;
}
```

**Add:** a `buildXCommand(state)` helper and push it (guarded by a condition such
as "only when enabled"). Read inputs from `ProductionState`.

## 8. `src/engine/mediaCoreSync.ts` — the sync engines

Two implementations behind the `MediaCoreSyncEngine` interface
(`syncProduction(state, elapsedMs): Promise<NativeMediaCoreStateSnapshot>`):

- `InMemoryMediaCoreSyncEngine` — the **mock** used in dev and tests. Its
  `snapshot()` reads commands out of the built list and simulates backend state
  (frame numbers, counts, health). **Teach it your command** so the mock matches
  the real backend.
- `NativeHostMediaCoreSyncEngine extends InMemoryMediaCoreSyncEngine` — forwards to
  `bridge.syncMediaCore(commands, elapsedMs)` when present, else falls back to the
  in-memory simulation with a warning. **No change needed.**

## 9. `src/App.tsx` — the "Native core" readout

A `<section className="panel">` titled "Native core" renders `<ControlReadout>`
rows from `mediaCoreSnapshot`:

```tsx
<ControlReadout label="Synced scene" value={mediaCoreSnapshot?.sceneId ?? "Pending"} />
<ControlReadout label="Routes" value={`${mediaCoreSnapshot?.routeCount ?? 0}`} />
<ControlReadout label="Recording" value={mediaCoreSnapshot?.recording?.status ?? "Off"} />
<ControlReadout label="Warnings" value={mediaCoreSnapshot?.warnings[0] ?? "None"} />
```

**Add:** rows for your new snapshot fields, with pending/empty fallbacks.

## 10. `nativeHostBridge.ts` / `engineBundle.ts` — injection (no change)

`NativeHostBridge` is the optional IPC transport (`window.coreVideoNative`).
`createMockEngineBundle()` wires the in-memory engine; `createNativeZoomEngineBundle()`
wires the native one (with mock fallback). Transport-agnostic — adding a command
needs nothing here.

## 11. `src/domain/production.ts` — producer-controlled state

If producers toggle/configure the capability, add the field(s) to
`ProductionState` (and `initialProduction`) so the command builder has something
to read. Add domain tests if there's logic (defaults, validation).

## Test files (one per changed layer)

- `native-core/src/mediaCore.test.ts` — send commands to a `MediaCoreRuntime`,
  assert the snapshot with `expect(...).toMatchObject(...)`. Also assert
  `lastCommandTypes` includes your `type`.
- `native-core/src/client.test.ts` — spawn the real service via `tsx` and assert
  the synced snapshot end-to-end.
- `src/engine/nativeMediaCoreCommands.test.ts` — assert the command is present
  when the feature is enabled and absent when disabled.
- `src/engine/mediaCoreSync.test.ts` — assert the in-memory engine populates your
  snapshot field, and (via a `vi.fn()` bridge) that `NativeHostMediaCoreSyncEngine`
  forwards commands.

Run `npm run test:native-core` for backend tests and `npm run test` for renderer
tests; `npm run typecheck` covers both and catches mirror drift.
