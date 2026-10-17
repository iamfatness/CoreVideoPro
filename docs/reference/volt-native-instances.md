# Native graphics templates and instances

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

The base native commands are: `graphics-instance-create`, `graphics-instance-update`,
`graphics-instance-cue`, `graphics-instance-trigger`, `graphics-instance-remove`
`graphics-program-clear`, `graphics-template-install` and `graphics-template-list`. They work as JSON-line requests or entries in the
existing `media-core-sync` batch. The shell registers the corresponding dotted
HTTP/OSC actions with one required string argument, `request`, containing the
JSON request object. `GraphicsCommandGateway` validates bounded shape, epoch,
styles and fields, limits concurrent sends and checks the native result's
request ID. It refuses an older/unavailable backend without launching it.
The development workspace uses this same boundary; it does not maintain a second
on-air state machine.

Each command carries a `graphics` object with `schemaVersion:1`, `requestId`,
`clientId` and the exact `coreEpoch` observed at `snapshot.graphics.coreEpoch`.
IDs use lowercase letters/digits/hyphens with a leading letter, at most 64 bytes.
A request ID cannot be reused with different content. The last 256 operation
results are replay-protected per client for five minutes (expired entries retire on the next admission). Read the matching `clientId/requestId` in `operations`, with `lastResult` retained for older consumers;
the existing envelope/batch acknowledgement does not establish resource readiness
or display delivery.

Create requires `instanceId`, `templateId`, an installed integer `templateVersion`, `scope` and,
for scene scope, `sceneId`. The three compatibility lower thirds are compact-solid,
minimal-accent and broadcast, all pinned at version 1. They retain `name`
and `title`; empty secondary text remains explicitly empty. Style is a validated
partial patch of the existing `LowerThirdAppearance` contract. Unsupported
request/style/field keys fail rather than becoming ignored controls.

The additional Corporate lower third uses the declarative loader, ordered native
rect/text/group layout, typed fields, declared brand tokens, flow collapse,
clipping, gradients and DirectWrite shaping. `graphics-template-install` takes a
UTF-8 JSON manifest string of at most 256 KiB. A bounded persistent worker parses,
validates, hashes and publishes definitions. Admission reports queued; inspect
`templateInstalls` for installed/failed completion. It never changes an instance.
Same ID/version with different content is refused. At most two imports are pending,
16 immutable declarative versions and 256 KiB canonical document bytes are retained;
the 16-entry result history is bounded. Catalog publication is an immutable lease.
The catalog includes the exact definition and canonical SHA-256 for lossless
save/export; shell reconstruction of omitted defaults is not authoritative.

`graphics.template.install` also accepts a local `packagePath` ZIP/`.cvgraphics`
archive through the shell/API gateway. A separate bounded input worker validates
32 MiB compressed, 128 MiB expanded and 128 files, unique paths, declared inert
PNG/JPEG members, hashes and metadata. It refuses traversal, case aliases, links,
special files, nested archives and executable/undeclared members. Archive validation
cannot hold the short native command gate: manual Hide/Clear remain available.
The native worker independently validates the manifest, package root, local file
paths, hashes and bounded image headers before installing a version. Raw native
clients can supply `manifest` plus `packageRoot` instead of an archive.

Images use `asset` IDs or typed image-field references, contain/cover alignment,
clipping and premultiplied transparency. Actual package files have the exact path
`assets/<lowercase-sha256>.png|jpg|jpeg`. No ID is interpreted as a file/URL.
The native owner retains verified immutable encoded bytes; later file changes
cannot change an accepted graphic. Identical asset hashes share bytes across
versions. The encoded pool is bounded at 128 MiB, with at most one 128 MiB package
read in progress per registry; it is separate from the decoded pixel budget.
Published `encodedTemplateAssetBytes/Limit` describe that pool. The inert disk
cache admits at most 512 MiB / 2,048 files and refuses when full rather than
deleting pinned assets. Explicit re-import can repair future instances without
mutating accepted Program/Cue revisions.

