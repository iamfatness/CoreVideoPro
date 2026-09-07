# Show Engine Host Bridge (Plan 7a) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the Windows shell the OHG show engine's first host — run it as a supervised Node subprocess, register its 28 `ohg.*` actions and feedback fields into the control server, carry its snapshot on `ControlState`, and apply (or shadow-record) its host commands on the operator ViewModel.

**Architecture:** A stdio host entry in `show-engine/` runs the real `ShowEngine` and speaks newline-delimited JSON. In .NET, `CoreVideoPro.Control` gains a `ControlCatalog` that composes the static registry with `IControlActionProvider`s; a new `CoreVideoPro.ShowEngine` project holds the supervisor, protocol, bridge (a provider), paths, and config store; the WinUI shell adds an `OhgHostAdapter` over a small `IOhgHostFacade` and one forwarding rule in `StudioControlSurface`. Every unsupported mapping, refusal, crash, and config error is visible on the state node.

**Tech Stack:** TypeScript 5.9 strict / NodeNext / vitest 4 / Node 24 (engine); C# .NET 9, `System.Text.Json`, xUnit 2.9 (shell). No new NuGet or npm runtime dependencies.

**Spec:** `docs/superpowers/specs/2026-09-07-show-engine-host-bridge-design.md` (this plan cites it as "spec §n"). Parent: `docs/superpowers/specs/2026-08-04-ohg-show-engine-design.md`. Read `docs/superpowers/plan-authoring-rules.md` before Task 1.

## Global Constraints

- **Prerequisite:** the landing PR (`chore/land-show-engine-plans-5-6`) is merged to `main`. Branch `plan/show-engine-host-bridge` from `main` after that. If it is not merged when you start, stop and say so.
- Engine package: NodeNext — every relative import ends in `.js`. vitest `globals: false` — every test imports `{ describe, expect, it }` from `"vitest"`. Strict TS, **no `any`**, no non-null assertions where a guard will do.
- **No I/O in engine modules** outside `src/host/`. `src/host/` is the only place `process`, `fs`, `fetch`, and timers may appear. Nothing under `src/host/` may be imported by anything outside `src/host/`.
- .NET: `CoreVideoPro.Control` and `CoreVideoPro.ShowEngine` target `net9.0` and reference **no** WinUI. Nullable enabled. JSON: `PropertyNamingPolicy = CamelCase`, `PropertyNameCaseInsensitive = true`, `WhenWritingNull` for outbound — the same as `HttpControlRouter.JsonOptions`.
- **No `Thread.Sleep`, no real timers in tests.** Delays and clocks are injected.
- **Do NOT use `git stash`.** The stash stack is shared with other worktrees.
- Every task carries a **Mutations to run** block. A task is not done until each named mutation has been applied, observed to red the named test for the named reason, and reverted. Report results.
- Commit after every task with the message given. Run the task's verification commands before committing.
- Engine slot numbers and Show Input slot numbers are the same 1..10 space (spec D10). Capacity is 10.

## Decisions this plan is built on (spec §2)

D1 Node subprocess · D2 catalog by composition · D3 engine ticks itself · D4 raw snapshot pass-through + flat fields · D5 `ohg.*` loopback-only on OSC by default · D6 new small supervisor with backoff · D7 shadow mode is `driveHost:false` · D8 Node bundled in app folder · D10 slot == Show Input slot · D11 look presets identify routes by id (`ohg-box-<n>`, `ohg-host`, `ohg-reader`).

## Carried obligations discharged here (authoring rule 6)

| Carried from | Obligation | Discharged in |
|---|---|---|
| Plan 6 outcomes | "wire the bridge": dynamic/proxied registration + extensible state subtree | Tasks 4, 5, 8, 11 |
| Plan 6 outcomes | OSC carries no auth; `ohg.*` reachable on LAN | Task 5 (`OscExposure`) |
| Plan 6 outcomes | per-shell adapter conformance | Task 13 |
| Plan 6 outcomes | a conforming (abort-honoring) fetch fixture (rule 9) | Task 2 |
| Plan 6 outcomes | `restoreWarnings`/`pagingRefused` must be rendered somewhere | Task 8 (`log` events) + Task 11 (state node); rendering is Plan 7b |

## File Structure

**Engine (`show-engine/`)** — create:
- `src/host/protocol.ts` — message types + line codec (`encodeLine`, `decodeLine`).
- `src/host/stdioHostAdapter.ts` — `HostAdapter` that emits `hostCommand` lines.
- `src/host/nodeStateFs.ts` — `StateFs` over `fs/promises` with atomic rename.
- `src/host/nodeFetch.ts` — `FetchLike` over global `fetch`, honoring `signal`.
- `src/host/hostLoop.ts` — the request/event loop, runtime-injected (testable).
- `src/host/main.ts` — argv parsing, config load, process wiring (untested glue).
- tests beside each: `protocol.test.ts`, `stdioHostAdapter.test.ts`, `nodeStateFs.test.ts`, `nodeFetch.test.ts`, `hostLoop.test.ts`.
- `scripts/smoke-host.mjs` — spawns `dist/host/main.js`, expects a handshake.

Modify: `package.json` (bin, engines, `smoke:host`), `src/index.ts` (no change — host is not part of the barrel).

**Control (`native-shell/CoreVideoPro.Control/`)** — create `IControlActionProvider.cs`, `ControlCatalog.cs`; modify `ControlActionRegistry.cs` (shims), `ControlManifest.cs`, `ControlState.cs`, `Http/HttpControlRouter.cs`, `Http/HttpControlServer.cs`, `Osc/OscControlRouter.cs`, `Osc/OscControlServer.cs`, `Osc/OscFeedback.cs`. Tests: `ControlCatalogTests.cs`, `OhgStateFieldsTests.cs`, plus edits to existing router tests.

**ShowEngine (`native-shell/CoreVideoPro.ShowEngine/`, new)** — `ShowEngineProtocol.cs`, `IShowEngineChild.cs` + `ProcessShowEngineChild.cs`, `ShowEngineRestartPolicy.cs`, `ShowEngineSupervisor.cs`, `ShowEngineBridge.cs`, `ShowEngineModels.cs`, `ShowEnginePaths.cs`, `ShowConfig.cs` + `ShowConfigStore.cs` + `ShowConfigValidator.cs`, `ShowEngineLog.cs`. Tests in `CoreVideoPro.ShowEngine.Tests/` with `FakeShowEngineChild.cs`.

**WinUI (`native-shell/CoreVideoPro.WinUI/`)** — create `Services/IOhgHostFacade.cs`, `Services/OhgHostAdapter.cs`, `Services/StudioViewModelOhgFacade.cs`, `Services/OhgParticipantMapper.cs`; modify `Services/StudioControlSurface.cs`, `MainWindow.xaml.cs`. Tests: `OhgHostAdapterTests.cs`, `OhgParticipantMapperTests.cs`, `StudioControlSurfaceOhgForwardingTests.cs`.

**Packaging / CI** — create `scripts/sync-node-runtime-to-app.ps1`, `scripts/validate-show-engine.mjs`; modify `scripts/package-native.ps1`, `scripts/package-native-msix.ps1`, `.github/workflows/ci.yml`, `native-shell/CoreVideoPro.WinUI.sln`.

**Companion** — modify `companion-module-corevideopro/src/main.ts`, `variables.ts`, `feedbacks.ts`.

---

### Task 1: Engine host protocol codec

**Files:** Create `show-engine/src/host/protocol.ts`, `show-engine/src/host/protocol.test.ts`.

**Interfaces — Produces:**

```ts
export const PROTOCOL_VERSION = 1;

export type RequestType = "handshake" | "invoke" | "zoomEvent" | "activeSpeaker" | "capacity" | "ping" | "shutdown";

export type Request =
  | { id: string; type: "handshake" }
  | { id: string; type: "invoke"; action: string; args: unknown[] }
  | { id: string; type: "zoomEvent"; event: ZoomEvent }
  | { id: string; type: "activeSpeaker"; participantId: string }
  | { id: string; type: "capacity"; capacity: number }
  | { id: string; type: "ping" }
  | { id: string; type: "shutdown" };

export type HandshakePayload = {
  protocolVersion: number;
  engineVersion: string;
  generation: number;
  actions: readonly ActionDefinition[];
  fieldTemplates: readonly string[];
  snapshot: ShowSnapshot;
  fields: Record<string, ControlFieldValue>;
};

export type Response =
  | { id: string | null; ok: true; [key: string]: unknown }
  | { id: string | null; ok: false; error: { message: string } };

export type HostCommandName =
  | "assignSlot" | "applyLook" | "setPreview" | "cut" | "auto" | "setGallery" | "setNameplates" | "setQuestion";

export type Event =
  | ({ event: "handshake" } & HandshakePayload)
  | { event: "snapshot"; generation: number; revision: number; snapshot: ShowSnapshot; fields: Record<string, ControlFieldValue> }
  | { event: "hostCommand"; generation: number; seq: number; name: HostCommandName; args: unknown[] }
  | { event: "log"; level: "info" | "warn" | "error"; message: string };

/** Serialize one message to a single line WITHOUT the trailing newline. Never throws: a value that
 *  cannot be serialized (bigint, circular) becomes `{"id":null,"ok":false,"error":{"message":…}}`
 *  for responses, or a `log` event at level "error" for events. */
export function encodeLine(message: Request | Response | Event): string;

/** Parse one line. Returns a discriminated result; never throws. */
export type DecodeResult =
  | { kind: "request"; request: Request }
  | { kind: "malformed"; id: string | null; reason: string };
export function decodeRequest(line: string): DecodeResult;
```

**Behavior:**
- `encodeLine` uses `JSON.stringify` with a replacer that turns `Map` into `[key, value][]` arrays (spec §4.1: `ReadonlyMap` args serialize as pair arrays) and rejects `bigint` by throwing inside a try that produces the fallback described above. The output MUST NOT contain `\n` or `\r`; if the serialized string does (only possible via string values containing raw newlines — `JSON.stringify` escapes them, so this is a guard, not a path), replace with `\\n`.
- `decodeRequest`: non-JSON ⇒ `malformed` with `id: null`; JSON without a string `id` ⇒ `malformed` with `id: null`; string `id` but unknown/missing `type` ⇒ `malformed` with that `id` and reason `unknown request type '<t>'`; `invoke` without a string `action` or an array `args` ⇒ `malformed` with the id; `zoomEvent` without an object `event` with a string `kind` ⇒ malformed; `activeSpeaker` without a string `participantId` ⇒ malformed; `capacity` without a finite integer ⇒ malformed. **The codec does not validate `ZoomEvent` deeper than `kind`** — `ZoomIngest.apply` is the authority and the host loop passes it through.

**Tests (`protocol.test.ts`):**

