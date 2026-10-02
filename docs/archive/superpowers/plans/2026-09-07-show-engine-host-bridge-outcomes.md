# Show Engine — Plan 7a Outcomes (the Windows host bridge)

Companion to `2026-09-07-show-engine-host-bridge.md` and the design spec
`../specs/2026-09-07-show-engine-host-bridge-design.md`. Written at the close of Plan 7a
execution (2026-09-07).

**Shipped:** the bridge Plan 6's outcomes said was owed. The OHG show engine now runs as a
**fourth, optional child process** — `node show-engine/dist/host/main.js` under
`CoreVideoPro.ShowEngine.ShowEngineSupervisor` — and its 28 `ohg.*` actions are reachable from
Companion, OSC and the HTTP control API on a real Windows shell. Five pieces:

1. **The host process** (`show-engine/src/host/`) — a pure line codec that never throws, a
   `StdioHostAdapter` that turns `HostAdapter` calls into `hostCommand` events, an abortable
   `nodeFetch` and an atomic `nodeStateFs`, and a `main.ts` entry with a real exit-code contract
   (64 usage / 78 config / 70 everything else) plus a `--conformance` mode.
2. **A catalog, not a mutable registry** (`CoreVideoPro.Control`) — `ControlCatalog` composes the
   closed static `ControlActionRegistry` with `IControlActionProvider`s; a provider may not shadow
   a static action. `OscExposure` makes `ohg.*` loopback-only unless `COREVIDEO_OSC_OHG_LAN=1`.
3. **The bridge project** (`CoreVideoPro.ShowEngine`, new) — supervisor with generation guards, a
   5→10→20→40→60 s restart policy that gives up after 5 and resets after 60 s healthy, path
   resolution (env pair → packaged → dev), config store + validator, and `ShowEngineBridge` as the
   action provider that re-arms capacity/roster/speaker on every new generation.
4. **The shell side** (`CoreVideoPro.WinUI`) — `OhgHostAdapter` maps host commands onto a facade
   over `StudioViewModel` with **shadow mode on by default**, `OhgRouteSlotWriter`,
   `OhgParticipantMapper`, the `ohg` state node + flattened `ohgFields`, and startup/teardown
   wiring in `MainWindow`.
5. **Packaging, Companion, and a drill** — the Node runtime and built engine staged beside the
   packaged app (`sync-node-runtime-to-app.ps1`, both packaging scripts, CI smoke); the Companion
   module reading `manifest.feedbackFields` instead of a hand-maintained list; and
   `scripts/validate-show-engine.mjs` with its judgement logic unit-tested offline.

**Counts.** 25 commits (`5972dbcd..75d7cfbd`), 89 files, +14,146 / −135 lines. Tests at HEAD, all
green: **vitest 945** (43 files; 915 at the Plan 6 close, +30 across 5 new `src/host/*.test.ts`
files), **`Control.Tests` 65** (from 54), **`ShowEngine.Tests` 135** (a new project — 14 files,
~2,200 lines), **`WinUI.Tests` 943** (from 856; 6 new files, ~1,785 lines, including the
node-spawning `AdapterConformanceTests`), **Companion 10** (a new vitest suite), and the **drill
judge 9** (`node --test`). Release WinUI build: 0 errors.

## The correction this plan is built on

Plan 6's outcomes stated the obligation exactly: *"`CoreVideoPro.Control` needs dynamic or proxied
registration and an extensible `ControlState` subtree before any `ohg.*` action is reachable."*
That is discharged — and the shape it took is the one thing worth restating, because the obvious
implementation was the wrong one.

**The registry was not made mutable.** `ControlActionRegistry` is a closed compile-time list whose
value is precisely that it *cannot* drift, and a coverage test enforces a 1:1 relationship with the
dispatch switch. Making it mutable would have destroyed both. Instead `ControlCatalog` **composes**
the static registry with providers at read time, re-reading `provider.Actions` live rather than
caching them at construction — so a handshake, a respawn, or a teardown changes what the catalog
offers without anything being mutated in place.

## Corrections to the plan discovered in execution

The plan and spec were written before the engine's own surface was read closely. Nine places where
execution found the plan wrong; each was corrected in the code and is listed here so 7b starts from
the truth.

