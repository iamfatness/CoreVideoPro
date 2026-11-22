# Outcomes: macOS control and audio parity, Phase 2 (RC5, RC6, A3, A4, A6)

Plan: `docs/superpowers/plans/2026-11-21-mac-control-audio-phase2.md`. Branch `mac-control-audio-phase2`
from `fc675541`. Owner steps: `docs/owner-acceptance/mac-control-audio-phase2.md`. Not pushed, no PR.

## Commits

| Commit | Slice | What |
|---|---|---|
| `8e3d601a` | plan | The plan, Defaults taken D1-D11, spec corrections |
| `a1932369` | A3 1/3 | 21-field rack, preset catalog, user-preset library (pure); limiter from the rack |
| `1723e7d6` | A3 2/3 | A/B compare, presets on the model, rack persisted and restored before the first mix; `audio.masterLimiter.set` + `masterLimiterOn` |
| `f096adde` | A3 3/3 | Rack UI (COMPARE A/B, Starting point, Reset stage, MY PRESETS, glue character, limiter) |
| `1bb8e8b8` | A4 1/2 | Windows meter scale, master readouts, integrated/momentary/loudnessLufs decode, GR and Zoom-muted per strip |
| `d306435c` | A4 2/2 | Guide meters, stereo rail, dBFS ticks, GR meter, ZOOM MUTED badge |
| `bd33a3a9` | A6 1/2 | 15 fixed buses, custom buses, bus sends, LISTEN on the full-state matrix; memo fix |
| `c0f9c950` | A6 2/2 | Bus UI (scrolling columns, pinned sources, routed counts, clear row, chips) |
| `a2b46a93` | RC6 | `media.rundown.load/.preview/.take`, `volt-rundown` bin kind |
| `1eef951b` | RC5 | Companion request shapes vs the Mac server, recorded fixture, module vitest, live-check script, README |

## Review round (independent review, 2026-11-21)

Each finding was checked against the code first; all seven were real. Fixed test-first, one commit each:

| # | Finding | Verdict | Commit |
|---|---|---|---|
| 1 | `requestAudioRouteControl` wrote the grid, operator intent, ISO-exclusive edits on other sources and the unrevisioned base matrix before the core judged `expectedRevision` | Real (pre-existing from RC3, same feature). A stale request was refused and then applied ~100 ms later by the base matrix. Now conflict/rejected write nothing and drop the remote draft; applied or unconfirmed take the grid's path (unconfirmed may have applied, so leaving it out of the base would revert a late apply) | `2bbf9c82` |
| 2 | Master meters not reset on a new core; no staleness gate on the guide meters | Real. `onConnected` resets every master meter to no-evidence; the guide meters carry the #870 MeterWarning words like the rail | `9128639f` |
| 3 | busSends / LISTEN never reach the core when nothing is routed | Real. The core applies busSends/monitorBusId from a matrix with empty `sends` when it holds none; the Mac now sends such a matrix when it carries bus state, and once more after the state is cleared | `8b5b6a4c` |
| 4 | Hardening-timeout test raced a 0.4 s timer | Real (Phase 1 test, file touched here). Handler held by the test, own 1 s server | `d3091b3f` |
| 5 | Pipelined-request check could pass vacuously | Real (Phase 1 test). Handler held until the extra bytes are on the socket | `d3091b3f` |
| 6 | `core.bodies.isEmpty` vacuous in volt-take | Real. Now asserts no refusal produced a Take batch | `d3091b3f` |
| 7 | A rack edit in the last 5 s before quit was lost | Real (any pref). Quit writes the prefs | `f93292e9` |

The `timerSleepForTesting` / `ManualTimers` seam from `def55f05` was not needed (none of the fixes waits on a product timer) and was not cherry-picked.

## Test counts (final, 2026-11-21, this worktree)

