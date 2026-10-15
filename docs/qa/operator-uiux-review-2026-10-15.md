# Operator UI/UX review candidate — October 15, 2026

This candidate extends `4366d5b` without changing native media code. The owner
authorized API/offscreen development while away and explicitly excluded changes
to the installed app. No installation, camera registration, desktop activation,
meeting join, production preference edit or release was performed.

Review changes are in [#912](https://github.com/iamfatness/CoreVideoPro/pull/912)
and [#913](https://github.com/iamfatness/CoreVideoPro/pull/913). Merge #912 before
#913; both target main so the repository's CI and CodeQL workflows run for the
submitted commits. The latter includes the complete combined review candidate.

## Changes available for review

| Area | Result |
|---|---|
| Starter scenes, #591 | New-scene chooser offers Solo, Two people and Screen + presenter. New and missing layout routes use stable named inputs and an explicit share, rather than following the current speaker. Empty inputs remain selectable. Existing custom routes and Program are preserved. |
| Sources, #590/#882 | A departed source retains its last available picker label for its own assignment, with Offline/Not in show state and recovery guidance. This label is presentation-only: no cached picture, lower-third override or automatic source substitution. Reassignment clears the old label association. |
| Shared adjustments, #848 | Compact sliders retain Reset. Units and accessible reset names identify the setting and default. Reset synchronizes entry and slider as one complete gesture. A valid slider/reset edit clears an earlier validation error. |
| Lower thirds, #847 | Preview, draft status, starter choice and Apply/Cancel precede expandable Typography, Colors and plate, Size and placement, Source logo and Saved looks sections. Existing native controls, presets and draft/apply behavior remain the consumers. |
| Grading, #848/#835 | Curve channel is named. Home/End and Ctrl+arrows select points; arrows move by 0.1%, Shift by 1%. Endpoint input is disabled and remains fixed. Curve hydration preserves the surrounding loading guard. |
| ROI, #853 | Existing rectangle/circle drawing, scopes-only semantics, native masks, original/graded taps and full-frame restore qualified in the isolated editor; no new grading mask or output overlay is introduced. |
| Settings, #594/#595 | Existing Health expansion and support export remain reachable. Compiled toggle checks cover enabling OHG, disabling while selected, returning to Settings and retaining saved scenes across preference serialization. |

## Evidence

Local evidence is in `artifacts/uiux-review/` in the review worktree. These are
development checks, not installed acceptance or a real-meeting bake.

| Check | Result |
|---|---|
| Release WinUI build | Pass; existing repository warnings remain. |
| Focused managed regression run | 232 passed, zero failed/skipped. Includes starter creation, canvas, source assignment/thumbnail identity, lower-third text, ROI and navigation preferences. |
| Compiled offscreen UI | 26 checks passed; `operator-report.json`. In-memory show and roster, no native core launched by this probe. |
| Compiled editor with an owned D3D11 core | 12 checks passed; `grade-report.json`. GPU preview/scopes, acknowledged grade apply, circle drawing, tap changes, full-frame restore, undo/preset reload and lower-third drafts. |
| Native control/framing/raster checks | 7 grade-preview control, 5 advanced-grade contract, 19 framing and 5 GPU overlay tests passed. |
| Native pixel checks, fixture mode | 9 grade-preview pixel, 1 advanced-grade pixel and 1 held-source framing checks passed with CPU preparation disabled; scopes-only rectangle/circle and private lower-third draft isolation are included. |

The first native invocation used a colon-separated filter unsupported by the
repository's small test runner and ran zero tests. It is retained, not counted.
The forced CPU-preparation trial retained seven pixel/framing failures. The
same unchanged native source passed all eleven affected fixtures with CPU
preparation disabled. The mode-qualified results do not waive production-mode
acceptance; the isolated app/editor check above is separate evidence. Logs and
both mode receipts are retained. No native test assertions were weakened.

The synthetic core was built locally in Release with D3D11 enabled, Zoom and
device/output adapters disabled. It cannot qualify a Zoom meeting, physical
camera reader, sound, recording or streaming. GPU control layout images exclude
SwapChainPanel pixels; placeholder pictures in these images are not live video.

The Sources compiled fixture initially expected a picker label without its role
suffix; the expectation was corrected to preserve the actual existing label.
Another fixture selected every descendant Button, including the TextBox's clear
button; it now selects the Reset action. The retained failure report identifies
that fixture error. Neither failure was treated as a passing trial.

## Review boundaries

The candidate is ready for an operator feature review, not a claim of complete
acceptance or zero-miss performance. Physical pointer/high-DPI interaction,
installed source thumbnail churn, actual receiver pixels, font/logo fallbacks,
all lower-third native style effects across enabled outputs and a sustained
meeting bake remain unverified here. Existing issues remain open for their
remaining acceptance. No runtime buffering, image quality or frame-rate policy
was changed to obtain these results.

Review artifacts include the three starter layouts, Tiles workspace, named
offline Sources row, curve controls, lower-third overview and individual setting
sections, native grade layout and Settings Health. The local review page labels
these capture limitations and links the machine-readable reports.