Native WIC header/decode/raster work runs on the preparation owner. New image
decoding reserves a decoder/conversion/destination allowance of 12 bytes per
source pixel in the shared CPU budget before full pixel conversion; source
headers are limited to 4,096 per edge / 16 megapixels. CPU owners release before
their admission charge. Driver/library overhead is not measured by this model.
An independent source fixture proves opaque red and half-alpha green over blue
Program pixels, including unchanged accepted output after the package file is
replaced. JPEG header and WIC paths are implemented; that pixel trial qualifies
PNG, not every JPEG/ICC/profile combination or output backend.

Missing required images block readiness. Optional omissions require both an
optional asset declaration and an image node's explicit `missing:"hide"` policy;
the resulting warning remains visible. Numeric field references drive geometry,
opacity and typography in reference units. Boolean `visible` references remove
the hidden node from layout while still validating its schema. Required fields
with no native layer consumer are refused. The editor uses image choices,
color/alpha pickers and declared choices; it does not enable an unused field or
unconsumed brand/style control. Incomplete private fields do not repeatedly submit
invalid preview requests.

Declarative fields follow advertised types/ranges/choices. The style boundary
accepts only consumed placement/color/font values; compatibility-only width,
typography-size, padding and logo controls are not forwarded to these templates.
The brand adapter supplies the declared color and font vocabulary. A field patch
has at most 32 values, 4,096 UTF-8 bytes per string and a 32 KiB aggregate bound;
the transport also bounds the complete non-import request. Required blank fields
can remain private but cannot be cued or shown.

## Private workspace and durable documents

`GraphicsWorkspaceSession` owns private editing, Original/Edited comparison,
bounded gesture undo and explicit Cue/Show/Hide/Update/Clear intents. Metadata
refresh cannot replace a local edit. Show explicitly targets the accepted Cue;
an on-air update uses the current optimistic draft revision. A core epoch change
clears selection rather than replaying an old reveal. The WinUI workspace adds
searchable template/cue lists and adaptive typed fields, shared slider/exact entry,
color pickers and placement controls. Existing lower-third, caption, brand and
browser controls remain available in a named section. The private layer editor supports selection, move/resize/nudge, grouping/order,
visibility/locks, text/font/fill/gradient and verified package image references.
Native resolved layout boxes drive hit-testing; the shell never reflows text or
renders the actual graphic. Whole-graphic placement is separate from node edits.
Scratch changes compile on one latest-only worker without installing a template
or changing Cue/Program. Saving creates a new immutable version, adopted only by
explicit Cue/Update. Short/long/blank-secondary/Unicode probes replace private
text consumers only and cannot be committed as sample copy. The complete
milestone still requires remaining library, authority/migration and automation
integration plus output and operator acceptance.

`GraphicsWorkspaceLibrary` saves selected drafts/styles and exact template pins
through `GraphicsDocumentStore`, separately from production preferences. Saves are
atomic with backup, invalid-primary recovery, newer-schema read-only protection,
external-write conflicts and unknown metadata preservation. Neither load nor save
creates native instances or arms automation. Explicit Restore creates a new hidden
instance, refuses a mismatched template hash and requires explicit rebinding of a
saved source-bound cue. Loading a different native core never replays Show.
The development profile can set `COREVIDEO_GRAPHICS_DOCUMENT_PATH` and
`COREVIDEO_GRAPHICS_ASSET_ROOT`; the normal
location is `CoreVideoPro/graphics/default.json` under local application data.
Document schema 2 persists unfinished private scratch edits with the exact parent
pin, not runtime source generations or arming. Schema 1 loads in memory and is
upgraded only on explicit save, retaining its backup. Template style presets
persist non-content typed fields; applying a style validates the installed
contract and retains copy, intentional blanks, clock values and image choices.
New cues choose global or scene scope. Subject selection edits only the draft,
checks the live native source generation, and is undoable. Explicit source text
copy uses existing operator-name and secondary-line overrides. Legacy
authority/source-override migration remains incomplete.