- **`setPreview` carries a `ProgramSource` *object*, not a formatted string.** `hostAdapter.ts`
  declares `setPreview(source: ProgramSource)`. Plan 6's outcomes note that `ProgramSource`
  *encodes* as one prefixed string in the projection — which is a fact about `projectControlFields`,
  not about the host port. The adapter parses the object.
- **`ohg.panelist.remove` takes a slot `int`, not a PIN.** The plan's drill specified a PIN; the
  action's real param is a slot, so the drill uses slot 42 to provoke its 422 refusal.
- **`IControlActionProvider`'s member is `Id`, not `ProviderId`.** Task 4 shipped it as `Id`, and
  the spec/plan text said `ProviderId`. Accepted as-is: name drift only, no behavior.
- **The adapter conformance test lives in `WinUI.Tests`, not `ShowEngine.Tests`.**
  `ShowEngine.Tests` cannot reference WinUI, and `OhgHostAdapter` lives in WinUI. The plan put it
  in the wrong project.
- **The supervisor drops pre-handshake and post-exit host commands — so conformance mode must
  bracket its run.** Measured: 19 commands dropped when the mode emitted freely. `--conformance`
  now handshakes, then *waits* for a parent request before running, and waits for shutdown/EOF
  afterwards, so every command it emits falls inside the live window. A
  `conformance: begin <name>` log line buckets commands per case.
- **`ShowEngine`'s revision bumps every tick, so the loop must content-diff.** The plan said
  "publish a snapshot when the revision changed"; `ShowEngine.tickRevision` increments
  unconditionally, so a revision check publishes every tick. `hostLoop` diffs snapshot *content*
  instead. The failure mode is redundant snapshots, never missed ones.
- **`--conformance` ignores `--config` and uses `CONFORMANCE_CONFIG` internally.** The exported
  cases name that exact config; letting a caller substitute one would make the suite lie.
- **The restart-policy stub needed four members, not three.** `RecordRunning` was missing from the
  plan's Task 6 stub and is required by Task 7's supervisor test.
- **Two surfaces the plan did not name were needed:** `HostRuntime.capacity` (Task 3) and
  `ShowEngineSpawnRequest.ExtraArgs` (Task 13, to pass `--conformance`).
- **A look's CHAIR routes must be written even when the chair is empty (C2, whole-branch review).**
  `OhgHostAdapter.RoutesForLook` wrote `ohg-host`/`ohg-reader` only when the slot was non-null, on
  the reading that an absent key means "not this look's business". An absent key leaves the route
  UNTOUCHED, so a look with `readerSlot: null` inherited the previous look's reader — the previous
  guest stayed on air through a look that seats nobody there. This is the boxes' rule ("a route to
  an unassigned slot MUST clear `ParticipantId`") one level up, and the chairs were the one place
  it was not applied. Fixed: the key is written whenever the placement CARRIES the field, with a
  null value, which `OhgRouteSlotWriter` turns into Mode None with the ids cleared. Only a field
  the placement omits entirely still means "leave it alone". The conformance goldens did not move
  (every case seats both chairs), so nothing but the unit tests could have caught it.

## Decisions worth not re-litigating

Every ruling from the execution ledger, in the order it was made.

**Shape and surface**

- **The Task 6 restart-policy stub carries all four members** (`NextDelay`, `RecordRunning`,
  `RecordHealthy`, `Reset`) as no-ops. Task 7 replaces the body, not the surface.
- **`Request.invoke.args` is `readonly unknown[]`.** Pure widening, so an `as const` fixture binds.
- **`HostRuntime.capacity` is part of the runtime**, and **`tick()` content-diffs the snapshot**
  rather than trusting the revision (see corrections above).
- **`IControlActionProvider.Id`** stands as shipped; the spec text is what drifted.
- **`ShowEngineRestartPolicy` is not sealed** and `ControlActionRegistry.ById` is now dead but kept
  — both consequences of honoring move-only discipline in a task that was not about them.

**Failure behavior — the theme is *loud, never silent*, and *terminal only when retrying cannot
help***

- **A missing `version` in the config loads as unsupported, not as an implicit v1.** A hand-written
  config must declare its version.
