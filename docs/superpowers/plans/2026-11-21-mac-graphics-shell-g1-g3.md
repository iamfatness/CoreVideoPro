# Graphics in the Mac shell, G1-G3: gateway, On Air strip, playlist, F-keys, authority

Status: plan, 2026-11-21. Worktree `.claude/worktrees/mac-graphics-shell`, branch `mac-graphics-shell`, from
`mac-metal-graphics-g1` afb05c26 (Metal G1+G2: the Mac core accepts `graphics-*` and draws templates).

> **For agentic workers:** use superpowers:executing-plans. `docs/superpowers/plan-authoring-rules.md` is
> binding. Each task names the mutations to try and the report says which went red. A supplied test that
> contradicts a reasonable implementation is the first suspect.

**Goal.** A Mac operator can see what graphics the core is drawing and take them off (On Air strip with Hide,
Clear all, Clear cued), keep a playlist of graphics rows that persists in the same schema-3 document as Windows,
Cue / Show / Hide each row (Guest rows per roster source), press F1-F12 to play rows and Shift+F12 to clear,
have cued graphics ride a Take, and drive the lower third through one authority-aware entry. The legacy keyer,
brand kit and captions move into an "Existing overlay controls" drawer with the L5/L7/L8/L18a fixes.

**Spec:** `docs/superpowers/specs/2026-11-15-mac-graphics-shell-design.md`, slices G1, G2, G3 (section 6),
sections 2.2-2.5, 4, 5.1-5.5, 5.9-5.10, 7. Master plan `docs/superpowers/plans/2026-11-15-mac-parity-master-plan.md`
Phase 3 rows "Graphics in the shell G1-G3" and "B1 Shared Keychain helper". Behavioural reference: Windows
`native-shell/` (W/ = `CoreVideoPro.WinUI/`, MC/ = `CoreVideoPro.MediaCore/`, M/ = `mac-shell/Sources/CoreVideoProShell/`).

**Owner decisions applied.** Mac release before the Designer (so "Open in Design" is a disabled affordance
naming the later slice). Guest rows before the Sources rebuild show roster/device names with **empty titles**.

---

## Defaults taken (owner may override)

