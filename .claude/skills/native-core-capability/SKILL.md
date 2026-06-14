---
name: native-core-capability
description: >-
  Add or extend a native media-core capability in CoreVideo Pro by threading it
  through every layer of the typed renderer↔native-core boundary, with a paired
  test at each layer. Use this whenever you are adding, wiring, or modifying
  anything that flows between the React operator renderer and the native-core
  media service — e.g. a new output/recording/streaming control, a scene-graph
  or overlay/transform/chroma feature, an audio-mix or caption capability, a new
  command/snapshot field, or anything described as "wire X into the native
  core", "surface Y in the Native core readout", "add a media-core command",
  "sync production state to the backend", or "add a native model for X".
  Reach for it even when the user just says "add feature Z to CoreVideo Pro" and
  Z touches capture, compositing, recording, output, or diagnostics — those all
  live behind this boundary.
---

# CoreVideo Pro — Native Media-Core Capability Slice

## What this is and why it matters

CoreVideo Pro is a desktop production app. The React/Vite surface is **only the
operator renderer** — it is deliberately *not* the real-time media engine. The
end-state product depends on a native media core (C++/Rust) for Zoom ingest, GPU
compositing, recording, and streaming. To keep that future replaceable, the repo
ships features as **vertical slices through a typed, shell-agnostic boundary**:
the renderer serializes production state into transport-neutral commands, a
`native-core` workspace simulates the backend service, and the UI displays a
read-only snapshot of synced state.

Almost every commit in this repo follows the same shape (e.g. "Add native output
profile model" touched the protocol, the backend state machine, the renderer
command builder, the sync engine, `App.tsx`, and `README.md` — each with a
paired test). When you add a capability, your job is to thread it cleanly through
that same chain so the renderer never secretly becomes the engine, and so every
layer stays in sync and tested.

**The golden rule:** the renderer and engine files must stay shell-agnostic — no
`electron`, OBS, or browser-capture (`getUserMedia`, `MediaRecorder`) imports in
the protocol mirror, command builder, sync engine, or anything under
`src/engine/`. State mutates in the backend state machine, never in the renderer;
the renderer only reads immutable `*Snapshot`s.

## The layer chain (the file map)

A capability that flows from backend to UI passes through these files in order.
Most slices touch the **bold** ones; the rest are usually no-change.

| # | Layer | File | Typical change |
|---|-------|------|----------------|
| 1 | **Protocol (wire types)** | `native-core/src/protocol.ts` | Add a command to the `MediaCoreCommand` union + any new snapshot fields |
| 2 | **Backend state machine** | `native-core/src/mediaCore.ts` | Add state field, a branch in `apply()`, snapshot fields in `snapshot()` |
| 3 | Service boundary | `native-core/src/service.ts` | Usually no change (validates + delegates) |
| 4 | Spawnable client | `native-core/src/client.ts` | Usually no change (extends `MediaCoreCommand` automatically) |
| 5 | Native-core exports | `native-core/src/index.ts` | Export any new public type |
| 6 | **Renderer protocol mirror** | `src/engine/nativeMediaCoreProtocol.ts` | Mirror the command/snapshot types *exactly*; add capability enum if needed |
| 7 | **Command builder** | `src/engine/nativeMediaCoreCommands.ts` | Add `buildXCommand()`, push it in `buildNativeMediaCoreCommands()` under a condition |
| 8 | **Sync engine** | `src/engine/mediaCoreSync.ts` | Teach `InMemoryMediaCoreSyncEngine.snapshot()` to populate the new snapshot fields |
| 9 | **Renderer UI readout** | `src/App.tsx` | Add `<ControlReadout>` rows in the "Native core" section |
| 10 | Host bridge / bundle | `nativeHostBridge.ts`, `engineBundle.ts` | Usually no change (transport-agnostic injection) |
| 11 | **Domain state** | `src/domain/production.ts` | Add the field(s) the builder reads, if the feature is producer-controlled |

Each layer that changes gets a **paired `*.test.ts`**. See
`references/layer-map.md` for the full per-file detail with code snippets — read
it before your first slice, or when a layer's exact shape is unclear.

## Workflow

Work back-to-front (backend first), because the renderer types mirror the
backend and the tests at each layer give you a fast feedback loop.

1. **Model the capability at the backend.** Add the command variant to the
   `MediaCoreCommand` union in `protocol.ts` and any snapshot fields it produces.
   In `mediaCore.ts`, add the state it needs, a branch in `apply()` that mutates
   that state and pushes a human-readable `warnings` string when the payload is
   invalid or incomplete, and surface it in `snapshot()`. Export new public types
   from `index.ts`.