- **Exit 78 (config rejected) is terminal with no backoff; a handshake *timeout* is recoverable
  through the normal policy.** Only a protocol-version mismatch and exit 78 refuse to respawn — a
  genuinely broken engine costs at most a few extra attempts, bounded by the 5-failure budget, and
  handshake failures share that budget with crash loops.
- **A stale (exited) child's log lines are TAGGED `[gen N, exited]`, never dropped.** Logs are
  inert text and the dying child's last words are the entire reason its stdout is drained to EOF.
  State from a stale generation is dropped; text from one is labelled.
- **Response-side `protocolVersion` tolerance stays** — three guarded lines, tested at the parser.
- **A bad manifest suppressing re-arm is accepted.** No registered actions means no operator control
  anyway; re-arming would be pointless. Cost if wrong: a healthy engine idles until the next
  generation.
- **The adapter refuses more than the spec required** — `setPreview` naming an unconfigured preset
  scene, an auto take when the transition is unavailable or refused, an unknown source kind, a null
  command name. Spec §8's "unset or missing" plus cut/auto symmetry make these the same class.
- **`OhgHostAdapter`'s `setGallery` status note is emitted ONCE PER ADAPTER LIFETIME**, not per
  change; spec §8's row was amended to say so rather than the code changed to match it.
- **Shadow mode logs and never applies**; the fake's scrambled route list is deliberately inert.

**Contracts nailed down under pressure**

- **`CueSceneWithRoutes`: an absent key leaves that route alone; an empty dictionary rewrites
  nothing.** Without this, "cue a scene" and "clear every route" are the same call.
- **The `setPreview`-before-`applyLook` refusal was fixed IN THIS PLAN, not carried.** The engine
  emits `setPreview(look:<id>)` one sequence *before* the `applyLook` that defines it, so the
  adapter refused every first look cue. Spec §8's "remember the preset from `applyLook`" was the
  wrong shape — the config already knows every look's preset, so `OhgHostAdapter` seeds its
  `lookId → scenePreset` map from `engine.looks[]` at construction. The `applyLook` memory stays as
  a second source.
- **The `--conformance` deviation** (handshake, wait for a parent request, run, wait for
  shutdown/EOF) is accepted; it is what makes the bracketing above possible.
- **The drill's judgement logic is unit-tested offline against fixture JSON.** A live run is
  attempted only when an app already answers on `127.0.0.1:8011`; otherwise the drill reports NOT
  RUN. No desktop takeover, per CLAUDE.md. On this box the live leg was **NOT RUN**.
- **`Debug.WriteLine` on every OSC router failure** (a superset of what was asked) is accepted —
  DEBUG-only, compiled out of Release.
- **An ASCII hyphen in the packaging script's missing-entry message** — an em-dash broke the
  PowerShell parser.
- **`StudioViewModelOhgFacade` ships with no unit tests, by plan.** Spec §11 states it is exercised
  by the Task 13 conformance run and the Task 14 drill; a reviewer flag here was expected and stays
  parked. Cost if wrong: facade bugs surface at 13/14 instead of 11.

**A ruling that became a required fix rather than a carry**

- The **disposed `scope.Token` hazard** — a child dead at spawn lets the reader loop dispose the CTS
  before `SpawnAndHandshakeAsync` reads `scope.Token` — was carried into Task 7 as a *required* fix
  (capture the token once, before starting the reader loop) rather than deferred. It is fixed and
  commented at the site.

## Carried into Plan 7b

**The plan-shaped carries.** Spec §10 (the WinUI OHG workspace) and the show-config importer were
Plan 7b by design and are untouched. Three more join them from execution:

- **Config-change restart, the config editor, and the importer.** The effective config is rewritten
  at each launch and read once; changing it requires an app restart. There is no UI to edit it.
- **`tallyUrl` posting.** The field is parsed, validated and reserved; nothing posts to it. Tally is
  published on the state node only.
- **Rendering `restoreWarnings` / `pagingRefused`.** They reach `ControlState`; nothing draws them.

**Defects and gaps carried, each with a reason it was not fixed here**