```ts
import { describe, expect, it } from "vitest";
import { decodeRequest, encodeLine, PROTOCOL_VERSION } from "./protocol.js";

describe("protocol codec", () => {
  it("round-trips every request type through one line with no newline", () => {
    const requests = [
      { id: "r1", type: "handshake" },
      { id: "r2", type: "invoke", action: "ohg.program.cut", args: [] },
      { id: "r3", type: "zoomEvent", event: { kind: "left", participantId: "p1" } },
      { id: "r4", type: "activeSpeaker", participantId: "p1" },
      { id: "r5", type: "capacity", capacity: 10 },
      { id: "r6", type: "ping" },
      { id: "r7", type: "shutdown" }
    ] as const;
    for (const request of requests) {
      const line = encodeLine(request);
      expect(line).not.toMatch(/[\r\n]/);
      const decoded = decodeRequest(line);
      expect(decoded.kind).toBe("request");
      if (decoded.kind === "request") expect(decoded.request).toEqual(request);
    }
  });

  it("serializes Map args as [key, value] pairs so a C# reader needs no Map type", () => {
    const line = encodeLine({
      event: "hostCommand", generation: 1, seq: 1, name: "applyLook",
      args: [{ lookId: "teatime", scenePreset: "s1", hostSlot: 1, readerSlot: null, boxes: new Map([[1, 2], [2, null]]) }]
    });
    expect(JSON.parse(line).args[0].boxes).toEqual([[1, 2], [2, null]]);
  });

  it("never throws on bigint or circular values — it emits an error line instead", () => {
    const circular: Record<string, unknown> = {};
    circular.self = circular;
    const bigLine = encodeLine({ id: "x", ok: true, value: 10n } as never);
    const circLine = encodeLine({ event: "log", level: "info", message: circular as never });
    expect(JSON.parse(bigLine)).toMatchObject({ id: null, ok: false });
    expect(JSON.parse(circLine)).toMatchObject({ event: "log", level: "error" });
  });

  it("classifies malformed lines with the exact reason and keeps the id when it has one", () => {
    expect(decodeRequest("not json")).toEqual({ kind: "malformed", id: null, reason: expect.stringContaining("JSON") });
    expect(decodeRequest('{"type":"ping"}')).toMatchObject({ kind: "malformed", id: null });
    expect(decodeRequest('{"id":"q","type":"nope"}')).toEqual({ kind: "malformed", id: "q", reason: "unknown request type 'nope'" });
    expect(decodeRequest('{"id":"q","type":"invoke","action":5,"args":[]}')).toMatchObject({ kind: "malformed", id: "q" });
    expect(decodeRequest('{"id":"q","type":"capacity","capacity":"10"}')).toMatchObject({ kind: "malformed", id: "q" });
  });

  it("pins the protocol version", () => {
    expect(PROTOCOL_VERSION).toBe(1);
  });
});
```

**Mutations to run:**
- Remove the Map replacer → the pairs test reds (`boxes` becomes `{}`).
- Remove the try/catch in `encodeLine` → the bigint test reds with a thrown `TypeError`.
- Make `decodeRequest` accept a numeric `id` → the `{"type":"ping"}`-style test still passes, so ALSO run: change `'{"id":7,"type":"ping"}'` expectation locally to confirm it is classified malformed (a numeric id must be malformed — the C# side generates string ids and correlates by string).

**Steps:**
- [ ] Write `protocol.test.ts`; run `npx vitest run src/host/protocol.test.ts` — fails (module missing).
- [ ] Write `protocol.ts`; run again — passes. `npm run typecheck && npm run typecheck:tests`.
- [ ] Run the three mutations; revert.
- [ ] Commit: `feat(show-engine): host protocol codec (Plan 7a Task 1)`.

---

### Task 2: Stdio host adapter, Node state fs, abortable fetch

**Files:** Create `src/host/stdioHostAdapter.ts`, `src/host/nodeStateFs.ts`, `src/host/nodeFetch.ts` and their three tests.

**Interfaces — Produces:**

```ts
// stdioHostAdapter.ts
export type LineSink = (line: string) => void;
export class StdioHostAdapter implements HostAdapter {
  constructor(deps: { sink: LineSink; generation: number; capabilities: HostCapabilities });
  /** Monotonic per instance, starts at 1. */
  readonly seq: number;
  capabilities(): HostCapabilities;
  // every HostAdapter method emits one encodeLine({event:"hostCommand", …}) to sink
}
export const WINDOWS_SHELL_CAPABILITIES: HostCapabilities = {
  hasPreviewBus: true, maxGalleryCells: 16, transitions: ["cut", "fade", "dip", "wipe"]
};

// nodeStateFs.ts
export function nodeStateFs(): StateFs;   // readFile/writeFile (utf8), rename, mkdir({recursive:true})

// nodeFetch.ts
export const nodeFetch: FetchLike;        // wraps global fetch; passes `signal` through
```

**Behavior:**
- `StdioHostAdapter`: `assignSlot(slot, pid)` → `args: [slot, pid]`; `applyLook(p)` → `args: [p]` (Map serialized by the codec); `setPreview(source)` → `[source]`; `cut()` → `[]`; `auto(t)` → `[t ?? null]`; `setGallery(cells)` → `[cells]`; `setNameplates(plates)` → `[plates]`; `setQuestion(q)` → `[q]`. `seq` increments per call. `capabilities()` returns the constructor value.
- `nodeStateFs.writeFile` writes to `<path>.tmp-<pid>` then renames over `<path>` — atomic on Windows for same-volume renames, which `StateStore` already relies on via `rename`. Actually `StateStore` calls `rename` itself; `writeFile` must therefore be a plain write. **Read `persistence.ts` before implementing**: implement exactly the four `StateFs` members as thin wrappers, nothing cleverer. `mkdir` uses `{ recursive: true }` and swallows `EEXIST`.
- `nodeFetch(url, init)` → `fetch(url, { signal: init?.signal, headers: init?.headers })` and maps the `Response` to `FetchResponse` per `mukanaClient.ts`'s type (read it). An aborted signal must surface as a rejection whose `name === "AbortError"`.

**Tests:**

`stdioHostAdapter.test.ts`:
```ts
import { describe, expect, it } from "vitest";
import { StdioHostAdapter, WINDOWS_SHELL_CAPABILITIES } from "./stdioHostAdapter.js";

function rig() {
  const lines: string[] = [];
  const host = new StdioHostAdapter({ sink: (l) => lines.push(l), generation: 3, capabilities: WINDOWS_SHELL_CAPABILITIES });
  return { host, parsed: () => lines.map((l) => JSON.parse(l) as { event: string; generation: number; seq: number; name: string; args: unknown[] }) };
}

describe("StdioHostAdapter", () => {
  it("emits one hostCommand per call, in order, with the generation and a 1-based seq", () => {
    const { host, parsed } = rig();
    host.assignSlot(2, "p-9");
    host.cut();
    host.auto();
    host.setQuestion(null);
    expect(parsed()).toEqual([
      { event: "hostCommand", generation: 3, seq: 1, name: "assignSlot", args: [2, "p-9"] },
      { event: "hostCommand", generation: 3, seq: 2, name: "cut", args: [] },
      { event: "hostCommand", generation: 3, seq: 3, name: "auto", args: [null] },
      { event: "hostCommand", generation: 3, seq: 4, name: "setQuestion", args: [null] }
    ]);
  });

  it("serializes applyLook boxes and setGallery cells as pair arrays", () => {
    const { host, parsed } = rig();
    host.applyLook({ lookId: "l", scenePreset: "s", hostSlot: null, readerSlot: 4, boxes: new Map([[1, 7]]) });
    host.setGallery(new Map([[1, 2], [2, 0]]));
    expect(parsed()[0].args[0]).toEqual({ lookId: "l", scenePreset: "s", hostSlot: null, readerSlot: 4, boxes: [[1, 7]] });
    expect(parsed()[1].args[0]).toEqual([[1, 2], [2, 0]]);
  });

  it("reports the capabilities it was constructed with, unchanged", () => {
    const { host } = rig();
    expect(host.capabilities()).toEqual({ hasPreviewBus: true, maxGalleryCells: 16, transitions: ["cut", "fade", "dip", "wipe"] });
  });
});
```

`nodeFetch.test.ts` — **this is the rule-9 conforming fixture and must use a real local server**, not a mock:
```ts
import { describe, expect, it } from "vitest";
import { createServer } from "node:http";
import { nodeFetch } from "./nodeFetch.js";

describe("nodeFetch", () => {
  it("honors an AbortSignal: a hung endpoint rejects with AbortError when the signal fires", async () => {
    const server = createServer(() => { /* never respond */ });
    await new Promise<void>((r) => server.listen(0, "127.0.0.1", r));
    const address = server.address();
    if (address === null || typeof address === "string") throw new Error("no port");
    const controller = new AbortController();
    const pending = nodeFetch(`http://127.0.0.1:${address.port}/hang`, { signal: controller.signal });
    setTimeout(() => controller.abort(), 20);
    await expect(pending).rejects.toMatchObject({ name: "AbortError" });
    server.closeAllConnections();
    await new Promise<void>((r) => server.close(() => r()));
  });

  it("returns status and body text for a normal response", async () => {
    const server = createServer((_, res) => { res.statusCode = 200; res.end('{"ok":1}'); });
    await new Promise<void>((r) => server.listen(0, "127.0.0.1", r));
    const address = server.address();
    if (address === null || typeof address === "string") throw new Error("no port");
    const response = await nodeFetch(`http://127.0.0.1:${address.port}/x`, {});
    expect(response.status).toBe(200);
    expect(await response.text()).toBe('{"ok":1}');
    await new Promise<void>((r) => server.close(() => r()));
  });
});
```
(Adjust the `FetchResponse` member names to what `mukanaClient.ts` declares — read it first; the shape above assumes `status` and `text()`.)

`nodeStateFs.test.ts`: uses `os.tmpdir()` + a unique dir; asserts `mkdir` is idempotent, `writeFile`+`readFile` round-trips UTF-8 incl. non-ASCII, `rename` replaces an existing target, and a `StateStore` constructed over it can `save` then `load` (import `StateStore` and a minimal `PersistedShowState` — copy the fixture from `persistence.test.ts` and state its invariants next to it per rule 1).

**Mutations to run:**
- Drop `signal` from the `fetch` call → the abort test reds (times out / never rejects). **This is the rule-9 guard for the whole plan.**
- Start `seq` at 0 → ordering test reds.
- Serialize Map via `Object.fromEntries` → pair-array test reds.

**Steps:** tests first, run red, implement, run green, typecheck both configs, mutations, commit
`feat(show-engine): stdio host adapter, node state fs, abortable fetch (Plan 7a Task 2)`.

---

### Task 3: The host loop and `main.ts`

**Files:** Create `src/host/hostLoop.ts`, `src/host/hostLoop.test.ts`, `src/host/main.ts`, `scripts/smoke-host.mjs`; modify `package.json`.

**Interfaces — Produces:**

```ts
// hostLoop.ts
export type HostRuntime = {
  engine: ShowEngine;
  generation: number;
  engineVersion: string;
  sink: LineSink;                 // where response/event lines go
  now: () => number;              // for log timestamps only
};

