# Agent brief — speaker-floor director (#668)

Read first: `AGENTS.md`, `CLAUDE.md`, `docs/speaker-floor-director-spec.md`,
`docs/speaker-floor-director-plan.md`, `docs/BACKLOG.md`.

Work this only if the owner has promoted #668 or explicitly dispatched this
brief. Otherwise finish the current Now rows (#659 / #661 / #663).

## Dispatch prompt

Copy everything below the line.

---

You are implementing CoreVideo Pro issue #668 using
`docs/speaker-floor-director-spec.md` and `docs/speaker-floor-director-plan.md`.

North star from `AGENTS.md`: shell owns no real-time media. Do not grow
`StudioViewModel.cs`. Do not put pixels/PCM in the shell. No ML. No foundation
PR without a shipping consumer. Stub core stays green. Name the issue in the
PR title (`#668 Slice A: ...`). One slice per PR.

Current director only picks a template from `liveCount`
(`native/src/core/Director.h`, `src/engine/localDirectorProvider.ts`). That is
the bug. Guests must earn slots from a talk ledger. Host is not an active
speaker. The same person must never occupy two slots.

Implement slices in order:

1. Slice A — `SpeakerFloor` person-id collapse + unique-bind refusal on the
   render plan (`RouteSourcePolicy` / plan builder). Consumer: two-up cannot
   publish duplicate person ids. Tests fail if the refusal is removed.
2. Slice B — exclude host/me/director-exclude from `ZoomActiveSpeakerDirector`
   and follow-speaker. If host talks and a guest has video, keep the guest.
3. Slice C — turn ledger (400 ms start, 600 ms end), score from the spec,
   `recommendScene` returns `slotBindings`. Port TS `selectLocalProposal`.
   Magic Scene coordinator writes bindings onto Preview only, with a loud
   empty reason. No auto-Take.
4. Slice D — binding holds for Set & Forget only (spec table). Magic Scene
   one-shot skips holds.

Invariants you must not violate:

- Interview exists only when two distinct guests have video and the second
  spoke within 12 s or is talking now. Never pad with host or with the same id.
- Follow-speaker never binds host while a guest has a content frame.
- Floor clears on speaker/meeting epoch change (`FollowSpeakerHold` already
  does this; match it).
- Screen share still outranks talker score after the existing 2.5 s enter hold.
- `recommend-auto-production` remains a query.

Keep #591 starter-scene authoring out of this work except that duplicate binds
those scenes would create must now be refused by the plan builder.

Done for a slice: native tests for the invariant, existing gates green, PR
links #668, no new ingest path, no NDI/SRT/DeckLink, no changelog of vision.

---
