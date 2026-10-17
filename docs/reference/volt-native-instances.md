# Native graphics compatibility instances

Implementation reference for the first native consumer in #926. This is a
technical slice of the approved [Volt specification](volt-graphics-spec.md),
not acceptance of the complete product milestone. Work order remains in
`../BACKLOG.md`.

## Authority and command path

`graphics/GraphicsInstances` owns accepted revisions under the core control
owner. `MediaCore` delegates commands, appends immutable render-plan values and
feeds back native preparation/draw facts. It copies the published immutable JSON
lease into `CoreStateCapture`; it does not assemble a graphics tree for every
snapshot under the core mutex.

Six native commands exist: `graphics-instance-create`, `graphics-instance-update`,
`graphics-instance-cue`, `graphics-instance-trigger`, `graphics-instance-remove`
and `graphics-program-clear`. They work as JSON-line requests or entries in the
existing `media-core-sync` batch. The shell registers the corresponding dotted
HTTP/OSC actions with one required string argument, `request`, containing the
JSON request object. `GraphicsCommandGateway` validates bounded shape, epoch,
styles and fields, limits concurrent sends and checks the native result's
request ID. It refuses an older/unavailable backend without launching it.
The new operator workspace remains a separate integration requirement.

Each command carries a `graphics` object with `schemaVersion:1`, `requestId`,
`clientId` and the exact `coreEpoch` observed at `snapshot.graphics.coreEpoch`.
IDs use lowercase letters/digits/hyphens with a leading letter, at most 64 bytes.
A request ID cannot be reused with different content. The last 32 operation
results are replay-protected per client. Read the correlated `lastResult`;
the existing envelope/batch acknowledgement does not establish resource readiness
or display delivery.

Create requires `instanceId`, `templateId`, `templateVersion:1`, `scope` and,
for scene scope, `sceneId`. The current catalog is the three compatibility
lower thirds: compact-solid, minimal-accent and broadcast. Fields are `name`
and `title`; empty secondary text remains explicitly empty. Style is a validated
partial patch of the existing `LowerThirdAppearance` contract. Unsupported
request/style/field keys fail rather than becoming ignored controls.

Create/Update also accept an optional `animations` object with `in` and/or `out`
definitions. Each definition accepts `type:none|fade|slide`,
`direction:left|right|up|down`, `durationMs:0..3000`, and
`easing:linear|ease-in|ease-out|ease-in-out`. An omitted phase retains its
previous definition; an omitted property within a supplied phase uses the V1
default (none/left/0/linear). The three compatibility templates default to cuts.
Definitions remain part of the private revision until explicitly adopted.

Update requires `expectedRevision` and an explicit `target:cue|program`. Cue,
Trigger and Remove require the same optimistic revision. Trigger uses
`action:show|hide|clear`. Program Clear requires explicit `channel:all` and
cancels pending reveals in this instance authority. The legacy overlay authority
has not yet migrated to these instances; complete-feature integration must
establish one authority before general operator use.

## Preparation and adoption

The instance limit is eight, with at most one draft and one accepted Program
revision each. Cue never changes Program. Show waits for the complete prepared
revision. An explicit Program update keeps the previous valid revision until
replacement readiness; a failed replacement retains it. A newer cue edit
cancels an older pending Show instead of silently changing what that Show would
air. Hide/Clear cancel pending reveals. Same-value updates do not increment the
revision or replay the reveal. Revision numbers are not reused after removal.

Ready facts must carry the actual immutable prepared resource. Accepted draft
and Program revisions retain independent resource leases, so cache pressure or
an unsuccessful edit cannot evict the raster currently needed by Program.
Readiness without a resource fails and cancels pending reveals. Content keys
include a SHA-256 of the accepted content/style/binding (excluding animation),
the existing scale signature and the device identity. Internal resource handles
never cross the control API; snapshots expose backend, dimensions and content
key only. Frame metadata drops preparation leases after the state owner adopts
them, rather than retaining textures in downstream encoder queues.

The native timeline samples `steady_clock`, not render-call count or shell
timers. An interrupted transition starts from its sampled opacity/position;
Show during an exit reverses continuously and repeated Show cannot restart an
entrance. Updating on-air content preserves the running transition. Hide
retires the Program revision when its out transition finishes; Clear cuts on
the next composition boundary. Snapshot visibility distinguishes entering,
on-air, exiting and hidden; the independent draw observation is still required
before reporting a composed on-air graphic. These control/timeline tests do
not establish scheduled-output timing qualification.

The Program render plan carries private preparation candidates independently
from visible layers. D3D11 prepares DirectWrite/WIC/D2D software rasters on one
persistent worker, creates immutable textures through the free-threaded device,
and publishes immutable leases. The worker never borrows the compositor's
immediate context. The render caller only performs lookup or attempts bounded
queue admission; a busy owner does not make it wait.

