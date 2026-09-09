# Wave 1 shadow integration design

This is an implementation plan, not an enabled runtime feature. The existing
MediaCore remains the only production authority and side-effect owner. Shadow
ShowStateOwner, SourceRegistry and ShowPlanGenerator produce comparison evidence
only. They must never subscribe to a source, start a writer, prepare GPU resources,
apply a Take, or publish a render/audio/output plan to production consumers.

## Startup and ownership

Proposed startup-only flag: `COREVIDEO_AUTHORITY_SHADOW=1`, default off. Read it
once in JsonRpcServer::run before workers start (`native/src/rpc/JsonRpcServer.cpp:304`,
initial handshake at 458). Reject unsupported flag values in diagnostics rather
than silently enabling. Use a fresh shadow authority epoch and registry epoch per
native process. No live toggle, persisted preference, or automatic enablement.
Handshake/profile should disclose mode and adapter version without claiming parity.

One dedicated low-priority shadow control worker owns both new owners and invokes
`generateShowPlans`. It consumes immutable metadata records from existing work;
it does not call back into MediaCore or acquire coreMutex, audioOutputMutex_, SDK,
compositor, or encoder locks. Shutdown stops intake and joins only this worker's
bounded CPU work; there is no cancellation of production resources. If a malformed
record or internal exception disables shadow processing, production continues and
evidence records `disabled-error`, not a successful comparison.

Use a fixed-capacity metadata mailbox with a byte budget, initially 64 records and
2 MiB total, and a bounded single latest diagnostic result. No pixel/audio buffers,
GPU handles, SDK pointers, resource leases, or source display names enter it.
Admission must be nonblocking. On contention/full/oversized input, increment a loss
counter and invalidate comparison continuity. Never block a render/audio worker or
silently classify a comparison made across dropped records as matching. After a
gap, require a complete bounded metadata checkpoint before comparison resumes.
Bounds need qualification; these initial values are not performance guarantees.

## Hook points and capture boundaries

Line numbers describe the current working tree and may move during integration.

| Current seam | Minimal future hook | Restrictions |
| --- | --- | --- |
| `MediaCore::applyCommands`, MediaCore.cpp:1203–1235 | Capture one post-batch desired-state metadata checkpoint after all `applyCommandMutation` calls, before any direct-mode synthetic render | One shadow control revision per semantic batch; never expose intermediate scene/overlay/output mutations |
| `MediaCore::applyCommand`, MediaCore.cpp:1249 | Same capture helper after its single mutation | Do not independently capture from `applyCommandMutation` at 1254; that would double-count batch changes |
| `MediaCore::syncZoomMediaSpine`, MediaCore.cpp:898 | Capture its completed authoritative routing/input state if it bypasses the batch path | Route all capture through a shared sequence allocator; duplicate semantic state must remain unchanged |
| JsonRpcServer::run command branch, JsonRpcServer.cpp:973–1003 | After `handle` has released coreMutex, submit the owning captured record | Existing response serialization already occurs outside coreMutex; do not add shadow generation to `handle` |
| `MediaCore::renderDisplayTick`, MediaCore.cpp:5311; existing ingestion at 5374–5429 | Copy only metadata from frames already accepted by the existing ingest path | Never call `pollVideoFrames`, drain SDK queues, request frames, or ingest a second time for shadow |
| Existing `buildCompositorRenderPlan` result, MediaCore.cpp:5603, and `lastRenderPlan_` publication at 5632 | Capture a compact legacy binding observation from the plan actually selected by this tick, with its control/source basis | Do not invoke `buildRenderPlanForScene` a second time; it includes current routing and animation-related state |
| JsonRpcServer render worker, JsonRpcServer.cpp:559–609 | Move the metadata observation out with existing owning event records and offer it after coreMutex release | Do not stringify or compare under the render lock; no new wait in the pacer |
| `MediaCore::sessionState`, MediaCore.cpp:612 | Copy the already-published bounded shadow diagnostic snapshot | No source polling, plan generation, registry mutation, or waiting for the shadow worker from a state request |

Single-thread direct tests must invoke the same capture helper explicitly after
their mutation/render boundary. They must not rely on the RPC worker to drain it.
Audit every command entry point before enabling comparisons; an uncovered mutation
sets `unsupported-entry-point`, not a new guessed revision.

## Typed adapter inputs

Introduce value-only adapter records, separate from raw command JSON:

- `ShadowBasis`: native process epoch, capture sequence, legacy semantic revision,
  source checkpoint sequence, show-clock generation, monotonic capture timestamp.
  Serialize wide timestamps as decimal strings. None is a frame-completion claim.
- `DesiredShowCheckpoint`: explicit Preview/Program scene IDs, stable scene/route
  IDs and generations, ordered layers and Show Inputs, source selectors, overlays,
  audio routing intents, output requests, Tiles membership and persistent ISO arming.
  Translate into `ShowStateData`; do not copy the legacy resolved plan into it.
- `SourceCheckpoint`: stable SourceId, SourceInstanceId, owning process epoch,
  generation, kind, availability, requested/observed subscription and accepted
  format/publication metadata. Explicit PersonId bindings require durable mapping
  evidence. A display name or raw reusable Zoom participant handle cannot create
  PersonId or an incarnation-safe source identity. Missing mappings are unsupported.
