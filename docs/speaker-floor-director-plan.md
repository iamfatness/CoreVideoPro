# Speaker-floor director execution plan

Linked issue: [#668](https://github.com/iamfatness/CoreVideoPro/issues/668).
Requirements: [speaker-floor-director-spec.md](speaker-floor-director-spec.md).
This is a dependency plan, not a ranked work queue. [BACKLOG](BACKLOG.md) owns
priority. No foundation-only PR lands on `main` without a shipping consumer.
Slices A–D merged as #670–#673. Issue #668 remains open for the installed
host-plus-three-guests, interview, solo, and share-priority acceptance below.
Current rank lives only in [BACKLOG](BACKLOG.md).

## Why this order

A better template with duplicate slots still looks broken. Unique bind and host
exclude are the first consumer. Scene selection from talk history is the second.
Magic Scene applying bindings is the third. Holds and Set & Forget calibration
come last.

## Slice A — person identity and unique bind

Add a pure `SpeakerFloor` type under `native/src/core/` that maps sources to
person ids and records host flags. Teach `RouteSourcePolicy` / the program and
preview render-plan builder to refuse a second bind of the same person on one
plan. Keep `FollowSpeakerHold` frame validation.

Consumer: an `interview` or two-layer Preview plan cannot publish two layers
with the same person id. Tests fail if the refusal is deleted.

No scene-selection change in this slice. No shell UI. No PCM.

## Slice B — host excluded from follow-speaker

`ZoomActiveSpeakerDirector` (and the shell source-id filter it already uses)
drops host / me / exclude-from-director persons from the directed set. If the
only talker is host and a guest has video and a recent frame, keep the last
guest. Clear on epoch change.

Consumer: follow-speaker routes on Preview/Program. Native test: host talking,
guest with frame → directed id is the guest.

## Slice C — floor ledger and scored recommendation

Extend `SpeakerFloor` with turn start/end (400 ms / 600 ms) and the spec score.
Change `Director.h` `recommendScene` to take the floor snapshot and return
`slotBindings`. Port the same selection into `selectLocalProposal` so the TS
mirror does not diverge.

Consumer: Magic Scene coordinator reads bindings from the snapshot/query and
writes them onto Preview layers. Empty bindings produce an operator-visible
reason, not a silent no-op.

Do not auto-Take. Do not grow `StudioViewModel` with slot math; keep a focused
coordinator type.

## Slice D — binding holds and gallery membership

Apply the spec binding holds for Set & Forget only. Magic Scene one-shot still
skips them. Gallery drop/add uses the 6 s / 1 s / 20 s quiet rules. Reuse
`requiredDirectorHoldMs` for scene-level share/scene holds.

Consumer: Set & Forget does not collapse a two-up the instant the second guest
inhaled. Test the 8 s silent drop.

## Qualification

- Stub core and native unit tests for A–D.
- WinUI coordinator test: Preview bindings only.
- Installed meeting evidence on #668 before close: three guests + host, host
  talking, two-guest interview, one guest drops to solo, screen share still
  wins. Attach SHA and the recommendation snapshot.

## Review gate

A reviewer must answer: who is a person, who is host, what score produced the
binds, why a two-up was or was not emitted, and which test fails if a duplicate
bind is reintroduced.
