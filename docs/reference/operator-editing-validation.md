# Operator editing review candidate

Implementation: `c136dd8`, on `codex/operator-editing-bundle`. The owner requested
one combined review pass for lower-third appearance (#847), shared adjustment
controls (#848), grading scope ROI (#835) and starter scenes (#591). This builds
on the existing grading/framer, Sources, Settings/Health, OHG and lower-third text
branches. The specification remains
[operator-editing-bundle-spec.md](operator-editing-bundle-spec.md).

## Implemented behavior

Continuous grading, scene position/framing and mastering controls now share a
slider, exact entry, displayed units/ranges and reset. Exact entry preserves
precision independently of slider steps; invalid values retain the previous
value. Grading records one undo step for a continuous gesture. Unfocused wheel
movement does not adjust a slider.

The lower-third designer offers compact solid, minimal accent and broadcast
presets; typography, name/title/background/accent colors, opacity, width, padding,
corners, four anchors, safe offsets and logo sizing. Native draft preview and
applied-look comparison are isolated from Program. Apply saves the appearance
and requests the live update while preserving key enablement and source text.
Named looks support save, load, duplication and reset. Missing font/image
fallbacks are reported by native rasterization. Existing saved looks retain
their legacy rendering until a new appearance is applied.

Grading scopes can sample a normalized ROI selected by drawing, moving/resizing,
keyboard or exact percentage entry. Letterboxing is excluded. Histogram,
waveform and vectorscope use the selected pixels; waveform spans the selected
region. Original/Graded taps remain independent. Geometry and revision travel
with each result, and stale selections are rejected. The guide is editor-only;
grades still affect the whole source. Stable source identities retain ROI
preferences separately from grade presets.

Starter scenes bind distinct show input slots, with an explicit share route for
speaker/slides. Initialization preserves previously saved custom routes.

## Short verification evidence

Release native and self-contained WinUI builds succeeded. Managed suites passed:
1,630 WinUI, 2,370 MediaCore and 84 Control tests (4,084 total). Focused native
suites passed 27 tests: grade previews (14), overlay raster (12) and advanced
grade pixels (1).

The self-contained executable passed 13 operator checks and 11 grading/native
integration checks. These include exact-entry precision and invalid input,
compiled lower-third designer at narrow width, ROI-only analysis without grade
changes, native preset replacement and existing source identity/flyout checks.
Known-color native pixel tests verify a one-pixel ROI excludes neighboring
pixels, waveform horizontal coverage and unchanged Program during isolated
lower-third preview.

Raw local evidence lives under `artifacts/operator-bundle-*`: build/test logs,
`operator-bundle-operator-probe.json`, `operator-bundle-grade-probe.json` and its
layout PNG. The review package contains copies of these reports and a checksum
manifest. The XAML capture API excludes GPU video pixels; physical display
presentation is explicitly unverified by this probe.

## Installed review and weekend bake

The local package is an unsigned review candidate, not a published beta. This
pass does not replace or restart the user's installed application or change
virtual-camera registration. Installation must run the normal installer and
verify the registered camera's app directory before using webcam output.

Installed acceptance still needs actual output comparison across Program,
Preview, webcam, decoded recording and stream; saved appearance/ROI restart;
font/logo/long-text review; pointer/keyboard and high-DPI interaction; and a
matched baseline/candidate delivery check with scopes enabled. The owner's
weekend meeting bake provides sustained media evidence. Keep the linked issues
open until merged implementation meets each issue's acceptance criteria.