export class HostLoop {
  constructor(runtime: HostRuntime);
  /** Emit the unsolicited handshake event. Call once after restore(). */
  announce(): void;
  /** Handle one stdin line. Never throws. Never awaits engine.tick(). */
  handleLine(line: string): void;
  /** One engine tick; emits `snapshot` iff revision changed. Never throws. Returns after tick settles. */
  tick(): Promise<void>;
  /** True after a `shutdown` request was handled. */
  readonly shuttingDown: boolean;
}
```

**Behavior:**
- `announce()` and the `handshake` request both emit the `HandshakePayload` (Task 1 type) with `actions: OHG_ACTIONS`, `fieldTemplates: OHG_FIELD_TEMPLATES`, `snapshot: engine.snapshot()`, `fields: projectControlFields(snapshot)`.
- `invoke` → `invokeAction(engine, action, args)` → response `{ id, ok: true, result }`. Then, if `engine.revision()` changed, emit a `snapshot` event immediately (an operator button must reflect within the same round trip, not at the next tick).
- `zoomEvent` → `engine.onZoomEvent(event)`; `activeSpeaker` → `engine.onActiveSpeaker(id)`; `capacity` → stored, compared to `config.capacity`, and a `log` `warn` emitted when they differ (`"host capacity <n> differs from config.capacity <m>"`); all respond `ok`.
- `ping` → `{ id, ok: true, revision }`. `shutdown` → `{ id, ok: true }` and `shuttingDown = true`.
- Malformed line → `{ id, ok: false, error: { message: reason } }` with the codec's id (null when unknown). Unknown type is the same path.
- An engine method that throws (e.g. `onZoomEvent` on a malformed event object) → response `ok: false` with the error's message, plus a `log` `error`. The loop never propagates.
- `tick()`: `await engine.tick()`; on rejection emit `log` `error` and return; else if revision changed emit `snapshot`. Revision tracking: keep `lastPublishedRevision`, update on every emitted snapshot (including the handshake's).
- Every `restoreWarnings` entry present in the first snapshot is emitted once as a `log` `warn` during `announce()`.

`main.ts` (glue, no unit test — the smoke script covers it):
- argv: `--config <path>` (required), `--generation <n>` (default 0). Missing config path ⇒ print a `log` error line and `process.exit(64)` (`EX_USAGE`).
- Read the file, `JSON.parse`, `parseShowEngineConfig(json.engine ?? json)` — the shell's file wraps the engine block under `engine` (spec §9); accept both. On any throw ⇒ `log` error, `process.exit(78)`.
- Construct `StateStore(config.statePath, { fs: nodeStateFs() })`, `StdioHostAdapter({ sink, generation, capabilities: WINDOWS_SHELL_CAPABILITIES })`, `MukanaClient` over `nodeFetch` only when `config.mukana !== null`, `ShowEngine({ config, host, clock: systemClock, store, mukana })`. `sink` writes `line + "\n"` to `process.stdout`.
- `await engine.restore()`; `loop.announce()`; `setInterval(() => void loop.tick(), 250)`; `readline` over `process.stdin` → `loop.handleLine`; on `shuttingDown` or stdin `close` ⇒ clear interval, `process.exit(0)` after `process.stdout.write("", () => …)` flush.
- `process.on("uncaughtException" | "unhandledRejection")` ⇒ write one `log` error line to **stderr** and exit 70.

`package.json` additions: `"bin": { "show-engine-host": "dist/host/main.js" }`, `"engines": { "node": ">=24" }`, script `"smoke:host": "node scripts/smoke-host.mjs"`. Root `package.json`: `"smoke:show-engine-host": "npm run smoke:host --workspace show-engine"`.

`scripts/smoke-host.mjs`: writes a temp config (copy `CONFORMANCE_CONFIG` from `dist/index.js` with `statePath` under `os.tmpdir()`), spawns `node dist/host/main.js --config <tmp>`, waits ≤5 s for a line whose `event === "handshake"` with `protocolVersion === 1` and `actions.length === 28`, sends `{"id":"s","type":"shutdown"}`, expects exit 0. Any other outcome ⇒ exit 1 with the collected stdout/stderr printed.

**Tests (`hostLoop.test.ts`):** build the rig the way `actionsPipeline.test.ts` does (`memoryFs`, `MockHost`, `CONFORMANCE_CONFIG`, `clock: { now: () => t }`), but with `StdioHostAdapter` as host so hostCommand lines land in the same `lines` array. State the fixture invariants next to it (rule 1): `CONFORMANCE_CONFIG.capacity` and its look ids are what `ohg.look.set` may reference.

```ts
it("announce emits a handshake with the full action registry and field templates", () => {
  const { loop, events } = rig();
  loop.announce();
  const hs = events().find((e) => e.event === "handshake");
  expect(hs).toMatchObject({ protocolVersion: 1, generation: 5 });
  expect(hs.actions.map((a: { id: string }) => a.id)).toEqual(OHG_ACTIONS.map((a) => a.id));
  expect(hs.fieldTemplates).toEqual(OHG_FIELD_TEMPLATES);
});

it("invoke answers with the ActionResult and publishes a snapshot in the same turn when revision moved", () => {
  const { loop, lines, revisionOf } = rig();
  loop.announce();
  const before = lines().length;
  loop.handleLine(JSON.stringify({ id: "a", type: "invoke", action: "ohg.look.set", args: [CONFORMANCE_LOOK_ID] }));
  const out = lines().slice(before).map((l) => JSON.parse(l));
  expect(out[0]).toEqual({ id: "a", ok: true, result: { kind: "ok" } });
  expect(out.some((m) => m.event === "snapshot")).toBe(true);
});

it("a refused or malformed action is a RESULT, never a transport failure", () => {
  const { loop, responses } = rig();
  loop.handleLine(JSON.stringify({ id: "b", type: "invoke", action: "ohg.nope", args: [] }));
  loop.handleLine(JSON.stringify({ id: "c", type: "invoke", action: "ohg.panelist.remove", args: ["0042"] }));
  expect(responses()[0]).toMatchObject({ id: "b", ok: true, result: { kind: "error" } });
  expect(responses()[1]).toMatchObject({ id: "c", ok: true, result: { kind: expect.stringMatching(/refused|error/) } });
});

it("a malformed line is answered, not fatal, and the loop keeps serving", () => {
  const { loop, responses } = rig();
  loop.handleLine("{{{");
  loop.handleLine(JSON.stringify({ id: "p", type: "ping" }));
  expect(responses()[0]).toMatchObject({ id: null, ok: false });
  expect(responses()[1]).toMatchObject({ id: "p", ok: true });
});

it("tick publishes a snapshot only when the revision changed", async () => {
  const { loop, events, engine } = rig();
  loop.announce();
  const n0 = events().filter((e) => e.event === "snapshot").length;
  await loop.tick();                                   // nothing changed
  expect(events().filter((e) => e.event === "snapshot").length).toBe(n0);
  loop.handleLine(JSON.stringify({ id: "z", type: "zoomEvent", event: { kind: "roster", participants: [participant("p1", "Ada 0042")] } }));
  await loop.tick();
  expect(events().filter((e) => e.event === "snapshot").length).toBe(n0 + 1);
});

it("hostCommand lines carry the loop's generation and appear in engine call order", async () => {
  const { loop, events } = rig();
  loop.announce();
  loop.handleLine(JSON.stringify({ id: "z", type: "zoomEvent", event: { kind: "roster", participants: [participant("p1", "Ada 0042")] } }));
  loop.handleLine(JSON.stringify({ id: "a", type: "invoke", action: "ohg.panelist.add", args: ["p1", 1] }));
  await loop.tick();
  const cmds = events().filter((e) => e.event === "hostCommand");
  expect(cmds.length).toBeGreaterThan(0);
  expect(cmds.every((c) => c.generation === 5)).toBe(true);
  expect(cmds.map((c) => c.seq)).toEqual(cmds.map((_, i) => i + 1));
});

it("capacity that disagrees with config is a warn log, not a failure", () => {
  const { loop, responses, events } = rig();
  loop.handleLine(JSON.stringify({ id: "k", type: "capacity", capacity: 99 }));
  expect(responses()[0]).toMatchObject({ id: "k", ok: true });
  expect(events().some((e) => e.event === "log" && e.level === "warn" && /capacity 99/.test(e.message))).toBe(true);
});

it("shutdown flips shuttingDown after answering", () => {
  const { loop, responses } = rig();
  loop.handleLine(JSON.stringify({ id: "s", type: "shutdown" }));
  expect(responses()[0]).toEqual({ id: "s", ok: true });
  expect(loop.shuttingDown).toBe(true);
});
```

`participant(id, rawName)` builds a full engine `Participant` (`online: true, videoOn: true, audioOn: true, handRaised: false, zoomRole: 0`).

**Mutations to run:**
- Remove the "snapshot after invoke when revision moved" branch → the invoke test reds.
- Remove revision tracking (always publish on tick) → the "only when changed" test reds.
- Wrap `invokeAction` so a thrown error propagates → the malformed-action test reds (the loop must stay alive).
- Emit hostCommands with `generation: 0` → the generation test reds.

**Steps:** tests red → implement `hostLoop.ts` → green → `main.ts` → `npm run build && npm run smoke:host` (must print the handshake OK line and exit 0) → typecheck both → mutations → commit
`feat(show-engine): the stdio host loop and process entry (Plan 7a Task 3)`.

---

### Task 4: `IControlActionProvider` and `ControlCatalog`

**Files:** Create `native-shell/CoreVideoPro.Control/IControlActionProvider.cs`, `ControlCatalog.cs`; `CoreVideoPro.Control.Tests/ControlCatalogTests.cs`, `FakeActionProvider.cs`. Modify `ControlActionRegistry.cs`.

**Interfaces — Produces:** exactly spec §5's `IControlActionProvider`, `OscExposure`, `ControlCatalog`. Additionally:

```csharp
public sealed class ControlCatalog
{
    // Providers are read live: Actions/TryGet/Contains reflect a provider's CURRENT Actions list.
    // The catalog subscribes to each provider's ActionsChanged and re-raises Changed.
    // Validation (duplicate ids, id regex) runs on construction AND on every ActionsChanged;
    // a violating provider's actions are EXCLUDED (not partially included) and
    // `LastValidationError` (string?) is set — never thrown from an event handler.
    public string? LastValidationError { get; }
    public static readonly System.Text.RegularExpressions.Regex ActionIdPattern =
        new("^[a-z][a-zA-Z0-9]*(\\.[a-z][a-zA-Z0-9]*)+$");
}
```

`ControlActionRegistry.TryGet/Contains/TryBind` become `=> ControlCatalog.StaticOnly.TryGet(...)` etc. `TryBind`'s coercion (`TryCoerce`) moves to the catalog as `internal static bool TryCoerce(...)` — **move-only**, the existing `ControlActionRegistryTests` must stay green unchanged.

**Tests (`ControlCatalogTests.cs`):**

```csharp
public sealed class ControlCatalogTests
{
    [Fact]
    public void StaticOnly_EqualsTheRegistry()
    {
        var catalog = ControlCatalog.StaticOnly;
        Assert.Equal(ControlActionRegistry.Actions.Select(a => a.Id), catalog.Actions.Select(a => a.Id));
        Assert.Equal(OscExposure.Lan, catalog.ExposureOf("transport.take"));
    }

