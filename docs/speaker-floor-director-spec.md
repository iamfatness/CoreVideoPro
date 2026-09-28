# Speaker-floor director

Linked issue: [#668](https://github.com/iamfatness/CoreVideoPro/issues/668).
Execution plan: [speaker-floor-director-plan.md](speaker-floor-director-plan.md).
Agent brief: [agent-briefs/speaker-floor-director.md](agent-briefs/speaker-floor-director.md).
This document is requirements and ownership. It is not a work queue.
[BACKLOG.md](BACKLOG.md) owns rank.

Owner intake 2026-09-26: Magic Scene must choose layouts from who spoke last,
how long they spoke, and when they last spoke; build dynamic galleries from
that floor; active-speaker follow must not include the host; the same person
must not appear twice in a two-up.

Owner amendment 2026-09-28: The guest-only rule still governs Magic Scene's
talker slots and the existing Active Speaker route, but a new **per-source**
Active Speaker choice may include host. See
[operator-source-controls-spec.md](operator-source-controls-spec.md#668--host-selectable-active-speaker).
That section supersedes unconditional host-exclusion language below only for
the new opt-in directed source. It does not change interview qualification.

## Problem

`native/src/core/Director.h` and `src/engine/localDirectorProvider.ts` pick a
**template id** (`intro`, `interview`, `panel`, `speaker-slides`) from
`liveCount`, `hasDominantSpeaker`, and screen-share presence. They do not
assign people to slots.

That produces three operator-visible failures:

1. Magic Scene looks dead because Preview gets a scene name without distinct
   faces, and Program does not change until Take.
2. A two-up can bind the same person twice (Zoom self-view + host UVC, starter
   scene layers from #591, or follow-speaker plus a fixed slot on the same id).
3. The host steals the talker box. Hosts talk constantly. A real show does not
   cut to the operator on every "mm-hmm".

`FollowSpeakerHold` remembers directed ids and requires a content frame. It
does not exclude the host and does not collapse two sources that are one person.

## Product rules

1. Guests earn boxes by talking. Quiet cameras do not keep a two-up or a wall
   seat forever.
2. The host is never a Magic Scene talker-slot or guest-only Active Speaker
   bind while any guest has video. An explicit include-host Active Speaker
   source may select the host under its separate eligibility policy.
3. One person occupies at most one visible slot on a recommendation.
4. Count of live cameras is not the input. Distinct guest identities with a
   talk ledger are the input.
5. No ML. Deterministic, pure, epoch-scoped. Same meeting epoch as the Zoom
   engine / speaker director.
6. Shell still Takes. The kernel returns scene + bindings. Magic Scene applies
   bindings to Preview. Set & Forget may Take after the existing stabilizer
   holds. The core does not cut Program from this kernel.

## Identity

A **person** is a Zoom user id for Zoom-originated feeds. Local capture that
the operator marked as the same person, or that is flagged Host / Me, shares
that person id.

Collapse before scoring:

- Two sources with the same Zoom user id are one person.
- Host UVC + the host's Zoom tile are one person.
- Screen share is not a person slot. It is a surface bound beside a talker.

Reuse of Zoom user ids across meetings is already an epoch problem (#659).
The floor ledger clears when the speaker/meeting epoch moves (join, leave,
Engine off, new engine process), same rule as `FollowSpeakerHold`.

## Host

A person is host when any of these is true:

- Zoom `isHost` or `isMe`
- source or participant flagged Host / Exclude from director
- local capture the operator marked as program host cam

Host may appear as:

- the only solo shot when **no guest has video**
- an optional dedicated host PIP on `speaker-slides` (not a talker slot)
- gallery membership only if a future setting "include host in gallery" is on
  (default **off**)

Under the original guest-only policy, host must not appear as:

- the follow-speaker bind (the new include-host source is an opt-in exception)
- either seat of an `interview` two-up when a guest exists
- a second copy of themselves next to their Zoom tile

If the host is the only person talking and a guest still has video, hold the
last guest talker. Do not switch to host-focus `intro`.

## Floor ledger

Per meeting epoch, per person:

| Field | Meaning |
|---|---|
| `id` | Person id |
| `isHost` | See Host |
| `hasVideo` | Content frame this tick |
| `talkingNow` | Active-speaker after turn-start debounce |
| `turnStartedAtMs` | Start of the current continuous turn |
| `lastSpokeAtMs` | Now if talking; else end of last turn |
| `lastTurnMs` | Duration of the last completed turn |
| `windowTalkMs` | Talk time inside the last 45 s |

Turn start: 400 ms of talking (filters backchannel).
Turn end: 600 ms of silence.

Guest score:

```text
S = 3 * talkingNow
  + 2 * exp(-ageSec / 8)
  + clamp(lastTurnMs / 8000, 0, 1)
  + 0.5 * clamp(windowTalkMs / 20000, 0, 1)
```

`ageSec = (now - lastSpokeAtMs) / 1000`. People who have never spoken this
meeting score 0 and may only fill leftover gallery holes after scored guests.

## Recommendation output

`DirectorRecommendation` grows slot bindings. A binding is `{ slotIndex, personId, sourceId }`.
`sourceId` is the preferred source for that person (guest Zoom tile over a
duplicate of the same person). Empty `personId` means the slot stays empty.
Never invent a second bind of an already used `personId`.

Existing rule ids stay: `screen-share-priority`, `single-speaker`,
`focused-interview`, `panel-discussion`. Add `hold-empty` only when the
stabilizer already holds; do not mint a parallel rule vocabulary.

## Scene selection

Candidate set = guests with `hasVideo`, unique by person id, sorted by `S`
descending, then by `lastSpokeAtMs` descending, then by id.

| Condition | Scene | Bindings |
|---|---|---|
| Screen share active (after 2.5 s enter hold) | `speaker-slides` | Share surface + current guest talker. If the sharer is a guest they may be the PIP. Host is not the PIP while a guest talker exists. |
| 0 guest video | `intro` | Host only, or no person bind |
| 1 scored guest, or second guest silent > 8 s | `intro` | Slot 0 = that guest |
| 2 guests with video and the #2 last spoke within 12 s or is talking now | `interview` | Slot 0 = highest `S` (current talker on the left), slot 1 = next distinct guest. If a second distinct guest with video does not exist, **do not** emit `interview`. |
| 3–4 guests with video and at least 2 with `S > 0` | `panel` | Top 4 unique by `S`; talking-now pinned first |
| 5+ guests with video | `panel` | Top 6 unique by `S` (9 only if the existing wall template already has 9 seats). Host off the wall by default. |

Quiet cameras drop from gallery membership after 20 s with `S` decaying and
not talking, unless dropping would leave the layout short of the scene's
minimum faces. Talking-now always pins.

## Holds

Reuse `autoProductionDirector.ts` numbers for **scene** changes:

- enter share 2500 ms
- leave share 3500 ms
- other scene change 4000 ms

Additional **binding** holds:

- follow-speaker switch 1500 ms (keep `FollowSpeakerHold` history of 8)
- solo → two-up: second guest talking 1200 ms
- two-up → solo: second guest silent 8000 ms
- gallery add: 1000 ms talking or already in top-N
- gallery drop: 6000 ms out of top-N and not talking

Magic Scene one-shot ignores binding holds and emits the current floor
immediately onto Preview. Set & Forget honors holds.

## Unique-bind invariant

Before any route is published from a director recommendation:

1. Collapse sources to person ids.
2. Walk slots in order.
3. Skip any person already bound.
4. If `interview` cannot fill two distinct guest person ids with video, emit
   `intro` with one bind instead.
5. Follow-speaker routes use the directed guest, never a positional fallback
   that could alias another slot's person (`FollowSpeakerHold` already forbids
   a no-frame positional fallback; keep that).

This invariant is core policy, not a shell cleanup. `RouteSourcePolicy` must
refuse a second bind of the same person on the same program/preview plan.

## Magic Scene actuation

`recommend-auto-production` stays a query. The shell Magic Scene coordinator:

1. Reads the latest recommendation including `slotBindings`.
2. Writes those bindings onto the Preview scene layers.
3. Does not cut Program.
4. Surfaces a loud reason if bindings are empty ("no guest video", "only host
   on camera").
5. Take remains the cut.

Set & Forget continues to use the existing confidence gate (≥88 take, else
queue) and producer-preview override. This spec does not change those numbers.

## Non-goals

- ML face framing
- Captions / ASR as a director input
- Changing Program from the kernel
- DeckLink/AJA, NDI, SRT
- Growing `StudioViewModel.cs` or moving pixels into the shell
- Rewriting starter-scene authoring (#591) beyond refusing duplicate binds
  the director would otherwise inherit
- Per-participant audio delay (#627)

## Tests the kernel must fail if removed

- Host talking, guest with video → follow-speaker id is the guest.
- Two sources, same Zoom user id → one bind.
- `interview` recommendation never contains duplicate `personId`.
- One guest with video → `intro`, not a padded two-up with host or self.
- Second guest silent 8 s → two-up becomes solo.
- Screen share still wins over talker score.
- Epoch change clears the ledger.
- Magic Scene path (shell test) applies bindings to Preview only.

## Evidence

Close #668 only after native kernel tests plus an installed meeting: three
guests plus host, host talking over guests, then two guests alternating, then
one guest silent. Record SHA, recommendation `ruleId`, bindings, and that
Program two-up never showed one face twice.
