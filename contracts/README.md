# Additive lifecycle and identity contract slices

`lifecycle.schema.json` is the source of truth for protocol version, output
lifecycle, asynchronous operation status and structured protocol failure objects.
`npm run contract:generate` emits checked-in C++, C#, browser TypeScript, Node
TypeScript and Swift models plus validators. `npm run contract:check` and CI
reject stale generated output. The two TypeScript outputs are generated identically
so the Node package preserves its `rootDir: src` build boundary.

The schema is deliberately a small first slice. It does **not** generate the entire
legacy protocol or replace its envelope/dispatch adapters. `lifecycle.fixtures.json`
contains identical raw wire messages for all language suites. Tests cover required
and optional fields, explicit null, booleans, integer bounds and decimal notation,
unsupported major versions, unknown enum values, and additive object fields.
C# and Swift also exercise typed decoding/encoding after validation; C++ exercises
its generated serializer and JSON validators. Legacy parity string tests remain
until their message families gain serialized-message tests.

Wire rules:

- Field names are case sensitive. Additive object fields are accepted and may be
  discarded by typed models. Clients must not rewrite unknown fields to persist
  a newer client's complete document.
- Required fields cannot be absent or null. Optional `error` may be absent;
  explicit null is invalid. Serializers omit absent optional fields.
- Lifecycle integer fields retain signed 32-bit bounds. Identity revisions use the
  JSON-safe integer range 0..9007199254740991, represented by Int64/long in native
  DTOs and number in TypeScript. Generation counters start at 1. JSON numeric
  notation such as `1.0` is a valid integer; fractions and overflow are invalid.
- Unknown lifecycle/health/operation enums fail validation. A consumer should
  display unknown/unverified state and report incompatibility, never coerce an
  unknown enum into live/success. An unknown additive field is different from an
  unknown value in a closed enum.
- Call the generated runtime validator **before** using a decoded object. DTO
  deserialization alone does not enforce every enum or semantic constraint.
- Protocol major 1 is supported; higher minor versions remain additive. Legacy
  messages without these new objects pass through explicit legacy adapters.

## OutputLifecycle vocabulary (PR22, truthful destination lifecycle)

`OutputLifecycle.state` is the destination state machine:

```
requested -> preparing -> producing -> stopping -> finalizing -> completed | failed | interrupted
```

- **requested** — a Start command was ACCEPTED. Nothing has been opened, nothing
  written. An acknowledgement is not evidence (engineering rule 7).
- **preparing** — the writer/sender has actually been asked to open. Still no output.
- **producing** — output progress has been OBSERVED, and it is FRESH. This state is
  re-decided against the clock every time the session is read, so a wedged writer
  cannot latch it. The staleness budget is
  `runtime-snapshot-qualification.mjs` `DEFAULT_RUNTIME_POLICY.encoderQueueAgeMs`
  (1000 ms) — one declared definition of "the encoder has stopped moving", not two.
- **stopping** — Stop has BEGUN. It does not claim, imply or approximate completion.
- **finalizing** — the stop barrier is draining / the container is being finalized.
- **completed** — the finalize returned AND media was written. `finalized` says so.
- **failed** — a writer/transport failure, or a finalize with nothing written.
- **interrupted** — it produced, then stopped producing, and nobody asked it to.
  Recoverable: it returns to `producing` if real progress resumes.

`starting` and `live` are the RETIRED names for `preparing` and `producing`. They
remain in the enum so a newer consumer can read an older producer; new producers
must not emit them. A consumer that treats an unknown state as healthy is wrong —
fail closed and report incompatibility.

The legacy `recording.status` / `recording.writerStatus` fields are PROJECTIONS of
this lifecycle whenever the core reports one (`core::publishedRecordingStatus`), so
they can no longer contradict it — which they did, for the whole finalize window,
because Stop assigned `"stopped"` before the writer had been asked to finalize.

Senders (RTMP/SRT/NDI) carry the same contract per destination
(`outputSenders.senders[].lifecycle`). Absent means UNKNOWN — an older core — never
healthy.

## Remaining supported protocol families