- `LegacyPlanObservation`: bus, scene identity, ordered route/slot binding outcomes,
  exact source tokens where available, intentional blanks/missing bindings, fixed
  geometry, and the exact ShadowBasis used to construct the plan. Record compiler
  intent separately from the eventual frame's rendered/delivered identity.

Zoom helper replacement must enqueue epoch retirement before the new checkpoint.
Serialize this on the same intake sequence as replacements; SourceRegistry must
reject late old-epoch records. Device replacement uses its source owner incarnation,
not path equality alone. Only immutable records are passed between threads.

Do not invent missing capabilities. The current SourceRegistry publication fact is
historical video observation; ShowPlanGenerator explicitly reports audio capability
as unknown. The v2 goldens' independent audio/video availability and freshness are
not yet completely expressible through these owners. Initially compare supported
identity/intent projections only. Freshness, audio eligibility, dynamic selectors,
unmapped durable-person following and unsupported Tiles behavior remain unverified.

## Comparison and evidence

Each comparison must use a coherent pair of desired/source checkpoints. Associate
the legacy observation with that exact pair at capture, rather than comparing it
against whatever state is newest when the shadow worker runs. If the pair is absent,
classify `basis-gap`. Keep at most the bounded mailbox/checkpoint history; do not
grow an archive waiting for old observations.

Suggested additive `authorityShadow` diagnostic fields:

- `version`, `enabled`, `status`, `nativeProcessEpoch`, `shadowAuthorityEpoch`,
  `registryEpoch`, adapter/generator version, supported-domain mask.
- `lastCapturedSequence`, `lastProcessedSequence`, `lastComparedSequence`,
  `controlRevision`, `registryRevision`, `showClockGeneration`, `progressAgeMs`.
- Cumulative `captured`, `compared`, `matched`, `diverged`, `unsupported`,
  `basisGaps`, `queueDrops`, `oversizedRecords`, `exceptions`; counts per domain.
- `queueDepth`, `queueBytes`, `maximumQueueDepth`, `maximumCaptureNs`,
  `maximumGenerateNs`, `maximumCompareNs` and total worker CPU time.
- First divergence and latest divergence: classification, domain, capture basis,
  bus/route/selection opaque IDs, expected/observed status and token generations.
  Bound each diagnostic record to 4 KiB; redact private names, content and paths.

Classification is explicit: `identity-mismatch`, `stale-instance`, `order-mismatch`,
`blank-vs-substitution`, `missing-vs-resolved`, `intent-loss`, `geometry-mismatch`,
`unsupported`, `basis-gap`, `adapter-invalid`, or `internal-error`. Animated geometry
must not be compared against an unanimated target; compare stable geometry only or
classify unsupported. A matching supported subset does not turn unsupported domains
into passes. Plan bindings do not prove fresh pixels, GPU completion, physical
presentation, encoder writes, or recording completion.

## Rollout and tests

1. Add typed adapters and offline fixtures without runtime hooks. Independently
   execute the authority goldens; expected outputs must not be generated from the
   production resolver or copied back from `LegacyPlanObservation`.
2. Add disabled-by-default hooks with side-effect spies: identical commands with
   flag off/on must cause identical SDK subscriptions, source polls, render calls,
   encoder starts/stops and output submissions. Shadow preparers/Take callbacks
   are never instantiated. Unknown flags must not enable hooks.
3. Test batch atomicity, direct-call parity, no-op revisions, source retirement,
   reconnect, duplicate names, device replacement, blank/fixed-missing routes,
   ordered Tiles holes and retained ISO intent. Unsupported A/V capability must
   stay explicit instead of interpreting historical video publication as audio.
4. Race capture against scene changes and source replacement. Delay/reorder shadow
   execution, saturate its mailbox, drop checkpoints, inject oversized metadata and
   generator exceptions. Assert bounded memory, no production waits, exact basis
   matching, and no match result after continuity loss until checkpoint recovery.
5. Run packaged, explicitly enabled headless and then coordinated live workloads.
   Record exact binaries/hardware/workload and compare flag-off/on capture overhead
   and actual output deadlines. Any shadow-induced missed delivery is a regression;
   do not mask it by enlarging Program buffering or changing media quality.

## Cutover and deletion boundary

Shadow comparison has no routing fallback and cannot promote its result. Keep all
legacy production resolvers intact until adapters cover every admitted domain,
golden conformance and real workload comparisons pass without unexplained divergence,
and independent output qualification passes. A separate reviewed cutover installs
one authoritative ShowState/SourceRegistry owner and makes consumers adopt only its
prepared immutable plan generation. Preparation and AtomicTakeCoordinator require
that later integration; they are not part of shadow mode.

In that cutover PR, remove the corresponding legacy mutation-to-resolver path and
its second state storage from MediaCore together, with consumer-level tests proving
one subscription/apply/output side effect per operation. Delete domain by domain
only when its replacement is authoritative; do not leave two executable authorities
behind a mismatch fallback. Shadow adapters may remain temporarily for evidence but
must remain read-only. Full MediaCore/coreMutex retirement is a later dependency,
not a claimed outcome of this minimal integration.
