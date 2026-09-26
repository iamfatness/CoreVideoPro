# Agent brief — Multiview input inspector (#674)

Read first: `AGENTS.md`, `CLAUDE.md`, `docs/multiview-input-inspector-spec.md`,
`docs/multiview-input-inspector-plan.md`, `docs/control-state-events-spec.md`,
`docs/BACKLOG.md`.

Work this only if the owner has promoted #674 or explicitly dispatched this
brief. Otherwise finish #659 / #661 / #663. This inspector consumes the
versioned roster/source projection; do not start Slice A against a one-off
snapshot.

## Dispatch prompt

Copy everything below the line.

---

You are implementing CoreVideo Pro issue #674 using
`docs/multiview-input-inspector-spec.md` and
`docs/multiview-input-inspector-plan.md`.

Read AGENTS.md and CLAUDE.md first. Shell owns no real-time media. Do not grow
`StudioViewModel.cs`. Do not add a thumbnail encoder or extra Zoom subscribe
for row previews. Use the existing Multiview GPU texture. Do not invent a
fourth snapshot (#610). No foundation PR without a shipping consumer. One
slice per PR (`#674 Slice A: ...`).

The bug is stale projections: Format / Status / Rec only move after Preview,
Take, or a row click. That is the #608 class. #657/#659 is the bus.

Implement slices in order:

1. Slice A — `MultiviewInputRow` projection, 10 fixed slots, cell patch on
   fact. Fake-bridge video-off updates Status/Format without SetScene/Take.
2. Slice B — negotiated WxH@fps + frameAgeMs. Show actual, dim configured cap
   when different. Epoch/Engine off wipes last-meeting numbers. Stale if age
   > 1500 ms.
3. Slice C — Rec chip from ISO `OutputLifecycleChanged` (off/armed/recording/
   error). framesWritten stays #551.
4. Slice D — ZoomISO-style chrome only after A–C flow. One row height,
   monospace format, chips, dimmed empty slots, texture crop preview + tally.
   No DeckLink column, no output-count stepper.

Invariants:

- Ten slots, never a variable output list.
- Do not rebuild the collection on roster churn.
- Tests fail if the row mapper only runs inside scene-sync / Take.
- Zoom mute and mixer mute stay separate.
- Pixels stay in the core.

Done for a slice: tests for the invariant, gates green, PR links #674.

---