| Command | Result |
|---|---|
| `cd mac-shell && swift build -c release && COREVIDEO_SHELL_TESTS=1 ./.build/release/CoreVideoProShell` | **5994 shell checks passed, 0 failed** after the review round (baseline 5676), run twice back to back with other agents building |
| `COREVIDEO_SHELL_SELFCHECK=1 mac-shell/.build/release/CoreVideoProShell` | `self-check: PASS` (prefs round trip covers the five new mastering keys) |
| `bash scripts/lint-mac-design.sh` | `Design lint passed.` |
| `npm run contract:check` | clean |
| `npm run test:scripts` | 225 tests, 221 pass, 0 fail, 4 skipped |
| `~/.dotnet/dotnet test native-shell/CoreVideoPro.Control.Tests/CoreVideoPro.Control.Tests.csproj` | 198 passed |
| Companion module, scratch copy: `npm install && npx vitest run && npm run lint` | 20 tests passed (17 existing + 3 new `src/mac.test.ts`), tsc clean |
| Live leg: `COREVIDEO_COMPANION_LIVE_SCRIPT=$PWD/scripts/companion-mac-live-check.mjs COREVIDEO_COMPANION_MODULE_DIR=<scratch module> COREVIDEO_SHELL_TESTS=1 mac-shell/.build/release/CoreVideoProShell` | `companion live check: PASS` (16 checks) |

## Mutations (each seen red, then restored)

- A3: a preset value, case-sensitive save, no 32 cap, dropped `glueMultiband` key, hard-coded limiter (red before the fix); load the other slot without storing the leaving one; restore after the first mix; `masteringCurrent` missing from `init(from:)` (self-check red); platform row flipped without the dispatch case (closure red).
- A4: truncation instead of rounding; `>= -119`; Zoom mute written into the A1's mute (wire red); integrated read from short-term.
- A6: memo on `sends` only (bus-send-only push red); fixed-bus sends allowed; counter reuse; `monitorBusId` dropped.
- RC6: upper-case keys accepted; first of two eligible layers picked; on-Program check skipped; Take without the decode check.
- RC5: `transport.engine.toggle` marked blocked (preset assertion and closure red); stale fixture (drift red); in vitest, an undocumented absent preset action (red).

One supplied assertion was wrong and was fixed rather than the code (rule 4): Task 3's "the other slot as stored" expected Neutral, but Windows stores the live rack into the slot being left, which was the default rack.

## Proven vs unverified

**Proven by tests:** every behaviour above against the model and, for RC5, over real sockets with the module's own request code. Pixels of the SETUP, SHOW and ROUTING surfaces reviewed from `COREVIDEO_SHELL_TAB_SNAPSHOT` captures (fixture strips, routes, a custom bus and LISTEN are now in the tab seed).

**Unverified (owner steps):** anything heard or seen on the installed app — presets audibly changing the master, the rack surviving a real relaunch, integrated LUFS and true peak tracking a real signal (the core's values, decoded but never observed live here), ZOOM MUTED in a real meeting, LISTEN on headphones, a Volt clip reaching Program with audio, and Companion on a physical Stream Deck. No test-launched app and no request to 127.0.0.1:8011 were made (the owner's app was live).

## Defaults taken

D1 A2's model pulled forward (10 missing fields, limiter, persistence); continuous target kept. D2 `audio.masterLimiter.set` supported. D3 buses, bus sends and LISTEN not persisted, re-sent on respawn, LISTEN not cleared (Windows). D4 `bus-01` ids, three send toggles sharing one gain. D5 GR meter in the COMP cell. D6 Zoom mute is display and meter only. D7 rail = core `loudnessLufs`. D8 no integrated reset. D9 `route` = Mac layer id, opacity > 0 eligible, Mac refusal when clip playback is unavailable. D10 `Volt Rundown/<key>.mp4`. D11 RC5 = recorded fixture + request-shape replay + optional live leg. Full text in the plan.

## Shared native changes

**None.** No file under `native/` changed. Contract file change: four rows in `contracts/control.platforms.json` flipped to `supported` (`audio.masterLimiter.set`, `media.rundown.load/.preview/.take`) and feedback `masterLimiterOn`; `ControlManifest.generated.swift` regenerated. The Mac now answers **40 of 110** actions.

## Installed

Nothing system-wide. `npm install` of the Companion module's dependencies into a scratch copy
(`<scratchpad>/companion`, not the repo) to run its vitest, `tsc` and the live leg.

## Carried forward

- A1 (Zoom audio mode), A5 (local audio source) and A2's target snapping are still open; with D1 the Mac target slider stays continuous.
- `automation.autoAssignInputs.set` (STU) is the one Companion preset that does nothing on a Mac; `autoLowerThirds` and `viewMode` are never published.
- The Companion module leaks a second `/ws` when an invoke fails with a network error while the socket is open (`main.ts` `scheduleReconnect`); module bug, not fixed here.
- Windows `RemoveBus` does not resync by itself; the Mac pushes on remove (a strict improvement, noted as a divergence).
