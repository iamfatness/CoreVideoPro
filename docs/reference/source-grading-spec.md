# Per-source grading workspace

Design contract for [#835](https://github.com/iamfatness/CoreVideoPro/issues/835).
Owner's visual reference: a large source image, three scopes underneath, and
grading controls at the right. Work order lives only in `docs/BACKLOG.md`.

## Operator contract

Basic retains the existing quick controls. Expand Grade opens a resizable Advanced
workspace for the same source and grade. Collapsing preserves advanced adjustments
and displays an Advanced adjustments active indicator. Allow maximizing the window.

Advanced shows the source name, live/draft state, bypass, intensity, original/graded
comparison, and reset. Comparison changes only the monitor. A frozen image cannot
be described as live. Show the observation age and source-unavailable state.

The three simultaneously visible scopes are histogram, waveform and vectorscope.
Each can expand. Histogram offers luma and RGB overlay; waveform offers luma, RGB
overlay and RGB parade. Vectorscope shows reference targets and a skin-tone line,
which is a reference rather than a claim that every skin tone must lie on it.
Original/Graded explicitly selects the analysis tap. Units and color-space assumptions
stay visible. Empty, unavailable and stale are distinct from a measured black frame.

Master/R/G/B curves support adding points, dragging, numerical entry and removal.
Keep endpoints ordered, allow vertical movement, reject duplicate input positions,
and provide channel reset and undo/redo. Primary controls extend to exposure,
temperature/tint, saturation, contrast/pivot and lift/gamma/gain or offset. Their
units must match the native implementation; temperature is not labeled Kelvin
unless a defined white-point transform implements that meaning.

Use an ordered adjustment stack with enable, intensity and reorder. Existing named
looks are built-in adjustments, not imported LUT files. Real `.cube` import validates
dimensions, domain, finite values, interpolation and size before admitting a GPU
resource. Retain the file content or a durable managed copy with its hash; an external
path alone cannot preserve a show. Presets, duplicate and copy/paste reuse the same
grade document. CDL/CLF and export are later compatibility extensions.

Keep working, applied and persisted grades separate. Existing Basic live editing
must be clearly labeled. Advanced provides explicit Apply Live and optional live
editing with a visible indicator. Draft comparison and bypass never change Program.
Cancel discards a draft; it must not overwrite a newer applied revision from another
editor. A failed apply leaves the previous on-air grade and reports its reason.

## Authoritative grade and compatibility

Introduce a versioned immutable native grade document with a source binding,
revision, ordered operations, bypass and intensity. Transport, persistence and
native parsing preserve every supported operation. Reject unsupported versions or
operations explicitly instead of producing an apparently successful partial grade.

Version 1 preserves the established GPU four-axis transform and scale. Existing
documents without a version migrate to that behavior, including neutral defaults.
Fix the discarded named-look selection from #764 as an integrated prerequisite;
the existing CPU editor preview is not a reference implementation for migration.
Version 2 defines the new primary and curve units. Moving an existing grade to the
new model requires equivalence evidence, not merely copying the slider numbers.

For SDR, declare normalized Rec.709 input, its transfer function and source range.
Native source normalization precedes the source grade. Define each operation's
working domain: exposure in stops needs a defined linear-light conversion;
display-referred curves operate in the declared encoded domain. Specify operation
order and clipping boundaries. Unknown transfer metadata and unsupported HDR are
explicit qualification limits; do not silently interpret them as SDR.

Source grade is applied once before composition. Route framing and scene overlays
do not enter selected-source scopes. Scene/Program grading, if present, is a separate
stage with explicit order. Verify Program, Preview, multiview, virtual camera,
streams and Program recording through their actual consumers. Preserve existing
raw ISO policy; do not introduce graded ISO by implication.

Curve evaluation and full-frame grading remain on the GPU. Compile validated
control points into a cached curve texture, with a documented interpolation rule
and precision checked on ramps. Cache by grade revision/content rather than frame
number. Held frames must update when the grade changes. Device loss recreates
resources from the immutable document; it never substitutes an unrelated look.
Share parameter translation and transform semantics between D3D11 and Metal.

## Preview and scope isolation

Remove independent per-pixel grading from `ColorGradeEditorViewModel`. The shell
displays native preview surfaces and compact scope observations. A private draft
uses the same native transform with a draft revision; it does not mutate live routes.
Extract editor coordination from `StudioViewModel.cs` into focused types.

The selected-source monitor registers explicit demand with source identity,
generation, grade revision and Original/Graded tap. Start with one selected source
per workspace and enforce a bounded aggregate demand budget. Close releases demand;
hidden scopes stop analysis. Reopening resumes with fresh evidence.

An independent monitor/analysis worker owns its D3D device/context and optional
monitor source leases. Never retain a production capture-pool lease behind pending
analysis. Use a capacity-one latest-request mailbox; replacing a request drops only
optional monitor work. Do not wait for analysis on Program or add pixel work under
the Zoom publication mutex. Worker failure reports unavailable and leaves live output
running. Bound every allocation, retained source and result buffer.

Analyze actual native original/graded pixels with asynchronous GPU reduction.
Read compact results only after completion; no synchronous full-frame GPU readback
on the render thread. An initial analysis target of 10 Hz is independent of production
video cadence. Sampling, if used, is disclosed in the scope observation and must
not reduce source or production resolution/quality.

Each observation carries source identity and epoch, source frame identity/time,
grade revision, tap, sample dimensions/count, color space, units, completion time,
status and failure reason. UI rejects a result from an older selection or revision.
Arrival time alone does not prove fresh source motion. Age derives from source
advancement; show stale when the selected source stops advancing. Unknown metadata
or a missing CPU fallback must not yield a fabricated scope.

## Qualification

Use neutral and colored ramps, gray/RGB patches, clipping, mixed luminance and
skin-tone reference signals. Check identity, channel isolation, curve interpolation,
primary units, operation order, alpha preservation and clipping counts against
independent expected values. Compile and exercise both shader variants. Pixel
parity tests cover BGRA and I420, full/limited range and supported matrices.

Exercise held-frame edits, source loss, reconnect/identity replacement, device loss,
draft/apply/cancel, monitor-only comparison, bypass, reset, undo/redo, persistence,
Basic/Advanced switching and concurrent edit revision conflicts. A Zoom identifier
reused by a different guest must not inherit the previous guest's grade.

For real Zoom inputs, verify the native preview and all applicable live consumers
and decode finalized recordings. Run matched baseline and candidate workloads with
Program/Preview, multiview, virtual camera and recording enabled, retaining hardware,
duration, source count, render readiness and each output's delivery evidence.
Any additional missed output deadline fails performance acceptance; average FPS or
an advancing UI counter cannot waive it. Verify bounded memory/resources during
rapid edits and open/close cycles. Missing receiver or live-meeting evidence remains
unverified. A specification or synthetic pass does not close #835.

## Native Basic preview implementation

The Basic editor sends an instance/source/revision demand through the small
`set-grade-preview` RPC acknowledgement. It does not request a production sync
snapshot. Edits coalesce on a 100 ms control timer; a 500 ms heartbeat renews a
three-second native lease. Up to three editors share one replaceable pending job.
Closing or shell shutdown releases demand; a lost close expires the lease.

D3D11 allocates a separate monitor device lazily on its worker, on Program's
adapter. Only selected immutable source references enter that worker. GPU inputs
must use monitor-private representations; production capture leases are stripped.
The existing native source-export shader applies the grade at source resolution,
without scene framing, borders, overlays, or CPU pixel processing. Each editor
has its own shared texture. Removing the final demand destroys the backend on its
owner. Unsupported builds acknowledge the command and report unavailable.

Observations carry editor, source, draft revision, source epoch/frame and capture
time. Export publication must match retained attribution before emission. The
shell rejects another editor/source or an older revision. New edits clear the old
picture while preparing. Unavailable inputs hold their actual prior source identity;
a source frame that does not advance for one second is labeled stale. Snapshot
`gradePreview` reports worker submissions/completions/failures and retained inputs,
with physical display presentation explicitly unverified.

Basic defaults to clearly labeled live editing, preserving its existing apply
behavior. Turning it off keeps edits private until Apply and Done; comparison
uses a neutral monitor demand and never changes the applied grade. Closing a draft
discards it. Closing does not undo edits already applied live. The workspace can
maximize. The expanded implementation below adds the versioned document, curves, scopes,
primaries, ordered stack, LUT import, presets and revisioned source apply.


## Expanded workspace implementation

Basic retains its established sliders and built-in looks. Expand opens a resizable
workspace and switches to private draft editing. Collapse preserves the advanced
document. Apply Live or Apply and Done submits one immutable grade; Close discards
unapplied draft edits. Compare Original changes only the selected-source monitor.
Analyze Original independently changes the scope tap. Enable Apply edits live only
when edits should reach the show automatically. Close does not undo accepted live
edits. Undo/redo retains the full Basic and Advanced grade, including a preset load
as one action, with a bounded 100-entry history.

The version-2 document declares `rec709-sdr`, global bypass/intensity and at most
eight uniquely identified operations. Operations have independent enable/intensity
and explicit list order. Primaries expose exposure in stops, encoded contrast/pivot,
saturation, normalized warm/cool and green/magenta balance, lift, gamma and gain.
Curves have Master/R/G/B channels and 2–16 strictly ordered float32 control points;
input endpoints stay at zero and one. Pointer editing, numeric input/output, Delete,
channel reset and undo use the same document. Native shaders evaluate exact
piecewise-linear segments from a 16×32 RG32_FLOAT point texture; curves are never
resampled into an 8-bit curve LUT.

The existing Basic transform and named look run first. For each enabled operation,
primaries decode the Rec.709 transfer, multiply linear light by `2^exposureStops`,
then encode. Contrast about the encoded pivot and saturation about Rec.709 luma
follow. Balance adds `(0.1T−0.05t, 0.1t, −0.1T−0.05t)` in encoded RGB. Lift/gain/gamma
apply `max((v+lift)*gain,0)^(1/gamma)`. Curves apply Master then the respective RGB
channel. Cube input is encoded RGB mapped from the declared domain into its
clamped lattice, with red varying fastest and trilinear interpolation. Each operation
clips its result to SDR [0,1], then mixes with its input by operation intensity.
Overall intensity mixes the original pre-Basic RGB with the completed grade.
Bypass bypasses the entire grade. These controls are SDR balance controls, not a
camera-specific Kelvin calibration or an HDR color-management system.

Import accepts finite 3D `.cube` lattices of size 2–33, explicit or default domain,
and at most 2 MB per file. A grade embeds at most 1.5 MB of LUT text; invalid,
incomplete, 1D, nonfinite, duplicate-header and hash-mismatched LUTs are rejected
before editing. Original text and SHA256 travel together, so a preset does not
rely on a workstation-specific path. `.cvgrade` presets and clipboard copy/paste
contain the entire Basic/Advanced grade. Documents register once by content hash;
subsequent route sync, preview and apply commands carry a small document reference.
Native registration is bounded; an expired reference is rejected rather than
substituting a look. The shell checks presence before use.

`set-source-grade` checks observed source epoch and expected applied revision.
Invalid documents, missing/replaced sources, revision conflicts and capacity
failures retain the previous authoritative grade. The shell saves accepted state
only after native acknowledgement and keeps rejected edits as a draft. The ack
means control-state acceptance; it is not evidence of a completed GPU frame or
receiver presentation. Source grades are applied once before composition, including
Tiles and participant exports outside Program. Raw ISO policy is unchanged.

Saved source grades use stable capture IDs or Zoom persistent participant IDs.
They never bind by an ephemeral Zoom ID alone. Restored grades open as drafts and
require explicit Apply Live to confirm the current source binding. Up to 64 source
grades are saved; saving a new source at capacity retires the oldest with an
operator status notice. Accepted changes save after a 750 ms debounce and flush
on orderly shutdown.

## GPU scope observations

The Windows worker grades a private 256×144 sample, then computes GPU bins for
histogram (256 RGB/luma bins), waveform (64×64 per RGB/luma channel) and vectorscope
(64×64 Rec.709 Cb/Cr). A second GPU reduction finds histogram peaks once per sample;
the display shader uses a shared RGB peak so channel distributions remain comparable.
Waveform/vectorscope show logarithmic sample density, not clipping counters.
Histogram supports luma/RGB; waveform supports luma, RGB overlay and parade.
Vectorscope includes 75% encoded RGB targets and a skin reference direction, which
is a reference rather than a skin classifier. Each scope can expand independently;
the vectorscope preserves its square geometry in the expanded texture.

The native worker exports one completed 768×256 GPU texture for the scopes, with no
CPU source/scope readback. The shell receives source epoch/frame/capture timestamp,
draft revision, original/graded tap, selected view, sample dimensions/count, assumed
Rec.709 SDR units and status. `completionObservedAtUnixMs` is the wall-clock time the
controller first observed the actual completed export token; it is not a fabricated
GPU timestamp. Source age measures time since observed source advancement, not
transport latency. Preview and scope staleness are judged independently. Hidden
scopes stop GPU analysis; closing/lease expiry retires worker-owned resources.
Initialization/render/export failures report unavailable. Physical display and
external receiver presentation remain separate qualification evidence.


The D3D document texture cache retains at most 64 documents by last use; immutable
cube textures reuse their validated content hash. Resource preparation currently
occurs on the owning GPU context. A native apply acknowledgement does not promise
that resource allocation or the first graded GPU frame has completed. Device-loss
and allocation-failure rollback require separate qualification before release.
Metal uses the same source-grade operations for composition; the private grading
workspace/scopes export backend is currently supported on Windows D3D11 only.