- **The core's positional fallback means an empty box is NOT guaranteed blank.**
  `native/src/core/RouteSourcePolicy.h` makes a route with no participant/capture/media id — under
  `fixed` **or** `none` — inherit `videoFrames[routeIndex]`, so a cleared OHG box can composite an
  arbitrary decoded guest. Fixing it needs a route-contract sentinel in the C++ core, which is not
  Plan 7a scope. This is the one carry with an on-air consequence: until it lands, "cleared" means
  *not the previous guest*, not *blank*, and the drill asserts accordingly.
- **The engine cues each look twice.** `setPreview(look:<id>)` cues the preset with empty routes one
  sequence before `applyLook` cues the same preset with routes. Now that the adapter seeds its
  preset map this is harmless — one redundant scene cue per look change — but the fix belongs in the
  engine's emission order, not in the adapter.
- **No per-input lower-third title.** The shell has no such surface, so `SetInputLowerThirdTitle` is
  a logged no-op (once per adapter lifetime). Nameplate locations are not shown until 7b.
- **Tiles cell ordering is recorded, not driven.** `setGallery` is captured and surfaced; CoreVideo
  Tiles has no explicit cell-order API. A Tiles ordering API is a separate design.
- **Raise-hand is not on the core protocol**, so `handRaised` is always `false`. The hands queue
  remains Mukana's.
- **The non-OHG `ControlState` / `StateFields` / `OscFeedback.Encode` drift predates this work** and
  is deliberately not fixed. `ControlStateFieldsTests` asserts the three lists agree **for the `ohg`
  fields only** — the new surface is pinned, the old one is still unpinned and known to be.
- **`smartGallery` is published and readable but not persisted** (a Plan 6 carry, unchanged): a
  restart reverts it to off, having possibly persisted a frozen smart-ordered arrangement.
- **OSC still carries no auth token.** Loopback-vs-LAN gating is the only control, so
  `COREVIDEO_OSC_OHG_LAN=1` puts on-air actions on the LAN unauthenticated. The gate defaults
  closed for `ohg.*` precisely because of this.
- **The `verify:barrel` script cannot run whole on Windows** (pre-existing, unchanged by this
  branch): `import(<absolute path>)` fails `ERR_UNSUPPORTED_ESM_URL_SCHEME` and the type-only step
  shells out to `npx`. Its runtime and conformance checks were run under a patched copy and pass;
  CI on ubuntu runs it intact. Worth a `pathToFileURL` one-liner in 7b.

## Deferred minors

Every `minor (deferred)` from the ledger, kept so they are findable rather than rediscovered.

| Task | Deferred minor |
|---|---|
| 1 | Dead `typeof`-string guard in `encodeLine` (`protocol.ts:138`); `mapReplacer` docstring could show an example |
| 2 | `nodeStateFs.mkdir`'s `EEXIST` catch is likely unreachable (recursive `mkdir` never throws `EEXIST`) |
| 3 | Double-answer after a throw post-response (`hostLoop:113/149`); `lastPublishedRevision` never read; `'engine' in parsed` vs `??`; `--generation` accepts a non-integer; `ENGINE_VERSION` literal; `restore()` failure exits 70 by fallthrough; `Rig.revisionOf` unused |
| 4 | `ControlActionRegistry.ById` is now dead (kept to honor move-only) |
| 6 | Timed-out request task leak; `Raise` swallows subscriber exceptions silently; exit-78 intermediate health; `TrimToNewestHalf` counts chars not bytes, and its tail may begin mid-line when there is no newline after the start; `_handshakeSignal` has no child guard; the supervisor is 701 lines against the spec's ~300 (`SpawnAndHandshakeAsync` extraction); `JsonDocument` leak when a response races `SendCoreAsync`'s finally; non-atomic check-then-claim window in the withdraw; response-side `protocolVersion` tolerance unexercised at supervisor level |
| 7 | The healthy-reset scenario is packed into the same `[Fact]` as the exhaustion scenario; `ShowEngineRestartPolicy` is not sealed |
| 8 | an `IsCanceled` continuation is swallowed; `IdOf` is a dead test helper; a weak drift assertion (`Contains` digits); the report said 10 facts, the diff has 9 |
| 9 | `ValidateCapacity`'s `found <ValueKind>` formatting for wrong-typed values; `ValidateLooks` silently skips malformed look entries (deliberate, uncommented); duplicated `Engine(json)` test helper |
| 10 | The fake's scrambled route list is inert (the comment says otherwise); every exception is labelled "malformed args"; `setPreview`'s slot has no upper bound; `ShadowLog` exposes its live list; lenient arity; the missing-route dedupe names only the first look |
| 11 | `ApplySlotToRoute` clears `SpotlightIndex` on every route (the picker only appears on non-slot branches); the new method lands on `StudioViewModel` via a partial file rather than a focused type; `PublishCapacity(ShowInputEditors.Count)` vs the brief's literal 10 (self-correcting); the report cites the wrong comment for the Fixed-renders-slate choice |
| 12 | The sync script auto-creates `AppDir` (siblings throw); forward slashes in the missing-entry message; `Assert-MsixPayloadReady` not extended to node/show-engine |
| 13 | Pinned seq numbers removed with the fix; discarded out-of-bracket commands; `argv.includes` over the full argv; trailing space in the spawn log; the logs list is formatted while being appended; the exit-code test's `ReadToEndAsync` path can orphan node if the child wedges with stdout open; sequential stdout/stderr `ReadToEnd` shape |
| 14 | The Companion module casts `ohgFields` values without a `typeof` guard; the drill probes speculative `looks` fields (always SKIP) |

