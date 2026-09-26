# Control-state contract execution plan

Linked issue: [#657](https://github.com/iamfatness/CoreVideoPro/issues/657).
Requirements and owners: [control-state-events-spec.md](control-state-events-spec.md).
This is a dependency plan, not a ranked work queue; [BACKLOG](BACKLOG.md) owns
priority. Each implementation slice needs an issue row before work begins. No
foundation-only PR lands on `main` without a shipping consumer.

## Preparation: enumerate state crossings

Trace every place that copies a Zoom/core observation into a shell control
document or reserializes snapshot telemetry as a command. Record producer,
consumer, field meaning, lifetime, trigger, and current test. Start with Zoom
mute/presence, audio channel mute/routing, source subscription, Preview/Take,
output session state and Control API writes. Use `architecture-ownership.md` as
the inventory, not as proof that transitions are correct. Mark fields that have
no known authority and create an issue for each independently actionable gap;
do not fold unrelated media fixes into #657.

## Slice A: one real Zoom-to-audio consumer

Extend the generated identity/contract vocabulary with the smallest roster
snapshot and participant-state fact needed by the current Windows shell and
core. Use the existing Zoom engine → core → shell link and a focused
participant-state reducer/adapter. Consume its state in the audio strip and
source availability projection; keep the #608 operator mixer mute independent.
The producer publishes a meeting/source incarnation and revision; the consumer
uses a snapshot barrier and rejects stale events. Do not grow
`StudioViewModel.cs` or move PCM into the shell. Include a fake-bridge replay
that runs mute → unmute without Preview, plus leave/rejoin with reused SDK ID.
The PR must show the generated wire fixture reaching this actual consumer, not
just a contract or a standalone event dispatcher.

## Slice B: editor intent and applied control state

Route Windows shell and Control API edits through one revision-aware command
admission path. First migrate one bounded domain, such as audio routing or a
Preview route; do not fold an atomic Take rewrite into it. Store the shell's
draft separately from the core's accepted revision. Publish an applied result,
conflict, or reconciliation state. A second client with a stale expected
revision must not overwrite the first. Once this consumer is working, port the
same golden scenarios to macOS with its main-actor projection.

## Slice C: recovery and pressure

Use the existing bounded mailbox (#622) and process epochs rather than adding
another queue. Add per-domain snapshot requests/barriers, duplicate rejection,
gap counters and explicit overrun/reconciling readouts. Exercise Zoom/core
restart, delayed old events, lost acknowledgements and command overload.
Coalesce replaceable observations and full-state updates; retain edge operation
order. Verify that no callback drives a full sync loop or exceeds the bound-list
refresh budget under snapshot load.

## Qualification and rollout

Run generated cross-language fixtures, fake-bridge transition scenarios, core
integration tests, and the normal Windows/macOS gates. Then run the first slice
on an installed build in the designated test meeting, including a natural or
controlled Zoom mute/unmute and a guest leave/rejoin. Record source SHA, binary
SHA, process and meeting epochs, event and command revisions, final applied
routing/mute state, shell strip state, PCM continuity and underrun/loss deltas.
Compare against the old-build #608 trace; an event arriving in a log without
the correct applied state is a failure. Preserve legacy clients behind the
existing capability/version adapters until both shipping shells consume the
new contract. Roll back the consumer slice if it harms media deadlines or
cannot recover from a missing event; retain the spec and evidence for repair.

## Review gate

For each state family, a reviewer must be able to answer: who may write it,
what revision identifies the change, how a cold/restarted subscriber obtains
the current value, what happens on loss or conflict, and which test proves the
shipping consumer follows those rules. Review #616/#621/#622 as supporting
transport/observation work, not substitutes for this transition proof.