The private canvas reuses the leased isolated native authoring monitor. Its
`set-grade-preview` demand carries `lowerThirdPreview.graphics` with installed
template identity, private fields, consumed style and animations; it uses no Cue
or Program slot. Validation resolves the installed immutable definition. Native
preparation failure remains explicit; a generated still is not marked as stalled
live video because its draft revision stays unchanged. Hidden Graphics pages
release monitor demand, and the legacy designer shares the one authoring slot.
Independent GPU samples verify native plate changes with unchanged Program pixels
and untouched instance/Cue state. This is not physical-display or output-continuity
evidence.

Create/Update also accept an optional `animations` object with `in` and/or `out`
definitions. Each definition accepts `type:none|fade|slide`,
`direction:left|right|up|down`, `durationMs:0..3000`, and
`easing:linear|ease-in|ease-out|ease-in-out`. An omitted phase retains its
previous definition; an omitted property within a supplied phase uses the V1
default (none/left/0/linear). The three compatibility templates default to cuts.
Definitions remain part of the private revision until explicitly adopted.

Update requires `expectedRevision` and an explicit `target:cue|program`. A cue
target edits the private revision; the separate Cue action accepts that exact
revision for Preview when preparation completes. Later private edits cannot
retarget a pending Cue or replace the accepted Cue. A failed replacement leaves
the prior Cue visible and exposes its warning independently of draft/Program.

Cue, direct Trigger and Remove require the private optimistic revision. Trigger
uses `action:show|hide|clear`. Show also accepts explicit `target:cue`; its
`expectedRevision` must name the accepted Cue, even if the private draft is newer.
That form refuses an absent/unprepared Cue and never airs private edits. A Show
without a target preserves the existing explicit private-revision API behavior.
Hide/Clear cannot carry a Cue target. Program Clear requires `channel:all` and
cancels pending reveals in this instance authority. The legacy overlay authority
has not yet migrated to these instances; complete-feature integration must
establish one authority before general operator use.

## Preparation and adoption

The instance limit is eight, with one private draft, one accepted Cue, one
pending Cue intent and one accepted Program revision each. Cue never changes
Program. Superseding a pending Cue requires another explicit Cue action. Show
waits for the complete prepared
revision. An explicit Program update keeps the previous valid revision until
replacement readiness; a failed replacement retains it. A newer cue edit
cancels an older pending Show instead of silently changing what that Show would
air. Hide/Clear cancel pending reveals. Same-value updates do not increment the
revision or replay the reveal. Revision numbers are not reused after removal.

Ready facts must carry the actual immutable prepared resource. Draft, accepted
Cue and Program revisions retain independent resource leases, so cache pressure or
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
and a 64-entry live/retiring limit; referenced results cannot disappear from
accounting through eviction. Lookup hits update eviction recency. Failed results
are sticky until content/device keys change. These are enforced allocation
model bounds, not measurements of driver overhead or fleet performance.
The count accommodates independent Program, accepted Cue, pending Cue and
private resources for eight instances plus bounded GPU retirement. A 16-entry
limit would pin eight Program plus eight Cue resources and prevent any private
edit from preparing even when the byte budget has ample room.

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

Accepted Cue intents and private authoring edits use distinct bounded preparation
slots; editing privately cannot cancel the revision previously requested by Cue.
Program resource requests are admitted before private/Cue preparation requests
in each D3D composition pass. A changed source incarnation invalidates obsolete
accepted/pending Cue bindings as well as Program, without replaying a reveal.

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

Cue observations include accepted/requested revision, pending status, preparation
and warning, accepted fields/style/animations/binding and its immutable resource
description. A late completion after a failed Cue is preparation only; another
Cue is required before replacing Preview. These facts remain independent of the
private authoring tap.

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
qualified. Automatic introductions consume these native source-draw facts, rather than roster names or director recommendations.

Scene filtering, direct-camera bindings and independent accepted Cue are present.
The workspace, verified image migration, title/bug/clock consumers and qualified introduction controller use this authority. Functional implementation does not establish complete output/installed acceptance. Metal does not announce this instance
capability yet. No installed app/profile/camera ownership changes are part of
this isolated development path.