| Family | Current handwritten owners | Next coverage boundary |
| --- | --- | --- |
| RPC envelopes, hello/capabilities, command acknowledgements | `native/src/rpc/JsonRpcServer.cpp`, `src/engine/nativeBridgeProtocol.ts`, C# client, Swift bridge | Envelope IDs, required fields, response/error unions |
| Scene graphs, preview, tiles, overlays, backgrounds, media playback | `MediaCore.h/.cpp`, `nativeMediaCoreProtocol.ts`, C#/Swift scene builders | Route modes, coordinate fields, nullability, atomic scene batch |
| Show inputs, participant roster, Zoom source/subscription/spine | `ZoomEngineRuntime`, `zoomMediaSpineSync.ts`, C#/Swift Zoom models | Durable identity vs session ID, partial roster updates, subscription limits |
| Recording/streaming configuration and full output telemetry | Encoder/sender interfaces, core snapshots, shell snapshot DTOs | Per-destination identity, writer stats, artifact/finalization proof |
| Audio buses, mixer, DSP/VST, device routing | Native audio module DTOs and shell builders | Numeric units/ranges, topology, plugin state |
| Capture and frame transport | Native capture/shared-texture messages and platform bridges | Handle ownership, dimensions/strides, timestamps, process epoch |
| Diagnostics, support, licensing, automation/control | Core/control servers and shell view models | Redacted diagnostics, action idempotency, compatibility capabilities |

Do not declare full generated-contract coverage until these families have their
own schemas, golden fixtures, runtime validation and tested legacy adapters.

## Identity/revision foundation (Wave 0 PR01)

`identity.schema.json` adds seven DTO/validator families. The existing generated
`Lifecycle` modules contain both slices so existing compilation and packaging
include them automatically. This is additive: no legacy command, saved preference,
SDK identifier, or runtime routing behavior is replaced by this PR.

| Contract | Meaning |
| --- | --- |
| EntityIdentity | Opaque ID tagged by person, participant, source, scene, route, audioRoute, showInput, output, or recorder; equal strings of different kinds are different entities. |
| EntityRevision | Entity identity plus its authority epoch and monotonic configuration revision. |
| SourceInstanceIdentity | Stable source ID plus a distinct live instance ID, owning process epoch and instance generation. |
| ParticipantBindingIdentity | Meeting participant ID explicitly bound to a particular source instance; never infer this mapping from display-name equality. |
| ControlRevision | Revision of the serialized authoritative control dataset within one authority epoch. |
| PlanGeneration | Render/audio/output plan identity and generation, its source control revision and show-clock generation. |
| ControlOperationIdentity | Idempotency key and expected control revision scoped to an authority epoch; not an acknowledgement that an operation was applied. |

A durable person is not a meeting participant, and a stable source is not a live
SDK handle. Preserve opaque IDs exactly, including case; do not normalize them to
names or array positions. Participant IDs are application-assigned references scoped by their meeting/source
instance owner; a raw reusable SDK numeric handle alone is not such an ID.
The authority must allocate a distinct participant reference for a new meeting
incarnation rather than aliasing an earlier participant. Changing a process, reconnecting a source, or replacing a resource
must not silently reuse an old instance generation. Scene, route, audio route,
show-input and output IDs describe configured entities and survive reorderings.
Recorder entity IDs describe configured recorders; existing OutputLifecycle
sessionId continues to identify each execution of a recording/output session.

Revisions order configuration changes only within the same authority epoch.
Generation identifies a replacement incarnation, not a rendered frame number.
Do not compare revisions from different epochs or roll counters through zero:
at the JSON-safe maximum the owner must reject further increments or establish
an explicit new epoch. Expected revision is a precondition, never last-writer-wins
permission. Plan clockGeneration links to the future shared show-clock contract;
this PR does not define wall-clock timestamps or introduce another clock.

Validators enforce wire shape, tags, nonempty identity fields and numeric bounds.
They do not prove global uniqueness, monotonic progression, referential integrity,
identity resolution, atomic Take, or an output's success. The future authoritative
state owner must enforce those invariants transactionally. Unknown entity/plan
kinds fail closed, while unknown object fields remain additive. Do not persist a
newer document by round-tripping these DTOs: unknown fields are not preserved.