| # | Default | Why | Tasks |
|---|---|---|---|
| D1 | **Thin gateway pre-check.** Ported from `GraphicsCommandRequest.TryBuild` (MC/Services/GraphicsCommandGateway.cs:104-273): action table and per-action fields, size caps (2 MiB install, 64 KiB automation configure, 32 KiB else), identity fields and `schemaVersion 1`, id syntax, `coreEpoch` equality, unknown/duplicate fields, the instanceId / expectedRevision requirements, the enum rules (trigger action and target, update target, program.clear channel, authority family/authority, cueOwner only with uncue). **Not** ported: deep validation of style, animations, clock, automation, binding, image and field values; the core re-validates all of it (`:78-79` "Native validation remains authoritative"). | spec 4: "Swift, kept thin" | 4 |
| D2 | **Vector exporter in C#**, `native-shell/CoreVideoPro.MediaCore.Tests/GraphicsShellVectorTests.cs`, computing `expected` with the real MediaCore types and failing when the committed `contracts/graphics-shell/*.vectors.json` differ (`COREVIDEO_WRITE_GRAPHICS_SHELL_VECTORS=1` rewrites). Runs on this Mac under `~/.dotnet`. Gateway vectors include only cases inside D1's scope. | spec 5.2 | 4, 6, 7 |
| D3 | **Document store types only what the Mac edits** (playlist rows, authority, template pins it reads); every other section and every unknown key is preserved verbatim on save. Validation covers the top level and the playlist rows (port of `GraphicsPlaylistRow.IsValid`); the unported sections are not re-validated (Windows wrote them). | spec 5.6 "unknown properties are preserved"; a Mac without the Designer must not strip designs | 6 |
| D4 | **Document path** `~/Library/Application Support/CoreVideoPro/graphics/default.json`, override `COREVIDEO_GRAPHICS_DOCUMENT_PATH` (Windows parity). The shell test harness always points it into a temp dir. | spec 5.6 | 6 |
| D5 | **The Windows-show import spike is not done here:** there is no Windows show folder on this Mac. A Windows-written `default.json` is covered by a fixture document (exported by the C# exporter from real Windows types), not by a real show. Asset folders (`PackageRoot`) stay UNVERIFIED. | spec risk 4 | 6 |
| D6 | **Default rows**: a show with no playlist gets Windows' five default rows for the "interview" starter (`GraphicsWorkspaceLibrary.DefaultPlaylistFor`, MC/Services/GraphicsWorkspaceLibrary.Playlist.cs:11-34) only on an explicit "Add default rows" button, never automatically (Windows seeds through its starter flow; the Mac has no starter picker until G8). The sponsor row stays a placeholder (no image import until G8). | Windows parity without inventing a starter UI | 8 |
| D7 | **F-keys** route through the existing in-app key monitor (`TransportHotkeyMonitor`), Studio and Graphics tabs, never while a text field has focus. The pane shows one hint line about macOS F-key settings when any row has a hotkey. No global hotkeys. | spec 5.4 / risk 5 | 9 |
| D8 | **Lower-third authority** routes per `StudioViewModel.ApplyStudioLowerThirdIntentAsync` (W/ViewModels/StudioViewModel.StudioLowerThird.cs:49-72) minus the "assigned studio look" branch, which has no Mac UI yet (G8). The suppression flag is recorded and published but nothing consumes it while Auto is retired on Mac (spec 5.5). | spec 5.5 | 11 |
| D9 | **Legacy drawer fixes**: L5 positions lower-left / lower-right / upper-left / upper-right (W/Views/OverlaysPage.xaml:128-131), L8 presets Quick 220/160, Smooth 420/320, Gentle 700/550, Custom with default Quick 220/160 (W/ViewModels/StudioViewModel.cs:758-768, 3476-3490), L7 timing changes re-send an on-air key live (`:3527-3541`), L18a the brand kit is sent at launch and on every core generation. A saved Mac preference with an old preset name ("snap", "standard", "gentle") loads as Custom with its saved numbers, never silently changing timing the operator chose. A saved "center" position loads as lower-left. | audit 2b rows L5/L7/L8/L18 | 5 |
| D10 | **Timers** (Show's cue wait, auto-out) go through an injected sleep (`AppModel.timerSleepForTesting`, cherry-picked from def55f05, and a playout `sleep` closure); tests drive them with `ShellTests.ManualTimers` or an immediate scripted core, never wall-clock races. | coordinator instruction | 7 |
| D11 | **Live check** runs an opt-in shell test (`COREVIDEO_SHELL_LIVE_CORE=<path to corevideo-native>`) that launches a real core through `MediaCoreBridge` with `CoreLaunchEnvironment.extras`, plays a playlist row through the real playout service and checks the instance state and Program pixels. Never the owner's app. | coordinator instruction | 12 |

---

## Conformance and test approach

- **Vectors** (`contracts/graphics-shell/<area>.vectors.json`, rows `{id, operation, input, expected}`): gateway,
  state, playout, document. The C# exporter is the oracle; the Swift harness replays every row.
- **Scripted core**: a fake core in the shell harness records each `media-core-sync` body and answers with a
  `graphics` node the test scripts, so sequencing (create → cue → wait → trigger), correlation, overload and epoch
  change run without a real core.
- **Pixels and live**: Task 12 with a test-launched Metal core.

## Carried in from G2 (Metal)

`CoreLaunchEnvironment` is the only way the core learns anything beside its executable (installed-layout rule).
Nothing in G1-G3 needs a new entry: the shell sends graphics over stdio only.

---

## Task 1: this plan

## Task 2: cherry-pick def55f05 (harness timer seam)

Already applied as `da9feb1c` before planning; shell suite must stay green.

## Task 3: `KeychainStore` (shared Keychain helper, master plan B1)

**Files:** new `M/KeychainStore.swift`; `M/StreamKeychain.swift`, `M/ZoomOAuth.swift` moved onto it; new
`M/ShellTestsGraphics.swift` (cases registered in `ShellTests.runAndExit`).

**Interfaces.** `struct KeychainStore { let service: String; let account: String; func read() -> Data?;
func write(_: Data) -> Bool; func delete() -> Bool }` over a `KeychainBackend` protocol (`SecItem` in
production, an in-memory backend in tests). Item names unchanged: `us.iamfatness.corevideopro.stream-key`/`rtmp`
and ZoomOAuth's own service/account constants.

**Tests.** In-memory: write/read/overwrite/delete; empty stream key deletes (StreamKeychain contract); the Zoom
store never names the OBS plugin item. Real Keychain round trip under a unique throwaway service, deleted after;
it skips loudly when the Keychain is not available (CI).
**Mutations:** StreamKeychain using a different service (red), write not updating an existing item (red).

## Task 4: graphics snapshot decode and gateway (+ gateway/state vectors)

**Files:** new `M/GraphicsState.swift`, `M/GraphicsGateway.swift`; exporter
`native-shell/CoreVideoPro.MediaCore.Tests/GraphicsShellVectorTests.cs`; `contracts/graphics-shell/gateway.vectors.json`,
`state.vectors.json`, and `contracts/graphics-shell/nodes/*.json` (real `graphics` nodes captured from a Metal core).

**Behaviour.** `GraphicsState` decodes the node lossily-safe (missing fields default as in
MC/Models/NativeGraphicsState.cs). Availability: running core, `profile.capabilities` contains
`graphics-instances`, node `schemaVersion == 1`, non-empty `coreEpoch`; else every intent is refused
`graphics-native-state-unavailable` and nothing is sent (`GraphicsCommandGateway.cs:39-41`). `GraphicsGateway.invoke(action:
request:)` runs the D1 pre-check, wraps `{type: action with "." -> "-", graphics: body}` in one `media-core-sync`,
holds one command in flight (`graphics-command-overloaded`, `:61`), correlates by `requestId` + `clientId` in
`graphics.operations[]` then `lastResult`, same epoch, else `graphics-result-not-correlated` (`:65-67`); social
client id refused (`:44-46`). On-air truth is only the instances (`IsOnProgram`/`IsOnPreview`,
GraphicsPlayoutService.cs:96-97).

**Tests.** Every gateway and state vector replays; scripted-core ports of `GraphicsCommandGatewayTests`
(`NativeRefusalAndUncorrelatedResponsesCannotBecomeSuccess`, `BusyGatewayRefusesNewIntentAndRecoversAfterTheFirstAdmission`,
unavailable core / missing capability / epoch conflict).
**Mutations:** skip the epoch check (red), accept unknown fields (red), correlate on lastResult only (red),
drop the in-flight gate (red).

## Task 5: Graphics tab frame, On Air strip, legacy drawer + L5/L7/L8/L18a

**Files:** new `M/GraphicsModel.swift` (the one `graphics` sub-model on AppModel; no graphics methods on AppModel
itself), `M/GraphicsOperatePane.swift`; `M/Overlays.swift`, `M/AppModel.swift` (snapshot hook, brand kit per
generation, timing).

**Behaviour.** When graphics is available the tab shows **Operate | Design**; Design holds the "Existing overlay
controls" drawer (today's pane, unchanged) and a "Designer is a later release" note; when unavailable the tab is
today's pane plus "Graphics need a newer media core". Strip (GraphicsPlayoutService.cs:261-295): Program chips
with Hide, cued chips with Clear cue, Clear all (Hide every on-Program or show-pending instance, `:317-324`), Clear
cued (`:325-333`), labels per `:276-277`. Status lines: busy, refusal reason verbatim, "Media core restarted:
graphics cleared" on epoch change. Legacy: D9.

**Tests.** Strip vectors (state → chips); Hide / Clear all / Clear cued transcripts (exact command lists);
timing presets and live re-send; brand kit sent on each new core generation (two `ProfileChanged`-equivalent
generations → two `set-brand-kit`); position list; old-preset migration.
**Mutations:** strip from accepted results instead of instances (red), Clear all skipping show-pending (red),
brand kit only on edit (red), timing not re-sent while on air (red).

## Task 6: graphics document store (+ document vectors)

**Files:** new `M/GraphicsDocument.swift`; `contracts/graphics-shell/document.vectors.json` + fixture docs.

**Behaviour.** Port of `GraphicsDocumentStore` (MC/Services/GraphicsDocumentStore.cs:78-160) within D3: missing
file = new document; primary invalid → `.bak`; schema newer than 3 = read-only, original untouched; v1/v2 load as 3
and the first save keeps a `.v2.bak`; duplicate keys refused; 2 MiB cap; atomic replace with `.bak`; refuse a save
when the file changed on disk since load; unknown properties and unported sections preserved; row validity =
`GraphicsPlaylistRow.IsValid` (MC/Models/GraphicsPlaylist.cs:62-74).

**Tests.** Document vectors (accept/reject per row and document); a Windows-written fixture loads and re-saves to
equal JSON; newer schema read-only; corrupt primary recovers backup; external change refuses save; unknown keys
preserved.
**Mutations:** drop unknown keys (red), allow save when read-only (red), skip the fingerprint check (red).

## Task 7: playout service (+ playout vectors and transcripts)

**Files:** new `M/GraphicsPlayout.swift`; `contracts/graphics-shell/playout.vectors.json`.

**Behaviour.** Port of `GraphicsPlayoutService` (MC/Services/GraphicsPlayoutService.cs) for G2/G3 scope:
`instanceIdFor` (SHA-256 of the UTF-8 source id, first 12 hex, lower case, `:93-94`); entries (`EntryFor`,
`GuestEntries` per roster slot de-duplicated, `:100-111`); `Ensure` (create or cue-copy update, template pin
reinstall through install-then-list, `:335-393`); `Cue` (rideTake true, wait until the cue revision lands, 5 s
budget polled at 50 ms on the injected sleep, `:419-446`); Show = Cue then trigger show from cue with the cue's
revision (`:300-313`); Hide (`:448-455`), Clear cue (`:457-463`), Clear all / Clear cued; auto-out after 8 s / 15 s
(`:466-495`); epoch change cancels auto-outs and every in-flight sequence (`Current(epoch)`, `:511-512`); one
operation at a time (`Run`, `:498-509`). Client id `graphics-playout` (`:41`).

**Tests.** Playout vectors (instance ids incl. non-ASCII source ids, guest entry expansion); scripted-core
transcripts for Show (create → cue → trigger), Show of an existing instance (update cue copy only when fields
changed), Hide, Clear cue, Clear all, auto-out firing on the manual timer and cancelled by Hide or an epoch change,
an epoch change mid-Show refusing without replay.
**Mutations:** hash without lowercasing (red), trigger without waiting for the cue revision (red), auto-out
surviving an epoch change (red), update sent when nothing changed (red).

## Task 8: Operate playlist, row inspector, Guest rows, default rows

**Files:** `M/GraphicsOperatePane.swift`, `M/GraphicsModel.swift`.

**Behaviour.** Row states and buttons (spec 5.4): idle (Cue, Show), cued (Show, Clear cue), on Program (Hide),
preparing (disabled + the core's preparation text), failed (the core's warning); a Show never without its Hide in
the same row. Inspector: name, title (typed fields from the template's `fields`), From Sources (Guest),
Hides after Off / 8 s / 15 s, Hotkey F1-F12 (one row per key), Open in Design (disabled, "Designer arrives in a
later release"). Text typed is committed before Cue/Show; a failed save does not block Show. Guest rows expand to one
line per roster source with the roster or device name and an empty title (owner decision). D6 default rows.

**Tests.** Row-state projection table; commit-before-Show (typed name reaches the create body even though the save
failed); Guest expansion from a roster; hotkey uniqueness on edit; default rows equal the exported interview rows.
**Mutations:** Show before commit (red), Guest title filled from anything but empty (red).

## Task 9: F-keys

**Files:** new `M/GraphicsHotkeys.swift`; `M/TransportKeys.swift` (F1-F12 kinds), the monitor hookup.

**Behaviour.** Port of `GraphicsHotkeyRouter.TryRoute` (W/Services/GraphicsHotkeyRouter.cs:17-34): text focus →
nothing; F1-F12 unmodified → play in the row with that hotkey (a Guest row plays the Program subject); Shift+F12 →
play out all; Shift+F1-F11 → layout cue (no layouts until G7: routes to nothing); any other modifier → nothing.

**Tests.** Port of `GraphicsHotkeyRouterTests` as a table (every key x modifier x focus x rows), plus a model
test that F2 reaches a Show transcript and Shift+F12 a Clear-all transcript.
**Mutations:** route while text focused (red), Shift+F12 cueing a layout (red).

## Task 10: cue rides Take

**Files:** tests; `M/GraphicsPlayout.swift` / `M/GraphicsOperatePane.swift` only if needed.

**Behaviour.** A Cue is `rideTake: true` (spec 5.4); the Mac Take batch already leads with
`begin-take-transition`, where the core promotes ride-Take cues (`GraphicsInstances::promoteCuedForTake`). Proven,
not assumed: a scripted-core test that the Take batch carries no `graphics-*` command and that the cue body was
`rideTake: true`; Task 12 shows the cued graphic on Program after a real Take.

## Task 11: lower-third authority routing

**Files:** new `M/LowerThirdAuthority.swift` (pure router + the one entry `applyLowerThirdIntent(visible:)`);
`M/ControlSurface+Production.swift`, `M/Overlays.swift`, `M/StudioFooter.swift` call it.

**Behaviour.** D8. With a Guest row and (Show, or authority already instances): playlist (`ApplyLowerThirdIntentAsync`,
GraphicsPlayoutService.cs:146-167; adopt `graphics-authority-set {family:"lower-third", authority:"instances"}` only
after an accepted Show); otherwise the legacy key. Every accepted Hide sets the suppression flag; an accepted Show
clears it.

**Tests.** Table over (authority legacy/instances) x (Guest row present/absent) x (show/hide) x (accepted/refused)
→ target, authority-set sent or not, suppression; the footer and both control actions reach the same entry.
**Mutations:** adopt authority before the Show is accepted (red), Hide to the playlist while authority is legacy
(red: the #1166 defect).

## Task 12: live check and outcomes

Opt-in live test (D11) against `native/build-metal/corevideo-native` (built from this branch): available,
Show a Title row, wait for `firstDrawSubmittedFrame`, Program preview pixels change; Hide returns Program;
Cue a row then Take (scenes loaded) and the graphic is on Program after the Take. Full shell suite, metal ctest
unaffected (no native change), MediaCore exporter test green. Outcomes doc.
