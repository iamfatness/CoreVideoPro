# OHG show engine host (Plan 7a, 2026-09-07)

_Moved verbatim from `CLAUDE.md` (#737). Paths are relative to the repo root._

The OHG show engine (`show-engine/`, TypeScript) runs **as a fourth, optional child process** and
drives the shell over the existing control surface. It is not media — it is show *direction*
(seating, looks, hands queue, gallery) that issues commands the shell applies.

**What runs where.** `CoreVideoPro.ShowEngine.ShowEngineSupervisor` spawns
`node show-engine/dist/host/main.js --config <path> --generation <n>` and speaks JSON lines over
stdin/stdout (the same shape as the core/engine pipes). `ShowEngineBridge` implements
`IControlActionProvider`, so the engine's 28 `ohg.*` actions are **merged into `ControlCatalog`**
at runtime — `ControlActionRegistry` stays a closed compile-time list and is NOT made mutable; the
catalog composes static + provider. State comes back two ways: the raw `ShowSnapshot` under
`ControlState.Ohg` (`/state` → `ohg`, camelCase straight from TS, never mirrored into a C# type)
and a **flattened** `ControlState.OhgFields` map (`ohg/slot/3/tally` → one scalar) that OSC and the
Companion module read without walking the nested object. `ohg.*` actions are **loopback-only over
OSC** by default (`OscExposure.LoopbackOnly`) — a LAN sender is refused with a message naming
`COREVIDEO_OSC_OHG_LAN=1`, which is the only way to open them up. OSC carries no auth token, so
opening them puts on-air actions on the LAN unauthenticated; that is the whole reason for the gate.

**Configure it:** `%LOCALAPPDATA%\CoreVideoPro\ohg-show-config.json` (`ShowConfigStore`). `engine`
is opaque to the shell (validated engine-side); `shell` is ours:
- `driveHost` (**default false = shadow mode**) — host commands are logged to
  `ohg/shadow/lastCommand` and the log, and NEVER applied. Ship a new show config in shadow first.
- `presets` — the four fixed scene ids the engine cues by name: `solo`, `activeSpeaker`, `black`,
  `gallery`. Unconfigured = null = refused **at use time**, loudly, not at load.
- Route ids are naming, not config: `ohg-box-<n>` per look box, plus `ohg-host` / `ohg-reader`
  for the two chairs (a chair route is only written when that chair is seated).
- `engine.capacity` **must be 10** and must equal the host capacity — a mismatch is a loud config
  refusal, not a clamp. A config with **no `version`** is refused as unsupported (never assumed v1).
- `tallyUrl` is parsed and reserved; nothing posts to it in 7a.

**Edit it in the app (Plan 7b Task 10):** Settings -> **OHG show** edits the same
`ShowConfigStore` document (integrations, Mukana polling, looks, the four preset scenes,
`driveHost`, default transition, tally URL). **Save** validates against the app's CURRENT
scene ids, writes the effective config, hot-swaps the `OhgHostAdapter`
(`StudioControlSurface.ReplaceOhgAdapter` -> `OhgAdapterSlot`), rebuilds `StudioViewModel.OhgShow`,
then restarts the engine — in that ORDER, which is pinned by the pure
`OhgConfigApplySteps.Order(engineRunning)` (restarting before materializing boots the engine on
the PREVIOUS config; swapping the adapter after the restart cues the wrong scene on air). With no
engine this launch it validates + writes only and the page says to restart the app. **The adapter
is read at APPLY time, never at enqueue time** — a host command queued behind an awaiting take
must use the adapter that is current when it RUNS, because the engine has by then been restarted
onto the new config. `OhgConfigEditModel` is deliberately NOT observable, so the section binds
`[ObservableProperty]` mirrors on `OhgSettingsViewModel` that write through to it; the intervals
and per-look box count are `double` because `NumberBox.Value` is a double and x:Bind will not
narrow it back. Every ComboBox in the section is filled and selected in guarded code-behind (never
x:Bind selection), and every interactive element is named `OHG settings ...`.

**OHG Show tab (Plan 7b).** Open it from the Produce nav group — the **"OHG Show"** button
(nav key `ohgshow`, `StudioTab.OhgShow`) — for the status strip, panelist board, program, gallery
and GFX/data panels. Four rules govern anything you change there:

- **The panels are THIN RENDERERS.** `OhgShowViewModel` ingests the engine snapshot on the UI
  thread and projects it (`OhgSnapshotView`, pure); rows are **diff-updated in place** via
  `ObservableCollectionSync` and scalars are `[ObservableProperty]`. So a `PropertyChanged` storm
  while this tab is up is a **snapshot-rate bug** (the engine publishing too often, or a projection
  that mints new values from equal input) — **not** a UI bug. Fix the rate or the projection; never
  add a UI-side throttle on top.
- **Every button is an `ohg.*` invoke through `IOhgActionInvoker`.** The page and its view models
  reach the engine ONLY through that seam (typed `OhgActionArgs` builders) — no direct bridge or
  supervisor calls, which is what makes every control testable without a running child process.
- **0xc000027b discipline, deliberately:** diff-updated rows (never a bound collection replaced at
  snapshot rate), every code-behind handler — **including DependencyProperty callbacks** — wrapped
  in `Guarded(...)` so a throwing callback logs instead of fail-fasting the process, and ComboBox
  selection applied in guarded code-behind rather than x:Bind. There is **no exception**: both
  `OhgShowPageContentTests.Page_GuardsEveryUiCallback` and its settings-window twin now read each
  handler's BODY and fail unless it routes through `Guarded(...)` (naming the handler was not
  enough — `OnRoleComboLoaded` was hooked from the XAML and delegated to a method with its own
  try/catch, which is a different guarantee and invisible at the handler).
- **The page's `ViewModel` DependencyProperty is assigned AFTER construction, so `Mode=OneTime` is
  forbidden on any `ViewModel.` path — commands included.** A OneTime binding evaluates against a
  null root and never re-evaluates: the "Set up OHG" button was inert and the Gallery note empty.
  Pinned by `Page_NeverBindsALateBoundViewModelPathOneTime`.
- **A settings picker may only offer values the ENGINE accepts.** `optionalPlateTone` and its
  siblings (`show-engine/src/config.ts`) THROW on anything outside their enum, and a rejected
  config is exit 78 — terminal, no respawn. The plate-tone picker shipped `warm`/`cool`, which no
  engine build has ever accepted. The one copy of the three enums is `OhgLookChoices`
  (`Services/OhgConfigEditModel.cs`); the pickers and `OhgSettingsViewModel.Validate()` both read
  it, and `OhgSettingsChoicesTests` pins it against `contracts.ts`. Validate also requires a
  non-blank look `label` — `ToConfig()` would otherwise emit `label:""`, which `requireString`
  refuses at parse time.
- **The seat button is one gesture with two meanings.** It selects the seat AND runs the assign
  command. A successful assign therefore CLEARS `SelectedParticipantId`, and a tap with nothing
  selected only selects the seat (silently) — without that, tapping a second seat merely to look at
  it fired `ohg.panelist.replace` on air with the previously selected guest.
- **Saving the OHG config hot-restarts the engine and REBUILDS the page view model** (the order is
  pinned by `OhgConfigApplySteps.Order`, above). The one exception is first-time setup with no
  engine running this launch: that writes the config and asks for an **app restart**.
- **The importer is HONEST by contract** (Settings -> OHG show -> import from the legacy Isadora
  files): it reports `Found`/`NotFound` per probe in plain words, **never guesses** a value it
  could not read, and **never changes capacity** (a legacy `videoPins` that disagrees is reported,
  not applied).

The **first-launch checklist** the owner runs on first open lives in
`docs/superpowers/plans/2026-09-07-show-engine-winui-workspace-outcomes.md`.

**Env vars:** `COREVIDEO_NODE_EXE` + `COREVIDEO_SHOW_ENGINE_DIR` (BOTH or neither — one alone is
ignored) select a dev/override host; otherwise `<app>\node\node.exe` + `<app>\show-engine\` (packaged,
staged by `scripts/sync-node-runtime-to-app.ps1`), then `node` on PATH + `<repo>\show-engine\` (dev).
`COREVIDEO_OSC_OHG_LAN=1` exposes `ohg.*` to LAN OSC. Node **>= 24** is required.

**Exit codes** (the host owns them; the supervisor reads them): `64` usage (bad argv), `78` config
rejected — **terminal, no backoff, no respawn**, because a bad config will be bad again — `70`
anything else (restartable). Restarts escalate 1→2→4→8→16 s, then Failed after the 5th consecutive
failure (the 30 s rung in `ShowEngineRestartPolicy.Delays` is reachable only with a raised budget);
a 60 s healthy run resets the budget. `--conformance` runs the exported host conformance
suite in-process and exits 0 iff every case passed (this is what the xUnit integration test drives).

**Logs:** `%LOCALAPPDATA%\CoreVideoPro\show-engine.log` (the engine's own `log` events, and the
supervisor's) and `launch.log` `ohg:` lines for startup/resolution/teardown (`ohg: show engine
starting (dev|packaged|env) node=… entry=… driveHost=…`).

**Test it:** `npm run typecheck:show-engine`, `npm run test:show-engine` (vitest),
`npm run smoke:show-engine-host` (spawns the real host, asserts handshake + 28 actions),
`dotnet test native-shell/CoreVideoPro.ShowEngine.Tests`, and — the one that actually proves the
seam — `AdapterConformanceTests` in `CoreVideoPro.WinUI.Tests`, which **spawns node** and drives
every conformance case through the real `OhgHostAdapter` to a golden facade sequence. (It lives in
WinUI.Tests, not ShowEngine.Tests, because ShowEngine cannot reference WinUI.) Operator drill:
`node scripts/validate-show-engine.mjs --base http://127.0.0.1:8011` against a running app;
its judgement logic is unit-tested offline by `npm run test:show-engine-drill-judge`.

**Three gotchas learned the hard way:**

- **Every supervisor event is guarded by generation, and a dead child is drained to EOF.** A
  respawn means responses, snapshots and host commands from the OLD child can still be in flight;
  each is checked against the current child (by reference, not just a generation number — a
  number-only guard reds nothing when the child object is swapped) and dropped. The exception is
  **log** lines: the dying child's last words are the whole reason we drain its stdout to EOF, so
  they are **tagged** `[gen N, exited]` rather than dropped. Logs are inert text; state is not.
- **A route to an unassigned slot MUST clear `ParticipantId`.** Writing only the slot number left
  the previous guest's participant id on the route — so cueing a look with an empty box put the
  PREVIOUS guest on air. `OhgRouteSlotWriter` clears participant/role/spotlight on every route it
  writes; the test is the contract.
- **An empty route renders BLANK (#480).** `RouteSourcePolicy` never inherits
  `videoFrames[routeIndex]`. A `fixed`/`none` route with no participant/capture/media
  id (and a follow-speaker route with nobody directed) binds nothing; the compositor
  paints a transparent fill instead of the default grey or a random guest. The shell
  may only change a scene layer's source on an operator gesture — a ComboBox list
  refresh that lands on the blank placeholder is ignored (`LayerSourceSelectionPolicy`,
  log `by=operator` / `by=refresh-ignored`).