2. **Prove the backend.** Add a `native-core/src/<feature>.test.ts` (or extend
   `mediaCore.test.ts` / `client.test.ts`) that sends the command and asserts the
   resulting snapshot via `expect(...).toMatchObject(...)`. Run
   `npm run test:native-core`.
3. **Mirror into the renderer.** Copy the command/snapshot types into
   `src/engine/nativeMediaCoreProtocol.ts` *exactly* (same discriminator strings,
   same field order/types). If the feature is a distinct hardware/output
   capability, add it to the `NativeMediaCoreCapability` union and the validation
   helper, and to `requiredMvpMediaCoreCapabilities` if it's MVP-critical.
4. **Build the command from production state.** If producers control it, add the
   field(s) to `ProductionState` in `src/domain/production.ts`. Add a
   `buildXCommand(state)` helper in `nativeMediaCoreCommands.ts` and push it in
   `buildNativeMediaCoreCommands()` — guarded by a condition (e.g. only when
   enabled). Add a test asserting the command is present when enabled and absent
   when disabled.
5. **Simulate it in the sync engine.** Teach `InMemoryMediaCoreSyncEngine.snapshot()`
   in `mediaCoreSync.ts` to read your command out of the command list and populate
   the new snapshot fields, so the mock used in dev/tests behaves like the backend.
   `NativeHostMediaCoreSyncEngine` needs no change — it forwards to the real bridge.
   Add a `mediaCoreSync.test.ts` assertion.
6. **Surface it in the UI.** Add `<ControlReadout label=... value={mediaCoreSnapshot?.<field>} />`
   rows to the "Native core" `<section>` in `App.tsx`. Give read-only/pending
   fallbacks (`?? "Pending"`, `?? 0`, `?? "None"`) like the existing rows.
7. **Document it.** Add one bullet under "Current Slice" in `README.md` describing
   the capability, and one bullet under the test list describing the new tests.
   This discipline is how the README stays an accurate capability inventory.
8. **Verify the whole slice** (see commands below).

## Commands

```bash
npm run test:native-core   # backend (native-core workspace) — Node env
npm run test               # renderer + engines — jsdom env
npm run typecheck          # tsc over both workspaces; catches mirror drift
npm run dev                # renderer with mock engines, to eyeball the readout
npm run dev:native-core    # run the native-core service in isolation
```

Run `npm run typecheck && npm run test:native-core && npm run test` before you
consider a slice done. `typecheck` is the cheapest way to catch the most common
mistake (protocol mirror drift between `protocol.ts` and
`nativeMediaCoreProtocol.ts`).

## Conventions worth matching

- **Naming:** `*Command` (wire payloads), `*Snapshot` (read-only backend state),
  `*Request`/`*Response` (IPC), `*Engine` (implementations), `*Adapter`
  (renderer→native), `*Sink` (stateful backend helpers), and `native*` filenames
  for anything that crosses the shell boundary.
- **Immutability:** the renderer reads snapshots and never mutates engine state.
  All mutation lives in `MediaCoreRuntime`.
- **Health & warnings:** statuses are small discriminated unions
  (`"idle" | "live" | "warning" | "failed"` etc.); free-form diagnostics go into
  the `warnings: string[]` array. Prefer pushing a clear warning over throwing.
- **Mirror discipline:** `protocol.ts` (backend) and `nativeMediaCoreProtocol.ts`
  (renderer) must stay byte-for-byte equivalent in shape. They are intentionally
  duplicated so the renderer has zero backend imports.

## Gotchas (where slices break)

- **Protocol mirror drift** — you added the command to `protocol.ts` but not
  `nativeMediaCoreProtocol.ts` (or vice versa). `npm run typecheck` fails. Keep
  them in lockstep.
- **Unhandled command in `apply()`** — the command serializes but the snapshot
  never changes. Assert `lastCommandTypes` includes your `type` in a test.
- **Mock sync engine not updated** — renderer tests pass but the dev UI shows
  stale/empty values because `InMemoryMediaCoreSyncEngine.snapshot()` doesn't read
  the new command. Both the backend handler and the mock simulation must learn the
  command.
- **Forgetting `README.md` bullets** — the README is the living capability +
  test inventory here; a slice isn't complete until it's listed.
- **Treating native-core as part of the root test run** — it's a separate npm
  workspace. `npm run test` does *not* run it; use `npm run test:native-core`.
- **Sneaking shell imports into the boundary** — importing `electron`/OBS/browser
  capture into a `src/engine/` file defeats the entire design. Keep the engine
  files transport-neutral.