Two Task 6 minors were struck from the table above rather than carried: "`Dispose` reads `_child`
unlocked and would dispose `_stdinGate` under a write" (verified stale — `Dispose` reads `_child`
INSIDE `lock (_gate)` and deliberately never disposes `_stdinGate`, with the reason written at the
site) and "`AbortStart` under `_stopping` leaves `_child` attached until `StopAsync`/`Dispose`"
(subsumed by C1 below: the attach is now a claim, and a superseded or stopping spawn attaches
nothing at all). Task 8's "fire-and-forget sends log a warn per roster change while the engine is
down" was fixed, not deferred — see I2.

## Final-review findings, fixed pre-merge

A whole-branch review after Task 15 found five defects the per-task reviews did not. All five are
fixed on this branch, each with a test that reds without its fix.

- **C1 — an orphaned engine process on Restart during backoff.** `SpawnAndHandshakeAsync` attached
  its child with no supersession check, and a `RecoverAsync` parked on its backoff delay survives
  BOTH `StopAsync` (which returns early when `_child` is already null, which it always is during a
  backoff) and `RestartAsync`. The parked task woke after the operator's restart had generation 2
  running, spawned generation 3, and overwrote `_child` — stranding generation 2's node.exe alive
  with its stdin held open by nobody. The attach block is now also a CLAIM (`_stopping ||
  _disposed || _child is not null` ⇒ kill + dispose the just-spawned child and its scope, return
  false), plus a cheap pre-spawn `_stopping`/`_disposed` check so a stop while parked spawns
  nothing at all rather than spawning and immediately killing. Normal recovery always has
  `_child == null`, so the guard fires only on supersession. `MainWindow` also now DISPOSES the
  supervisor on the shutdown path — it was only ever stopping and disposing the bridge, and the
  bridge does not own the supervisor. The supervisor is held in a `MainWindow` field rather than
  given to the bridge to own: the bridge is handed a supervisor in its constructor and unhooks its
  events on Dispose, and changing that to ownership would change the disposal contract of every
  caller that constructs the pair.
- **C2 — an unseated chair kept the previous guest on air.** See the corrections list above.
- **I1 — `ohg.panelist.add` without a slot was refused on every transport.**
  `ControlCatalog.TryBind` pads omitted optional params with `null`; the engine's `bindArgs` treats
  only `undefined` as absent, so `["p1", null]` failed coercion. Fixed SHELL-side (controller
  ruling): `ShowEngineBridge.InvokeAsync` trims TRAILING nulls before serializing. An interior null
  is positional and stays.
- **I2 — roster republished at media-core snapshot rate, and a warn-flood when the engine was
  down.** `PublishRoster` now compares against the last roster (order-sensitive, all seven fields)
  and returns early when unchanged; all three publishers RECORD but do not send — and do not log —
  while `Health.State != Running`. The next handshake's re-arm delivers exactly what was recorded,
  and health is the operator's signal that the engine is down.