    [Fact]
    public void ProviderActions_AppearAfterStaticOnes_AndBindLikeAnyOther()
    {
        var provider = new FakeActionProvider("show-engine", OscExposure.LoopbackOnly,
            new ControlAction("ohg.panelist.remove", "Remove", "…", new[] { new ControlParam("slot", ControlParamType.Int) }),
            new ControlAction("ohg.program.cut", "Cut", "…"));
        var catalog = new ControlCatalog(new[] { provider });

        Assert.Equal(ControlActionRegistry.Actions.Count + 2, catalog.Actions.Count);
        Assert.Equal("ohg.panelist.remove", catalog.Actions[^2].Id);
        Assert.True(catalog.TryBind("ohg.panelist.remove", new object?[] { "3" }, out var bound, out var error));
        Assert.Null(error);
        Assert.Equal(3, bound[0]);
        Assert.False(catalog.TryBind("ohg.panelist.remove", Array.Empty<object?>(), out _, out var missing));
        Assert.Contains("requires parameter 'slot'", missing);
        Assert.Equal(OscExposure.LoopbackOnly, catalog.ExposureOf("ohg.program.cut"));
        Assert.Equal(OscExposure.Lan, catalog.ExposureOf("transport.take"));
    }

    [Fact]
    public void ProviderMayNotShadowAStaticAction_OrUseAnInvalidId()
    {
        var shadowing = new FakeActionProvider("p", OscExposure.Lan, new ControlAction("transport.take", "x", "y"));
        var catalog = new ControlCatalog(new[] { shadowing });
        Assert.False(catalog.Contains("transport.take") && catalog.Actions.Count(a => a.Id == "transport.take") > 1);
        Assert.Contains("duplicate", catalog.LastValidationError, StringComparison.OrdinalIgnoreCase);

        var badId = new FakeActionProvider("p", OscExposure.Lan, new ControlAction("Ohg.Bad", "x", "y"));
        var catalog2 = new ControlCatalog(new[] { badId });
        Assert.False(catalog2.Contains("Ohg.Bad"));
        Assert.Contains("Ohg.Bad", catalog2.LastValidationError);
    }

    [Fact]
    public void FeedbackFields_AreStaticFieldsThenProviderTemplates()
    {
        var provider = new FakeActionProvider("p", OscExposure.Lan) { FeedbackFieldTemplates = new[] { "ohg/slot/{slot}/name" } };
        var catalog = new ControlCatalog(new[] { provider });
        Assert.Equal(ControlManifest.StateFields.Concat(new[] { "ohg/slot/{slot}/name" }), catalog.FeedbackFields);
    }

    [Fact]
    public void ActionsChanged_OnAProvider_IsVisibleImmediately_AndRaisesChanged()
    {
        var provider = new FakeActionProvider("p", OscExposure.Lan);
        var catalog = new ControlCatalog(new[] { provider });
        var raised = 0;
        catalog.Changed += (_, _) => raised++;
        Assert.False(catalog.Contains("ohg.program.cut"));
        provider.Set(new ControlAction("ohg.program.cut", "Cut", "…"));
        Assert.True(catalog.Contains("ohg.program.cut"));
        Assert.Equal(1, raised);
        provider.Set();   // provider went away
        Assert.False(catalog.Contains("ohg.program.cut"));
        Assert.Equal(2, raised);
    }
}
```

`FakeActionProvider`: constructor `(string id, OscExposure exposure, params ControlAction[] actions)`, `Set(params ControlAction[])` replaces and raises `ActionsChanged`, settable `FeedbackFieldTemplates`.

**Mutations to run:**
- Make duplicate detection skip providers → the shadow test reds.
- Cache provider actions at construction → the `ActionsChanged` test reds on `Contains` after `Set`.
- Return `OscExposure.Lan` unconditionally from `ExposureOf` → the bind test's exposure assertion reds.

**Steps:** tests red → implement → `dotnet test native-shell/CoreVideoPro.Control.Tests` (all pre-existing tests still green, count unchanged + 5) → mutations → commit
`feat(control): ControlCatalog composes the static registry with action providers (Plan 7a Task 4)`.

---

### Task 5: Routers, manifest, servers, OSC exposure, and the `ohg` state members

**Files:** Modify `Http/HttpControlRouter.cs`, `Http/HttpControlServer.cs`, `Osc/OscControlRouter.cs`, `Osc/OscControlServer.cs`, `Osc/OscFeedback.cs`, `ControlManifest.cs`, `ControlState.cs`. Tests: add to `OscControlRoutingTests.cs`, `HttpControlServerTests.cs`; create `OhgStateFieldsTests.cs`.

**Interfaces — Produces:**

```csharp
// Routers/servers gain a catalog parameter (default StaticOnly keeps every existing test compiling):
public HttpControlRouter(IControlSurface surface, ControlCatalog? catalog = null);
public OscControlRouter(IControlSurface surface, OscAddressMap? addressMap = null, ControlCatalog? catalog = null);
public Task<ControlInvokeResult?> RouteAsync(OscMessage message, IPEndPoint? sender = null, CancellationToken ct = default);
public HttpControlServer(IControlSurface surface, HttpControlServerOptions? options = null, ControlCatalog? catalog = null);
public OscControlServer(IControlSurface surface, OscControlServerOptions? options = null, ControlCatalog? catalog = null);
public static ControlManifest Build(OscAddressMap? addressMap = null, ControlCatalog? catalog = null);

// ControlState additions (spec §7):
public System.Text.Json.JsonElement? Ohg { get; init; }
public IReadOnlyDictionary<string, System.Text.Json.JsonElement>? OhgFields { get; init; }
public string OhgEngineHealth { get; init; } = "stopped";
public string OhgShadowLastCommand { get; init; } = string.Empty;

// OscFeedback: after the existing fields —
//   Str("ohg/health/engine", state.OhgEngineHealth); Str("ohg/shadow/lastCommand", state.OhgShadowLastCommand);
//   foreach (field, value) in state.OhgFields ?? empty: encode by JsonValueKind —
//     True/False → int 1/0 ; Number → int when TryGetInt32 else float ; String → string ; Null/other → skipped.
// ControlManifest.StateFields gains "ohg/health/engine", "ohg/shadow/lastCommand".
```

**Behavior:** the HTTP router calls `_catalog.TryBind`; `/manifest` calls `ControlManifest.Build(catalog: _catalog)`. The OSC router: `!_catalog.Contains(id)` ⇒ `null` (unchanged); then **before bind**, if `_catalog.ExposureOf(id) == LoopbackOnly && sender is not null && !IPAddress.IsLoopback(sender.Address)` ⇒ `Fail($"'{id}' is not exposed to LAN OSC senders (set COREVIDEO_OSC_OHG_LAN=1)")`. `OscControlServer.ReceiveLoopAsync` passes `received.RemoteEndPoint`. A `null` sender (unit tests, in-process callers) is treated as loopback.

**Tests:**

```csharp
// OscControlRoutingTests additions
[Fact]
public async Task Router_RefusesLoopbackOnlyActionsFromLanSenders_AndLogsNothingSilently()
{
    var surface = new FakeControlSurface();
    var provider = new FakeActionProvider("p", OscExposure.LoopbackOnly, new ControlAction("ohg.program.cut", "Cut", "…"));
    var router = new OscControlRouter(surface, catalog: new ControlCatalog(new[] { provider }));

    var lan = await router.RouteAsync(new OscMessage("/cvp/ohg/program/cut"), new IPEndPoint(IPAddress.Parse("192.168.1.20"), 5000));
    Assert.NotNull(lan);
    Assert.False(lan!.Ok);
    Assert.Contains("not exposed to LAN", lan.Error);
    Assert.Empty(surface.Invocations);

    var local = await router.RouteAsync(new OscMessage("/cvp/ohg/program/cut"), new IPEndPoint(IPAddress.Loopback, 5000));
    Assert.True(local!.Ok);
    Assert.Single(surface.Invocations);
}

