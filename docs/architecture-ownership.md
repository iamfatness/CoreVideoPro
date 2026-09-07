# Production policy ownership

This map describes current ownership and the remaining migration. The native
engine, Zoom subprocess, platform GPU adapters and native shells remain the
architecture. A shared runtime decision is different from matching handwritten
shell implementations; only the former has one execution owner.

## Implemented common boundary

`native/src/core/RouteSourcePolicy.h` is the pure route-to-source binding policy
used by `MediaCore::buildCompositorRenderPlan`. Both shipping shells already send
their scene graphs to this core, so program and preview rendering use this single
policy on Windows and macOS. It is constructible/testable without the core, a GPU,
Zoom, a dispatcher or either shell. The extraction preserves existing behavior:

| Input | Binding |
| --- | --- |
| Media ID and valid path present | Media source takes precedence |
| Capture-input mode with device ID | Exact `capture:` identity used by capture frames |
| Explicit participant ID | Keep that guest identity, even if its current frame is absent |
| No explicit identity, fallback frame at route position | Existing positional fallback |
| No explicit identity or fallback | Unbound source |

Screen-share routes retain their screen-share frame kind. An incomplete media
reference falls through to the prior participant/capture behavior. Native tests
cover these precedence rules, missing guests and a reordered fallback roster.

The positional fallback also exists for legacy `none` mode. It is compatibility
debt, not the desired future meaning of an intentionally blank source. This
extraction does not silently change a live show's blank/fallback semantics; a
versioned route contract must distinguish intentional blank from omitted legacy
assignment before changing that rule.

`ZoomActiveSpeakerDirector` remains the native owner of speaker sensitivity,
minimum hold, frame freshness, exclusions and temporary roster absence grace.
`core/Director.h` remains the pure scene-recommendation kernel. These decisions
should be reused through core snapshots, not reimplemented in new shell code.

## Ownership inventory

| Responsibility | Current owner(s) | Intended boundary / remaining action |
| --- | --- | --- |
| Persisted production preferences and scene document | Windows `ProductionOutputPreferencesStore`, Swift `AppModel` preferences | Shell persists user document; native owns accepted rendering state. Define document schema and migration independently from runtime snapshots. |
| Scene editing and route pickers | Windows `SceneRoutingService`, `StudioViewModel`; Swift `AppModel.buildRoutes`; React scene helpers | UI labels, canvas gestures and selection stay local. Route resolution and identity rules should converge on explicit core requests. |
| Final source binding and render plan | Native `RouteSourcePolicy`, `MediaCore`, compositor adapters | Shared pure binding policy now extracted; GPU allocation/rendering stays in adapters. |
| Stable editorial identity | TypeScript show-engine `personKey`, `identity`, `panelistDb`, `liveSlots`; Windows session role registry; Swift slot source IDs | No universal durable identity contract exists yet. Define durable person key separately from Zoom participant/session ID, including ambiguous-name/PIN collision handling. |
| Program/preview Take | Windows `TransportCoordinator.TakeAsync`; Swift `AppModel.take`; TypeScript `ProgramBus` | Still shell-owned. Introduce a native atomic Take operation with an operation ID and scene revision before retiring shell swaps. |
| Zoom lifecycle and roster | Native `ZoomEngineRuntime` / subprocess; shell supervisors and participant UI | Core owns SDK operation state; shells own interaction and presentation, consume operation/status snapshots. |
| Output sessions and observed health | Native encoder/output sender/core snapshots; shell transport coordinators | Core owns writer/destination lifecycle and session IDs; requested state belongs to commands/reconciliation. |
| Audio topology and DSP | Core/audio modules; shell mixer presenters | Core owns applied routing/mix state. UI owns controls, meters and editing intent. |
| Platform UI-thread work | Windows dispatcher and Swift main actor | Retain platform scheduling at a narrow adapter boundary; pure policy must not reference UI controls. |
| Development show-engine | TypeScript identity, hands queue, roles, program bus, overlays, gallery, speaker gate | Supported deterministic development client/harness. Do not advertise every show-engine feature as shipping native behavior. |

## Deliberate differences requiring migration work

- Windows can Take a pending media cue when preview and program use the same
  scene ID, promoting playback with a new Take version. Swift currently rejects
  same-scene Take. TypeScript `ProgramBus` models a bus swap without native media
  playback promotion. These are behavioral differences, not naming differences.
- Windows production roles are session assignments and intentionally not persisted.
  TypeScript show-engine roles can follow `pin:`, normalized `name:`, then `id:`
  person keys across reconnects. Swift production inputs bind source IDs and have
  their own offline/rebind behavior. Porting name/PIN matching without collision
  rules would risk routing the wrong guest.
- Swift coalesces scene edits with a 120 ms task debounce. Windows transport sends
  a scene sync after Take. A future native Take must be an ordered edge operation,
  distinct from replaceable full scene state, and must not be removed by debounce
  or replayed after an unknown acknowledgement.

## Next bounded migration

1. Generate `SceneRevision`, durable identity references, route intent, and native
   Take request/result schemas. Establish explicit blank/missing-guest behavior.
2. Add a pure native program/preview transition policy with expected revision,
   Take operation ID, same-scene draft promotion and media playback generation.
   Execute it on the serialized core state owner and expose confirmed scene IDs.
3. Drive the same golden scenarios through both shell fake bridges and the native
   policy: reconnect with new participant ID, ambiguous identity, missing guest,
   preview edit without Take, normal swap, same-scene media cue, active speaker,
   stale revision, lost acknowledgement and duplicate operation ID.
4. Migrate Windows and Swift independently behind capability negotiation. Remove
   duplicated shell mutations only after both consume confirmed native results.
5. Narrow `ITransportHost` into transport intent, scene promotion and UI feedback
   capabilities incrementally; do not combine that extraction with semantic changes.

This change establishes shared render binding and a concrete ownership map. It
does **not** complete durable-identity migration, atomic native Take, cross-shell
Take parity, full host-interface decomposition, or generated legacy-protocol
coverage. Those remain explicit acceptance items in the remediation plan.
