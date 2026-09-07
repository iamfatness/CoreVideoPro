# Show Engine Host Bridge — Design Spec (Plan 7)

**Status:** approved in design review 2026-09-07; awaiting owner review of this document.
**Parent:** `2026-08-04-ohg-show-engine-design.md` (the OHG show engine). This spec discharges the
obligation carried out of Plan 6's outcomes: *"wire the bridge."*
**Scope decision (owner, 2026-09-07):** Plan 7 covers the Windows shell bridge **and** the native
WinUI "OHG Show" workspace, delivered as two plans under this one spec: **7a** (bridge) and **7b**
(workspace). macOS and the OBS plugin remain Plans 8 and 9.

## 1. What this is

`show-engine/` (Plans 1–6) is a complete, tested, host-agnostic brain with zero consumers. It exposes
28 `ohg.*` actions (`OHG_ACTIONS`, `invokeAction`), a flattened feedback projection
(`projectControlFields`, `OHG_FIELD_TEMPLATES`), a `HostAdapter` port, and a conformance suite. Nothing
in `native-shell/` references it, no process runs it, and `CoreVideoPro.Control` — the shell's control
server — is a closed compile-time registry that cannot learn a new action.

This spec makes the Windows shell the engine's first host:

1. run the engine as a supervised Node subprocess;
2. register its actions and feedback fields into the control server so Companion, HTTP, WebSocket,
   and OSC reach every one of them;
3. carry its state on the control state as an `ohg` node;
4. apply its host commands to the operator ViewModel (or, in shadow mode, record them);
5. give operators a native four-panel workspace that is a thin renderer over 2–4.

### Goals

- Every `ohg.*` action reachable from Companion, HTTP, WebSocket, OSC, and the native panels through
  **one** dispatch path. The panels can do nothing OSC cannot.
- The engine owns time, state, and semantics. The shell owns process lifecycle, transport, and the
  mapping of commands onto its own scene/input model.
- Loud, never silent: every unsupported mapping, missing preset, config error, crash, and refusal is
  visible on the state node and in the workspace strip.
- Shadow mode (parent spec §8, migration step 1) as a config flag from day one.
- Discharge the two Plan 6 carries: the bridge, and per-shell adapter conformance.

### Non-goals (this spec)

- Companion preset pages (parent §4.5). The only Companion change is reading feedback fields from
  the manifest instead of a hardcoded list.
- Driving CoreVideo Tiles cell order from `setGallery`. Tiles has no explicit cell-order API today;
  7a records the command and says so on the panel. A Tiles ordering API is a separate design.
- Raise-hand events from the Zoom SDK. Not on the core protocol today; the hands queue is Mukana's.
- mac-shell and OBS plugin bridges (Plans 8–9).
- OSC authentication. Decision below is exposure gating, not a token.

## 2. Decisions taken in design review (owner, 2026-09-07)