[Fact]
public async Task Router_InvokesEveryProviderActionThroughItsOscAddress()   // authoring rule 10
{
    var surface = new FakeControlSurface();
    var actions = new[]
    {
        new ControlAction("ohg.a.one", "1", "…"),
        new ControlAction("ohg.a.two", "2", "…", new[] { new ControlParam("pin", ControlParamType.String) }),
        new ControlAction("ohg.a.three", "3", "…", new[] { new ControlParam("on", ControlParamType.Bool) })
    };
    var catalog = new ControlCatalog(new[] { new FakeActionProvider("p", OscExposure.Lan, actions) });
    var router = new OscControlRouter(surface, catalog: catalog);
    var map = new OscAddressMap();
    var manifest = ControlManifest.Build(catalog: catalog);

    foreach (var action in actions)
    {
        object[] args = action.Params.Count == 0 ? Array.Empty<object>() : new object[] { action.Params[0].Type == ControlParamType.Bool ? 1 : "0042" };
        var result = await router.RouteAsync(new OscMessage(map.ActionIdToAddress(action.Id), args));
        Assert.True(result!.Ok, action.Id);
        Assert.Contains(manifest.Actions, m => m.Id == action.Id && m.OscAddress == map.ActionIdToAddress(action.Id));
    }
    Assert.Equal(actions.Select(a => a.Id), surface.Invocations.Select(i => i.ActionId));
    Assert.Equal("0042", surface.Invocations[1].Args[0]);   // string param stays a string — no leading-zero loss
    Assert.Equal(true, surface.Invocations[2].Args[0]);
}
```

`HttpControlServerTests` addition: `POST /invoke` for a provider action reaches the surface and `GET /manifest` lists it with `feedbackFields` containing the provider template; `GET /state` serializes `Ohg` under `"ohg"` **verbatim** (construct a `ControlState` with `Ohg = JsonDocument.Parse("{\"revision\":7,\"slots\":[]}").RootElement.Clone()` and assert the response contains `"ohg":{"revision":7,"slots":[]}`).

`OhgStateFieldsTests.cs` (the spec §7 agreement test): construct a `ControlState` with `OhgFields` = `{ "ohg/slot/1/name": "Ada", "ohg/slot/1/tally": true, "ohg/program/mode": "cut", "ohg/queue/current": null, "ohg/gallery/smart": false }` (JsonElements), encode with `OscFeedback.Encode`, and assert: `/cvp/state/ohg/slot/1/name` = `"Ada"`, `/cvp/state/ohg/slot/1/tally` = `1`, `/cvp/state/ohg/gallery/smart` = `0`, no message for the null field, and `/cvp/state/ohg/health/engine` = `"stopped"`. Then assert `ControlManifest.StateFields` contains both shell-owned `ohg/*` names and that **every** `ohg/*` field name emitted by `Encode` for this state either equals a `StateFields` entry or matches a template from `OHG_FIELD_TEMPLATES` after `{slot}` substitution — copy the ten template strings from `show-engine/src/controlState.ts` into the test as literals (they are the contract; a drift is a red test on purpose).

**Mutations to run:**
- Remove the `IsLoopback` check → LAN test reds.
- Change the OSC router to bind before the exposure check and invoke anyway → LAN test reds on `Invocations`.
- Drop one of the three provider actions from the router's catalog lookup (e.g. skip ids ending in `.three`) → rule-10 test reds on the sequence assertion.
- Encode bool `OhgFields` as strings → the tally assertion reds.

**Steps:** tests red → implement → `dotnet test native-shell/CoreVideoPro.Control.Tests` → mutations → commit
`feat(control): catalog-driven routers, OSC exposure policy, and the ohg state members (Plan 7a Task 5)`.

---

### Task 6: `CoreVideoPro.ShowEngine` project — protocol, child abstraction, supervisor core

**Files:** Create project `native-shell/CoreVideoPro.ShowEngine/CoreVideoPro.ShowEngine.csproj` (`net9.0`, refs `CoreVideoPro.Control`), `ShowEngineProtocol.cs`, `ShowEngineModels.cs`, `IShowEngineChild.cs`, `ProcessShowEngineChild.cs`, `ShowEngineLog.cs`, `ShowEngineSupervisor.cs`; test project `native-shell/CoreVideoPro.ShowEngine.Tests/` (same packages as Control.Tests) with `FakeShowEngineChild.cs`, `ShowEngineProtocolTests.cs`, `ShowEngineSupervisorTests.cs`. Add both projects to `CoreVideoPro.WinUI.sln`.

**Interfaces — Produces:**

```csharp
namespace CoreVideoPro.ShowEngine;

// ShowEngineModels.cs
public sealed record ShowEngineActionParam(string Name, string Type, bool Required, string Description);
public sealed record ShowEngineActionDefinition(string Id, string Title, string Description, IReadOnlyList<ShowEngineActionParam> Params);
public sealed record ShowEngineHandshake(int ProtocolVersion, string EngineVersion, int Generation,
    IReadOnlyList<ShowEngineActionDefinition> Actions, IReadOnlyList<string> FieldTemplates,
    JsonElement Snapshot, IReadOnlyDictionary<string, JsonElement> Fields);
public sealed record ShowEngineSnapshot(int Generation, long Revision, JsonElement Snapshot, IReadOnlyDictionary<string, JsonElement> Fields);
public sealed record ShowEngineHostCommand(int Generation, long Seq, string Name, JsonElement Args);
public sealed record ShowEngineLogLine(string Level, string Message);
public enum ShowEngineState { Stopped, Starting, Running, Recovering, Failed }
public sealed record ShowEngineHealth(ShowEngineState State, int Generation, int RestartCount, string? LastError, DateTimeOffset? LastCrashAt);
public sealed record ShowEngineActionResult(string Kind, string? Reason, string? Message);   // "ok" | "refused" | "error"

// ShowEngineProtocol.cs — pure, static
public static class ShowEngineProtocol
{
    public static readonly JsonSerializerOptions Json;   // camelCase, case-insensitive, WhenWritingNull
    public static string EncodeRequest(string id, string type, object? payload = null);  // payload's public props merged at top level
    public enum LineKind { Response, Handshake, Snapshot, HostCommand, Log, Unknown, Malformed }
    public static LineKind Classify(JsonDocument doc);
    public static bool TryParseHandshake(JsonElement root, out ShowEngineHandshake handshake, out string? error);
    public static ShowEngineSnapshot ParseSnapshot(JsonElement root);
    public static ShowEngineHostCommand ParseHostCommand(JsonElement root);
    public static ShowEngineLogLine ParseLog(JsonElement root);
    public static ShowEngineActionResult ParseActionResult(JsonElement responseRoot);   // reads "result"
}

// IShowEngineChild.cs — the process seam the supervisor is tested through
public interface IShowEngineChild : IDisposable
{
    int Generation { get; }
    Task WriteLineAsync(string line, CancellationToken ct);
    /// Completes with the next stdout line, or null at EOF.
    Task<string?> ReadLineAsync(CancellationToken ct);
    /// Completes with the exit code when the process ends.
    Task<int> Exited { get; }
    void Kill();
}
public interface IShowEngineChildFactory { IShowEngineChild Spawn(int generation, ShowEngineSpawnRequest request); }
public sealed record ShowEngineSpawnRequest(string NodeExe, string EntryScript, string ConfigPath, string WorkingDirectory, IReadOnlyDictionary<string, string> Environment);

// ProcessShowEngineChild.cs — the real one (ProcessStartInfo per spec §6.3; stderr → ShowEngineLog)

// ShowEngineLog.cs — bounded append (256 KB, keep the newest half on overflow) to
//   %LOCALAPPDATA%\CoreVideoPro\show-engine.log; `Append(string line)`; injectable path for tests.

// ShowEngineSupervisor.cs
public sealed class ShowEngineSupervisorOptions
{
    public TimeSpan RequestTimeout { get; init; } = TimeSpan.FromSeconds(4);
    public TimeSpan HandshakeTimeout { get; init; } = TimeSpan.FromSeconds(15);
    public TimeSpan HeartbeatInterval { get; init; } = TimeSpan.FromSeconds(1);
    public int MissedHeartbeatsBeforeHang { get; init; } = 2;
}
public sealed class ShowEngineSupervisor : IDisposable
{
    public ShowEngineSupervisor(IShowEngineChildFactory factory, ShowEngineRestartPolicy policy,
        Func<TimeSpan, CancellationToken, Task> delay, Func<DateTimeOffset> now, ShowEngineSupervisorOptions? options = null);
    public ShowEngineHealth Health { get; }
    public event Action<ShowEngineHealth>? HealthChanged;
    public event Action<ShowEngineHandshake>? Handshaken;      // per generation
    public event Action<ShowEngineSnapshot>? SnapshotReceived;
    public event Action<ShowEngineHostCommand>? HostCommandReceived;   // only for the CURRENT generation
    public event Action<ShowEngineLogLine>? LogReceived;
    public long StaleHostCommandsDropped { get; }
    public Task StartAsync(ShowEngineSpawnRequest request, CancellationToken ct);   // returns after first handshake or Failed
    public Task StopAsync();                                     // shutdown → grace → kill; State = Stopped
    public Task RestartAsync(ShowEngineSpawnRequest request, CancellationToken ct);   // explicit operator restart; resets the policy
    public Task<JsonDocument> SendAsync(string type, object? payload, CancellationToken ct);   // throws InvalidOperationException when not Running
}
```

`ShowEngineRestartPolicy` is Task 7; for Task 6 stub it as a class with `TimeSpan? NextDelay(DateTimeOffset now)` returning `TimeSpan.Zero` always and `void RecordHealthy(DateTimeOffset)`, `void Reset()` no-ops — Task 7 replaces the body and tests it.

**Behavior (supervisor core, this task):**
- `StartAsync`: State `Starting`; spawn generation `n+1`; reader loop on a background task; wait for an unsolicited `handshake` line up to `HandshakeTimeout`; if none, send an explicit `handshake` request; on success validate `ProtocolVersion == 1` (else `Failed`, `LastError = "unsupported protocol version <v>"`, kill, **no restart**); State `Running`; raise `Handshaken`.
- Reader loop classifies each line: `Response` ⇒ complete the pending TCS by `id` **only if the line's child is the current child** (generation guard); `Snapshot`/`HostCommand`/`Log` ⇒ raise the event (HostCommand: drop and count when `Generation != current`); `Malformed` ⇒ `LogReceived("warn", …)`.
- Heartbeat: every `HeartbeatInterval` (via the injected `delay`), `SendAsync("ping")`; `MissedHeartbeatsBeforeHang` consecutive timeouts ⇒ treat as exit code `-1` (hang) ⇒ recovery path.
- Child exit (or hang) while not stopping: reject all pending with `InvalidOperationException("Show engine exited.")`; State `Recovering`; `RestartCount++`; `LastCrashAt = now()`; ask `policy.NextDelay(now())`; `null` ⇒ `Failed`; else await `delay(d)` then respawn and re-handshake. Exit code **78** ⇒ `Failed` immediately with `LastError = "show engine rejected its config (exit 78) — see show-engine.log"` and no delay (spec §9).
- `SendAsync`: `id = $"se-{Interlocked.Increment}"`; the timeout covers write + response; writes are serialized by a `SemaphoreSlim`.
- `StopAsync`: send `shutdown` (ignore failure), wait ≤1.5 s for `Exited`, else `Kill()`; State `Stopped`; the reader loop ends.

**`FakeShowEngineChild`**: in-memory: `Queue<string>` of scripted stdout lines plus `Channel<string>`; `WriteLineAsync` records to `Written` and, when a scripted `Func<string, IEnumerable<string>>? Responder` is set, enqueues its replies; `Complete(int exitCode)` ends `ReadLineAsync` with null and completes `Exited`. `FakeChildFactory` records each `Spawn` and hands out a fresh fake per generation; tests preload each fake's first line with a handshake.

**Tests (`ShowEngineSupervisorTests.cs`)** — use a `Deferred` delay helper: `delay = (d, ct) => { delays.Add(d); return Task.CompletedTask; }` and `now` from a mutable `DateTimeOffset`.

```csharp
[Fact] public async Task Start_AcceptsTheUnsolicitedHandshake_AndBecomesRunning()
// factory's fake preloaded with HandshakeLine(generation: 1); assert Health.State == Running,
// Handshaken raised with 28 actions when the line carries them (use a 2-action handshake for speed),
// Written contains no explicit "handshake" request.

[Fact] public async Task Start_FallsBackToAnExplicitHandshakeRequest()
// fake preloaded with nothing; Responder answers type "handshake" with the handshake payload as a
// RESPONSE (id echoed); assert Running.

[Fact] public async Task UnsupportedProtocolVersion_IsFailed_WithoutRestart()
// handshake with protocolVersion 2 → Failed, LastError contains "protocol version 2", factory.SpawnCount == 1.

[Fact] public async Task Send_CorrelatesById_AndTimesOut()
// Responder echoes {"id":…,"ok":true,"revision":9} for "ping"; assert SendAsync returns revision 9.
// Second: Responder answers nothing → SendAsync throws TimeoutException (the injected delay is used
// for the timeout too: make `delay` complete immediately so the timeout fires).

[Fact] public async Task StaleGeneration_ResponsesAndHostCommands_AreDropped()
// gen 1 running; simulate exit → respawn gen 2 handshaken; then push onto gen 1's fake (still
// referenced by the test) a response line for a pending id and a hostCommand with generation 1;
// assert the pending was not completed by it and StaleHostCommandsDropped == 1 and the
// HostCommandReceived event did not fire. Then push a gen-2 hostCommand and assert it fires.

[Fact] public async Task Exit78_IsFailedImmediately_NoDelay()
// fake.Complete(78) → Failed, delays is empty, SpawnCount == 1, LastError mentions "exit 78".

[Fact] public async Task Hang_TwoMissedHeartbeats_TriggersRecovery()
// Responder ignores "ping"; drive the heartbeat by completing the injected delay twice; assert the
// fake was killed (fake.Killed == true) and Health.State == Recovering (policy stub returns Zero, so
// a new spawn occurs: SpawnCount == 2).

[Fact] public async Task Stop_SendsShutdown_ThenIsStopped()
// Responder answers shutdown and calls Complete(0); assert Written last line has "shutdown",
// Health.State == Stopped, and fake.Killed == false.
```

**Mutations to run:**
- Remove the generation guard on responses → stale test reds (pending completes).
- Remove the `Generation != current` drop on host commands → stale test reds on the event.
- Treat exit 78 like any other exit → exit-78 test reds (a delay is recorded / SpawnCount 2).
- Skip the protocol version check → version test reds.

**Steps:** create projects + sln entries → tests red → implement → `dotnet test native-shell/CoreVideoPro.ShowEngine.Tests` → mutations → commit
`feat(show-engine-shell): CoreVideoPro.ShowEngine project — protocol, child seam, supervisor core (Plan 7a Task 6)`.

---

### Task 7: Restart policy with backoff

**Files:** Create `ShowEngineRestartPolicy.cs` (replace the Task 6 stub), `ShowEngineRestartPolicyTests.cs`; add one supervisor test.

**Interfaces — Produces:**

```csharp
public sealed class ShowEngineRestartPolicy
{
    public ShowEngineRestartPolicy(int maxConsecutiveFailures = 5, TimeSpan? healthyResetAfter = null /* 60 s */);
    public static readonly TimeSpan[] Delays = { 1s, 2s, 4s, 8s, 16s, 30s };   // 30 s repeats
    /// Called when the child died. Returns the delay before the next spawn, or null when the
    /// consecutive-failure budget is exhausted.
    public TimeSpan? NextDelay(DateTimeOffset now);
    /// Called when a handshake succeeded (the child is Running as of `now`).
    public void RecordRunning(DateTimeOffset now);
    /// Called from the heartbeat while Running; resets the failure counter once the child has been
    /// running for `healthyResetAfter`.
    public void RecordHealthy(DateTimeOffset now);
    public int ConsecutiveFailures { get; }
    public void Reset();   // operator restart
}
```

**Tests:**

```csharp
[Fact] public void Delays_Escalate_ThenCapAt30s()
// t0; NextDelay ×7 → 1,2,4,8,16,30,30 (seconds); the 8th → null (max 5 consecutive? no — see next test).
```
Careful: the budget is **5 consecutive failures** ⇒ delays for failures 1..5 are 1,2,4,8,16 and the 6th call returns `null`. Write the test to that: `[1,2,4,8,16]` then `null`. Keep `Delays` with 30 s entries for a raised budget (`maxConsecutiveFailures: 8` ⇒ 1,2,4,8,16,30,30,30 then null) and test that too.

```csharp
[Fact] public void SixtySecondsOfRunning_ResetsTheBudget()
// NextDelay twice (1s,2s); RecordRunning(t); RecordHealthy(t+59s) → ConsecutiveFailures still 2 and
// NextDelay → 4s; RecordRunning(t); RecordHealthy(t+60s) → ConsecutiveFailures 0; NextDelay → 1s.

