# macOS control and audio parity, Phase 2 (RC5, RC6, A3, A4, A6) Implementation Plan

Status: executing, 2026-11-21. Lane: Shell B. Branch `mac-control-audio-phase2` from `fc675541` (Mac Phase 1, PR #1220).

> `docs/superpowers/plan-authoring-rules.md` is binding. Every task lists the mutations it must turn red, and its
> report records whether each one did. When a supplied test contradicts a reasonable implementation, suspect the
> test first.

**Goal.** A Mac operator gets mastering presets with A/B compare (and a rack that survives a relaunch), loudness
guide meters, a stereo master rail, gain-reduction and ZOOM MUTED readouts, all 15 buses plus custom buses with bus
sends and LISTEN. Volt can drive a staged clip through `media.rundown.*`, and the Companion module's own request
shapes are proven against the Mac control server.

**Specs.** `docs/superpowers/specs/2026-11-15-mac-audio-parity-design.md` (sections 4.4-4.6, slices A3, A4, A6) and
`docs/superpowers/specs/2026-11-15-mac-remote-control-parity-design.md` (slices RC5, RC6). Master plan Phase 2 rows.
The shared-manifest machinery is Phase 1's (`docs/superpowers/plans/2026-11-16-mac-remote-control-phase1.md`):
`contracts/control.platforms.json` rows are flipped, `npm run contract:generate` regenerates
`ControlManifest.generated.swift`, and the closure/fixture tests police it. Nothing is forked.

Path shorthand: `W/` = `native-shell/CoreVideoPro.WinUI/`, `WT/` = `native-shell/CoreVideoPro.WinUI.Tests/`,
`M/` = `mac-shell/Sources/CoreVideoProShell/`, `SVM` = `W/ViewModels/StudioViewModel.cs`.

---

## Defaults taken (owner may override)

| # | Default | Why | Tasks |
|---|---|---|---|
| D1 | **A2's mastering model is pulled forward.** The 10 missing fields (low-pass, limiter, glue ratio/attack/release/makeup/multiband, three band trims) are added with the Windows defaults, and the rack persists (current, compare A, compare B, active slot, user presets). The continuous Mac target slider and its RC3 `masteringTarget = -1` rule (Phase 1 D10) are unchanged; presets set exactly -14/-16/-23. The 11 existing Mac field defaults are unchanged. A1 and A5 stay open. | A3 cannot exist without it: every Windows preset, reset and A/B recall writes all 21 fields (`SVM:3318-3364`), and the Windows rack row says "A/B slots and the whole rack persist across restarts" (`W/Views/AudioPage.xaml:283`). A2 was never landed on the Mac. | T2, T3, T4 |
| D2 | `audio.masterLimiter.set` flips to `supported`, and `masterLimiterOn` joins the Mac feedback fields. | The limiter switch exists once D1 lands. Windows sets `MasterLimiterEnabled` and succeeds (`W/Services/StudioControlSurface.cs:659-661`), and the Companion module reads `masterLimiterOn` (`companion-module-corevideopro/src/feedbacks.ts`). Spec RC7 rule: a feature flips its own rows. | T3 |
| D3 | Custom buses, bus sends and LISTEN are **not persisted**. They live for the session and are re-sent with every base matrix (so a respawned core gets them back). LISTEN is **not** cleared on a core respawn. | Windows persists none of it (no member in `ProductionOutputPreferencesStore.cs`), keeps LISTEN lit, and re-sends it on its next production sync after a core restart (`SVM:1674`, `SVM:9416-9419`). Spec 4.6 asked for custom-bus persistence and a LISTEN clear on respawn; both contradict the Windows reference, which is what parity matches. | T7 |
| D4 | Custom bus ids are `bus-01`, `bus-02`, ... with labels `BUS 1`, ...; a session counter that never reuses a number. Bus sends are three toggles per aux/custom bus (MASTER, MON, STREAM) sharing one output gain in [-60, +10] dB. | `W/ViewModels/AudioRoutingMatrixViewModel.cs:121,287-300` and `:444-526`. The spec's `{from, to, gainDb}` is the wire shape, not the UI model. | T7, T8 |
| D5 | The gain-reduction meter sits in the COMP cell of the selected channel, 0-24 dB, "GR —" at or below 0.05 dB. | Windows shows one GR meter in the workspace COMP row (`W/Views/AudioPage.xaml:1160-1169`, `SVM:2401-2407`), not one per strip. | T5, T6 |
| D6 | A Zoom-muted guest's strip shows a **ZOOM MUTED** badge and its meter shows the pre-mute input, dimmed. The badge never writes the strip's mute or the wire mute. | Windows `MeterLevel = ResolveChannelMeterLevel(TruePeakDb, InputTruePeakDb, Muted || SourceMuted)` and `SourceMuted` comes only from the roster (`SVM:8818-8825`, `:11355-11381`); #481 / #608. | T5, T6 |
| D7 | The console rail's number is the core's `loudnessLufs` (short-term, else momentary) labelled LUFS, "LUFS —" when the mix has mixed no frames or reads -59 or below. The L and R meters are the existing `masterLevelLeft`/`masterLevelRight`. | Windows `TransportViewModel.cs:255-291` (`MasterLufsLabel`, `ResolveMasterMeterReadout`). The Mac already computes L/R the footer's way. | T5, T6 |
| D8 | Integrated LUFS has no reset button. | The core has no reset command; the meter accumulates for the life of the core process (`native/src/core/MediaCore.cpp:4292-4293` is a sample-rate reset only). The Mac shows the value as published. | T5 |
| D9 | `media.rundown.*`'s `route` argument names a Mac `SceneLayer.id`. An eligible layer has opacity above 0 (Mac layers have no visible, locked or graphic flag). Windows' scene-draft refusal does not apply (the Mac has no scene draft). When clip playback is unavailable in the core, preview refuses with the Mac text the Media tab already uses. | Mac wire route ids are positional (`M/AppModel.swift:2961-2974`); the layer id is the stable identity. Windows rules: `W/ViewModels/StudioViewModel.VoltRundown.cs`. | T9 |
| D10 | Volt's staged clips live in `<media root>/Volt Rundown/<key>.mp4`; the bin gains the `volt-rundown` kind (label "Volt Rundown"), `.mp4` only, dot-prefixed working folders ignored. | `native-shell/CoreVideoPro.MediaCore/Services/MediaBinClassifier.cs:23-24,77-83,167`. | T9 |
| D11 | RC5's proof is a recorded fixture of what the Mac serves (`companion-module-corevideopro/src/fixtures/mac-control.json`, written by a Mac harness case and drift-checked by it) plus a Mac harness case that replays the module's exact request shapes over a real socket. A vitest case in the module parses the fixture with the module's own `normalizeManifest`. The live Companion and Stream Deck check is an owner step. | The module has no test of its requests (`companion-module-corevideopro/src/manifest.test.ts` only parses inline objects). | T10 |

## Spec corrections (verified in code, 2026-11-21)

1. **A2 never landed on the Mac.** `MasteringParams` has 11 fields, `limiterEnabled` is hard-coded `true`, and nothing persists (`M/AppModel.swift:252-279`, `:4950`). The audio spec's Phase 1 rows A1, A2 and A5 are all still open. See D1.
2. **Windows custom bus ids are `bus-01`** (two digits, `AudioRoutingMatrixViewModel.cs:121,293`), not `bus-<n>`; nothing about buses is persisted (spec 4.6 says custom buses are).
3. **Windows LISTEN accepts any bus**, fixed buses included (`AudioRoutingMatrixViewModel.cs:480-499`), and the core only uses it while the monitor is on (`MediaCore.cpp:7285-7299`).
4. **The GR meter is not a strip meter** on Windows (D5).
5. **The Mac footer already has stereo L/R**; only the console rail draws mono twice (`M/AudioConsole.swift:388-389`). Confirmed.
6. **`media.rundown.*` refusal texts are not in the social-comms audit**; they are in `W/ViewModels/StudioViewModel.VoltRundown.cs` and are quoted from there. Windows has no test for the three actions; the Mac tests are the first.
7. **The Companion module never calls `/state` or `/snapshot`**; it reads state only from `/ws`. It sends `Content-Type: application/json` on every request, including GETs, and the `ws` package sends no `Origin` (both pass the Mac hardening, `M/ControlHttpServer.swift`).
8. **Spec section 2.5's `CoreStateCapture.cpp:2633-2641`** is now `:2685-2693`; `loudnessLufs` is `:1582`. Contents hold.

## Before you start

Baseline at `fc675541`, measured 2026-11-21 in this worktree:

| Command | Result |
|---|---|
| `cd mac-shell && swift build -c release && COREVIDEO_SHELL_TESTS=1 ./.build/release/CoreVideoProShell` | `5676 shell checks passed, 0 failed.` |
| `COREVIDEO_SHELL_SELFCHECK=1 mac-shell/.build/release/CoreVideoProShell` | `self-check: PASS` |
| `bash scripts/lint-mac-design.sh` | `Design lint passed.` |

- **No native change.** Every field the meters need is already published (`masterMeter.integratedLufs`,
  `momentaryLufs`, `truePeakDbfs`, `audioMixSession.loudnessLufs`, per-channel `gainReductionDb`,
  `audioRoutingMatrix.busTaps`); every command the buses need is already parsed (`busSends`, `monitorBusId`,
  `MediaCore.cpp:3366-3393`). Nothing touches the render or audio worker, so `mac-show-drill.py --load 8` is not a gate.
- New shell tests go in `M/ShellTestsAudio.swift` and `M/ControlApiTestsPhase2.swift` (ContractParity skips
  `ShellTests*` and `*Tests.swift`; no `"type":` request literal outside those files may name a command the core
  does not handle).
- One commit per task; message ends `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

---

## A3 (with D1): mastering presets, A/B compare, a rack that survives a relaunch

### Task 2: the 21-field rack, the preset catalog and the user-preset library (pure)

**Files.** NEW `M/MasteringPresets.swift`; modify `MasteringParams` in `M/AppModel.swift`; NEW `M/ShellTestsAudio.swift`.

**Interfaces.**
- `MasteringParams` gains `lowPassHz = 0`, `limiterEnabled = true`, `glueRatio = 2`, `glueAttackMs = 30`,
  `glueReleaseMs = 250`, `glueMakeupDb = 0`, `glueMultiband = false`, `glueBandLowDb/MidDb/HighDb = 0`, and becomes
  `Codable`. `json` emits the 20 `mastering` keys Windows sends (`MediaCoreCommandBuilder.cs:519-545`;
  `limiterEnabled` stays top-level, never inside `mastering`).
- `enum MasteringPresetCatalog { static func create(_ id: String) -> MasteringParams?; static var neutral; static func resetStage(_ params:, stage:) -> MasteringParams? }`
  — a port of `W/Models/MasteringPresetCatalog.cs:35-74` (ids trimmed and lower-cased; unknown id or stage is nil,
  where Windows throws).
- `struct MasteringUserPreset: Codable, Equatable { id, name, settings }` and
  `enum MasteringPresetLibrary { maxPresets = 32, maxNameLength = 48, builtInNames, normalizeName, save, rename, delete, clamped(_:), loaded(_:) }`
  — a port of `W/Models/MasteringPresetLibrary.cs` (`clamped` = `FromPersisted`, `loaded` = `FromPersistedPresets`).

**Tests** (ports of `WT/MasteringPresetCatalogTests.cs` and `WT/MasteringPresetLibraryTests.cs`, every vector a
fixture constant from the Windows file):
- table test pinning every built-in value (streaming, podcast, broadcast, neutral: all 13 positional fields plus
  the glue defaults);
- reset stage per id (input, filter, tone, ride, glue/dynamics, width, loudness/image, ceiling/limiter), starting
  from podcast with input 2, ride 8, width 1.4, multiband on, ceiling -0.6;
- library: trim on save, `user-` id, overwrite by case-insensitive name keeping id and position, rejects
  nil/""/"   "/"Streaming"/"neutral"/"  Podcast  ", 32-preset cap with overwrite still allowed, rename rules incl.
  own-name recase, delete by id, load drops blank id / blank name / built-in name / duplicate name (first wins);
- clamp: target 9→2 is the Windows index; on the Mac the clamp table applies to the Windows ranges
  (ceiling 3.0→-0.5, ratio 100→10, width 5→2, attack 0→1, maxRide 99→12);
- the mix command's `mastering` object carries all 20 keys with the values set, and the top-level `limiterEnabled`
  follows the params (a fixture rack with `limiterEnabled = false`).

**Mutations.** Swap two preset values (table red). Drop `glueMultiband` from `json` (key test red). Make `save`
case-sensitive (overwrite test red). Remove the cap (full-library test red). `audioMixCommand` back to
`"limiterEnabled": true` (limiter test red).

### Task 3: A/B compare, presets on the model, persistence before the first mix, the limiter action

**Files.** `M/AppModel.swift`, `M/ShellPrefs.swift`, `M/ControlSurface+Production.swift`, `M/ControlSurface.swift`,
`contracts/control.platforms.json` (+ regenerate), `M/ShellTestsAudio.swift`, `M/ControlApiTestsPhase2.swift`.

**Behaviour** (Windows `SVM:3241-3443`, `:12155-12163`, `:12513-12578`):
- Model state `masteringCompareSlot` ("A"/"B"), `masteringCompareA/B` (start `neutral`), `masteringUserPresets`,
  `masteringStatus` (the Windows status sentences verbatim).
- `selectMasteringCompareSlot(_:)`: anything but "B" (case-insensitive) is "A"; same slot = no change; otherwise the
  live rack is stored into the slot being left and the other slot's rack is loaded (one mix sync).
- `applyMasteringPreset(_:)`, `applyMasteringUserPreset(id:)`, `resetMasteringStage(_:)`: write all fields and
  store the result into the **active** slot. Slider edits (`editMastering`) do not touch the slot; the slot is
  refreshed from the live rack when prefs are written (Windows serialises the active slot from the live controls).
- `saveMasteringUserPreset(name:)`, `renameMasteringUserPreset(id:name:)`, `deleteMasteringUserPreset(id:)`.
- `ShellPrefs` gains optional `masteringCurrent`, `masteringCompareA`, `masteringCompareB`, `masteringCompareSlot`,
  `masteringUserPresets`, decoded in `init(from:)` and covered by `roundTripSelfCheck`. Restore (in
  `applyRestoredPrefs`, before any core connects): presets always; when `masteringCurrent` is nil keep defaults;
  else clamp each, slot "B" only on "B", missing A/B fall back to current.
- `audio.masterLimiter.set(on)`: `editMastering { $0.limiterEnabled = on }`, success. `/state.masterLimiterOn`.

**Tests.**
- The first `audioMixCommand()` after `applyRestoredPrefs` of a prefs fixture (glue 0.62, low-pass 16000,
  `limiterEnabled = false`, slot "B") carries those values — the regression is "silently resets to bypass".
- A/B: edit live glue to 0.7 on A, switch to B (B = neutral → glue 0), switch back to A (glue 0.7); same-slot switch
  sends nothing; "b" means B, "x" means A.
- Preset into the active slot: on B, apply podcast; switch to A and back: B is podcast.
- Prefs round trip: `currentPrefs()` stores the live rack into the active slot.
- Control: `audio.masterLimiter.set false` → the next mix command has `limiterEnabled: false`; `/state`
  `masterLimiterOn` false; closure and manifest counts move by one (37 supported).

**Mutations.** Load the other slot without storing the leaving slot (switch-back red). Restore after the first mix
(first-command red). Drop `masteringCurrent` from `init(from:)` (self-check red). Flip the platform row without the
dispatch case (closure red).

### Task 4: the rack UI

`M/AudioConsole.swift` `MasteringRack`: COMPARE A/B toggles, "Starting point" menu (Streaming · -14 LUFS, Podcast ·
-16 LUFS, Broadcast · -23 LUFS, Neutral · protection only), "Reset stage" menu (01 · Input ... 07 · Ceiling),
"A/B slots and the whole rack persist across restarts.", MY PRESETS row (name field, Save, picker, Apply, Rename,
Delete; the last three disabled with no presets), new stage controls (FILTER low-pass, GLUE ratio/attack/release/
makeup/multiband + three band trims, CEILING "Master protection" In/Bypass). The FILTER light follows the value sent
(engaged when high-pass > 0 or low-pass > 0, the core's own condition, `AudioMastering.h:421-424`). Proof: build,
`COREVIDEO_SHELL_TAB_SNAPSHOT` capture of the Audio SETUP surface, design lint.

---

## A4: meters

### Task 5: meter scale, readouts and decode (pure + model)

**Files.** NEW `M/AudioMeters.swift`; `M/AppModel.swift`; `M/AudioConsolePolicy.swift`; tests.

**Interfaces.**
- `enum AudioMeterScale { minimumDbfs = -60; majorTicksDbfs = [0,-6,-12,-24,-36,-48,-60]; toLevel(_ dbfs:, muted:) -> Int }`
  — Windows `W/Models/AudioMeterScale.cs` (rounded, not truncated). `AudioConsolePolicy.meterLevel` delegates to it.
- `enum MasterMeterReadout`: `integratedLabel(_:)` "x.x LUFS" or "—" at or below -119; `truePeakLabel` "x.x dBTP" or
  "—"; `targetLabel` "target -14 LUFS"; `ceilingLabel` "ceiling -1.0 dBTP"; `guideFraction(value:min:max:)` (0 when
  value <= min); `railLufsLabel(loudnessLufs:mixedFrames:)` "x.x LUFS" or "LUFS —"; `gainReduction(_ db:) ->
  (level: Int, label: String)` (0-24 dB → 0-100, "GR −x.x dB" or "GR —").
- Model decodes `masterMeter.integratedLufs`, `momentaryLufs`, `audioMixSession.loudnessLufs`, `mixedFrameCount`,
  and per-channel `gainReductionDb` onto `AudioStrip.gainReductionDb`; `AudioStrip.sourceMuted` comes from the roster
  `muted` flag in `mergeStrips` (a new `zoomMuted: Set<String>` parameter), never from the echo and never into
  `muted`.
- `AudioConsolePolicy.meterReading`: `muted || sourceMuted` → pre-mute input, dimmed.

**Tests.** Windows vectors (-120→0, -60→0, -48→20, -36→40, -24→60, -12→80, -6→90, 0→100, muted -3 → 0, and -0.3 →
100 by rounding where truncation gives 99); label rules at -119.0 / -118.9; GR 0.05 → "GR —", 6 → 25 and
"GR −6.0 dB", 30 → 100; decode from a snapshot fixture with integrated -16.4, momentary -15.0, short-term -120,
loudnessLufs -15.0; a roster-muted guest's strip reads `sourceMuted`, keeps `muted == false`, meter shows input
dimmed, and the next `audioMixCommand` sends `muted: false` for it.

**Mutations.** Truncate instead of round (-0.3 row red). `> -119` → `>= -119` (boundary red). Write `sourceMuted`
into `muted` (wire red). Read `integratedLufs` into `shortTermLufs` (decode red).

### Task 6: meters UI

SETUP gets two guide meters (INTEGRATED · POST-MASTER, -36..0 with the target marker; TRUE PEAK · POST-MASTER,
-24..0 with the ceiling marker, warn colour above the ceiling); both dim when the rack is bypassed. The rail draws
L and R, its number is D7's label; strips show a calibrated tick scale, the ZOOM MUTED badge, and the COMP cell a GR
meter. Proof: tab snapshot capture, design lint.

---

## A6: buses

### Task 7: bus model, bus sends, LISTEN and the full-state wire (pure + model)

**Files.** NEW `M/AudioBuses.swift`; `M/AppModel.swift`; `M/ControlSurface+Production.swift`; tests.

**Interfaces.**
- `struct AudioBus: Equatable { id, label, sendsToMaster, sendsToMonitor, sendsToStream, outputGainDb, isRemovable }`.
- `enum AudioBusPolicy`: `fixedBuses` (15, Windows order and labels), `isRemovable(_:)` (`aux-*`, `bus-*`),
  `addCustom(_ buses:, counter:)`, `remove(_:from:)`, `rename`, `setSend(_:target:enabled:)` (master|mon|stream,
  removable only), `setOutputGain` (clamp -60..10), `busSends(_ buses:) -> [JSONObject]` (Windows
  `BuildBusSends` order), `listen(_ current:, bus:, on:) -> String?`.
- `AppModel.routingBuses` becomes an instance `[AudioBus]` (`busIds` for the grid); `listenBusId: String?`.
  `routingBaseCommand()` sends `busSends` and `monitorBusId` (`""` for none); the send memo compares the whole
  command, not only `sends`. Removing a bus disables its crosspoints through the existing operator-edit path. The
  legacy `pushAudioRouting` fallback sends the same `busSends`/`monitorBusId`.
- `audio.route.set` validates against the live bus ids.

**Tests** (ports of `WT/RoutingMatrixViewModelTests.cs` `AddBus*`, `BusOutputs_*`, `BusListen_*`, ISO rule):
fixed list order and labels; `bus-01`, `bus-02`, never reused after removing `bus-01`; fixed buses refuse send,
rename and remove; aux-1 → master+mon at -6 gives exactly `[{aux-1,master,-6},{aux-1,mon,-6}]`; a send to "iso-1"
is refused; LISTEN exclusive (aux-1, then aux-2 clears aux-1, off → nil, a fixed bus can be listened); the base
command carries the bus sends and `monitorBusId`; **a bus-send-only change produces a second push** (the memo
regression); removing a routed custom bus unroutes its cells and drops its sends and LISTEN from the next command;
`audio.route.set` to `bus-01` is accepted after `addCustomBus`, refused before.

**Mutations.** Memo on `sends` only (bus-send push red). Allow fixed-bus sends (refusal red). Reuse the counter
(never-reused red). Drop `monitorBusId` from the command (LISTEN wire red).

### Task 8: bus UI

ROUTING grid: all buses as columns with the source column pinned and horizontal scroll, routed count per bus,
clear-row; a bus strip with a chip per bus (label, editable for aux/custom, routed count, LISTEN, and for aux/custom
`→ MASTER MON STREAM` toggles, gain, remove), "+ Bus". Copy says ISO buses are routing taps (spec section 10: ISO
recording does not use them). Proof: tab snapshot capture at 1080 by 700, lint.

---

## RC6: Volt rundown

### Task 9: `media.rundown.load`, `.preview`, `.take`

**Files.** `M/MediaBin.swift`, NEW `M/VoltRundown.swift`, NEW `M/ControlSurface+Media.swift`, `M/ControlSurface.swift`,
platform file (+ regenerate), `M/ControlApiTestsPhase2.swift`.

**Behaviour** (Windows `StudioViewModel.VoltRundown.cs`, `StudioControlSurface.cs:361-369`; texts verbatim):
- key regex `^[a-z0-9-]{1,72}$`; the clip is `<root>/Volt Rundown/<key>.mp4`, asset id from the relative path.
- load: bad key → "Invalid staged clip key."; refresh the bin; missing → "The staged clip is missing from the local
  Volt Rundown bin."; else `data {"sourceId":"media:<id>"}`.
- preview: missing → "The staged clip is missing. Send it from Volt again."; scene on Program or Take in flight →
  "The clip scene must be off Program before cueing another clip."; unknown scene or not exactly one eligible layer
  → "The clip scene or its editable video route does not exist."; clip playback unavailable → "Clip playback is
  unavailable in this build" (Mac, D9); else select the scene to Preview, point the layer at the clip (slot nil),
  `syncScenes()`, answer `appliedPreview(scene)`.
- take: the readiness check ("The staged Volt clip must be cued on Preview before Take.", "The staged clip is already
  on Program. Take it off Program before replaying it.", "Wait for the staged clip to decode on Preview before
  Take.") then the existing `transport.take` wrapper.

**Tests.** Each refusal, each through the real router; load's `data`; preview rewrites exactly the named layer and
leaves others; empty route with two eligible layers refuses; take refuses until a `mediaSources` row says
`onPreview` with `decodedVideoFrames > 0`, then calls the Take path; classify: `Volt Rundown/a.mp4` is
`volt-rundown`, `Volt Rundown/a.mov` is not, `Volt Rundown/.volt-working/p.mp4` is never listed.

**Mutations.** Accept upper-case keys (regex red). Skip the on-Program check (refusal red). Pick the first eligible
layer when two exist (ambiguity red). Take without the decode check (decode red).

---

## RC5: Companion on the Mac

### Task 10: the module's requests against the Mac server

- Mac harness case `control/companion-request-shapes`: a real `ControlHttpServer` on a test port, a real model;
  requests built exactly as `companion-module-corevideopro/src/main.ts` and `manifest.ts` build them
  (`Content-Type: application/json` on GET and POST, `Host: 127.0.0.1:<port>`, positional args, `/ws` upgrade with no
  `Origin`, `?token=` on `/ws` and `Authorization: Bearer` on HTTP when a token is configured). Asserts: `/manifest`
  200 PascalCase; each preset action that the Mac supports invokes (or refuses with the model's own reason, not 400);
  `automation.autoAssignInputs.set` (the `autoassign` preset) answers 422 with the generated reason; the first `/ws`
  frame has every feedback field the module's presets read except `autoAssignInputs`.
- Recorded fixture `companion-module-corevideopro/src/fixtures/mac-control.json` (manifest + the first state frame
  keys), drift-checked by the same harness case (`COREVIDEO_WRITE_COMPANION_FIXTURE=1` rewrites it).
- Module vitest `src/mac.test.ts`: `normalizeManifest(fixture.manifest)` keeps every action; each preset action id is
  either in it or in the documented Mac-absent list.
- Module README gains a macOS column. Owner acceptance doc: `docs/owner-acceptance/mac-control-audio-phase2.md`.

**Mutations.** Mac POST without `Content-Type` accepted? (no: the case sends it; drop the header in the case and the
415 assertion must red). Mark `transport.engine.toggle` blocked (preset assertion red). Stale fixture (drift red).

---

## Task 11: outcomes

`docs/superpowers/plans/2026-11-21-mac-control-audio-phase2-outcomes.md`: commits, test counts with commands,
mutations, proven vs unverified, owner steps.

## Out of scope

A1 (Zoom audio mode), A2's target snapping, A5 (local source), A7 processing workspace, A9, the strip-level dBFS
tick labels beyond the scale itself, any native change, Companion live check (owner).