| # | Decision | Alternatives rejected |
|---|---|---|
| D1 | **Node subprocess + proxied registration** (parent §2 process model). | Port to C# (≈20k lines + 915 tests, abandons parity by construction); embed JS in-process (Jint/ClearScript: no crash isolation, new heavyweight dependency). |
| D2 | **Registry opens by composition, not mutation.** Static registry unchanged; a catalog merges providers. | Making `ControlActionRegistry` mutable (breaks its "cannot drift" guarantee and the coverage test's meaning). |
| D3 | **Engine ticks itself** in the Node process. Shell feeds events and reads snapshots. | Shell-driven tick over the pipe (adds a round-trip to every resolve; couples engine cadence to UI thread health). |
| D4 | **State = raw snapshot pass-through + flat field map.** | Typed C# mirror of `ShowSnapshot` (a second schema to drift). |
| D5 | **`ohg.*` over OSC is loopback-only by default**; separate explicit LAN opt-in. HTTP/WS keep the bearer token. | Same as everything else (LAN device can reseat the host); token on OSC (protocol change, breaks presets). |
| D6 | **New small supervisor with real backoff**, not a fork of `MediaCoreSupervisor`. | Reusing the 1510-line core supervisor (immediate respawn, lifetime counter, core-specific handshake). |
| D7 | **Shadow mode is a config flag** (`driveHost: false`). | A separate build or a separate adapter class. |
| D8 | **Node ships in the app folder** like the Zoom and FFmpeg runtimes; dev fallback to PATH; env overrides. | Requiring Node on the operator machine (brittle; owner rule: robustness over fragile wins). |
| D9 | **Two plans, one spec**: 7a bridge, 7b workspace. Plans 5–6 land on `main` first. | One plan (serial tail: panels untestable before the bridge exists). |
| D10 | **Engine slot number == Show Input slot number** (1..capacity, capacity = 10). | An indirection table (nothing to gain; two id spaces to keep aligned). |
| D11 | **Look presets are operator-authored scenes** whose routes are identified by **route id naming**, not layer order. | Order-based mapping (a reordered layer silently swaps guests). |

## 3. Architecture

```
 Companion / HTTP / WS / OSC            WinUI "OHG Show" workspace (7b)
            │                                      │
            ▼                                      ▼
 ┌─────────────────────────── CoreVideoPro.Control ───────────────────────────┐
 │ ControlCatalog = ControlActionRegistry (static) + IControlActionProvider[] │
 │ HttpControlRouter / OscControlRouter / ControlManifest read the catalog    │
 │ ControlState { …, Ohg: JsonElement?, OhgFields: dict }                    │
 └───────────────┬────────────────────────────────────────────┬───────────────┘
                 │ ohg.* → bridge                              │ everything else → switch
                 ▼                                             ▼
 ┌── CoreVideoPro.ShowEngine (new project) ──┐        StudioControlSurface → StudioViewModel
 │ ShowEngineBridge  : IControlActionProvider │                  ▲
 │ ShowEngineSupervisor (spawn/backoff/gen)   │                  │ host commands
 │ ShowEngineProtocol (NDJSON codec)          │──────────────────┘ (OhgHostAdapter, UI thread)
 │ ShowEnginePaths / ShowConfigStore          │
 └───────────────┬────────────────────────────┘
                 │ stdin/stdout, newline-delimited JSON
                 ▼
 node.exe  show-engine/dist/host/main.js  --config <path>
 ┌────────────────────────────────────────────┐
 │ ShowEngine (Plans 1–6) + StdioHostAdapter  │
 │ systemClock · NodeStateFs · abortable fetch│
 └────────────────────────────────────────────┘
```

**Threading.** `ohg.*` invokes never touch the UI thread: `StudioControlSurface` forwards them to the
bridge before its UI-thread marshal. Host commands coming back **do** touch the ViewModel and are
marshaled onto the `DispatcherQueue` exactly like every other invoke today. Snapshot events raise the
bridge's `StateChanged`, which `StudioControlSurface` folds into its existing 150 ms feedback debounce.

## 4. The engine host process (`show-engine/src/host/`)

New files, all under `show-engine/src/host/`: `main.ts` (entry), `protocol.ts` (line codec + message
types), `stdioHostAdapter.ts`, `nodeStateFs.ts`, `nodeFetch.ts`. Built to `dist/host/main.js`.
`package.json` gains `"bin": { "show-engine-host": "dist/host/main.js" }` and a `host` export path.

`main.ts` constructs a real `ShowEngine` with:

- `clock: systemClock`;
- `store: new StateStore(nodeStateFs, config.statePath)` — `nodeStateFs` is `fs/promises` with
  atomic write via temp-file + rename, as `StateFs` requires;
- `mukana: new MukanaClient(nodeFetch, …)` only when `config.mukana !== null` — `nodeFetch` is global
  `fetch` and **honors `signal`** (parent authoring rule 9: a conforming fixture must exist, and this
  is the production one);
- `host: new StdioHostAdapter(writer)` — every `HostAdapter` method serializes one `hostCommand` line.

Startup order: parse argv (`--config <path>`, `--generation <n>`); read + `parseShowEngineConfig`
(a parse failure prints one `log` line at `error` and exits 78, `EX_CONFIG`); construct; `await
restore()`; emit the unsolicited `handshake` event; start the tick timer at **250 ms**; then process
stdin lines. A `tick()` rejection is caught, logged at `error`, and the timer continues — a single bad
tick must not kill the show. stdin EOF ⇒ exit 0.

### 4.1 Wire format

Newline-delimited JSON, UTF-8 without BOM, one message per line, no message may contain a raw newline.
Same idiom as the media-core protocol so the C# reader pattern is reused.

**Requests (shell → engine)** carry `id: string` and `type`. Every request gets exactly one response
line `{ id, ok: true, …payload }` or `{ id, ok: false, error: { message } }`.

| `type` | payload | response |
|---|---|---|
| `handshake` | — | `protocolVersion: 1`, `engineVersion`, `generation`, `actions: ActionDefinition[]` (= `OHG_ACTIONS` verbatim), `fieldTemplates: string[]` (= `OHG_FIELD_TEMPLATES`), `snapshot`, `fields` |
| `invoke` | `action: string`, `args: unknown[]` | `result: ActionResult` (`{kind:"ok"}` / `{kind:"refused",reason}` / `{kind:"error",message}`). **Always `ok: true`** at the envelope level — a bad action or bad arg is a *result*, never a transport failure, because `invokeAction` never throws. |
| `zoomEvent` | `event: ZoomEvent` | `ok` |
| `activeSpeaker` | `participantId: string` | `ok` |
| `capacity` | `capacity: number` | `ok` (currently informational; engine capacity is config-fixed and the shell validates equality — see §8) |
| `ping` | — | `ok`, `revision` |
| `shutdown` | — | `ok`, then the process exits 0 after flushing stdout |

An unknown `type` is answered `ok: false` with `error.message = "unknown request type '<t>'"`. A line
that is not valid JSON is answered with `id: null`, `ok: false`. Neither kills the process.

**Events (engine → shell)** have no `id` and carry `event`:

| `event` | payload |
|---|---|
| `handshake` | same payload as the handshake response — emitted **unsolicited once at startup**, so the shell can accept it without asking (mirrors `MediaCoreHandshakeRules.IsUnsolicitedBootstrapHandshake`) |
| `snapshot` | `revision`, `generation`, `snapshot: ShowSnapshot`, `fields: Record<string, ControlFieldValue>` — emitted after every `tick()` whose revision changed, and after every `invoke` that changed revision |
| `hostCommand` | `generation`, `seq` (monotonic per process), `name` (`assignSlot` \| `applyLook` \| `setPreview` \| `cut` \| `auto` \| `setGallery` \| `setNameplates` \| `setQuestion`), `args` — one per `HostAdapter` call, in call order. `ReadonlyMap` args are serialized as arrays of `[key, value]` pairs. |
| `log` | `level` (`info` \| `warn` \| `error`), `message` — includes every `restoreWarnings` entry at startup |

The engine writes **nothing to stderr** except an uncaught crash. The shell treats any stderr line as
a crash diagnostic and logs it.

### 4.2 Protocol versioning

`protocolVersion` is an integer. The shell refuses a handshake whose version it does not know, reports
`ohg/health/engine = failed` with the message, and does not retry (a version mismatch is a packaging
bug, not a transient fault).

## 5. Control catalog (`CoreVideoPro.Control`)

```csharp
public interface IControlActionProvider
{
    /// Stable provider id, used in diagnostics and exposure policy ("show-engine").
    string ProviderId { get; }
    IReadOnlyList<ControlAction> Actions { get; }          // empty until the provider is ready
    IReadOnlyList<string> FeedbackFieldTemplates { get; } // e.g. "ohg/slot/{slot}/name"
    OscExposure DefaultOscExposure { get; }               // LoopbackOnly for show-engine
    event EventHandler? ActionsChanged;                   // handshake / teardown
}

public enum OscExposure { Lan, LoopbackOnly }

public sealed class ControlCatalog
{
    public ControlCatalog(IEnumerable<IControlActionProvider> providers);
    public static ControlCatalog StaticOnly { get; }      // registry, no providers
    public IReadOnlyList<ControlAction> Actions { get; }  // static first, then providers in order
    public bool TryGet(string id, out ControlAction action);
    public bool Contains(string id);
    public bool TryBind(string id, IReadOnlyList<object?> raw, out IReadOnlyList<object?> bound, out string? error);
    public OscExposure ExposureOf(string id);             // Lan for static; provider default otherwise
    public IReadOnlyList<string> FeedbackFields { get; }  // ControlManifest.StateFields + provider templates
    public event EventHandler? Changed;
}
```

- `ControlActionRegistry`'s static `TryGet/Contains/TryBind` become forwarding shims over
  `ControlCatalog.StaticOnly`. Nothing outside Control changes on day one.
- `HttpControlRouter`, `OscControlRouter`, `HttpControlServer`, `OscControlServer` take a
  `ControlCatalog` (constructor parameter; default `StaticOnly` for existing tests).
- `ControlManifest.Build(catalog)` lists `catalog.Actions` and `catalog.FeedbackFields`. Duplicate ids
  across static + providers are a construction-time `InvalidOperationException` — a provider may not
  shadow a static action.
- Action id validation for providers is the same regex the registry test pins
  (`^[a-z][a-zA-Z0-9]*(\.[a-z][a-zA-Z0-9]*)+$`); a provider action failing it is rejected at
  construction, loudly.
- **OSC exposure.** `OscControlServer` already tracks `RemoteEndPoint`; it passes the sender to
  `OscControlRouter.RouteAsync(message, sender)`. If `catalog.ExposureOf(id) == LoopbackOnly` and
  `!IPAddress.IsLoopback(sender.Address)`, the router returns
  `ControlInvokeResult.Fail("'<id>' is not exposed to LAN OSC senders (set COREVIDEO_OSC_OHG_LAN=1)")`
  and the server logs the sender. `COREVIDEO_OSC_OHG_LAN=1` sets the show-engine provider's
  `DefaultOscExposure` to `Lan` at construction.

**Coverage tests.** `StudioControlSurfaceCoverageTests.Adapter_HandlesEveryRegisteredAction` keeps
asserting the static registry ⇔ `SupportedActionIds` 1:1. A new `ControlCatalogTests` constructs a
catalog with a fake provider of three actions and, per authoring rule 10, **invokes every provider id
through `HttpControlRouter` and `OscControlRouter`** against a recording surface, asserting each
arrived with bound args; asserts the manifest lists each with its OSC address; asserts a LAN sender is
refused for `LoopbackOnly` and accepted after the flag.

## 6. The bridge (`CoreVideoPro.ShowEngine`, new project)

New class-library project `native-shell/CoreVideoPro.ShowEngine/` referencing `CoreVideoPro.Control`
and `CoreVideoPro.MediaCore` (for the snapshot models it consumes). It does **not** reference WinUI.
Test project `CoreVideoPro.ShowEngine.Tests/`.

### 6.1 `ShowEngineBridge : IControlActionProvider, IDisposable`

- Owns a `ShowEngineSupervisor` and the current `generation`.
- On handshake: converts `actions` to `ControlAction`s (`string|int|double|bool` map 1:1 to
  `ControlParamType`; `required` and `description` carried), publishes them, raises `ActionsChanged`.
  If a later generation's manifest differs from the first, the difference is logged at `warn` and the
  **new** manifest wins.
- `Task<ControlInvokeResult> InvokeAsync(string id, IReadOnlyList<object?> boundArgs, CancellationToken)`:
  sends `invoke`; maps `ActionResult` → `Success` / `Fail(reason)` / `Fail(message)`. Engine not
  running ⇒ `Fail("OHG show engine is <health>")` immediately.
- `event EventHandler<ShowEngineSnapshot>? SnapshotChanged` and `ShowEngineSnapshot Latest { get; }`
  where `ShowEngineSnapshot = (int Generation, long Revision, JsonElement Snapshot,
  IReadOnlyDictionary<string, JsonElement> Fields)`.
- `event EventHandler<ShowEngineHostCommand>? HostCommand` — raised on the reader thread; the
  WinUI adapter marshals. Commands whose `generation != current` are dropped and counted.
- `ShowEngineHealth Health { get; }` = `{ State: Stopped|Starting|Running|Recovering|Failed,
  Generation, RestartCount, LastError, LastCrashAt }`; `HealthChanged` event.
- Intake: `PublishRoster(IReadOnlyList<ShowEngineParticipant>)`, `PublishActiveSpeaker(string?)`,
  `PublishCapacity(int)`. Roster is sent as one `zoomEvent {kind:"roster"}`; the engine diffs. Active
  speaker is sent only on change. Both are re-sent on every new generation after handshake.

### 6.2 Participant mapping (MediaCore snapshot → engine `Participant`)

| engine field | source |
|---|---|
| `participantId` | core participant id (the same string `input.assign zoom:<pid>` uses) |
| `rawName` | display name as reported by Zoom |
| `online` | present in the current snapshot |
| `videoOn` | `Health != VideoOff` |
| `audioOn` | `!IsMuted` |
| `handRaised` | `false` always (not on the protocol; recorded gap) |
| `zoomRole` | numeric Zoom role from the core wire, else 0 |

### 6.3 `ShowEngineSupervisor`

Spawn/read/write/teardown, cloned in *shape* from `MediaCoreSupervisor` but ≈300 lines:

- `ProcessStartInfo`: `UseShellExecute=false`, three streams redirected, UTF-8 no BOM, `CreateNoWindow`,
  working directory = the engine dir, args `dist/host/main.js --config <path> --generation <n>`.
- stdout loop on a background task; `id` correlation via `ConcurrentDictionary<string,
  TaskCompletionSource<JsonDocument>>`; a `ReferenceEquals(process)` generation guard so a stale
  child cannot complete a new request.
- stdin writes under a `SemaphoreSlim`; the request timeout (4 s default; 15 s for handshake) covers
  gate + write + flush + response, as the core supervisor learned to.
- stderr → `%LOCALAPPDATA%\CoreVideoPro\show-engine.log` (bounded, same `BoundedLogFile`).
- Heartbeat `ping` every 1 s; two consecutive timeouts ⇒ treated as a hang ⇒ kill + respawn.
- **Backoff:** delays 1, 2, 4, 8, 16, 30, 30… seconds; the consecutive-failure counter resets after
  **60 s** of `Running`; after **5** consecutive failures ⇒ `Failed`, no further respawn until the
  operator presses Restart (a workspace button / `ohg.engine.restart` is **not** an engine action —
  it is a shell action `showEngine.restart` added to the static registry). Delays go through an
  injected `Func<TimeSpan, CancellationToken, Task>` so tests never sleep.
- Teardown: `shutdown` request with a 1.5 s grace, then `Kill(entireProcessTree: true)`; always off
  the UI thread.

### 6.4 `ShowEnginePaths`

Resolves `node.exe` and the engine directory, in order, and records the choice in the launch log:

1. `COREVIDEO_NODE_EXE` / `COREVIDEO_SHOW_ENGINE_DIR` environment overrides;
2. packaged: `<appDir>\node\node.exe`, `<appDir>\show-engine\dist\host\main.js`;
3. dev: `<repoRoot>\show-engine\dist\host\main.js` with `node` from `PATH` (repo root found the way
   `MediaCorePaths.RepoRoot` finds it).

Missing ⇒ `Health = Failed`, `LastError` names the path probed. The app launches regardless.

### 6.5 Packaging

- `scripts/sync-node-runtime-to-app.ps1`: stages `node.exe` (pinned **Node 24.x**, the version the
  package is tested on — recorded in `show-engine/package.json` `engines`) under `<appDir>\node\` and
  copies `show-engine/dist` + `package.json` under `<appDir>\show-engine\`. The package has **no
  runtime dependencies**, so no `node_modules` is staged; the script fails if `dist/host/main.js` is
  missing.
- `package-native.ps1` and `package-native-msix.ps1` call it and hard-fail on its failure, the way they
  already fail on a missing `corevideo-native.exe`.
- CI: the existing show-engine job additionally runs `npm run build` and a smoke test that spawns
  `dist/host/main.js` with the conformance config and receives a handshake.

## 7. State node

`ControlState` gains:

```csharp
public System.Text.Json.JsonElement? Ohg { get; init; }                       // raw ShowSnapshot
public IReadOnlyDictionary<string, System.Text.Json.JsonElement>? OhgFields { get; init; }
public string OhgEngineHealth { get; init; } = "stopped";                     // always present
public string OhgShadowLastCommand { get; init; } = string.Empty;             // shadow mode only
```

- `/state` and the WS push serialize `Ohg` under `ohg` untouched (already camelCase from TS).
- `OscFeedback.Encode` emits every `OhgFields` entry at `/cvp/state/<field>` (so
  `ohg/slot/3/tally` → `/cvp/state/ohg/slot/3/tally`), plus `ohg/health/engine` and
  `ohg/shadow/lastCommand`. Bool → `T`/`F`, number → `f` or `i` by integrality, string → `s`, null →
  omitted.
- `ControlManifest.FeedbackFields` includes the provider templates plus the two shell-owned fields.
- A new `ControlStateFieldsTests` asserts the three hand-maintained lists (`ControlState` scalars,
  `StateFields`, `OscFeedback.Encode`) **agree for the `ohg` fields**. (The pre-existing drift among
  the non-OHG fields is recorded in this spec as a known defect and is *not* fixed here — out of scope.)

**Companion module.** `variables.ts`/`feedbacks.ts` stop being the only source of field ids. The
module does **not** walk the nested `state.ohg` object; instead it reads `manifest.feedbackFields`
(declared in its `Manifest` type today but never read), and for each template containing `{slot}`
expands slots 1..10, registering one variable per field. Values come from `state.ohgFields[field]`
(the flat map), so no nested walk is needed. Boolean-typed `ohg/slot/{n}/tally` also registers as a feedback with the tally color. This is
the only Companion change in Plan 7.

## 8. Host adapter (`OhgHostAdapter`, in `CoreVideoPro.WinUI/Services/`)

Subscribes to `ShowEngineBridge.HostCommand`, marshals onto the `DispatcherQueue`, and applies each
command to `StudioViewModel` through the **same entry points the control surface uses** (so the
"operator-equivalent path" write scopes and logs apply).

| command | application | refusal (logged + status line) |
|---|---|---|
| `assignSlot(slot, pid)` | Show Input `slot` ← `zoom:<pid>`, `InShow=true`; `null` ⇒ clear participant, `InShow=false` | slot out of 1..10 |
| `applyLook(p)` | Cue scene `p.scenePreset` to preview; in that scene, for each box `i` set route `ohg-box-<i>`.`ShowInputSlotNumber = p.boxes[i]` (null ⇒ route mode `None`); route `ohg-host` ← `p.hostSlot`; `ohg-reader` ← `p.readerSlot` | preset scene missing ⇒ refuse whole command; a missing box route ⇒ apply the rest, report once per (preset, route) |
| `setPreview(source)` | `look:<id>` ⇒ cue that look's preset; `slot:<n>` ⇒ cue `presets.solo` and set its `ohg-box-1` route to slot n; `activeSpeaker` ⇒ cue `presets.activeSpeaker`; `black` ⇒ cue `presets.black`; `gallery` ⇒ cue `presets.gallery` | the named preset is unset or missing |
| `cut()` | `TakeForControlAsync` with transition `cut` | take unavailable (`CanTake` false) |
| `auto(t)` | set transition `t ?? config.defaultTransition`, then take | unknown transition |
| `setGallery(cells)` | **not applied in 7a** — recorded to the shadow list regardless of `driveHost`; one status line per change | — |
| `setNameplates(plates)` | for each plate: Show Input `plate.slot` display name ← `plate.name`; lower-third title for that input ← `plate.location` | slot unassigned |
| `setQuestion(q)` | `CaptionText ← q.text` (`null` ⇒ empty); `CaptionSpeaker` unchanged | — |

`capabilities()` reported to the engine (sent by the host process, but its values come from this
spec and are asserted by the conformance run): `hasPreviewBus: true`, `maxGalleryCells: 16`,
`transitions: ["cut","fade","dip","wipe"]`.

**Capacity.** The shell sends `capacity: 10`. `ShowEngineConfig.capacity` must equal it; the shell
refuses to spawn with `"config.capacity must be 10 (the Show Input count); found <n>"`.

**Shadow mode.** When `config.driveHost == false`, every command (including `setGallery`) is appended
to a bounded (50) list of `"<seq> <name>(<args>)"` strings, the newest published as
`OhgShadowLastCommand`, and **nothing** on the ViewModel changes. Switching `driveHost` requires a
respawn (config change ⇒ respawn, §9).

## 9. Config store

`%LOCALAPPDATA%\CoreVideoPro\ohg-show-config.json`, versioned (`version: 1`), read/written by
`ShowConfigStore` following `ProductionOutputPreferencesStore`'s pattern (atomic write, migration
switch, loud parse failure). Shape:

```jsonc
{
  "version": 1,
  "engine": { /* exactly ShowEngineConfig — validated by parseShowEngineConfig in the engine */ },
  "shell": {
    "driveHost": false,
    "presets": { "solo": "<sceneId>", "activeSpeaker": "<sceneId>", "black": "<sceneId>", "gallery": "<sceneId>" },
    "defaultTransition": "cut",
    "tallyUrl": null
  }
}
```

- `engine.statePath` defaults to `%LOCALAPPDATA%\CoreVideoPro\ohg-show-state.json` when absent.
- The shell validates before spawning: every `looks[].scenePreset` and every `shell.presets.*` names
  an existing scene; `engine.capacity == 10`. Failure ⇒ `Health = Failed`, `LastError` = the first
  problem, engine not started. The engine validates the rest at startup (exit 78 ⇒ `Failed`, no
  backoff — a config error is not transient).
- No secrets. `tallyUrl` is reserved for the tally publisher's HTTP target (parent §3.11); Plan 7a
  does **not** post tally; it is published on the state node only.
- Any save of this file by the 7b editor ⇒ `ShowEngineBridge.Restart()`.
- **Importer** (7b): one-shot from the legacy `infraestructure-*.js` + `mukana-*.js` (Mukana URLs,
  capacity, look definitions) → `engine` block; presets left empty for the operator to pick.

## 10. The WinUI workspace (Plan 7b)

`Views/OhgShowPage.xaml` + `ViewModels/OhgShowViewModel.cs`, a new operator tab under
`StudioWorkspace` alongside Audio/Automation/Overlays/Routing, using `OperatorTabResources` and the
app's existing design language. Tab visible only when the config file exists; otherwise a single
"Set up OHG" surface opens the settings section.

**Two inputs, no logic.** `OhgShowViewModel(ShowEngineBridge bridge, ShowConfigStore config)` binds
`bridge.Latest` and `bridge.Health` onto observable properties; every command is
`bridge.InvokeAsync("ohg.…", args)`. No show rule lives here. Projection tests (snapshot → bound
properties) and dispatch tests (button → id + args) are the whole test surface.

Panels (parent §4.4):

1. **Panelist board** — master list (`panelists`: name, location, PIN badge, Mukana/video/online,
   role chip) beside the slot grid (`slots`, holes shown). Tap panelist → tap slot ⇒
   `ohg.panelist.add(pid, slot)`; occupied slot ⇒ remove / replace; role chip ⇒
   `ohg.panelist.role.set(pin, role)`. `unseated` renders as a warning row.
2. **Program panel** — `program` (pgm/pvw/mode/asFollow), preview / cut / auto / direct-cut, AS-follow
   toggle with the current speaker's plate, look picker over config `looks`, hands pager over `queue`
   with prev/next guest and `pagingRefused` shown inline.
3. **Gallery panel** — 16-cell grid from `gallery`, tap-to-replace/remove, smart toggle
   (`smartGallery`), reset-from-slots; a one-line note that the shell does not drive Tiles order yet.
4. **GFX & data panel** — question in/out, headline in/out/change, Mukana sync with the three endpoint
   lamps (`health`, `capabilities.*.detail`), override editor over `ohg.mukana.override.*`.

**Status strip** (always visible on the tab): `OhgEngineHealth` with Restart, shadow-mode badge with
`OhgShadowLastCommand`, `restoreWarnings`, `pagingRefused`, and the adapter's refusal lines. This is
the "loud, never silent" channel; a surface that hid it would defeat Plan 6's design.

**Settings section** in the production settings window: edits every field of §9 with scene pickers
for presets and look scene ids; Save ⇒ store ⇒ restart. Import button ⇒ §9 importer.

## 11. Testing

| layer | runner | what it proves |
|---|---|---|
| host process | vitest (`show-engine/src/host/*.test.ts`) | codec round-trip incl. bigint/circular refusal; handshake payload equals `OHG_ACTIONS`/`OHG_FIELD_TEMPLATES`; invoke ⇒ `ActionResult`; snapshot only on revision change; hostCommand order & `seq`; a **conforming** abortable fetch fixture (rule 9); bad line / unknown type never exit; config parse failure exits 78 |
| catalog | xUnit `Control.Tests` | §5 coverage test; duplicate-id rejection; exposure refusal w/ LAN sender; manifest fields concatenation |
| bridge + supervisor | xUnit `ShowEngine.Tests` w/ in-memory stream fake child | backoff sequence via injected delay; reset after 60 s healthy; `Failed` after 5; generation guard drops stale responses **and** stale host commands; re-arm sends capacity + roster after handshake; hang detection after 2 missed pings; exit 78 ⇒ Failed without backoff |
| adapter | xUnit `WinUI.Tests` against a ViewModel facade | every §8 row, every refusal, shadow mode records-not-applies, route-id naming (a reordered layer does not move a guest) |
| state | xUnit `Control.Tests` | `ohg` field agreement across the three lists; OSC encoding of each value type |
| conformance | Node runner invoked by an xUnit test | `HOST_CONFORMANCE_CASES` against a recording facade over the real `OhgHostAdapter` mapping — discharges Plan 6's "per-shell adapter conformance" |
| workspace | xUnit `WinUI.Tests` | projection + dispatch tests for every panel control |
| end-to-end | `scripts/validate-show-engine.mjs` (fake Zoom engine + real app, headless harness pattern) | pre-show fill → host handoff → question in/out, asserting the `/state` `ohg` node and the preview scene routes |

**Mutations** are named per task in the plans (authoring rule 7). Spec-level ones the plans must
include: drop the generation guard (stale hostCommand test reds); make `invoke` throw on bad arg
(envelope test reds); remove the loopback check (exposure test reds); swap route-id lookup for
order (reordered-layer test reds); drain the 60 s reset (backoff test reds).

## 12. Sequencing

0. **Prerequisite PR:** merge `main` into `plan/show-engine-orchestrator` (≈156 behind), resolve,
   915 engine tests + CI green, merge to `main`. Plans 5–6 land. This spec's branch is rebased onto
   the result.
1. **Plan 7a** — `plan/show-engine-host-bridge`: §4, §5, §6, §7, §8, §9 (store + validation, no
   editor), §11 rows 1–6, packaging. Outcomes doc at close.
2. **Plan 7b** — `plan/show-engine-winui-workspace`, stacked on 7a: §10, importer, §11 rows 7–8.
   Outcomes doc at close.

## 13. Known gaps recorded, not fixed here

- Raise-hand is not on the core protocol; `handRaised` is always `false`.
- `setGallery` is recorded, not applied; Tiles needs an explicit cell-order API.
- The non-OHG drift among `ControlState`/`StateFields`/`OscFeedback` predates this work.
- The control API is unauthenticated when `COREVIDEO_CONTROL_TOKEN` is unset (pre-existing).
- `smartGallery` is not persisted by the engine (Plan 6 carry).
- Tally is published on the state node only; posting to `tallyUrl` is deferred.