[Fact] public void Reset_ClearsTheBudget()
```

Supervisor test addition: `Recovery_UsesThePolicyDelay_AndFailsWhenExhausted` — wire a real policy with `maxConsecutiveFailures: 2`; complete the fake with exit 1 three times (each respawn's fake immediately handshakes then exits); assert `delays == [1s, 2s]`, `SpawnCount == 3`, final `Health.State == Failed`, `RestartCount == 3`. Also assert the heartbeat calls `RecordHealthy` (advance `now` by 61 s across two heartbeats while running and assert `ConsecutiveFailures == 0` afterwards).

**Mutations to run:**
- Make `RecordHealthy` reset unconditionally → the 59 s assertion reds.
- Return `Delays[0]` always → escalation test reds.
- Off-by-one on the budget (`>` vs `>=`) → the `null` assertion reds.

**Steps:** red → implement → green → mutations → commit
`feat(show-engine-shell): exponential restart policy with healthy reset (Plan 7a Task 7)`.

---

### Task 8: `ShowEngineBridge`

**Files:** Create `ShowEngineBridge.cs`, `ShowEngineBridgeTests.cs`.

**Interfaces — Produces:** spec §6.1 verbatim:

```csharp
public sealed record ShowEngineParticipant(string ParticipantId, string RawName, bool Online, bool VideoOn, bool AudioOn, bool HandRaised, int ZoomRole);

public sealed class ShowEngineBridge : IControlActionProvider, IDisposable
{
    public ShowEngineBridge(ShowEngineSupervisor supervisor, OscExposure oscExposure /* from COREVIDEO_OSC_OHG_LAN */);
    // IControlActionProvider
    public string ProviderId => "show-engine";
    public IReadOnlyList<ControlAction> Actions { get; }              // empty until handshake; empty again on Stopped/Failed
    public IReadOnlyList<string> FeedbackFieldTemplates { get; }      // from handshake
    public OscExposure DefaultOscExposure { get; }
    public event EventHandler? ActionsChanged;
    // bridge
    public Task<ControlInvokeResult> InvokeAsync(string actionId, IReadOnlyList<object?> boundArgs, CancellationToken ct);
    public ShowEngineSnapshot? Latest { get; }
    public event EventHandler<ShowEngineSnapshot>? SnapshotChanged;
    public event EventHandler<ShowEngineHostCommand>? HostCommand;
    public ShowEngineHealth Health { get; }
    public event EventHandler<ShowEngineHealth>? HealthChanged;
    public event EventHandler<ShowEngineLogLine>? Log;
    public void PublishRoster(IReadOnlyList<ShowEngineParticipant> roster);   // sends zoomEvent roster (fire-and-forget, logged on failure)
    public void PublishActiveSpeaker(string? participantId);                  // sends only on change; null clears (sends nothing — engine has no "no speaker" event; recorded)
    public void PublishCapacity(int capacity);
    public Task StartAsync(ShowEngineSpawnRequest request, CancellationToken ct);
    public Task StopAsync();
    public Task RestartAsync(CancellationToken ct);   // uses the last request
    public static IReadOnlyList<ControlAction> ToControlActions(IReadOnlyList<ShowEngineActionDefinition> defs);   // pure
}
```

**Behavior:**
- `ToControlActions`: `"string"→String`, `"int"→Int`, `"double"→Double`, `"bool"→Bool`; any other type string ⇒ throw `InvalidOperationException($"unknown param type '{t}' on {id}")` — a manifest we don't understand must not be half-registered.
- On `Handshaken`: set `Actions`/`FeedbackFieldTemplates`; if a previous manifest existed and differs (compare ids + param types in order), raise `Log("warn", "engine manifest changed between generations: …")`; raise `ActionsChanged`; then **re-arm**: `PublishCapacity(lastCapacity)` and `PublishRoster(lastRoster)` if any were ever published, then `PublishActiveSpeaker(last)`.
- `InvokeAsync`: not Running ⇒ `Fail($"OHG show engine is {Health.State.ToString().ToLowerInvariant()}")`; else `SendAsync("invoke", new { action, args })`, parse `result`: `ok`⇒`Success`; `refused`⇒`Fail(reason)`; `error`⇒`Fail(message)`; a transport exception ⇒ `Fail(ex.Message)`.
- `SnapshotReceived` ⇒ `Latest = s` and `SnapshotChanged`. `HostCommandReceived` ⇒ `HostCommand`. Health/Log pass through. On `Stopped`/`Failed` health, clear `Actions` and raise `ActionsChanged` (Companion sees the actions vanish rather than 422 on every press).

**Tests:** drive with the Task 6 fake child through a real supervisor (no mocks of the supervisor).

```csharp
[Fact] public async Task Handshake_PublishesActions_AndFieldTemplates_AsControlActions()
// handshake with two actions incl. a string "pin" param and a bool; assert Actions[0].Params[0].Type == String, ActionsChanged raised once.

[Fact] public async Task Invoke_MapsEachResultKind()
// Responder for "invoke": action "ohg.program.cut" → {"result":{"kind":"ok"}}; "ohg.look.nextGuest" → refused w/ reason "manual box fill"; "ohg.x" → error w/ message "unknown action".
// assert Success / Fail("manual box fill") / Fail("unknown action").

[Fact] public async Task Invoke_WhenNotRunning_FailsWithTheHealthState_WithoutSending()

[Fact] public async Task Rearm_AfterRespawn_SendsCapacityThenRosterThenSpeaker()
// PublishCapacity(10), PublishRoster([a]), PublishActiveSpeaker("a"); kill gen 1; gen 2 handshakes;
// assert gen-2 fake.Written, in order, contains types capacity, zoomEvent(roster), activeSpeaker.

[Fact] public async Task ActiveSpeaker_IsSentOnlyOnChange()

[Fact] public async Task ManifestDrift_BetweenGenerations_IsLogged_AndTheNewOneWins()

[Fact] public async Task StoppedOrFailed_ClearsActions()

[Fact] public void ToControlActions_RejectsUnknownParamTypes()
```

**Mutations to run:**
- Re-arm sends roster before capacity → order test reds.
- Send active speaker unconditionally → change test reds.
- Map `refused` to `Success` → result-kind test reds.
- Keep `Actions` on `Failed` → clear test reds.

**Steps:** red → implement → green → mutations → commit
`feat(show-engine-shell): ShowEngineBridge — action provider, invoke, snapshot, re-arm (Plan 7a Task 8)`.

---

### Task 9: Paths, config store, validator

**Files:** Create `ShowEnginePaths.cs`, `ShowConfig.cs`, `ShowConfigStore.cs`, `ShowConfigValidator.cs`; tests `ShowEnginePathsTests.cs`, `ShowConfigStoreTests.cs`, `ShowConfigValidatorTests.cs`.

**Interfaces — Produces:**

```csharp
public sealed record ShowEngineResolvedPaths(string NodeExe, string EntryScript, string WorkingDirectory, string Source /* "env" | "packaged" | "dev" */);
public static class ShowEnginePaths
{
    public static IReadOnlyList<(string NodeExe, string EntryScript, string Source)> Candidates(string appBaseDir, string repoRoot, Func<string, string?> getEnv, Func<string?> nodeOnPath);
    public static ShowEngineResolvedPaths? Resolve(Func<string, bool> fileExists, string appBaseDir, string repoRoot, Func<string, string?> getEnv, Func<string?> nodeOnPath);
}

public sealed class ShowConfig            // spec §9
{
    public const int CurrentVersion = 1;
    public int Version { get; init; } = CurrentVersion;
    public JsonElement Engine { get; init; }                 // opaque to the shell except capacity/statePath/looks[].scenePreset
    public ShowShellConfig Shell { get; init; } = new();
}
public sealed class ShowShellConfig
{
    public bool DriveHost { get; init; }                     // default false = shadow mode
    public ShowPresetScenes Presets { get; init; } = new();
    public string DefaultTransition { get; init; } = "cut";
    public string? TallyUrl { get; init; }
}
public sealed class ShowPresetScenes { public string? Solo { get; init; } public string? ActiveSpeaker { get; init; } public string? Black { get; init; } public string? Gallery { get; init; } }

public sealed class ShowConfigStore
{
    public const string DefaultFileName = "ohg-show-config.json";
    public ShowConfigStore(string folderPath, string? fileName = null);
    public string FilePath { get; }
    public bool Exists { get; }
    public ShowConfig? Load(out string? error);     // null + error on parse failure or unknown version
    public void Save(ShowConfig config);            // atomic: tmp + File.Move(overwrite)
    /// Ensures engine.statePath is set (default: <folder>\ohg-show-state.json) — returns the JSON text the engine will be given.
    public string MaterializeEngineConfig(ShowConfig config);
}

