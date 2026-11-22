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
- **The reinstall takes the playout gate** (plan W5). Windows runs it under the design service's own
  gate, so a Windows Show is never blocked by it. The cost here: an operator action waits behind it, and
  an install that never lands holds the gate for up to 5 s per missing design (the install budget).
  Healthy installs land in milliseconds.

## Open items

1. **Independent review not run by this lane** (the fork cannot start subagents): the coordinator should
   have a reviewer read `7361c8c9..HEAD` cold.
2. **Gate hold on a stuck install** (above): bounded at 5 s per missing design; not seen in practice.
3. **No library-full eviction** on the Mac reinstall (Windows evicts unused designs once): the Mac has no
   design lifecycle until G8, so a full library reports "The graphics library is full ..." instead.
4. The in-process timing probe was temporary and is not committed; the numbers above are its output.
5. Cold preparation was measured for the curated `centered-title` only; image-heavy or crawl templates
   were not timed (crawls refuse on the Mac until G5/G6).