- **I3 — host commands were not serialized across an await.** Each command was fire-and-forgotten
  onto the dispatcher, which orders only the STARTS: `cut`/`auto` await `TakeAsync`, and the next
  command's `applyLook` ran inside that await and rewrote the preview draft mid-take. They now
  chain through a `SequentialAsyncQueue` (a small testable seam in `WinUI/Services`), one link at a
  time in `seq` order, with each link's exception reported and swallowed so a fault can never stop
  the chain draining.

Three cheap minors rode the same wave: `ShowEngineProtocol.ParseSnapshot` now throws
`FormatException` on a missing `snapshot` node (it used to yield `default(JsonElement)`, which
survived to `ControlState.Ohg` and threw inside the control server's JSON writer) and the reader
loop logs it on its malformed path; the Companion module no longer double-registers
`ohg_health_engine`/`ohg_shadow_lastCommand` (the shell publishes them as top-level ControlState
scalars, and they were ALSO being expanded out of `/manifest`'s feedbackFields); and the
`AdapterConformanceTests` exit-code test wraps its whole read/wait span in a `finally` that kills
the node tree, so a child that wedges with stdout still open cannot be orphaned.

CLAUDE.md's OHG section also had the restart ladder wrong (`5→10→20→40→60 s`); the shipped
`ShowEngineRestartPolicy.Delays` is `1, 2, 4, 8, 16, 30 s`.

## Mutation results

Rule 7 was operationalized again: every task brief carried a named "Mutations to run" block and
every implementer ran them and reported signatures. **45 mutations across 12 of 14 tasks; every one
red, and two of them found a broken *guard* rather than broken code.**

| Task | Mutations | All red? | Note |
|---|---|---|---|
| 1 | 3 | yes | `Map` replacer, `encodeLine` try/catch, numeric-id guard |
| 2 | 3 | yes | dropped `signal`, `seq` off-by-one, `Object.fromEntries` for Maps |
| 3 | 4 | yes, after correction | mutation 4 was first run on the **test rig** rather than the emit site — a mutation that proves nothing. Re-run against `stdioHostAdapter.ts` in fix round 1 |
| 4 | 3 | yes | provider shadowing, ctor-time caching of `provider.Actions`, unconditional LAN exposure |
| 5 | 4 | yes | loopback refusal disabled, refusal moved after invocation, catalog-lookup short circuit, OSC bool encoding |
| 6 | 9 (4 + 5 in fix round 1) | yes | includes check-then-detach with a 5 ms window, `IsCurrent` dropped on the snapshot raise, a generation-number-only host-command guard, `catch { return; }` on the watchdog, `terminal: true` on handshake failure |
| 7 | 3 | yes | unconditional healthy reset, no escalation index, `>` to `>=` on the budget |
| 8 | 4 | yes | **mutation 4 found a weak test**: keeping `Actions` on `Failed` was invisible to the Stopped-only test. A second test was added |
| 9 | 3 | yes | env-pair `&&` to `||`, a capacity check that also accepts 9, dropped `HasNonEmptyStatePath` guard |
| 10 | 4 | yes | box naming by enumeration index, shadow short-circuit removed, once-per-report guard removed, `slot < 1` to `slot < 0` |
| 11 | 3 | yes | unknown-mute defaulting, `StartsWith("ohg")` without the dot, engine health when no snapshot has arrived |
| 12 | 2 (narrative) | yes | a missing `dist/host/main.js` produced the exact named message; the Node-under-24 gate rejected two real sub-24 installs found on this box |
| 13 | 3 (2 + 1 in fix round 1) | yes | swapped `hostSlot`/`readerSlot`, dropped `assignSlot` forwarding, and a deleted `RecordingStdioFacade.cut()` override reds the wrapper-law test |
| 14 | none listed | — | the drill judge's own suite is nine cases over fixture JSON, each an inverted assertion |

## Process note

**Fix rounds: 8, across 14 tasks.** Tasks 3, 4, 8, 9, 11 and 13 took one round each; Task 6 (the
supervisor) took two; Tasks 1, 2, 5, 7, 10, 12 and 14 needed none. Task 6 is the outlier and it is
the right one to be the outlier — it is the only task holding a live child process, a reader thread,
a generation counter and a restart budget at the same time, and every one of its five first-round
Importants was a concurrency hole (non-atomic `OnChildEnded` giving double recovery, an unguarded
stale snapshot, a host-command guard blind during the recovery window, a `catch` that killed the
watchdog, a terminal handshake path). The re-review then found a *sixth*, introduced by the fixes:
an exit-before-handshake race that let exit 78 take the recoverable path.

**Which authoring rules fired.**

- **Rule 7 (put the mutations in the brief)** — again the load-bearing one. 45 mutations, all
  reported unprompted, and twice it did what rule 7 exists to do: surface a broken *guard*. Task 8's
  fourth mutation proved a Stopped-only test could not see `Failed`; Task 3's fourth proved the
  implementer had mutated the fixture rather than the emit site, so the "red" it produced was
  meaningless. Both were caught by implementers mid-mutation, not by reviewers.
- **Rule 10 (a structural test must exercise the structure)** — Task 13's wrapper-law finding is a
  textbook instance: `RecordingStdioFacade` forwarded calls by hand, so a facade that silently
  *stopped* forwarding one would still look like a facade. The fix was a test that deletes an
  override and watches it red. Same family as the `ICaptureDevice` wrapper law in CLAUDE.md.
- **Rule 9 (a new contract needs a conforming fixture)** — the whole of Task 13. The plan could have
  stopped at unit tests on both sides of the stdio seam; instead the conformance suite runs **the
  real engine against the real adapter**, and that is what caught the ordering defect below.
- **Rule 4 (an async fixture must drain)** — `DelayController` in `ShowEngine.Tests` exists so the
  backoff tests observe real orderings rather than whatever a fixed number of awaits happens to
  reach.
- **Rule 6 (walk the carried obligations line by line)** — the plan's obligation table maps each
  Plan 6 carry to the task that discharges it, and the carries this plan could not discharge are
  named above rather than dropped.
- **Rules 1 and 5** fired quietly: the capacity-must-be-10 fixture invariant, and Task 14 reading
  `ohg.panelist.remove`'s real signature rather than the plan's claim that it takes a PIN.

**What reviewers found that mutations did not.** Every Important in this plan came from a reviewer
or from an integration run — never from a listed mutation. Task 4's unsynchronized `_snapshot` read;
Task 6's six concurrency holes; Task 8's two test gaps (the roster/capacity wire shape was
unasserted, and the not-running invoke test could not observe "without sending"); Task 9's untested
missing-version behavior; Task 11's stale `ParticipantId` — the one operator-visible on-air defect
in the plan, where a route to an unassigned slot kept the previous guest on air; and Task 13's four,
including a gallery golden that cited a spec row saying the opposite of what it asserted.

**And what neither found — the integration run did.** The engine emits `setPreview(look:<id>)` one
sequence **before** the `applyLook` that defines that look, so the adapter refused *every first look
cue*. 943 WinUI tests, 135 bridge tests and 945 vitest tests were green on both sides of the seam;
the defect existed only in the ordering *between* them, and it appeared the first time the real
adapter was driven by the real engine. This is the same lesson as Plan 6's "sync marks a healthy
poll hung" — a defect no amount of testing within one population can reach — but one level up: not a
missing fixture population, a missing *process boundary*.

**A candidate rule 11, offered not added.** *A plan that introduces a cross-process or
cross-language seam must drive it end-to-end inside the plan, not after it.* Rule 9 says a new
contract needs a conforming fixture; this says a new **seam** needs a conforming *run*. The evidence
is that Task 13 — the only task whose job was to run both sides together — found a defect that the
1,023 unit tests written specifically for those two sides could not, and that its second finding (19
host commands silently dropped outside the handshake window) was a property of the *supervisor* that
only a live child could exhibit. Whether that is a new rule or a sharper reading of rule 9 is a
judgement for Plan 8's author; recorded here either way.

**A smaller note, on honesty of scope.** Two things in this plan were shipped as deliberate no-ops
and said so at the code site: `SetInputLowerThirdTitle` (no shell surface) and `setGallery` (no
Tiles ordering API). Both log once per lifetime rather than per call, so an operator sees the gap
without a log flood. That is the right shape for "we know, and it is 7b" — better than either
silence or a fabricated behavior.
