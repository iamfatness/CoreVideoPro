# Wave 1 authority golden scenarios

`test/data/wave1-authority.json` contains 35 normative, implementation-independent
scenarios, version `authority-goldens-v2`. The validator imports no runtime code.
Adapters must independently execute facts/operations and compare actual decisions
with `expected`; they must not use the validator as their authority implementation.

```powershell
node test/validate-authority-goldens.mjs
node --test test/authority-goldens.test.mjs
```

The suite verifies fixture consistency and rejects contradiction mutants. Passing
it does not establish native/client conformance. Existing production owners remain
unmodified; their adapters must explicitly map every concept below.

## Machine-readable structure

Each scenario has unique `id`, `kind`, explicit `state`, and `expected`. Route,
Tiles and ISO scenarios contain `request`; Take contains ordered `operations`.
Top-level ledgerPolicy is `retain-rejection-tombstone-until-authority-epoch-ends`.

| Kind | Facts | Expected observation |
| --- | --- | --- |
| route | sources, optional ordered inputs/people/retiredSources; independent video and audio selectors | source token or null and separate reason for each medium |
| tiles | ordered inputs, roster, sources; automatic/manual mode, exclusions and explicit roster opt-in | ordered eligible members, or exact manual slots including holes |
| iso | selections with persistent selection IDs and selectors; current sources | all armed IDs, current writer media eligibility, waiting IDs |
| take | authority epoch/revisions, result ledger, expired IDs, operationCapacity; one or more operations | per-operation acceptance/error/original result and total additional promotions |

## Identity and replacement

A source token contains all four fields: stable `sourceId`, concrete `instanceId`,
`processEpoch`, and positive `generation`. Rejoin preserves stable sourceId while
changing the concrete instance, process epoch and generation in the golden.
A current source additionally identifies personId. Duplicate names are attributes;
name-only selection never establishes authority, even if discovery has one match.

Selectors are `person` with personId, `pinned` with exact token, `input` with inputId,
`blank`, `muted`, and deliberately unauthoritative `name`. Person-following resolves
the one explicitly bound current source; exact pinning never moves automatically
to a new instance/generation. Multiple eligible instances require explicit selection
and resolve ambiguous. The fixture represents current sources only; retiredSources
records tokens that must not match. An adapter must not merge tombstones back into
current candidates. Rebinding an input uses its current authoritative personId,
not its previous person or array position.

## Independent media availability

Source facts separately declare videoAvailable, videoFresh, audioAvailable,
audioFresh and audioMuted. Freshness is supplied by the media authority, never
inferred from availability or a remembered first frame. Camera off makes video
ineligible without disabling fresh audio. Camera return remains stale until a new
video frame; the old cached frame cannot establish readiness. Muted/stale audio
never blanks eligible video. An explicit video blank may retain separately selected
audio; explicit audio mute is separate. Deliberately different audiovisual selectors
are valid. Agreement is required only where selectors resolve to the same identity.

These fixtures assume one explicitly selected audiovisual source per person.
Camera versus screen-share choice needs a future explicit kind/source preference
rather than falling through by collection order.

## Tiles policy

Show Inputs are an ordered array of `{id,personId}`; their editorial ordering is
not a map's lexical order. Automatic Tiles includes current fresh eligible video
from these inputs only. With explicit allowRosterAdditions, unseen roster people
append in roster order. Person identities are deduplicated while retaining first
position; exclusions apply to both input and roster candidates. Camera-off, stale
or departed candidates are omitted in automatic mode.

Manual mode is different: every slot ID remains in place. Explicit blank, excluded
person or departed source yields a null slot with its reason. No following person
is promoted into the hole. These are membership/identity goldens; pixel geometry,
animations and reserved-slot rendering remain separate compositor tests.

## ISO policy

Selection is persistent arming intent. `armed` retains every selected ID even while
its source is missing or all media is ineligible. `writers` describes current media
eligibility, not a command to recreate/terminate an encoder on each media toggle.
Camera-off with fresh audio yields audio-only eligibility; no synthetic video is
claimed. `waiting` lists selections with neither eligible medium. A recorder may
retain its file generation and journal missing video intervals; it must not silently
claim continuous video or disarm the operator's selection. Actual audio-only/slate
recording format and recovery are output-policy work, not inferred here.

Person-following ISO resumes on the newly authorized source token; an exact pinned
ISO remains waiting after replacement until deliberately rebound. Unselected roster
members never acquire writers. Generation changes must be preserved in recording
identity/journals, even when the editorial selection persists.

## Atomic Take, replay and bounded retention

Operation identity includes authorityEpoch and request fingerprint (expectedRevision,
previewRevision; future payload fields must join the fingerprint). A fresh accepted
Take promotes that exact Preview revision and increments authority revision once.
A duplicate with the identical payload replays its original result, even after newer
state or Preview changes. Lost acknowledgement has the same replay rule. Same ID
with altered payload fails operation-id-conflict. Old authority epochs always fail,
including after process restart; an old operation cannot become a new Take.

At the maximum JSON-safe authority revision (9,007,199,254,740,991), a new
otherwise-valid Take fails `revision-exhausted` with no promotion or increment.
Known-ID replay still returns its original result, including when both revision
and ledger capacity are exhausted. Historical accepted ledger entries must contain
valid fingerprints, increment their expected revision exactly once, and promote
the Preview revision requested by that fingerprint. Impossible historical results
are invalid fixture facts, even if the expected replay output copies them.

Evicting a detailed result leaves a rejection tombstone until that authority epoch
ends. Retrying that ID yields operation-expired with zero promotions, never a fresh
operation. The combined result/tombstone capacity is explicit and bounded. At
capacity, new IDs fail operation-capacity; known-ID replay/rejection still works.
An implementation can use equivalent compact sequence watermarks, but must preserve
these outcomes. Epoch renewal requires explicit lifecycle coordination and invalidates
old clients; it must not silently renew during a show merely to discard deduplication.

`concurrent:true` supplies a legal authority serialization order in operations.
Two identical first submissions produce one promotion and one replay; two distinct
operations against the same expected revision produce one promotion and one stale
conflict. Actual adapters must test concurrent submission and validate equivalent
linearizable results (either distinct request can win if order is unspecified).

## Adapter conformance

1. Build isolated authoritative state with declared source tokens, input order,
   revisions and ledger. Apply replacement facts before resolving requests.
2. Resolve actual route/Tiles/ISO observations without GPU/network/writer effects.
3. For replay scenarios, apply prior accepted operations once, advance state as
   declared, discard acknowledgements where requested, then submit operations.
4. Compare normalized observed decisions and side-effect counts with expected.
5. Treat unexplained divergence as failure. Changing expected policy to fit an
   implementation requires explicit review, not regeneration from that implementation.

Runtime owners, generated contracts, CMake and existing production files are untouched.
