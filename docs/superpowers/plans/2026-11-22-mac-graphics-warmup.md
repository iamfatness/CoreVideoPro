# Mac graphics: first Show after a core start (warm-up) — plan (2026-11-22)

Owner approval (2026-11-22 09:15): fix "the first Show after a core start is refused" before
installer 12. Source: `2026-11-21-mac-graphics-shell-g1-g3-outcomes.md` open items 1-2.

## Measurement first (done before any fix)

| Measurement | Result |
|---|---|
| Cold `centered-title` cue -> ready, fresh Metal compositor, in-process probe | **39 ms over 2 frames** |
| First font resolution (registers bundled Selawik) | 15 ms |
| First Metal render (pipeline creation) | 266 ms, once per core, at start |
| Shell live check, first Cue after a core start, as shipped | **8.06 s, refused** (reproduced) |
| Same, with timing prints in `cueCore` | 2 polls: t=0.008 s, then **t=8.065 s** |

The cue loop slept once for 8 s. The live check replaced `playout.sleep` with
`Task.sleep` + `poll()`, and `poll()` is `awaitMain(8) { bridge.request(...) }`: a synchronous
run-loop spin waiting for a nested `@MainActor` task. The playout's wait already runs inside a
main-actor job, so the nested task cannot start until that job yields; `poll()` waits out its
whole 8 s limit and returns nothing, the 5 s cue budget has passed, the cue refuses. The core
had finished preparing ~60 ms after the cue. **The 8 s was the test fixture, not the core**
(plan-authoring rule 4: an async fixture that cannot drain steers the diagnosis).

Production does not have this shape: `playout.sleep` is `Task.sleep`, and the app's 2 Hz
media-core sync refreshes graphics state between polls.

## Decisions

| # | Decision | Why |
|---|---|---|
| W1 | Fix the live-check fixture: the cue wait awaits the bridge request directly. | Root cause of the 8 s. |
| W2 | The live check asserts **the first Show after a core start is accepted** within the shipped 5 s budget, and logs its time. | Directive: prove it on a real Metal core; the old leg-0 was diagnostic only. |
| W3 | No core-side warm-up and no wider budget. | Cold prep measured 39-70 ms; Windows uses the same 5 s budget (`GraphicsPlayoutService` cue wait). Warming would add a startup cost for no measured gain. |
| W4 | Spec 5.3 reinstall, matching Windows exactly: on each core generation, once the new core's graphics state is present, install the **design** template versions referenced by playlist rows, row images, saved cues and the studio lower-third cue that the core lacks, from the pin (else the show's `designs` entry); once per epoch; failures on the status line ("Graphics designs: ..."). | Windows `GraphicsDesignResync` + `GraphicsDesignService.EnsureInstalledForPlaylistAsync` (design ids only; `ReferencedTemplates`). Non-design pins stay lazy on Ensure, as on Windows. |
| W5 | The reinstall takes the playout gate (no busy/status change on success). | It must not race an operator's Show installing the same pin. |

## Tasks

1. Live-check fixture fix + first-Show assertion (W1, W2). Mutation: restore the nested `poll()` in
   `sleep` -> the first-Show assertion goes red against a real core.
2. Design reinstall (W4, W5), test-first on the scripted core:
   - after `coreGenerationChanged()` + the first snapshot of the new epoch: one install+list per
     missing referenced design version; installed ones and non-design pins untouched;
   - a second snapshot of the same epoch installs nothing; a new epoch arms again;
   - a design version with no pin installs from the `designs` section;
   - a refused install puts "Graphics designs: <reason>" on the status line.
   Mutations: drop the design-id filter; fire on every snapshot; skip the `designs` fallback;
   swallow the failure.
3. Outcomes doc; correct open items 1-2 of the G1-G3 outcomes.
