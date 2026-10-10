# Operator editing bundle specification

This bundle covers lower-third text/stability (#592/#593), appearance (#847),
shared control usability (#848), grading and scope ROI (#835), and starter scenes
(#591). Supporting settings/navigation changes (#590/#594/#595) enter the same
review candidate after their acceptance. Order remains in `docs/BACKLOG.md`;
delivery and validation are described in [the implementation plan](operator-editing-bundle-plan.md).

## Operator experience

Use a consistent inspector pattern: clear section title, visible current value,
slider for continuous adjustment, synchronized exact entry, unit, neutral mark,
reset and useful feedback. Direct canvas manipulation, curves and color pickers
remain available where they help. Native Fluent control behavior, coherent spacing
and readable hierarchy take precedence over custom decoration.

Common controls stay visible; advanced options expand in place. A user must be
able to identify the selected source, whether edits are live or draft, which
changes are active, and what Apply/Cancel will do. Preserve existing live audio
and grading policies. Scrolling a page must not alter an unfocused value.

The detailed value, keyboard, validation, precision, conversion and coalescing
contract is [operator-adjustment-controls-spec.md](operator-adjustment-controls-spec.md).
Reuse a focused control through real consumers. Do not create a separate UI
framework or migrate every numeric field blindly.

## Lower thirds

Sources owns primary name and secondary text; shared appearance lives in
Overlays. Blank secondary text suppresses the default and collapses its line.
Edits follow source identity, not slot number; existing identity/reconnect limits
remain explicit. Known role fallback casing is consistent across roster paths.

Provide compact solid, minimal accent, and two-line broadcast presets. The
designer supports typography and independent line sizes, text/background/accent
colors, background opacity, padding, bounded width, corner radius, safe-margin
anchor/offsets and an optional aspect-preserving transparent logo. Every enabled
control must have a verified native effect; address the related #764 style fields
through this consumer rather than leaving an actionable no-op.

Draft appearance changes preview immediately and reach the on-air key only on
Apply. Show original/edited comparison. Save, duplicate and reset named presets;
persist a versioned appearance document without changing existing saved looks
unless the operator chooses a new preset. Missing fonts/assets report a clear
fallback or failure. Define long-text wrapping/truncation and output bounds.

One apply publishes a complete immutable content/style revision. Source changes
use the existing build transition once; content/style edits do not restart it.
Raster work and cache invalidation depend on content/style changes, not roster
polls or frame number. WinUI does not process production pixels.

## Grade scope ROI

ROI is a scope analysis selection, not a local grading mask, crop, source route,
or output overlay. The grade still applies across the full source. Histogram,
waveform and vectorscope all use the same region and Original/Graded tap. Graded
analysis uses the actual editor revision, including a draft where selected;
the revision must be identified so a draft is not presented as on-air evidence.

Provide `Full frame` / `ROI` next to the existing scope tap controls. Entering
ROI uses the last valid region for this source, or a centered default region.
The user can draw a replacement rectangle and move/resize it with handles.
Provide keyboard manipulation, synchronized X/Y/Width/Height percentage fields,
Reset region and a separate Hide guide action. Hide guide leaves analysis active;
Full frame disables the region. All scopes show an `ROI` badge and the bounds.

Use normalized coordinates in the oriented source image before scene framing
or overlays: x/y in [0,1], positive width/height, x+width and y+height at most 1.
Translate pointer coordinates through the monitor's fit/letterbox transform;
letterbox bars are not selectable pixels. During original/graded comparison,
both views refer to the same source coordinates. Do not include the guide in
the analyzed pixels.

For a W by H source, select the half-open pixel rectangle with left/top equal to
floor(x*W), floor(y*H), and right/bottom equal to ceil((x+width)*W),
ceil((y+height)*H), clipped to image bounds. Require at least one pixel in each
dimension after conversion. Invalid numeric edits keep the last valid region
and explain the constraint. A valid tiny region is sampled from that region;
never silently analyze the full frame or report synthetic black.

Preserve the existing bounded native analysis budget and graph resolution.
Map the analysis grid into the selected pixel rectangle rather than discarding
most of a full-frame grid. Disclose actual sample dimensions/count and ROI pixel
bounds; do not claim full-resolution measurement for subsampled analysis.
Histogram excludes all pixels outside ROI and normalizes using selected valid
samples. Vectorscope does the same. Waveform and RGB parade use the selected
region's horizontal extent across the graph (left ROI edge to right ROI edge);
vertical signal scale/units remain unchanged. This avoids squeezing a narrow
region into a tiny part of the waveform. Transparent/invalid pixels follow the
declared source normalization policy rather than being counted as black.

Each analysis request/result identifies source and generation, source frame/time,
grade/draft revision, tap, ROI enabled state, geometry and ROI revision, sample
count/dimensions, signal units and status. The UI rejects older ROI/selection/tap
results. While a new region is pending, show updating; do not label the old trace
as the newly selected region. Source advancement governs stale status.

Coalesce dragging in the existing latest-request mailbox; never queue every
pointer event. Analysis remains optional and asynchronous. No full-frame readback,
production context wait, or extra work under the Zoom publication mutex is added.
Hiding scopes or closing the editor releases demand; hiding only the ROI guide
does not. Analysis failure leaves production running and reports unavailable.

Keep ROI as per-source editor analysis preferences separate from grade documents
and presets. Persist only where a stable source identity already exists. Never
transfer a departed guest's region to a different guest with a reused meeting ID.
Unknown/replaced identity starts at Full frame. Normalized bounds survive source
resolution changes, but results from the previous generation are rejected.

## Scenes and audio

Starter scenes use distinct eligible inputs and clear empty placeholders while
preserving custom saved scenes. Solo, two-person and screen-plus-presenter looks
are straightforward to choose, assign, preview and take. Improve scalar controls
with the shared slider/value pattern; reuse the existing canvas for drag/resize,
alignment, aspect lock and legible fit/fill/crop behavior.

Retain effective audio mixer faders. Convert confusing continuous processing
controls to slider/value pairs with meaningful dB, Hz, ms and ratio labels,
appropriate scaling and observable processor/meter feedback. Verify native
parameter semantics before converting display units; this bundle does not invent
new DSP or quietly change an existing sound.

## Acceptance contract

| Area | Required evidence |
|---|---|
| Shared controls | Pointer/keyboard/value synchronization, exact-value preservation, defaults/reset, invalid input, range limits, saved-value round trips, high DPI/narrow windows and final-value delivery after rapid edits |
| Lower thirds | Stable unchanged key; clean one-time real edit; source/slot replacement, blank/reset, persistence, fonts/logo/long text; actual Program/Preview, webcam, decoded recording and local stream agree |
| ROI correctness | A known multi-patch image: select each patch and a boundary crossing; expected bins/counts/levels change only with included pixels for original and graded taps; Full frame restores baseline |
| ROI interaction | Letterboxing, moving/resizing, exact bounds, single-pixel region, comparison view, resolution/generation change, rapid drag, stale results and close/reopen demand |
| Scenes | Fresh and upgraded profiles, distinct sources/placeholders, preserved custom scenes, assign/preview/take and geometry changes through actual native output |
| Audio | Existing settings preserve sound; parameter conversions, intended processor response and metering agree; keyboard/scroll interaction is safe |
| Delivery | Matched baseline/candidate with Program/Preview, webcam and recording; optional analysis/editing adds no missed production deadline, unbounded queue or retained capture lease |

Retain failing evidence. Short functional checks do not substitute for the weekend
meeting bake. Issue closure requires merged implementation meeting its own
acceptance, not this document or the presence of controls.
