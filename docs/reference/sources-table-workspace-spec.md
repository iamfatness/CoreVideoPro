# Sources table and inspector

Implementation contract for #590, following the owner-approved table-first
review at https://corevideo.io/reviews/sources-table/. This supersedes the
inspector-first proposal; work order remains in BACKLOG.md.

## Interaction

Sources retains ten stable numbered inputs as its default overview. Selection,
search, category filters, column visibility, resizing and opening/closing the
inspector never assign a source, change MV/ISO selection, modify a scene or Take.
Each source row retains its own identity-specific preview, name/category,
observed resolution/FPS, signal state, audio meter, MV checkbox, ISO selection
and separate observed writer state. Secondary columns have saved UI-only
preferences; narrow layouts expose their information in row Details. The table
and inspector must fit the available width without a horizontal page scroller.

An optional inspector opens for the selected input. On wide windows it sits
beside the overview; at smaller widths it stacks below. The selected slot is
stable across filtering and roster/format updates. A filter may hide the selected
row without selecting a different source. All-hidden results have explicit
guidance. Unassigned inputs remain assignable and retain their numbers.

## Existing consumers, not web behavior

Each row exposes a source picker for immediate patching without opening the inspector.
Assignment uses the existing categorized unified picker and canonical source ID.
Opening a menu never writes; its source identity is captured until the explicit
choice. Roster refresh does not rebuild an open menu. Stale choices cannot patch
a replacement source or an obsolete row.
MV and ISO delegate to ShowInputSlotViewModel.InShow/IsoEnabled without inventing
a new transport or eligibility policy. Writer state remains the observed
MultiviewInputRow lifecycle value, not inferred from arming. Source format comes
from the existing observed projection; requested Zoom quality settings never
become delivered measurements. Stale frame rates are not presented as live FPS.

The inspector reuses display-name persistence, audio pairing, source policy and
lower-third flyouts. Open editors retain their existing source identity guards;
selection and metadata refresh do not hydrate a write back into another source.
The existing Grade command and SourceColorGradeId open the actual native-backed
grade workspace from both the overview and inspector. No simplified web grade
implementation, CSS effect, synthetic scope, new ROI or alternate grade store
enters the desktop app. Existing curves, stack, LUTs, presets, live-edit choice,
drawn rectangle/circle scopes-only ROI and Apply/Cancel remain intact.

## Preservation map

| Capability | Destination / consumer |
|---|---|
| Ten stable inputs, last-known offline identity | Same slot/roster models, table and inspector |
| Source type/category, observed resolution/FPS and freshness | Table; secondary details in narrow rows |
| Per-source preview and audio meter | Existing source-specific surface and meter projection; never wall cropping |
| MV inclusion and ISO eligibility/selection | Direct existing slot properties in row and inspector |
| ISO arm versus recording lifecycle | Checkbox plus observed RecChipLabel |
| Unified categorized assignment and unassign | Inline row picker plus inspector; existing canonical picker/lifecycle |
| Display name and source-bound lower-third text | Inspector; existing name store and guarded flyout |
| Guest role and dropout policy | Inspector; existing guarded SourcePolicyFlyout |
| Paired microphone | Inspector; explicit source-bound audio selection |
| Native grade, curves, scopes and ROI | Existing grade command/editor, not a replacement |
| Zoom resolution/FPS request ceilings | Discoverable quality section, same preference properties/status |
| Capture connect/offline/sync, browser creation | Preserved Capture devices catalog section |
| SRT/RTMP creation/configuration/removal/runtime state | Preserved adapter catalog sections |
| Scene layers/Tiles/framing/Take, routing, DSP and webcam overlays | Existing workspaces unchanged |

## Lifetime and validation

Rows are retained by slot object and are not rebuilt on meter/format snapshots.
Filter changes only visibility. Resize changes grid geometry, not collection
identity. Release subscriptions when unloaded and reconcile when reloaded;
do not add a poller, native frame consumer or long-held media lock.

Validate pure selection/filter/column behavior and settings round-trip. A compiled
offscreen consumer must exercise a mixed Zoom/capture/media/network roster at
normal and narrow widths, explicit MV/ISO edits, offline/reconnect/reassignment,
selection/column stability, keyboard/focus and real grade command identity. Use
in-memory preferences and no interactive startup. Package the resulting shell
as an uninstalled candidate. RenderTargetBitmap cannot prove GPU video/scope
pixels; installed high-DPI interaction and a real meeting/output bake remain
separate acceptance. Do not close the issue based on a spec or mockup.
