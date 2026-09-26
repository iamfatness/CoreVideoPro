# Control-state authority and event propagation

Status: proposed contract for [#657](https://github.com/iamfatness/CoreVideoPro/issues/657).
Baseline: `main` `7d0d363735fc972bcdc3b3dbd4e7c1975677cc99` (2026-09-26).
The issue owns acceptance and owner decisions; [BACKLOG](BACKLOG.md) alone owns work order.
The [execution plan](control-state-events-plan.md) describes the implementation slices.

## Why this contract is needed

The [ownership map](architecture-ownership.md) identifies owners of Zoom, audio,
scenes and outputs, but does not define the transition between a newly observed
fact and already applied state in another component. On 2026-09-26, #608 showed
the missing rule: the shell serialized a guest's Zoom mute into the core's
persistent mixer mute. A Zoom unmute refreshed the roster and strip, while the
core remained muted for about nine seconds despite receiving that guest's PCM.
Selecting the guest into Preview caused a full scene sync and happened to clear
the stale mute. PR #656 removes that particular invalid state copy; it is not a
general propagation contract or a live acceptance result for #657.

The 2026-09-24 review covered static ownership, command admission (#616/#622),
snapshot observation (#621), and source media delivery (#540). It did not audit
every transient-to-durable assignment or replay a Zoom fact change with no
operator action. Closing those scoped findings did not establish cross-component
state convergence. This is the missing review dimension, not evidence that all
earlier architecture work was wrong.

## Scope and principles

- One writer has authority for each state field. Other components hold read
  projections or submit intent; they do not silently become another writer.
- A command requests a change, a fact reports an authority's change, an applied
  result confirms a committed change, and a snapshot states the current value.
  An accepted command and a requested boolean do not prove application or media.
- Events notify subscribers that a revision exists. An authority's current
  snapshot is the recovery source. Delivery alone is never the state store.
- Extend `contracts/identity.schema.json` and existing generated models where
  needed. Use the present JSON-line command/snapshot links. An in-process typed
  dispatcher may fan out state changes; no global broker or new policy process is
  required.
- Video frames, PCM, shared textures, packet payloads and 50/60 Hz media ticks
  stay in the C++ core and existing SHM/DXGI paths. This is a control-plane
  contract, not another media transport or a replacement for `SourceBus`.
- Windows and macOS may present state differently, but must agree on authority,
  identity, revision and applied-state meaning. The optional show engine is a
  client/producer for its declared domain, not a second native Program authority.

## State ownership inventory

| State family | Authoritative writer | Consumers and permitted intent | Lifetime / persistence |
| --- | --- | --- | --- |
| Zoom meeting lifecycle, SDK roster, raw mute, talking, screen share, active speaker and recording privilege | Zoom engine for SDK facts; media core publishes the validated meeting projection | Shells, source registry, audio-strip presentation, raw-media gate, optional show engine | Meeting instance and engine epoch; never persisted as operator preference |
| Participant/source binding and subscription instance | Core Zoom runtime/source registry | Shell tiles, routing pickers, diagnostics; shells request subscriptions through typed commands | Source instance generation; a reused SDK user ID is a new incarnation |
| Operator scene document, editable Preview, show-input assignments, mixer strip controls and persisted output preferences | Shell control document; whichever client edits it submits a revisioned command | Core applies accepted production configuration; other controls read confirmed state | Document revision and explicit save; local UI selection/hover remain ephemeral |
| Applied scene, Program/Preview, audio routing, DSP parameters and Take result | Media core serialized control executor | Shells, control API, optional show engine observation | Core authority epoch and control revision; Take is an edge operation with operation ID |
| Produced Program, source health, recording/stream session and destination progress | Media core and output adapters | Shells, monitoring and qualification | Per process/output session generation; observed progress never inferred from desired state |
| Physical capture/device discovery and link state | Owning platform adapter, reported through core | Shell pickers and diagnostics; shell requests selected device | Device/source instance; preferences store selected ID, not a fabricated connected state |
| Local panel layout, selection, dialogs, accessibility focus and in-progress text edit | Each native shell | Its own views only | Local UI session; not sent through the control-state stream |
| Optional show-engine editorial state (hands, looks, panelist metadata) | Show engine for its explicitly hosted domain | Shell show-engine views; native core only through an explicit command/result boundary | Host generation and revision; its snapshot cannot overwrite native applied Program state |

The local Control API is another command client, not an authority. Its writes
must enter the same admission and revision checks as shell actions. Two clients
editing one field cannot both claim success for the same expected revision.
Audio `SourceMuted` is an observed Zoom fact; mixer `Muted` is an operator
control. The UI may display their combined effective state, but must preserve
the two underlying fields and cannot persist the combination.

## Event and command ownership

| Family | Producer / authority | Subscribers | Delivery class |
| --- | --- | --- | --- |
| `MeetingStateChanged`, `ParticipantJoined/Updated/Left`, `ActiveSpeakerChanged`, `RecordingPrivilegeChanged` | Zoom engine SDK adapter; core validates and publishes its meeting projection | Core source/subscription and raw-media state, shell roster/audio strip, optional show engine | Ordered by meeting instance and participant incarnation; roster/privilege snapshot repairs gaps; active speaker may coalesce to latest |
| `SourceInstanceChanged`, `SubscriptionStateChanged` | Core Zoom/source registry | Shell source picker, multiview and diagnostics | State facts with generation; latest snapshot authoritative after restart |
| `SetScene`, `SetAudioRoute`, `SetMuted`, `SetDevice`, `Take` | Shell or Control API submits intent; core is the applied-state authority | Core executor; result returns to every interested control client | Revisioned commands; `Take` is noncoalescible, setters may replace an unsent older value only before admission |
| `ControlApplied` / `ControlRejected` and core control snapshot | Core serialized executor | Shell views, Control API, optional show engine | Ordered applied results; snapshot barrier on gap or lost reply |
| `OutputLifecycleChanged`, `CaptureDeviceChanged`, `SourceHealthChanged` | Core adapter for the actual device/destination | Shell readouts, Control API, qualification | Owner facts and snapshots; health observations may coalesce, lifecycle edges may not |
| `ShowEditorialChanged` and show-engine snapshot | Optional show engine | Its shell views and explicit native command adapter | Host generation/revision; cannot directly write native Program state |
| Meters, frame timing and packet counters | Media core measurement owner | UI meters and diagnostics | Sampled observation, latest value and freshness; no command replay |

Fan-out is inside each process after its authority or reducer commits. Across
processes, typed envelopes ride the existing Zoom/core and core/shell links;
no subscriber writes another component's private state directly. A new
subscriber first installs a domain snapshot, then applies newer facts. A
component that only needs one projection subscribes to that projection rather
than the entire event catalog. SourceBus remains the media source/health bus,
not this control-state dispatcher.

## Contract vocabulary

| Message | Meaning and required behavior |
| --- | --- |
| `Command` | Names an intent, target entity, `ControlOperationIdentity`/idempotency key, expected authority epoch and revision, and typed payload. The receiver returns accepted, rejected-conflict, rejected-overload, or incompatible; it does not claim effect at admission. |
| `AppliedResult` | Authority committed the operation. Names operation ID, resulting `ControlRevision`, applied values and any failure. A retry of the same ID returns the same result within its defined lifetime. |
| `DomainFact` | Owner observed a new fact such as participant mute, join/leave, device loss, or output progress. Includes owner, entity/instance identity, owner epoch, monotonic revision and payload schema version. Facts do not carry another owner's desired state. |
| `StateSnapshot` | Complete current state for a declared domain and revision, including an explicit empty roster/route set. It is a reconciliation barrier; absent fields in a partial snapshot are not deletions. |
| `Observation` | Measured health/meters/counters with sample time and freshness. It may coalesce and is not replayed as a state mutation. The generated observation policy controls public redaction. |

Reuse `EntityIdentity`, `SourceInstanceIdentity`, `ParticipantBindingIdentity`,
`ControlRevision`, `ControlOperationIdentity` and `PlanGeneration` from
`contracts/identity.schema.json`. Add a control-event envelope only where those
types lack an owner-scoped sequence, meeting instance, or snapshot barrier.
Do not use a wall clock to order events. Diagnostic timestamps identify a named
clock/epoch per `contracts/evidence.schema.json`; different process clocks are
not implicitly comparable. Unknown additive fields are tolerated; unknown
required enum values or unsupported major versions are explicit incompatibility.

An event envelope must identify its producer process epoch, authority epoch,
domain, entity kind/ID, entity incarnation, revision or sequence, event ID,
payload schema version, and optional causation/operation ID. A Zoom roster
event also names the meeting instance. A core applied-state event names the
resulting control revision. A snapshot names the highest included revision.
Public/control views pass through the existing redaction policy; participant
names and meeting tokens are not diagnostic identifiers.

## Ordering, recovery and conflict rules

1. Each authority serializes mutations of its domain and increments its own
   revision. The receiver applies a revision once, ignores exact duplicates and
   older revisions, and detects a gap. Revisions from different epochs are never
   compared. The entire snapshot is installed atomically before buffered newer
   facts; older buffered facts are discarded.
2. A new process, Zoom meeting or source incarnation retires prior-epoch facts.
   Source identity remains separate from a raw reusable Zoom SDK user ID. A
   leave/rejoin with the same SDK ID must not inherit mute, subscription or audio
   state from the previous incarnation.
3. On startup, reconnect, gap, ambiguous acknowledgement or subscriber overrun,
   request a full snapshot of the affected domain. Mark it `reconciling` or
   `unknown` until that snapshot is installed. Never treat an old UI projection
   or a missing event as current truth.
4. Desired configuration and observed state remain separate. The shell can
   optimistically show a pending edit, but applied badges and routing readbacks
   come from the core revision/result. Failed or stale edits retain an explicit
   conflict or rollback state; a snapshot cannot silently erase an unsent draft.
5. Replaceable state (roster snapshots, meter observations, draft configuration)
   may coalesce by key, retaining the newest revision. Edge operations (Take,
   Record start/stop, Join/Leave) and applied results are
   ordered and may not be overwritten by a newer snapshot. A bounded queue
   reports overload and exposes counters; it cannot silently drop a durable
   operation. Preserve stop/cancel admission under load. A UI mute toggle must
   resolve to an explicit revisioned `SetMuted(value)` intent before transport,
   so duplicate delivery cannot invert it twice.
6. Dispatch to WinUI/Swift views happens on their UI actor/thread after the
   domain reducer commits. It uses structural diffs and bounded refresh rates;
   high-rate observations cannot rebuild participant collections. A subscriber
   callback cannot synchronously re-enter the full production sync. Derived
   values are recomputed from the current authoritative fields, never echoed
   back as fresh intent.
7. A core or Zoom process restart invalidates that process's applied/observed
   session state. Persisted shell intent is reconciled deliberately through
   typed commands after capabilities and a fresh snapshot, with new operation
   IDs where the operation is not safe to replay. Recording does not resume into
   an old output session ID.

## Required behavior scenarios

The contract is accepted by exercising these sequences through the real command
builder/reducer and fake bridge, then through the first installed consumer:

| Sequence | Required result |
| --- | --- |
| Guest muted in Zoom, then unmutes with no operator action | Source mute indicator updates; operator mixer mute is unchanged; incoming PCM is eligible for the same routed buses without Preview or full scene sync. |
| Operator mutes a strip while Zoom mute toggles | Operator mute remains effective until an operator/control command changes it; Zoom mute never overwrites the operator value. |
| Guest leaves and returns with reused SDK user ID | Old incarnation's late facts/subscriptions are rejected; new guest binds only after identity resolution. |
| Zoom recording right is requested, denied, then granted after another request | Raw-media availability follows the SDK's observed privilege state for the current meeting epoch; request acceptance never fabricates a feed. |
| Preview edit during roster churn | Preview revision applies without accidentally repairing or changing Program audio intent; stale sync responses cannot roll back newer state. |
| Lost command acknowledgement and duplicate retry | Query operation/snapshot, dedupe the operation ID, and do not execute a Take or Record action twice. |
| Core/Zoom restart, old queued event, then new snapshot | Old epoch cannot revive state; UI reports reconciling until the new snapshot barrier arrives. |
| Two shells or shell plus Control API edit one route | One accepted revision wins; the other receives a conflict and can rebase. No last-writer-wins overwrite. |
| Snapshot flood and bounded queue pressure | Replaceable observations coalesce, operations retain order or reject visibly, and render/audio deadlines and bound-list refresh budgets are preserved. |

## Evidence and limits

Unit tests must fail if ownership checks, epoch/revision filtering, snapshot
reconciliation, or the first consumer's mapping are removed. Contract/golden
fixtures run in supported C++, C#, TypeScript and Swift adapters. A controlled
installed meeting records exact binary SHA, process/meeting epochs, command and
fact IDs/revisions, final core applied state, shell projection, audio PCM/meter
evidence and underrun/loss counters. Hardware tests that skip are
`MISSING_EVIDENCE`. A green recorded Program alone does not prove a live
monitor path. The first slice may close only its scoped issue after this proof;
the broader inventory is not declared complete by a transport-only test.

Measure from the engine's observed fact to the core projection and then to the
shell projection; report p50/p95/max separately. On an otherwise healthy local
run, the first consumer must converge within 500 ms at p95 and must not require
an unrelated UI action. A larger tail must be diagnosed against the existing
250 ms snapshot poll and bounded queues rather than hidden by a new periodic
full scene sync. Render deadline misses, audio loss and monitor underruns are
reported as deltas during qualification; this control path may not add any.