The native catalog includes the original compatibility looks, the pinned corporate
reference, logo-accent and transparent-name lower thirds, centered title, corner
logo bug and clock-panel design. The clock panel uses a native timing controller; static text is not used as a
substitute for clock progress.

`placement` is optional and independent of legacy appearance: nine anchors,
reference-pixel offsets at 1920x1080, scale 0.1–4. Null restores legacy placement.
The private native monitor and accepted Cue/Program use the same resolved rect.
Scene graphics enter each scene before the video transition; global graphics are
composed once downstream, with captions last. Animated entrances wait for a native
`composition-eligible` observation. `currentlyComposed` means a draw was submitted
for that revision on the latest observed Program frame, not receiver delivery.

Operation results include the correlated instance/current revision. Private edits
retain their captured base revision; an external edit conflicts rather than being
silently overwritten or accidentally cued. Browsing a library look is private and
creation preserves its edited values. Switching instances retains private edits;
Cancel explicitly reloads the current accepted draft.


`graphics.clock.control` / `graphics-clock-control` configures the clock attached
to an instance. It shares epoch, request-id and expected-revision admission with
other graphics commands. Configuration writes additionally require the observed
`expectedClockRevision`; this revision advances on changed configuration, never
on a tick. Identical configuration is a no-op; use Reset explicitly. Actions are configure/start/pause/resume/reset; none
reveals the instance. Configuration names mode (wall/countdown), zone
(local/utc/fixed), format (hms/hm), fixed offset minutes, duration seconds and
finish (hold-zero/hide/suggest-next). Suggest-next publishes status only.

Formatting runs on a bounded native clock controller, not the compositor.
Countdowns use monotonic anchors; wall clocks use system wall time. Clock digits
have a separate content identity while the logical edit revision stays stable.
The accepted Program texture remains pinned while replacement digits prepare;
requested-content hashes fence old completions. Generated time is read-only.
Saved and duplicated countdowns restore paused from their sampled remaining time;
reset restores the original configured duration. Restore never sends Start/Show.

The private canvas adds reference-pixel drag/nudge and proportional corner scaling.
Its selection/safe-region guides are editor chrome over the native shared texture,
not a second graphic renderer. Escape cancels one gesture and one gesture is one
Undo. The same 10 Hz preview coordinator coalesces canvas edits and inspector edits.


## Catalog, compatibility and operator arbitration

`graphics.template.list` accepts `includeThumbnails:true`. This is explicit worker demand, not work performed by reading a snapshot. The catalog publishes bounded 320×180 PNGs made by the same native DirectWrite/D2D/WIC renderer; thumbnails are labeled reference appearances rather than current Program. Card and compact views share the same catalog and selection. Invalid PNG dimensions/encoding are refused before shell decoding.

Compatibility appearances can refer to a verified `image:{templateId,templateVersion,assetId}`. Neither the renderer nor a cue interprets this as a path. Explicit image import stages content-addressed inert assets and submits the ordinary native package validator. Package export copies the exact installed declaration and verified asset bytes into an atomic ZIP/`.cvgraphics` result. Scratch declarations remain private until explicit import/version installation.

Showing a lower third or bug selects its managed family authority and strips that family's legacy key layers before drawing. Hide and Clear do not silently reveal an older retained key. `graphics.authority.set` explicitly selects `legacy` or `instances`; returning lower thirds to legacy clears managed lower thirds and inhibits their automation. Captions remain independent. Existing lower-third Show/Hide/rebuild actions route to the native managed instances while that authority is selected. Rebuild uses the current qualified subject and preserves an intentionally blank secondary line.

Rack labels and order are metadata on `graphics.instance.update` (`label`, `rackOrder`, `target:"cue"`). This update is separate from content changes, conflict-fenced and does not allocate a raster revision. Rack order is native composition order within the existing scene/global scope boundary. Saved cue names, template/image pins, source overrides and unknown brand metadata survive explicit saves; loading a document does not activate an authority or replay visibility.

## Native introduction controller

