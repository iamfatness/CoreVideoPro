# CoreVideo Pro Graphics specification

Working subsystem name: **Volt Graphics**. App navigation: **Graphics**.
Revision 1, October 16, 2026. **Proposed for owner review; not implemented by this
document.** Tracking: [#924](https://github.com/iamfatness/CoreVideoPro/issues/924).
Read with the [source reconciliation](volt-graphics-reconciliation.md) and
[implementation plan](volt-graphics-implementation-plan.md). Work order remains
in `../BACKLOG.md`. The supplied Volt document is input; this document resolves
its ambiguous lifecycle, performance, compatibility and operator semantics.

## 1. Product outcome and scope

An operator can create a branded interview/webinar look, cue it safely, put it on
air, update it and take it off without leaving CoreVideo Pro. The same native
composition reaches Program, webcam, recording and streaming. The operator can
see what is draft, ready, actually composited, manually held or blocked.

The first complete milestone includes:

- The existing compact-solid, minimal-accent and broadcast lower thirds, preserved
  as compatibility looks; five additional curated looks described below.
- Source-bound names/secondary lines, explicit local instance overrides, brand
  tokens, transparent logos, reusable named templates and named style presets.
- Logo bugs, single/two-line titles, wall clocks and countdowns.
- A template library, layer canvas, precise inspector, native preview, cue rack,
  show/hide/update controls and keyboard/control-API parity.
- Opt-in participant-driven lower thirds, clock/cue timing and a dependable
  Suggest/Rehearsal/Automatic graphics workflow with manual override.

The next gated capability is data-driven captions-as-graphics, CSV/XLSX, JSON
HTTP, RSS, tables, leaderboards, crawls and sequential playlists. Caption
transcription itself remains in its existing subsystem. Google Sheets can first
use an explicitly configured published endpoint; private OAuth/Sheets and custom
WebSocket adapters require separate implementation scopes and credentials work.

Sports protocol adapters, arbitrary scripts/shaders, 3D, PSD/AE import, media-rich
crawls, MAM integration, a 50–80-template library and external key/fill/NDI export
are later extensions. None is required to finish the first usable milestone.
The design supports them without claiming compatibility or competitive parity.

## 2. Terms and invariants

| Term | Meaning |
|---|---|
| Template | Immutable, versioned definition of fields, layers, layout, style tokens and allowed animations |
| Style preset | Named parameter/token overrides for a compatible template family; not a live graphic |
| Instance | Stable graphic identity with explicit scope, bindings, parameters and accepted revision |
| Draft | Shell-owned proposed edit; cannot mutate Program or a saved template until explicitly committed |
| Cue | Validated instance/revision prepared for native Preview; no Program visibility implied |
| Prepared revision | Complete assets/layout exist and pass admission for a specific device generation |
| Presented revision | Compositor recorded that revision in a named bus/frame; not proof of receiver display |
| Featured subject | Source identity actually selected by the accepted native scene/render plan, not a proposed speaker recommendation |
| Scope | `scene` (travels with a scene) or `global` (survives scene Takes), plus explicit bus targeting |

Required invariants:

1. Shell owns no production pixels or real-time animation. Existing C++ media
   core remains compositor/encoder authority; no new production video bus.
2. Identical effective values are a no-op: no new raster, reveal, auto-hide reset,
   parameter revision or animation restart. Repeated commands are idempotent.
3. No disk/network/font lookup/decode/rasterization, worker wait, unbounded queue
   or immediate-context sharing is introduced on a render/output deadline.
4. An acknowledged edit is not `ready`; `ready` is not `on air`; `on air` does not
   claim physical display or camera-receiver completion.
5. Existing grades, ROI, Scenes/Tiles, source assignment, text overrides, output
   scopes, safe regions, clocks and manual overrides survive the migration.
6. No missing/invalid value silently becomes another guest, a successful cue,
   zero, a fabricated live value or an enabled automation rule.

## 3. Operator workflows

### 3.1 Workspace layout

Use one vertically usable workspace: a searchable library/cue list at left,
large native canvas in the center, selected-item inspector at right, and compact
Preview/Program cue controls. On narrow windows, collapse library/inspector into
drawers; preserve selection and keep the canvas and transport accessible.
There is no endless horizontal strip of controls.

Library filters are Lower thirds, Titles, Bugs, Clocks, Saved and later Data.
Cards include an actual rendered thumbnail, family, editable fields, asset/font
warnings and a compatible brand indicator. A compact list view exposes the same
information for operators who prefer density. Browsing loads no live instance.

Canvas manipulation supports select, move, resize, alignment/safe-region guides,
layer order, visibility and lock. Keyboard arrows nudge one 1080-design pixel;
Shift nudges ten. Escape cancels the current gesture without removing the
selection; undo is one complete gesture. The inspector uses the established
slider + synchronized exact entry + units/default/reset contract. Name and
secondary text remain easy to reach from Sources, without opening the designer.

Show selected instance, source identity, template version, draft/accepted/native
revision, bus/scope and stale/unavailable state. Status must remain readable
while lists receive metadata updates; do not rebuild bound collections per frame.

### 3.2 From library to air

1. Choose a look; edit fields or select `Follow featured source`.
2. Choose the existing brand or explicit style overrides. Preview representative
   short, long, blank-secondary and Unicode text before saving a template.
3. **Save template** saves a version; **Save style preset** saves compatible
   overrides. Neither changes a queued/on-air instance.
4. **Cue to Preview** prepares the selected revision on the native Preview bus.
   Preparation errors leave the previous cue available with an explicit warning.
5. **Show on Program** reveals that prepared revision; the button shows a pending
   state until native composition is observed. It does not Take video.
6. **Hide from Program** starts its out transition; **Clear Program graphics**
   removes managed graphics immediately at the next composition boundary and
   suppresses automatic reappearance until explicitly re-armed.

The cue rack includes a thumbnail, subject/field summary, status, scope, next
action, hold/auto-hide and template version. It supports duplicate, rename,
reorder and delete; deleting an on-air cue requires an explicit Hide/Delete
choice. A transient HTTP/data error cannot delete the cue.

### 3.3 Editing an on-air graphic

Editing opens a private draft with Original/Edited comparison and a visible
`Editing on-air instance — changes not yet applied` indicator. **Update on air**
validates/prepares the full revision, then atomically swaps it at a frame boundary
without another build-in. While preparing, the old valid revision stays on air.
Cancel discards the draft. Undo after an applied on-air change creates another
explicit update; it does not silently rewrite the live frame.

The legacy designer's **Apply look** remains an explicit atomic on-air style
action with equivalent behavior; retain it or migrate its label clearly to
**Update on air**. Do not convert its existing semantics into a hidden auto-apply
or silently make it save-only. A separate template edit updates future instances
only; existing instances display `New template version available` until updated.

Changing the brand updates drafts/future cues. Existing Program instances pin
their accepted brand revision. **Update on-air graphics to this brand** prepares
and commits a complete multi-instance revision or leaves the old brand intact.
No mixed-color/font frames or implicit updates from editing Settings.

### 3.4 Source text and identity

Sources remains authoritative for the operator name/secondary-line override.
The precedence for an instance field is: explicit instance override, explicit
source override (including intentionally blank), approved metadata mapping,
template default. Reset restores the next level and states which value it uses.
Blank title collapses its line and spacing; default `Guest` cannot reappear over
an intentional blank. Read-only metadata and editable override are distinguishable.

Instances bind a canonical source plus meeting/ingest generation, not a Show Input
row number or a bare recycled participant ID. Reassignment/guest replacement
cannot inherit the previous guest's name. Cross-meeting name matching is never
automatic; preserve the existing source-identity policy and offer explicit rebinding.

For `Follow featured source`, pre-prepare the incoming content when available.
On a Take/source-epoch change, remove an obsolete subject label in the same
composition transaction as the changed video. If the new label is not ready,
show no label until it is ready; never retain the old guest's name over a new
guest. A real subject change can reveal once. Identical roster/health updates
cannot replay animation. Changes to the same subject's values update atomically.

### 3.5 Scope, scene Takes and clean outputs

Each cue explicitly selects Scene or Global. Scene graphics are part of that
scene's Preview revision and are committed with its video Take. Global graphics
remain through video Takes until hidden. Duplicate scene edits do not duplicate
global bugs or reset a global clock. Scene deletion cannot remove an on-air
global instance. Within a video dissolve, scene graphics use the same accepted
transition and bus revision; they do not independently Take the video.

Program graphics are upstream of shared Program outputs. Webcam, recording and
streaming inherit those pixels. Source ISO recordings retain their existing
source-only semantics. Editor guides, ROI guides, thumbnails and draft previews
are never burned into outputs. Webcam-only OH framer/LUFS remains separate under
#839. Clearing Volt graphics does not clear captions or webcam-only diagnostics;
the control is labeled accordingly. Any future per-output graphics needs its own
explicit routing contract, not silent divergence between Preview and Program.

V1 layer channels have explicit order: scene graphics, global graphics, then
existing caption accessibility overlays. Order within a channel is editable and
stable by instance ID for ties; template node order is independent of cue order.
The editor displays channel and cannot silently put a graphic behind video.
Caption safe bands and operator-defined protected rectangles are layout inputs.
Automatic placement tries configured anchors in a deterministic order and refuses
a new reveal with `scope-conflict` if no allowed slot fits. Reserve the caption
band while captions are enabled so changing caption text does not bounce a lower
third. Manual positioning can explicitly allow overlap, with an editor warning;
automation never overrides that protected-region policy. V1 does not promise
face detection or automatic face tracking: protected subject regions are manual
or use an already-qualified upstream signal, not speculative new vision logic.

## 4. Template and style contract

### 4.1 Package and versioning

A package contains `template.json` and content-hashed local assets. Required
header: `schemaVersion:1`, stable `templateId`, integer `templateVersion`, `name`,
`family`, `canvas`, `bounds`, typed `fields`, ordered `layers`, `animations` and
declared `brandTokens`. IDs match `[a-z][a-z0-9-]{0,63}`; layer/field IDs are
unique within their namespace. Versions are immutable; save creates a new version.

Supported first-milestone families are lower-third, title, bug, clock and
countdown. V1 node types are rect, text, image and group; groups allow explicit
positioning or horizontal/vertical flow with gap/padding. No HTML, script,
expression evaluation, executable plugin, shader source or network asset URL.
Unsupported major versions, node types, field types or animation operations fail
validation before installation. Unknown additive metadata is preserved when
round-tripping; clients must not silently strip a newer package's definition.

All geometry is finite numeric **1920x1080 reference units**, scaled by output
height / 1080. No mixed `100%` strings and pixels. Root `bounds` define the local
graphic rectangle; the instance's anchor, safe offsets and transforms place it
on the output. Text sizes/padding/radius scale with the same factor. On other
aspect ratios, use the actual native frame and safe-region constraints; no video
resolution change is allowed to fit a graphic. Pixel snapping is applied only
when resolving final layout. Premultiplied alpha is the shared pixel contract.

Fields declare type (`string`, `number`, `boolean`, `color`, `image`), default,
required/bindable, maximum length/range and optional choices. Missing optional
strings resolve to empty; a missing required field is a cue error. Template
field references are objects such as `{ "field":"name" }`; brand references
are `{ "token":"brand.primary" }`. Literal strings are literal, not expressions.
Tokens resolve from a finite declared vocabulary, not arbitrary object traversal.

Text nodes declare font, weight (400/500/600/700), size, line height, color,
alignment, maxLines (1–3) and overflow (`ellipsis` or `wrap-then-ellipsis`). They
use native shaping for Unicode/bidi; no ASCII-only production fallback. Default
lower thirds use one line for each field, ellipsis at the right output boundary,
and `collapseWhenEmpty` in a flow group. Font substitution produces a visible
warning and prevents automatic airing until the operator accepts the fallback.
Images use contain/cover plus alignment; transparent logos default to contain.
Invalid/missing required assets block readiness. Optional assets may be omitted
only with an explicit warning and the template's defined fallback.

V1 rectangles allow a solid color/brand token or a two-stop linear gradient;
opacity is 0–1 and corner radius is clamped to half the resolved smaller edge.
Colors use `#RRGGBB` or `#RRGGBBAA`, resolved as SDR sRGB before the backend's
documented blend conversion. Images use a package-local asset ID or a declared
image field; an ID is never interpreted as an arbitrary filesystem/network URL.
Groups can nest to the resource limit and inherit opacity/transform/clip.
There are no arbitrary blend modes or node-to-node dependency expressions in V1.
Every node has explicit finite bounds; flow children may omit x/y, which the
parent resolves. Required geometry cannot be missing, negative or non-finite.

V1 in/out animation types are `none`, `fade` and `slide`; slide directions are
left/right/up/down, durations 0–3000 ms, and easing is linear/ease-in/ease-out/
ease-in-out. Zero duration is an atomic cut. No per-node arbitrary keyframes or
looping reveal in the first milestone. The runtime applies the same resolved
geometry and alpha rules to interruptions and native authoring simulation.

### 4.2 Illustrative valid definition

The layout illustrates the proposed V1 contract; runtime/schema generation is a
delivery step, not provided as an already-supported import format by this spec.

```json
{
  "schemaVersion": 1,
  "templateId": "corporate-lower-third",
  "templateVersion": 1,
  "name": "Corporate lower third",
  "family": "lower-third",
  "canvas": { "width": 1920, "height": 1080 },
  "bounds": { "width": 1050, "height": 176 },
  "brandTokens": ["brand.primary", "brand.onPrimary", "brand.fontBody"],
  "fields": [
    { "id": "name", "type": "string", "default": "", "required": true, "bindable": true, "maxLength": 256 },
    { "id": "title", "type": "string", "default": "", "required": false, "bindable": true, "maxLength": 512 }
  ],
  "layers": [
    { "id": "plate", "type": "rect", "x": 0, "y": 0, "width": 1050, "height": 176,
      "fill": { "token": "brand.primary" }, "opacity": 0.92, "cornerRadius": 8 },
    { "id": "text-stack", "type": "group", "x": 24, "y": 20, "width": 1002, "height": 136,
      "flow": "vertical", "gap": 8, "children": [
        { "id": "name-text", "type": "text", "text": { "field": "name" },
          "width": 1002, "height": 62, "font": { "token": "brand.fontBody" },
          "fontWeight": 600, "fontSize": 42, "lineHeight": 1.2,
          "color": { "token": "brand.onPrimary" }, "align": "left", "maxLines": 1, "overflow": "ellipsis" },
        { "id": "title-text", "type": "text", "text": { "field": "title" },
          "width": 1002, "height": 42, "font": { "token": "brand.fontBody" },
          "fontWeight": 400, "fontSize": 28, "lineHeight": 1.2,
          "color": { "token": "brand.onPrimary" }, "align": "left", "maxLines": 1,
          "overflow": "ellipsis", "collapseWhenEmpty": true }
      ] }
  ],
  "animations": {
    "in": { "type": "slide", "direction": "left", "durationMs": 400, "easing": "ease-out" },
    "out": { "type": "fade", "durationMs": 300, "easing": "linear" }
  }
}
```

Flow collapse removes the empty node and adjacent flow gap. The example plate
keeps its explicit height; templates that shrink the plate additionally declare
group/root `autoHeight:true` with min/max height. Auto size is resolved off-thread
within explicit bounds; it cannot move other graphic channels unpredictably.

### 4.3 Brand vocabulary and library

V1 brand tokens: primary, secondary, accent, background, foreground, onPrimary,
onPrimaryMuted, fontHeading, fontBody and logo. Existing color/font/logo settings
map explicitly to these, with migration defaults stored in the converted brand
document. Derived contrast tokens use a documented light/dark choice shown in
preview; they do not reinterpret an existing saved color without opt-in.
Per-template and per-instance overrides remain visible and resettable.

First-milestone library: the three preserved lower thirds, logo-accent lower
third, transparent-name lower third, centered title, corner logo bug and
clock/countdown panel. Clock and countdown are modes of one family. Each ships
short/long/blank/Unicode examples and light/dark brand previews. Content quality
is reviewed in actual rendered frames, not accepted by counting template files.

## 5. Native ownership and preparation

Add focused modules behind a graphics capability flag: template/package registry,
instance state/arbitration, binding evaluator, preparation owner, animation runtime,
render-plan adapter and controller supervisor. `MediaCore.cpp` delegates typed
commands/snapshot assembly to these; it is not the implementation container.
Shell graphics coordinators stay separate from `StudioViewModel.cs`.

The command boundary validates bounded input and admits intent. An owner applies
logical revisions, schedules preparation and publishes immutable snapshots.
Data/controller callbacks submit typed observations only. A bounded preparation
worker resolves tokens, shapes text, decodes images and builds immutable assets.
D3D11 immediate contexts and Metal encoders have explicit owners; workers cannot
borrow the render thread's immediate context. Publish ready GPU resources only
through a backend-owned fence/lease path; final render adoption never waits.
If a separate GPU upload owner is unavailable, explicitly budget a bounded upload
handoff and prove its deadline cost before enabling it; do not call the old
synchronous raster path from the new cache-miss branch.

Cache keys include template/content/style/brand revisions, asset hashes, scale,
font resolution, device generation and binding identity. They exclude frame
number, wall-clock tick and animation phase. Deduplicate identical prepared
resources across buses. Replaced work is canceled/stale-fenced; disposal retires
leases after GPU completion, including device loss and app teardown.

Authoring uses a bounded native preview tap of the draft revision, isolated from
Program accepted state, through the existing shared-texture/monitor presentation
path. Coalesce draft preparation requests to at most 10 Hz while dragging and
commit the final exact value when the gesture ends; presentation can remain at
the existing source-monitor cadence. Hidden editors release interest and stop
submissions. Draft work is lower priority than accepted Program preparation;
its cache/resources count against the same byte budget. Do not add synchronous
D3D Present or texture-readback work to XAML to make the designer look live.

The compositor samples a complete accepted revision and evaluates transform,
opacity, clip and crawl offsets against the existing presentation timeline.
Animation advances by elapsed timeline time, never by counting render calls.
Clock text is regenerated only when displayed digits change. Default clock
resolution is seconds; finer display updates are capped at 10 Hz and admitted
through the same preparation budget. Wall clocks use wall time for content and
monotonic presentation time for animation. Time jumps do not replay reveal.

## 6. Lifecycle and protocol

### 6.1 Two independent state machines

Preparation: `unprepared -> preparing -> ready | failed`; replacement preparation
can run while a previous revision remains usable. Visibility:
`hidden -> entering -> on-air -> exiting -> hidden`. Failure/health is separate
from visibility; an old valid on-air revision can remain while a new one fails.

Show while entering/on-air is idempotent. Hide while exiting/hidden is idempotent.
Show during exiting reverses continuously from its current evaluated transform;
Hide during entering does likewise. Clear invalidates pending reveals immediately.
Update while exiting prepares the next revision but cannot reveal it. Pause/hold
freeze countdown/data progression only when explicitly selected; they do not
freeze GPU delivery. Hold cancels auto-hide; an ordinary same-value data refresh
never extends an auto-hide timer. Duration is measured from the first native
on-air composition event. On device loss, rebuild current accepted resources and
resume logical visibility without another reveal; report the interruption.

### 6.2 Wire commands

Use the existing JSON-line envelope and additive generated lifecycle/identity
contract slice. Symbolic action names below map to explicit native command names
and registered HTTP/OSC actions. V1 native names replace each dot with a hyphen:
`graphics.instance.update` -> `graphics-instance-update`. Existing dispatchers do
not already support these names; adapters and generated validation are required.

| Action | Required payload / behavior |
|---|---|
| `graphics.template.install` | Package identity/hash and validated local staged reference; asynchronous install, no Program effect |
| `graphics.template.list` | Bounded/paged catalog with versions, compatibility and warnings |
| `graphics.instance.create` | instanceId, template/version, scope/scene, initial fields/style/bindings; initially hidden |
| `graphics.instance.update` | expectedRevision, complete field/style/binding patch, target cue or explicit Program update; atomic prepare/adopt |
| `graphics.instance.cue` | instance/revision and Preview target; readiness reported separately |
| `graphics.instance.trigger` | instance/revision, `show|hide|clear`; optional expected Program scene/subject generation |
| `graphics.instance.order` | expectedRevision and bounded order/channel; no animation reset |
| `graphics.instance.remove` | Hidden instance only, or explicit hide-then-remove transaction |
| `graphics.program.clear` | Explicit graphics channels; increments manual inhibition generation |
| `graphics.clock.control` | start/pause/resume/reset plus validated duration; no implicit reveal |
| `graphics.controller.configure` | Kind, mapping, freshness/error policy, credential reference; starts only by explicit enable |
| `graphics.automation.configure` | Mode, complete rules/preset, allowed scopes, manual hold and expected revision |
| `graphics.playlist.control` | Later gate: play/pause/next/stop and explicit Program effect |

For the existing HTTP/OSC registry's scalar parameter contract, each mutation
has one required string parameter `request`: a JSON-encoded, action-specific
graphics request document. `/invoke` still uses `{action,args:[requestJson]}`;
OSC uses the same JSON string and canonical dotted-action/address mapping.
The shell parses and validates it before dispatch, then sends an object payload
to the native command, not an unvalidated string or arbitrary property setter.
The native command also validates it; shell validation is not a trust boundary.
The UI uses the same typed request builder. No new unauthenticated endpoint.

Native example (the payload field values are illustrative):

```json
{
  "type": "graphics-instance-update",
  "graphics": {
    "schemaVersion": 1,
    "requestId": "update-42",
    "clientId": "operator-console",
    "coreEpoch": "core-session-example",
    "instanceId": "featured-lower-third",
    "expectedRevision": 7,
    "target": "program",
    "fields": { "title": "Production engineer" }
  }
}
```

An omitted patch field keeps its accepted value; explicit null removes an
override only if that field is optional and nullable by schema. Empty string is
an intentional empty string, not reset. Complete validation/resolution produces
one full candidate revision before any mutation. IDs/epochs use the existing
generated identity vocabulary; the example's strings do not define a new epoch
format. Queries/list requests expose results through a bounded catalog page or
operation record on the published graphics snapshot.

New graphics revision/instance DTOs live in the graphics schema and reference
existing `SourceInstanceIdentity`/binding types where appropriate. Do not insert
new values into an older client's closed `EntityIdentity.kind` enum or silently
coerce an unknown kind. Core accepts source observations only after validating
their epoch/generation and normalizes timestamps to its presentation timeline;
cross-process wall-clock values are not directly usable animation timestamps.

Mutations carry requestId, clientId, schemaVersion, coreEpoch, expectedRevision
and bounded target identity. Per-instance revisions are JSON-safe integers.
Deduplicate the last 256 request results per core epoch for five minutes; a
replayed request returns its prior outcome, never repeats a reveal. A new request
with an old revision returns `revision-conflict`. Timeout means unknown outcome;
query native state before retrying under a new ID. No automatic replay of pending
Show/Take after reconnect or restart. Concurrent editors cannot overwrite each
other; show the newer accepted revision and offer rebase/discard.

Acknowledgement distinguishes rejected, accepted/preparing and completed/adopted.
The existing HTTP `ok:true` means request accepted, not prepared or composited.
Callers correlate their requestId with `graphics.operations` and the instance
revision on `GET /snapshot`; the operation carries existing generated lifecycle
status, error and completion identity. Retain at most 256 result records within
the deduplication window. A disappeared result is unknown, never inferred success.
OSC feedback and shell buttons use those same facts rather than relabeling an
acknowledgement. No change to unrelated control actions' success semantics.
Structured errors: invalid-template, unsupported-version, invalid-field,
asset-unavailable, font-fallback-required, budget-exceeded, revision-conflict,
stale-source, not-ready, scope-conflict, automation-inhibited and device-unavailable.
Errors identify a safe field/path, never credentials or raw remote content.

Snapshots include core/device epoch, catalog revision/capabilities, per-instance
accepted/preparing/ready/presented revisions, visibility/health, actual bus/frame
sequence, subject generation, active override, effective fields with provenance,
safe asset/font warnings, animation progress and auto-hide remainder. A bounded
trace records input event -> rule decision -> command outcome -> composition.
Controller health includes last attempt/success/source version, data age, pending
request, backoff and stale policy; snapshots redact secret-bearing field values
and endpoint credentials. UI refresh reads published snapshots without blocking
the owner or render threads.

## 7. Automation and bindings

Retain the existing deterministic director and prove #761's complete observation
and execution path before treating it as an event source. Derivation may remain
in the optional show engine, but the graphics owner arbitrates one final intent
per instance. No duplicate TS/native automatic Show or independent animation clock.

Modes: **Manual** (no automatic action), **Suggest** (explanation and Cue button),
**Rehearsal** (native Preview only with would-act log), **Automatic** (explicitly
armed, scoped Program actions). Default is Manual; imported/saved shows do not
re-arm after restart. Automation configuration saves separately from activation.

First rules: show a prepared lower third after the featured subject is stable for
1.5 seconds; hide after eight seconds or subject departure; suppress/reposition
to a reviewed safe region during screen share; optionally retain a global bug;
clock/countdown follows its explicit start event. These are editable defaults,
not universal behavior. No scene Take is enabled by choosing graphics automation.
Scene auto-take remains its separately armed existing feature.

Default subject rules fire once per featured-subject generation. Auto-hide does
not make the unchanged subject eligible again on the next tick. Repetition, if
later exposed, is an explicit separately bounded rule option and defaults off.

Priority: emergency Clear -> manual instance/channel hold -> explicit operator
Show/Hide/Update -> armed rule -> suggestion/default. Manual Hide suppresses that
rule for the current subject/event generation; it cannot immediately re-show on
the next snapshot. Manual hold persists for the session until Resume automation;
duration holds are explicit. Disarming cancels pending automatic actions, leaves
current graphics as shown, and offers an explicit Hide automatic graphics action.

Rules contain ID/version/enabled, event/source/scene filters, conditions, dwell,
cooldown, action, scope and fallback. Evaluate canonical data changes, not every
UI refresh. Evidence gives rule/subject/instance IDs, why fired or blocked, and
the accepted/native result. Stale recommendations, missing bindings, unsupported
commands or unready assets cause a named refusal, not `Automation enabled` success.

Bindings preserve canonical source generation and resolved value provenance.
Zoom identity updates are delivered from the existing native ingest/roster path,
not shell-render snapshot round trips. Unchanged values cause no revision.
Data updates for the same subject prepare/swap without replaying the in animation.
Empty values respect required/default/intentional-empty semantics. Removing a
binding never resurrects old data; it resolves its explicit fallback or requires
an operator value. Controller error policy is per cue, visible and overridable.

## 8. Clocks and later data controllers

Countdown values include duration, remaining-at-anchor, monotonic start anchor,
paused/finished state and finish behavior (`hold-zero|hide|suggest-next`). Default
is hold-zero; no implicit Take or audio playback. A restored countdown is paused
with a restore indicator. Wall-clock format/timezone are explicit; locale and
daylight-saving transitions are test cases.

Controllers execute outside command/render owners with timeout, cancellation,
bounded response sizes and one in-flight request per controller. Initial generic
JSON polling default is two seconds, minimum one second, five-second timeout,
1 MiB response and exponential 2–60 second failure backoff with jitter. A normal
poll does not overlap an outstanding request. First milestone does not promise
sub-500 ms remote polling. After a successful fetch/local change notification,
the target is an adopted same-subject update within 500 ms under the declared
workload; fetch interval and network latency are reported separately.

Local CSV/XLSX is an explicit operator-selected file. Read a stable snapshot after
write settlement; never hold its writer lock. XLSX is values-only: no macros,
external-link execution or formula evaluation; use cached values and warn if
absent. JSON mappings use bounded property paths, not code. RSS/XML disables
external entities and DTD expansion. Row bindings require stable configured keys;
no key means a named error, not animated reassignment by row index.

Default staleAfter is max(three polling intervals, ten seconds). Distinguish
pending, current, stale, failed and disabled. Generic same-subject stale data
holds the last valid value with an operator warning. Subject replacement or
unavailable identity hides its name graphic rather than retaining someone else's
label. Sensitive/event-critical templates can opt to hide on staleness. Identical
responses do not reset timers, animate or reraster.

Tables support at most 20 visible rows initially, keyed insert/remove/move
transitions and bounded hidden rows. Crawls use cached text segments translated
on the GPU, never reraster per frame. Playlist advances only after native
visibility/timing facts, supports manual pause/next and cannot silently Take video.

## 9. Resource, safety and performance contract

Initial admission defaults, all subject to qualification before being advertised:

| Resource | Bound / behavior |
|---|---|
| Visible instances | Eight per Program or Preview bus; at most sixteen distinct resident revisions including authoring preview; shared revisions count once |
| Nodes | 64 per instance, 256 per bus; depth at most eight; no recursive/cyclic templates |
| GPU graphics residency | 256 MiB combined prepared/cache/pending/retiring resources; referenced resources cannot be evicted; refuse new preparation when not safely admissible |
| CPU decoded/prepared assets | 128 MiB combined, two preparation workers initially; per-instance latest-only replaceable update, bounded 32 command intents |
| Cache | Byte-bounded LRU for unreferenced resources, content-addressed reuse; active assets never disappear because a count limit is crossed |
| Packages | 32 MiB archive, 128 MiB expanded, 128 files; no path escape, absolute paths, symlinks, nested archives or executable assets |
| Images | PNG/JPEG first; max 4096 per dimension and 16 megapixels after decode; dimension/header checks before allocation; animated formats need a separate qualified adapter |
| Text | 256 name/512 secondary/4096 generic characters by default; shape within declared bounds, reject control characters except supported line breaks |
| Controller inventory | Eight enabled controllers, at most 1000 source rows per table controller; bounded field and payload parsing |
| Rule history | Last 256 decisions with dropped-history count; no synchronous per-frame logging |

Admission is atomic across a multi-instance update. Exceeding a bound leaves the
previous valid revision active and reports a refusal. Never lower video quality,
drop unrelated graphics, force a global GPU sync, or allocate indefinitely to
make the request succeed. A whole-frame 4K graphic can exhaust these limits;
layer count alone is not a capability promise. Account all intermediate copies,
retiring device resources and both buses in the reported budget.

At 1080p60, warm eight-instance composition targets <=1 ms incremental p99 GPU
cost on each named qualification machine. This is a target for engineering, not
a waiver if an individual delivery deadline is missed. Cold template/font/image
changes must occur during active capture/outputs with zero new scheduled output
misses/underruns/sequence gaps attributable to graphics. Measure baseline and
graphics load separately, including preparation latency, cache hits/misses,
queue supersession/refusal, GPU fences, composition cost and each output reader.

Qualify 4K at the same quality and explicit workload if advertised; otherwise
publish the exact unsupported capability instead of falling back to low-resolution
graphics or claiming Windows evidence proves Metal/fleet performance. Transparent
edges, premultiplication, color space/SDR handling and render-resolution changes
are pixel tests. HDR graphics/color-management support is a separate capability;
unsupported transfer/color spaces produce a warning/refusal, not guessed colors.

Package paths are rooted at a private staging directory with canonical path,
decompressed-size and hash checks. A hash is integrity, not proof of a trusted
publisher. Unsigned local packages are labeled and require explicit import;
imports cannot enable controllers or automation. Asset fonts require valid
licenses; first release references installed/bundled licensed fonts rather than
executing arbitrary font installation. Remote endpoints are explicit settings,
allowlisted HTTP(S), constrained redirects and credential references. Local/LAN
data URLs require explicit authorization; reject automatic redirects to loopback,
link-local or cloud metadata targets. Store secrets with the existing protected
store, redact endpoint/field values from snapshots, traces and support bundles.

## 10. Persistence and compatibility

Store versioned template packages/assets, brand/style definitions, show-scoped
instance/cue documents and controller/rule configuration separately from transient
animation state. Use existing atomic JSON/backup and protected-secret mechanisms.
Core runtime applies accepted revisions; shell/store service persists explicit
edits without file I/O on a render deadline. Restoring a show starts with graphics
hidden and automation unarmed; present a cue-ready restore summary. No automatic
stream, meeting, countdown, controller network request or replayed Show command.

Migration snapshots the old document and preserves unknown fields. Existing
three presets and custom appearances map to compatibility templates with the
same text/default/blank/anchor/logo/timing semantics. Legacy data remains available
until converted definitions and native frames pass acceptance. Do not overwrite
source overrides or unrelated recording, destination, grading or audio settings.
Missing assets/fonts retain saved references and report remediation; never erase
them while loading. An older app opening an incompatible graphics schema refuses
to rewrite it; rollback restores the backed-up graphics documents and compatible
package versions without changing other operator preferences.

A per-show adapter selects exactly one authority: legacy overlays or graphics
instances for a migrated family. The adapter translates legacy API/hotkey/TS
requests to the same instance IDs and priority rules. No dual rendering or double
animation. Keep old paths until decoded-output comparisons, control parity and
restore tests pass. Removal is a separately reviewed migration step.

## 11. Acceptance and review

Owner review must follow a complete workflow: select template -> bind subject ->
edit draft -> cue -> show -> update -> hide -> save/restore -> enable rehearsal ->
manually override automatic action. Show actual short/long/blank/Unicode renders,
not only inspector screenshots. Preserve a feature inventory of every previously
accepted control; removal or changed behavior needs an explicit review decision.

Mandatory evidence includes:

- Schema/identity/revision fixtures across native and supported shells; malformed,
  oversized, stale, duplicate and out-of-order input cases.
- Actual template/style effects for every enabled field, blank collapse, fonts,
  logos, alpha, safe areas, ordering, DPI and 1080p/4K geometry.
- Frame sequences proving no fragmented same-subject update, no replay on identical
  metadata, no wrong-subject graphic on Take and atomic multi-instance Apply.
- Cue/Program scope independence, global continuity through Takes, Preview/draft
  isolation and real visible output from hotkeys/API/automation.
- Native Program/Preview, OS webcam capture, finalized decoded recording and local
  decoded stream comparisons at matching composition identities. Source ISOs and
  webcam-only framer retain their separate intended pixels.
- Cold/steady/error workloads with the full render/output counters and independent
  receiver evidence. Report average FPS only as context, never zero-miss proof.
- Save/load/upgrade/downgrade, guest replacement, source reconnect, device loss,
  controller failure, manual Clear inhibition and clean shutdown.

First usable result targets: cold app -> first manual lower third on air within
30 seconds for a configured show; cue readiness p95 <=500 ms warm and <=2 seconds
cold local assets on named hardware. Separate startup/network time and retain
failures. Run the declared full-output configuration for at least 60 minutes and
a two-hour mixed-workload bake before sustained production acceptance. Fleet and
Metal claims require their own measured hardware trials. No test or document
alone closes #847/#592/#593/#761/#764 or the media-delivery issues.