public static class ShowConfigValidator
{
    /// Returns the FIRST problem, or null. sceneIds = the scenes the app currently has.
    public static string? Validate(ShowConfig config, IReadOnlySet<string> sceneIds, int hostCapacity = 10);
}
```

**Behavior:**
- `Candidates` order (spec §6.4): env pair (only when **both** env vars are set), packaged (`<app>\node\node.exe` + `<app>\show-engine\dist\host\main.js`), dev (`nodeOnPath()` + `<repo>\show-engine\dist\host\main.js`). `Resolve` returns the first whose two files exist.
- `Validate` checks, in order: `engine` is an object; `engine.capacity` is an integer equal to `hostCapacity` (message `"config.capacity must be 10 (the Show Input count); found <n>"`); every `engine.looks[i].scenePreset` is in `sceneIds` (`"look '<id>' names scene '<preset>' which does not exist"`); each of the four presets, **when set**, is in `sceneIds`; `defaultTransition` ∈ {cut,fade,dip,wipe}. Presets left `null` are allowed here — the adapter refuses at use time (Task 10).

**Tests:** straightforward table tests per rule; `ShowConfigStoreTests` uses a temp folder: round-trip, unknown version ⇒ error mentioning the version, `MaterializeEngineConfig` injects `statePath` only when absent and preserves an existing one, `Save` leaves no `.tmp` file.

**Mutations to run:**
- `Resolve` picks env when only one var is set → candidates test reds.
- Validator accepts capacity 9 → capacity test reds.
- `MaterializeEngineConfig` overwrites an existing `statePath` → preservation test reds.

**Steps:** red → implement → green → mutations → commit
`feat(show-engine-shell): paths resolver, show config store and validator (Plan 7a Task 9)`.

---

### Task 10: `IOhgHostFacade` and `OhgHostAdapter` (WinUI, no ViewModel yet)

**Files:** Create `native-shell/CoreVideoPro.WinUI/Services/IOhgHostFacade.cs`, `Services/OhgHostAdapter.cs`; test `CoreVideoPro.WinUI.Tests/OhgHostAdapterTests.cs` with `FakeOhgHostFacade`.

**Interfaces — Produces:**

```csharp
public interface IOhgHostFacade
{
    int ShowInputCount { get; }                                            // 10
    bool AssignZoomParticipant(int slot, string? participantId);          // false when slot out of range
    bool SetInputDisplayName(int slot, string name);
    bool SetInputLowerThirdTitle(int slot, string title);
    bool SceneExists(string sceneId);
    /// Cue to preview and rewrite routes by id. Returns the ids of routes that were REQUESTED but not found.
    IReadOnlyList<string> CueSceneWithRoutes(string sceneId, IReadOnlyDictionary<string, int?> routeSlots);
    bool CanTake { get; }
    Task<bool> TakeAsync(string transition);                               // false when refused
    bool IsKnownTransition(string transition);
    void SetCaption(string text);
    void ReportStatus(string line);                                        // operator-visible status line (CommandStatus)
}

public sealed class OhgHostAdapter
{
    public OhgHostAdapter(IOhgHostFacade facade, ShowShellConfig shell, Action<string> log);
    public bool DriveHost { get; }
    public IReadOnlyList<string> ShadowLog { get; }        // bounded 50, newest last
    public string? ShadowLastCommand { get; }
    /// Apply one command. Never throws. Returns the refusal text, or null. Async because takes are.
    public Task<string?> ApplyAsync(ShowEngineHostCommand command);
    public static IReadOnlyDictionary<string, int?> RoutesForLook(JsonElement placement);   // pure: "ohg-box-<i>" → slot, "ohg-host"/"ohg-reader"
    public static string FormatForShadow(ShowEngineHostCommand command);   // "<seq> <name>(<args json>)"
}
```

**Behavior (spec §8 table, exactly):**
- `assignSlot [slot, pid|null]`: `AssignZoomParticipant`; false ⇒ refusal `"assignSlot: slot <n> is outside 1..10"`.
- `applyLook [placement]`: `!SceneExists(scenePreset)` ⇒ refuse `"applyLook: preset scene '<id>' does not exist"` and apply nothing. Else `CueSceneWithRoutes(scenePreset, RoutesForLook(placement))`; any missing route ids ⇒ `ReportStatus($"look '{lookId}': preset '{scenePreset}' has no route(s) {ids}")` **once per (preset, route-id set)** (keep a `HashSet<string>` of reported keys) — return null (applied partially, reported).
- `setPreview [source]`: parse the prefixed string; `look:<id>` ⇒ needs the look's preset, which the adapter does not know from `source` alone — **the adapter keeps the last `applyLook` placement per lookId** (a `Dictionary<string, string presetSceneId>` filled on every `applyLook`) and refuses `"setPreview: look '<id>' has not been applied yet"` when unknown; `slot:<n>` ⇒ `Presets.Solo` null ⇒ refuse `"setPreview: no solo preset configured"`, else `CueSceneWithRoutes(solo, {"ohg-box-1": n})`; `activeSpeaker`/`black`/`gallery` ⇒ the matching preset or the analogous refusal.
- `cut []` ⇒ `!CanTake` ⇒ refuse `"cut: take unavailable"`; else `await TakeAsync("cut")`; a `false` return ⇒ refuse `"cut: take refused by the shell"`. Never block on a task synchronously (`.GetAwaiter().GetResult()` is forbidden here — it deadlocks the UI thread).
- `auto [t|null]` ⇒ `t ?? shell.DefaultTransition`; `!IsKnownTransition` ⇒ refuse `"auto: unknown transition '<t>'"`; else take with it.
- `setGallery` ⇒ **always** shadow-recorded (even when `DriveHost`), `ReportStatus("gallery: cell order not applied (Tiles has no explicit order API)")` once per adapter lifetime; return null.
- `setNameplates [plates]` ⇒ per plate `{slot, name, location}` (read `lookDirector.ts`'s `Nameplate` for the exact field names before coding): `SetInputDisplayName` + `SetInputLowerThirdTitle`; a false ⇒ refusal listing the slot.
- `setQuestion [q|null]` ⇒ `SetCaption(q?.text ?? "")`.
- **Shadow mode:** when `!DriveHost`, every command is `FormatForShadow`'d into `ShadowLog`, `ShadowLastCommand` updated, and **no facade method except `ReportStatus`** is called. The facade fake asserts this.
- Unknown command name ⇒ refusal `"unknown host command '<name>'"`.

**Tests:** one `[Fact]` per row above, plus:
```csharp
[Fact] public async Task RouteIdNaming_ARerderedLayerDoesNotMoveAGuest()
// facade fake stores routes as a list in a scrambled order ["ohg-box-2","ohg-host","ohg-box-1"];
// applyLook boxes {1→4, 2→7}, hostSlot 1; assert fake.RouteSlots["ohg-box-1"]==4, ["ohg-box-2"]==7, ["ohg-host"]==1 regardless of list order.
[Fact] public async Task MissingRoute_IsReportedOncePerPresetAndRouteSet()
[Fact] public async Task ShadowMode_RecordsEverything_AndTouchesNothing()
[Fact] public async Task SetGallery_IsRecordedEvenWhenDriving()
```

**Mutations to run:**
- Look routes up by index instead of id → reordered-layer test reds.
- Call the facade in shadow mode → shadow test reds.
- Report missing routes every time → once-per test reds.
- Treat `slot:0` as valid → the `setPreview` slot test reds (add an assertion for `slot:0` refusing — `parseProgramSource` refuses it engine-side, but the adapter parses the string itself and must too).

**Steps:** red → implement → `dotnet test native-shell/CoreVideoPro.WinUI.Tests --filter OhgHostAdapter` → mutations → commit
`feat(winui): OhgHostAdapter maps engine host commands onto a facade, with shadow mode (Plan 7a Task 10)`.

---

### Task 11: Wire the shell — facade over the ViewModel, participant mapper, control-surface forwarding, startup

**Files:** Create `Services/StudioViewModelOhgFacade.cs`, `Services/OhgParticipantMapper.cs`; modify `Services/StudioControlSurface.cs`, `MainWindow.xaml.cs`, `ControlActionRegistry.cs` (one static action). Tests: `OhgParticipantMapperTests.cs`, `StudioControlSurfaceOhgForwardingTests.cs`.

**Interfaces — Produces:**

```csharp
public static class OhgParticipantMapper
{
    public static IReadOnlyList<ShowEngineParticipant> Map(IReadOnlyList<RawParticipantEvent> participants);
    // participantId=UserId; rawName=DisplayName; online=true; videoOn = VideoOn ?? true;
    // audioOn = !(Muted ?? false); handRaised=false; zoomRole = Role switch { "host"=>1, "cohost"=>2, _=>0 }  (case-insensitive)
}