The preparation cache reserves an estimated scratch/result allocation before
running work. Its 256 MiB accounting includes published and retired texture
leases plus queued/in-flight preparation reservations. It has 32 pending slots
and a 16-entry live/retiring limit; referenced results cannot disappear from
accounting through eviction. Lookup hits update eviction recency. Failed results
are sticky until content/device keys change. These are enforced allocation
model bounds, not measurements of driver overhead or fleet performance.

All D3D11 raster owners additionally share `GraphicsResourceBudget` in the
native process: 128 MiB modeled CPU pixel allocations, 256 MiB GPU textures,
and at most two concurrent pixel preparation producers. The worker reserves
capacity before allocating pixel storage. Image headers are probed once and
the same WIC frame is retained through conversion; its conversion/destination
pixel allowance is included. CPU scratch and the producer slot release at
preparation completion. The GPU charge stays with the texture until its final
cache/instance/read-completion lease releases. Device replacement therefore
does not erase charges for the old device. Temporary pressure refuses the job
without caching a permanent failure; permanently oversized assets fail with
an explicit reason. This model excludes decoder/library/driver overhead and
is not a claim about process working-set or VRAM telemetry.

`resourceBudgetAtStatePublication` reports that allocation model, limits,
high-water counters and refusals when graphics state publishes. It is explicitly
a publication-time sample, not live driver memory or deadline evidence. Queued
jobs have bounded metadata and each cache's conservative future-work reservation;
the shared pixel budget is acquired when a producer starts allocating pixels.

Each private cue uses one preparation slot. A newer admitted edit replaces
older queued edits for that slot; an already executing producer keeps its
reservation until completion but its superseded result is not published.
Other slots and the accepted Program revision are independent. An oversized
or otherwise refused request cannot cancel valid admitted work. Slot metadata
is bounded by queued/in-flight jobs rather than a growing revision history.
Shared content jobs remain valid while another slot needs them. Canceled
request metadata/device leases move to a bounded disposal queue (at most the
pending capacity) and are destroyed on the preparation owner; when that queue
is full, further replacements are refused until the owner drains it. Refusal
is observable backpressure, not successful admission. Tests retry transient
try-lock refusal before asserting an admitted job's completion.

The compositor retains raster results through its existing preallocated GPU
read-completion query leases. Empty passes poll retirement too, preventing Hide
from stranding a lease. Query exhaustion refuses a new graphics draw and reports
degradation. Waiting and shutdown joins are outside the render-call path.
Factory cleanup occurs on the preparation owner before its COM apartment exits.

## Evidence vocabulary and limits

Preparation states are unprepared/preparing/ready/failed. Program visibility is
logical native state and independent of preparation. Snapshots expose draft and
Program revisions, separate field/style values, warnings and the first native
`draw-submitted` frame. That observation proves a native draw submission only;
it is not a GPU-completion, camera-reader, decoded-stream or physical-display
continuity claim.

## Subject identity integration boundary

The existing Zoom ingest exposes two different lifetimes: `VideoFrame.sourceEpoch`
changes when width/height or stream incarnation changes, while
`ZoomEngineParticipant.sourceGeneration` identifies the roster incarnation and
survives a resolution-only change. `ZoomEngineRuntime::registerVideoStream`
and `ZoomEngineState`'s roster adoption are the authoritative producers.
A graphics binding must not treat a resolution upgrade as a different guest,
nor treat a reused numeric SDK participant ID as the same guest.

`GraphicsSourceCatalog` projects the authoritative Zoom roster and configured
video frames into a bounded catalog. Bindings carry `sourceId`, `instanceId`,
string `processEpoch` (core plus Zoom meeting/process epoch) and exact safe
integer `generation`. The catalog does not invent source availability or
subscription state, and does not create a second lifecycle registry. Create
and Update accept this complete `binding`, or explicit null to unbind; a naked
numeric/source ID is insufficient. Native validation remains authoritative
when the shell snapshot is stale.

Roster reincarnation/session replacement cancels obsolete preparation and
retires the prior subject's Program label before composition. Resolution-only
pixel epochs do not revise the binding or overwrite manual text/blank titles.
Composition requires the exact source generation, an explicit matching camera
layer selector and actual textured source draw submission in the current pass;
metadata alone, a slate, or a different routed participant does not qualify.
This observes draw submission, not receiver continuity. Current source binding
supports direct Zoom cameras; Tiles and nested scene membership remain to be
qualified. There is no automatic participant reveal in this slice.

Scene filtering and direct-camera bindings are present. A separate accepted Cue,
native draft-authoring tap, package loading,
legacy migration, the operator workspace, other graphic families and automation
remain requirements of the approved feature. Their absence must not be hidden
by the compatibility instance capability. Metal does not announce this instance
capability yet. No installed app/profile/camera ownership changes are part of
this isolated development path.
