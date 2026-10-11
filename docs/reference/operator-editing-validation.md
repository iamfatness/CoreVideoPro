# Operator editing review candidate

Original review implementation: `c136dd8`, on `codex/operator-editing-bundle`. The owner requested
one combined review pass for lower-third appearance (#847), shared adjustment
controls (#848), grading scope ROI (#835) and starter scenes (#591). This builds
on the existing grading/framer, Sources, Settings/Health, OHG and lower-third text
branches. The specification remains
[operator-editing-bundle-spec.md](operator-editing-bundle-spec.md).

October 10 delivery update: #849 merged the bundle, followed by #855 for drawn
rectangle/circle ROI. The combined implementation is published and installed in
[beta-2026-10-10-abb97fb](https://github.com/iamfatness/CoreVideoPro/releases/tag/beta-2026-10-10-abb97fb),
commit `abb97fb89fd75527a1c2e425f72e0135877592d8`. Earlier branch counts below are
historical evidence; they are not the current release gate totals. The remaining
checks are in the [installed UI/UX acceptance protocol](../qa/operator-uiux-installed-acceptance.md).

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

The original local package was an unsigned review candidate. It has been
superseded by the published, installed beta above. Current release evidence in
`artifacts/releases/beta-2026-10-10-abb97fb/VALIDATION.json` records 1,566 native
tests, 13 installed operator checks, 12 installed grading checks, hosted gates,
asset digests and installed file/registration verification.

The synthetic OS-camera reference passed its measured 35 seconds with 2,101
decoded counter samples and zero invalid/repeated/missing/reordered identities.
It does not qualify full meeting load, physical presentation or A/V. A subsequent
real meeting lost the SDK helper (#856), and a later launch failed webcam ownership
checks (#857). Restart recovered webcam activation; a five-second OS receiver read
300 samples at 1080p60. The exit cause and ownership failure cause remain unresolved.
These incidents are not UI acceptance passes or reasons to close their issues.

Installed acceptance still needs actual output comparison across Program,
Preview, webcam, decoded recording and stream; saved appearance/ROI restart;
font/logo/long-text review; pointer/keyboard and high-DPI interaction; and a
matched baseline/candidate delivery check with scopes enabled. The owner's
weekend meeting bake provides sustained media evidence. Keep the linked issues
open until merged implementation meets each issue's acceptance criteria.