`identity.fixtures.json` exercises all entity kinds, missing/null/wrong-type
fields, additive fields, unknown kinds, generation zero, fractions and boolean
numbers, the Int32 boundary and maximum safe integer. C++, C#, both TypeScript
outputs and Swift consume the same fixtures. C++ additionally round-trips the
maximum revision through the actual JSON serializer; C#/Swift round-trip valid
DTOs. Swift execution requires macOS CI.

## Stage-specific evidence (Wave 0 PR02)

`evidence.schema.json` is an additive vocabulary, not runtime instrumentation. Its
11 DTOs share observation ID, process and authority epochs, control revision,
clock ID/generation, and an observation timestamp. Observation timestamps and
content times are **integer nanoseconds relative to the named clock epoch**, not
Unix timestamps or a process-specific clock silently compared across processes.
Values must remain JSON-safe; change epochs explicitly before exhausting that
range. Producers must translate device/SDK clocks into this named timeline.

Stage meanings are deliberately different:

| Contract stage | Evidence required from its eventual producer |
| --- | --- |
| accepted | An operation was admitted against an expected revision; no state/media success is implied. |
| applied | The authority committed the operation at `appliedRevision`; `controlRevision` must name that resulting revision. |
| rendered | The described media range completed rendering into a leased resource. `completionToken` must refer to actual backend completion, not merely CPU submission. |
| delivered | The named destination consumed the media range; scheduled and consumed times are separate. Enqueue alone is insufficient. |
| presented | Presentation feedback identifies the range actually presented at `presentedAtNs`. An export, swap-chain submission, or UI snapshot is insufficient. |
| muxed | The muxer accepted the media range into the artifact packet range. This does not establish durable bytes or decodability. |
| committed | The storage adapter committed the named artifact byte range under a `commitId`; the adapter must define and test its durability boundary. |
| completed | The destination session finalized for the named final stream and plan generations. Empty sessions can complete; this is not an artifact validation verdict. |

Media observations identify the plan, stream, and their generations. Audio
sequence units are sample frames; video sequence units are frames. Ranges are
half-open `[sequenceStart, sequenceStart + sequenceCount)`. Content time and
duration describe the same range. A producer must reject range-end overflow,
stale generations, mismatched clock identities, duplicate evidence, and invalid
ordering. These semantic checks require state and are **not** performed by the
shape-only generated validators. In particular, receipt of a later-stage DTO
must never synthesize missing observations for earlier stages.

`DestinationProgress` is per destination/session/stream generation. Its
`counterEpoch` changes on an explicit counter reset. Produced/delivered/presented
units, muxed packets, and committed bytes are different quantities; they cannot
be compared as if all count frames. Missing instrumentation must be reported as
unavailable by the enclosing protocol, not fabricated as a zero-filled progress
object. Cumulative counters must be monotonic within one counter epoch.

`ResourceLeaseDescriptor` describes a resource incarnation and owner domain; it
does not transfer ownership or authorize release. Lease lifetime is explicit
(active/retiring/released), with no invented TTL. The runtime must reject stale
lease generations and keep resources alive until all actual users finish.

`ArtifactValidationResult` identifies the exact artifact revision, validator
version and `checkSetId` used. `passed` means that **all checks in that named set**
passed, not that arbitrary production requirements were tested. Required checks,
file identity/hash binding, successful decoding, nonempty expectations, packet
reconciliation, and per-frame timing remain qualification responsibilities.
Counts of zero are structurally valid for an incomplete or empty artifact; shape
validation alone never makes it a successful recording. Artifact identity must
change or revision must advance whenever its bytes change.

All types accept additive unknown fields and reject unknown enum values. The
shared evidence fixtures run through browser/Node, C++, C#, and Swift validators;
C# and Swift additionally exercise valid DTO round trips. Swift execution still
requires macOS CI. The existing `Lifecycle` generated filenames are retained so
this foundational vocabulary adds no runtime/build-system migration.