`graphics.automation.configure` takes `expectedAutomationRevision` and a bounded `configuration:{rules,sourceTexts}`. At most eight unique rules target existing lower-third instances. The rule contains `id`, `instanceId`, `enabled`, optional scene/source filters, `dwellMs`, `durationMs`, `cooldownMs` and `suppressScreenShare`. Default dwell is 1,500 ms and auto-hide is 8,000 ms; duration zero explicitly holds. Unqualified/ambiguous subjects, screen sharing, wrong scope or disabled rules do not reveal a label. Source text overrides include the exact `SourceInstanceIdentity`; recycled IDs cannot inherit another generation's copy.

`graphics.automation.control` provides `set-mode`, `arm`, `disarm`, `hold`, `resume` and `hide-automatic`. Modes are Manual, Suggest, Rehearsal and Automatic. Setting a mode/configuration does not arm. Suggest publishes a current qualified suggestion; the explicit shell Cue action applies it through the ordinary revision-fenced update/Cue commands and never Shows it. Rehearsal prepares native Cue only. Automatic requires explicit arming and actual qualifying native source draw. Multiple visible guests require an explicit source filter.

One reveal is admitted per subject incarnation. Metadata updates of an owned, on-air introduction use Update without replaying entrance. Auto-hide begins at the matching native first draw, rather than at request admission. Manual content/Cue/Show/Hide/Remove intent holds that rule for the session. Resume is explicit; Clear inhibits and cancels pending automatic reveals. Reconnect disarms; saved documents retain rules only, never source generations, modes or arming. Restored rule targets must resolve to explicitly restored hidden cue IDs in the current core epoch.

The bounded trace retains 256 decisions and a dropped-history count. Requested/accepted/refused and native draw-submitted facts remain separate; they do not prove physical display or receiver delivery. #761 retains its own director acceptance; this controller does not execute its recommendations or enable scene auto-take.

## Resource capacity and isolated qualification

The process budget separately accounts CPU scratch (128 MiB), GPU reservations including retiring leases (256 MiB), two active preparers, and sixteen distinct managed content revisions including authoring. The same immutable content on another device/scale shares a revision slot but its GPU bytes remain fully charged. Existing caption/legacy resources remain byte bounded and do not consume managed revision slots. A seventeenth pinned native revision is refused atomically with `graphics-resident-revision-capacity`; accepted Program/Cue remain intact. Nodes are conservatively bounded at 256 across retained native instance content, preserving the per-bus maximum.

Independent preparation owners receive lifetime-safe pressure notifications. Their own workers trim disposable entries, never accepted leases, so an idle old renderer's obsolete cache cannot permanently starve a new private owner. Reclamation does not wait on the render caller. A full revision limit is named in pending preparation diagnostics.

`scripts/qa/volt-graphics-isolated.mjs` launches only a supplied native executable and an explicitly named synthetic helper. It uses native JSON-line commands, never the installed API, OS camera or desktop. Evidence separates exact binary hash, synthetic roster, native rule admission/draw, measured Program-buffer interval, recording finalization and decoded file format. Neither zero counter deltas nor a decoded 60 fps file establishes webcam/display continuity or a real-meeting bake. The output directory contains full bounded evidence and private native logs.


Explicit arming is fenced by the configuration revision, current mode and an independent `activationRevision`. Clear, disarm, hold, source-session change and mode transitions revoke earlier activation intents without changing saved rule configuration. An old request cannot rearm after Clear just because its rules stayed unchanged. `graphics.automation.control` requests for `arm` carry `expectedAutomationRevision`, `expectedActivationRevision` and the expected mode. The `enable-automatic` control action is the atomic mode/arm adapter for the existing `automation.lowerThirds.set` UI/API intent; it requires both revision fences and `mode:"automatic"`. It validates rules, current source qualification and manual holds before changing either mode or arming. No shell set-mode/await/arm sequence can adopt a later Clear token. Restored configuration never emits this intent. Old Show/Hide controls select only global/current-scene lower thirds and preserve custom declarative fields without injecting an undeclared secondary field.
