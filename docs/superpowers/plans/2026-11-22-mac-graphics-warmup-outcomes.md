# Mac graphics: first Show after a core start — outcomes (2026-11-22)

Plan: `2026-11-22-mac-graphics-warmup.md`. Branch `mac-graphics-warmup` off `mac-parity-phase2`
(PR #1223). Shell only: no native code changed.

## The finding

**There was no slow first preparation.** The "~8 s first preparation" in the G1-G3 outcomes was the
live check's own fixture. Its cue wait (`playout.sleep`) called the synchronous `poll()`, which is
`awaitMain(8) { bridge.request(...) }` (a run-loop spin waiting for a nested `@MainActor` task). The
playout's wait already runs inside a main-actor job, so the nested task could not start; `poll()`
waited out its 8 s limit and returned nothing, the 5 s cue budget had passed, and the cue refused.

| Measurement (this Mac, load 2-3 unless noted) | Before | After |
|---|---|---|
| Cold `centered-title` cue -> ready, in-process Metal probe | 39 ms, 2 frames | (unchanged, nothing to fix) |
| First font resolution (bundled Selawik registration) | 15 ms | — |
| First Metal render (pipeline creation, once per core) | 266 ms | — |
| Live check: polls during the first cue | t=0.008 s, t=8.065 s | t=0.007, 0.065 s |
| Live check: first Cue / first Show after a core start | refused at 8.06-8.07 s (load 6-7) | **accepted, 0.06-0.08 s** (5 runs) |

Production never had the fixture's shape: `playout.sleep` is `Task.sleep` and the app's 2 Hz sync
refreshes graphics state between polls, so the shipped first Show was never refused for this reason.
Windows uses the same 5 s budget; no Mac-only widening and no core warm-up were added (plan W3), since
a warm-up would add start-up cost for a 40-70 ms cold path.

## Commits

| Commit | What |
|---|---|
| `7361c8c9` | Live check: the cue wait awaits the bridge request directly; the diagnostic first-cue leg becomes an assertion: **the first Show after a core start is accepted, no retry, inside 5 s**. Plan doc. |
| `8e9eb138` | Spec 5.3, Windows parity: on every core generation, reinstall the DESIGN template versions the show references (rows, row images, saved cues, studio lower-third cue) from the pin, else the `designs` entry; once per epoch, after graphics is available; failures on the status line. |
| (this) | Reinstall also waits for the show document (launch race); outcomes; G1-G3 open items 1-2 corrected. |

## Verification

- Shell suite with the live Metal core (`COREVIDEO_SHELL_LIVE_CORE`): **7837 checks, 0 failed**, 3 runs
  (first Show 0.08 / 0.07 / 0.07 s); without it **7827, 0 failed**. Final tree (after the document-load
  fix): **7839, 0 failed** with the live core (first Show 0.08 s), **7829, 0 failed** without it.
- Mutations, each red against the full suite:
  - restore the nested `poll()` in the live check's sleep -> refused after 8.07 s, both assertions fail;
  - no design-id filter (a non-design pin installed eagerly);
  - fire on every snapshot (a refused design retried in a runaway: 4 -> 32 -> 80 installs) — first
    survived; the once-per-epoch test now makes one design refuse so a rerun is observable;
  - no `designs` fallback; swallowed failure; no availability guard; no capability re-check;
  - no `documentLoaded` guard; no re-check in `adopt()`.
- Native: built `build-metal` (SDKROOT = Xcode MacOSX26.5 SDK) only to run the live core; no native change.

## Decisions taken in implementation

- **Wait for availability and for the show document**, not just "graphics state present" (Windows
  `GraphicsDesignResync.Observe`): a snapshot can carry the graphics node before the capability is
  confirmed (a reinstall then is refused by the gateway and the epoch's one attempt is spent), and at
  launch the show loads off the main thread. Both re-check when they become true (capability flip in
  `observe`, `adopt()`), because `observe` skips an unchanged graphics node.
- **The reinstall takes the playout gate one design at a time** (plan W5, corrected in review round 1
  below). The first version held it across every design, which was wrong; see the review round.

## Open items

1. ~~Independent review not run by this lane~~ — done by the coordinator; findings and fixes below.
2. **Worst case on a stuck core** (corrected in review round 1): an operator action waits for at most ONE
   install, i.e. up to the 5 s install budget, or the 6 s bridge timeout (`MediaCoreBridge.swift:311`) on
   a dead transport. The reinstall as a whole stops at the first transport failure or core change; a core
   that answers but never installs costs up to 5 s per missing design in the background, never in front
   of the operator.
3. **No library-full eviction** on the Mac reinstall (Windows evicts unused designs once): the Mac has no
   design lifecycle until G8, so a full library reports "The graphics library is full ..." instead.
4. The in-process timing probe was temporary and is not committed; the numbers above are its output.
5. Cold preparation was measured for the curated `centered-title` only; image-heavy or crawl templates
   were not timed (crawls refuse on the Mac until G5/G6).

## Review round 1 (independent review, 2026-11-22)

Verified sound by the reviewer: once per generation, stale-epoch handling, the launch races, and that
the cue budget starts after `acquire()`.

| # | Finding | Verdict | Fix |
|---|---|---|---|
| I1 | The reinstall held the playout gate across ALL designs; `install()` waits up to 5 s each and the loop kept going after failures (6 s bridge timeout) -> on a stuck core about 5-6 s x N designs (16-slot library: ~80 s) of blocked Show/Hide/Clear all/F-keys, right when the operator re-Shows after a restart. The earlier outcomes text ("up to 5 s per missing design") understated it as harmless. | **Real** | The gate is held per design (`installDesign`): an operator action waits for at most the one install in flight. The loop stops at a transport failure (`graphics-transport-failed`, now `GraphicsGateway.transportFailedPrefix`) or a core change. A separate lock, as on Windows, was rejected: both platforms share one single-slot gateway, so it would only turn concurrent Shows into "Graphics busy". |
| I2 | `adoptInstancesAuthority()` called the gateway OUTSIDE the playout gate. After the first lower-third Show following a restart, its adopt could take the single-slot gateway while the reinstall's next install was sent; that install refused `graphics-command-overloaded`, was reported failed and was never retried this generation (a design only in `designs` then fails its Show). | **Real, reproduced** (mutation M-b below: 1 of 4 designs installed, status "Graphics designs: graphics-command-overloaded") | The adopt runs through the gate (`GraphicsPlayout.gated`), no busy flag or status of its own. It was the only gateway caller outside the gate. |
| T1 | No test ran an operator Show DURING a reinstall; deleting the reinstall's `acquire()` passed everything. | Real | `operator-show-waits-at-most-one-install`: the reinstall parks mid-install (between an install and its list), a Show is started, and the transcript must show exactly one install before the Show and the Show never inside an install. |
| T2 | `!model.busy` ran after awaiting the reinstall, so it could not fail. | Real | Removed there; the same check now runs WHILE the reinstall is parked mid-install, and busy is asserted true once the operator's Show is queued. |
| T3 | No test for I2's sequence. | Real | `lower-third-adopt-during-reinstall`: from the Show's on-air trigger on, every send parks, so whichever command reaches the gateway next holds its slot; the lower third must be shown with authority adopted AND all 4 designs installed. |
| M | Reinstall failures only reached the status line, which the next report() overwrites. | Real | Also written to ShellLog (`graphics: design reinstall failed epoch=...`). Not covered by a test. |

Also added: `dead-bridge-stops-the-reinstall` (a transport failure on the first install -> one attempt,
named on the status line). The scripted core gained `failTransport` and `heldCount` for these.

**Evidence.** The old implementation (`63d7ee2d`) against the new tests: Show-during-reinstall red (all 4
installs before the Show) and dead-bridge red (4 attempts). The I2 test passes on the old code: there
the whole-loop gate made a Show wait for the entire reinstall, so the interleaving it pins could not
happen in that order (it could with the Show first, then the reinstall; mutation M-b covers the ungated
adopt under the new code). Mutations, each red against the full suite:
- M-a, delete `acquire()` in `installDesign`: 4 failures (Show not waiting, Show lands inside an install,
  only 2 designs installed, "graphics-command-overloaded");
- M-b, adopt not gated: 2 failures (1 of 4 designs, overloaded on the status line);
- M-c, no abort; M-d, transport failure not treated as abort: dead-bridge test red (4 attempts).
A first run of the I2 test failed on the fixed code because of the test itself: the guest row's
auto-out Hide re-armed the test's hold and parked the next install. The hook now arms only on the Show
trigger and that row's auto-out is off.