// StudioControlSurface additions
public StudioControlSurface(StudioViewModel vm, DispatcherQueue dispatcher, ShowEngineBridge? bridge = null, OhgHostAdapter? adapter = null);
// InvokeAsync: if actionId starts with "ohg." → return bridge.InvokeAsync(...) WITHOUT marshaling (bridge null ⇒ Fail("OHG show engine is not configured")).
// "showEngine.restart" (new static registry action, no params) → bridge.RestartAsync.
// bridge.SnapshotChanged / HealthChanged → start the existing feedback debounce timer (must TryEnqueue onto the dispatcher — the events arrive on the reader thread).
// bridge.HostCommand → dispatcher.TryEnqueue(() => adapter.ApplyAsync(cmd)) ; a refusal string → _vm.CommandStatus = refusal.
// GetState(): Ohg = bridge?.Latest?.Snapshot, OhgFields = bridge?.Latest?.Fields, OhgEngineHealth = state string, OhgShadowLastCommand = adapter?.ShadowLastCommand ?? "".
```

`StudioViewModelOhgFacade : IOhgHostFacade` over `StudioViewModel` — implement each member with the VM's real entry points (`ShowInputEditors[slot-1]` for assign/name, `SelectSceneCommand` + the preview working-routes mutation path for `CueSceneWithRoutes`, `TakeForControlAsync`/`SetTakeTransitionCommand` for takes, `CaptionText` for captions, `CommandStatus` for status). Read the current `StudioViewModel` on `main` for the exact members; the ones named in `StudioControlSurface.DispatchAsync` are the contract. This class is glue and is exercised by Task 14's drill, not by unit tests.

`MainWindow.StartControlServer()` additions, in order: build `ShowConfigStore` (LocalAppData folder); if `Exists` and `Load` ok and `ShowConfigValidator.Validate(config, sceneIds)` is null ⇒ resolve paths ⇒ `ShowEngineBridge` over a `ShowEngineSupervisor(new ProcessShowEngineChildFactory(), new ShowEngineRestartPolicy(), Task.Delay, () => DateTimeOffset.UtcNow)` ⇒ `OhgHostAdapter(new StudioViewModelOhgFacade(ViewModel), config.Shell, LaunchLog.Write)` ⇒ `ControlCatalog(new[] { bridge })` ⇒ pass the catalog to both servers and the bridge/adapter to the surface ⇒ subscribe `ViewModel`'s media-core `SnapshotChanged` to `bridge.PublishRoster(OhgParticipantMapper.Map(snapshot.Participants ?? []))` and `PublishActiveSpeaker(snapshot.ActiveSpeakerId)` ⇒ `bridge.PublishCapacity(10)` ⇒ `_ = bridge.StartAsync(request, ct)` (never block launch). Any failure ⇒ `LaunchLog.Write("ohg: …")` and the app runs without OHG (`OhgEngineHealth = "failed"` with the reason reachable through `bridge.Health.LastError`). A validation error ⇒ same, with the validator's message. On window close ⇒ `await bridge.StopAsync()` before the control servers stop.

**Tests:**
- `OhgParticipantMapperTests`: null `VideoOn` ⇒ `true`; `Muted true` ⇒ `audioOn false`; role strings; ids preserved verbatim including leading zeros.
- `StudioControlSurfaceOhgForwardingTests` — `StudioControlSurface` needs a `DispatcherQueue`; the existing coverage tests avoid constructing it. Extract the forwarding decision into a pure static: `public static bool IsBridgeAction(string id) => id.StartsWith("ohg.", StringComparison.Ordinal)`; test it and the `GetState` projection via a small pure helper `public static ControlState WithOhg(ControlState baseState, ShowEngineSnapshot? latest, ShowEngineHealth health, string? shadowLast)` — test that helper directly (null latest ⇒ `Ohg` null, `OhgEngineHealth` = `"failed"` for `Failed`, etc.).
- `StudioControlSurfaceCoverageTests`: add `"showEngine.restart"` to `SupportedActionIds`; the 1:1 test stays green.

**Mutations to run:**
- Map `Muted null` to `audioOn false` → mapper test reds.
- `IsBridgeAction` matches `"ohg"` prefix without the dot → a test with id `"ohgx.y"` reds.
- `WithOhg` sets health `"running"` when `latest` is null → projection test reds.

**Steps:** red → implement → `dotnet build native-shell/CoreVideoPro.WinUI.sln` (the WinUI app must compile) → `dotnet test` on Control, ShowEngine, WinUI test projects → mutations → commit
`feat(winui): wire the show-engine bridge into the control surface and app startup (Plan 7a Task 11)`.

---

### Task 12: Packaging and CI

**Files:** Create `scripts/sync-node-runtime-to-app.ps1`; modify `scripts/package-native.ps1`, `scripts/package-native-msix.ps1`, `.github/workflows/ci.yml`, root `package.json`.

**Behavior:**
- `sync-node-runtime-to-app.ps1 -AppDir <dir> [-NodeExe <path>]`: resolves `node.exe` from `-NodeExe`, then `$env:COREVIDEO_NODE_EXE`, then `(Get-Command node).Source`; requires `node --version` major **≥ 24** (fail otherwise: `"show-engine requires Node 24+, found <v>"`); copies it to `<AppDir>\node\node.exe`; copies `show-engine\dist\**` and `show-engine\package.json` to `<AppDir>\show-engine\`; fails if `show-engine\dist\host\main.js` is missing with `"run npm run build:show-engine first"`; writes `<AppDir>\corevideo-show-engine-runtime.json` `{ nodeVersion, stagedAtUtc, entry }` like the FFmpeg manifest.
- Both package scripts call it after the FFmpeg sync and `exit $LASTEXITCODE` on failure (same shape as the Zoom sync block).
- CI `show-engine` job: `node-version: 24` (currently 22 — the package's `engines` now says 24), and a new step `- run: npm run smoke:show-engine-host` after Build.
- Root `package.json`: add `smoke:show-engine-host` (Task 3) to `test:gate`.

**Verification:** run `pwsh scripts/sync-node-runtime-to-app.ps1 -AppDir <temp>` locally and list the staged tree; run the packaged entry: `<temp>\node\node.exe <temp>\show-engine\dist\host\main.js --config <conformance config>` and see the handshake line. Paste both outputs in the task report.

**Mutations to run:** delete `dist/host/main.js` and confirm the sync script fails with the named message; set `COREVIDEO_NODE_EXE` to a Node 22 binary if one is available and confirm the version gate fails (if none is available, say so).

**Steps:** implement → verify → commit `build(show-engine): stage the Node runtime and engine beside the packaged app; CI smoke (Plan 7a Task 12)`.

---

### Task 13: Adapter conformance run

**Files:** Create `show-engine/src/host/conformance.ts` (a `--conformance` mode for `main.ts`), `native-shell/CoreVideoPro.ShowEngine.Tests/AdapterConformanceTests.cs`, `CoreVideoPro.WinUI.Tests/OhgConformanceGoldens.cs`.

**Behavior:**
- `main.ts --conformance --config <path>`: instead of the loop, runs every `HOST_CONFORMANCE_CASES` entry with a fresh `ShowEngine` whose host is a **recording facade over `StdioHostAdapter`** (so every call goes down the pipe AND is recorded for the case's own assertions), `clock: { now: () => 1000 }`, in-memory `StateFs`; prints one `log` line per case (`"conformance: <name> ok|FAIL <message>"`) and a final `log` `"conformance: <passed>/<total>"`; exits 0 iff all passed. It emits `hostCommand` lines with `generation` from argv.
- `AdapterConformanceTests` (xUnit, category `Integration`, skipped when `node` is not on PATH or `dist/host/main.js` is not built — skip **with the reason printed**): spawns the real `ProcessShowEngineChild` via the real supervisor in conformance mode, feeds every received `HostCommand` through a real `OhgHostAdapter` over a `RecordingOhgHostFacade` (all scenes exist, all routes present, `DriveHost = true`), waits for exit, and asserts: exit 0; the final log says `<n>/<n>`; **and** the recorded facade calls match `OhgConformanceGoldens` — a per-case expected sequence of facade method names (e.g. case "panelist.remove clears a slot" ⇒ `["AssignZoomParticipant(2,null)", …]`). Write the goldens by running once, reading each sequence **against the case's stated intent in `conformance.ts`**, and pinning it; a sequence that does not match the intent is a Task 10 bug, not a golden.

**Mutations to run:**
- In `OhgHostAdapter`, swap `hostSlot` and `readerSlot` handling → the golden for the look case reds.
- Drop `assignSlot` forwarding → the remove case's golden reds.

**Steps:** implement the TS mode (+ vitest test that `--conformance` over an in-memory runtime reports all cases ok) → C# test → run locally with output pasted → mutations → commit
`test(show-engine): run the host conformance suite through the real Windows adapter (Plan 7a Task 13)`.

---

### Task 14: Companion module reads feedback fields from the manifest; operator drill script

**Files:** Modify `companion-module-corevideopro/src/main.ts`, `variables.ts`, `feedbacks.ts`; create `scripts/validate-show-engine.mjs`.

**Companion behavior:** after `loadManifest`, for each `manifest.feedbackFields` entry: if it contains `{slot}`, expand 1..10; register a variable `ohg_<path with / → _>` for every `ohg/` field; register a boolean feedback for every field ending in `/tally` with the tally color. `onStateMessage` reads `state.ohgFields?.[field]` for those. Existing static lists stay as they are. Add a vitest (the module has a test setup — check `package.json`; if it has none, add a minimal one) asserting the expansion of `"ohg/slot/{slot}/tally"` yields 10 fields and the variable id `ohg_slot_3_tally`.

**Drill script** `scripts/validate-show-engine.mjs --base http://127.0.0.1:8011 [--token …]`: an **operator-run** check against a running app (the CI harness cannot launch the WinUI app): `GET /manifest` ⇒ assert 28 `ohg.*` actions and `ohg/` feedback fields; `GET /state` ⇒ assert `ohg` present and `ohgEngineHealth === "running"`; `POST /invoke ohg.program.preview ["black"]` ⇒ ok; `GET /state` ⇒ `ohg.program.preview` (read the exact snapshot path from `programBus.ts`) equals black; `POST /invoke ohg.panelist.remove ["0042"]` ⇒ `422` with a refusal (nobody has that PIN); prints a PASS/FAIL table. Document in the script header that it needs the app open on the fake Zoom engine with an OHG config whose presets exist.

**Steps:** implement → run the Companion test → run the drill against the app launched from Task 11 with the fake engine and paste its table → commit
`feat(companion): manifest-driven ohg feedback fields; operator drill for the show engine (Plan 7a Task 14)`.

---

### Task 15: CLAUDE.md, outcomes doc, and the branch review

**Files:** Modify `CLAUDE.md` (add an "OHG show engine host" section: how to configure, where logs are, the env vars `COREVIDEO_NODE_EXE`, `COREVIDEO_SHOW_ENGINE_DIR`, `COREVIDEO_OSC_OHG_LAN`, exit codes 64/70/78, shadow mode); create `docs/superpowers/plans/2026-09-07-show-engine-host-bridge-outcomes.md`.

**Steps:**
- [ ] Update `CLAUDE.md` in the same branch (owner standing rule: docs-updated is part of done).
- [ ] Run the full gates: `npm run typecheck:show-engine && npm run test:show-engine && npm run verify:barrel:show-engine && npm run smoke:show-engine-host`; `dotnet test` on Control, ShowEngine, WinUI test projects; `dotnet build` the WinUI sln. Paste counts.
- [ ] Write the outcomes doc in the format of `2026-08-12-show-engine-control-surface-outcomes.md`: shipped, corrections to this plan discovered during execution, decisions not to re-litigate, carries into 7b (at minimum: Tiles ordering, raise-hand, `tallyUrl` posting, the non-OHG feedback-list drift, `setPreview look:<id>` before any `applyLook`), and the mutation results table.
- [ ] Request a whole-branch review (superpowers:requesting-code-review) before opening the PR; address findings; commit `docs: record Plan 7a outcomes for the show engine host bridge`.

---

## Self-review (done at authoring)

- **Spec coverage:** §4 → T1–T3; §5 → T4–T5; §6.1 → T8; §6.3 → T6–T7; §6.4 → T9; §6.5 → T12; §7 → T5, T11; §8 → T10–T11; §9 → T9, T11; §11 rows 1–6 → T1–T13; Companion + drill → T14; §12 outcomes → T15. §10 (workspace) and the importer are Plan 7b by design.
- **Type consistency:** `ShowEngineHostCommand.Args` is a `JsonElement` (T6) and `OhgHostAdapter.ApplyAsync` (T10, corrected inline) parses it; `ShowEngineSnapshot` fields feed `ControlState.OhgFields` as `JsonElement` values (T5, T11); `IControlActionProvider` (T4) is what `ShowEngineBridge` (T8) implements; `ShowShellConfig.Presets` (T9) is what `OhgHostAdapter` (T10) reads.
- **Known softness, stated rather than hidden:** T11's `StudioViewModelOhgFacade` is glue over a 14k-line ViewModel and is verified by T13's conformance run and T14's drill, not by unit tests. The route-mutation entry point on `StudioViewModel` must be located on `main` at execution time (`PublishPreviewCompositionState` is the current sink; the implementer must find or add a public method that sets `ShowInputSlotNumber` on a preview route by id and republishes).
