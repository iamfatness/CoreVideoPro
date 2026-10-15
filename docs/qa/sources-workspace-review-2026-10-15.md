# Sources workspace implementation review — October 15, 2026

Implementation for #590 builds on the submitted #912/#913 operator review
candidate. The approved direction is the table-first workspace, optional
inspector and existing connected grading editor; it is not the earlier
inspector-first proposal. The preservation contract is in
`docs/reference/sources-table-workspace-spec.md`.

## Implemented behavior

The native WinUI overview retains the ten existing input models, source-specific
preview surfaces, name/type, observed resolution/FPS, signal and tally, audio
meter, quick MV inclusion and ISO selection with separate observed writer state.
Selection, search, category filters and resize only affect presentation. Secondary
column preferences live in a separate UI-layout file and do not rewrite
production configuration. Narrow layouts expose omitted fields in row Details.

The inspector uses the existing categorized source picker, display-name store,
guarded lower-third/policy flyouts and explicit paired-audio assignment. A pending
name edit cannot follow an input whose source was replaced. Overview and inspector
Grade actions use the existing native editor command and source identity; no web
scope or simplified CSS grade entered the app. Capture/browser, SRT and RTMP setup
controls remain in expandable catalog sections, including connection, sync,
creation/removal and runtime status. Zoom request ceilings remain discoverable and
are not substituted for measured signal values.

Rows and their preview controls are retained across filtering/resizing and
metadata refresh. The native media core and Zoom/virtual-camera binaries are not
modified by this work. Format/FPS availability continues to follow the existing
observed input projection; unknown/stale data is not manufactured as live signal.

## Evidence

Local records and captures live in `artifacts/sources-workspace/` in the isolated
review checkout. This is development validation, not installed acceptance.

| Check | Result / boundary |
|---|---|
| Release WinUI build | Pass; existing repository warnings remain |
| Focused source/assignment tests | 42 passed, no failed/skipped |
| Expanded source, preference, scene and grade regression run | 192 passed, no failed/skipped |
| Compiled offscreen operator integration | 30 checks passed, including mixed synthetic source assignment, ten retained rows, filter/selection/resize invariance, native grade target IDs, offline/reconnect, stale rename rejection, MV/ISO, preserved adapters and prior Scene Builder/lower-third/navigation checks |
| Layout widths | 1600, 1100, 900 and 720, overview and inspector |
| Existing native-backed grade editor | 12 offscreen checks passed against the owned D3D11 fixture core: GPU preview/scopes, native acknowledged Apply, curve editing, scope expansion, comparison, scopes-only drawn ROI/full frame, undo/preset reload and isolated lower-third drafts |
| Desktop preservation | Recorded foreground handle unchanged during the offscreen operator run; installed beta process remained running |

An initial compiled fixture built its expected offline label from an already
inactive synthetic roster, producing a duplicate “Not in show · Offline”
expectation. The actual picker matched the authoritative model. The fixture now
hydrates its in-memory coordinator roster consistently; the failed receipts are
retained. Another capture showed Fluent transitional control plates; explicit
scoped dark field/button styles made the captured controls legible. These are
not treated as successful trials before correction.

The grade fixture is a private synthetic source/core with Zoom/device/output
adapters disabled. It does not validate the test meeting, physical capture,
stream/record receivers or camera registration. RenderTargetBitmap captures
control layout, not SwapChainPanel picture/scope pixels. Native grade command
targets are checked without activating an on-screen popout; the separate editor
probe exercises the actual native-backed workspace offscreen.

## Remaining acceptance

The owner must still review real-window/high-DPI ergonomics and actual mixed
Zoom/camera/media behavior when ready. Installed thumbnail churn, physical
presentation, output continuity and a sustained real-meeting bake remain open.
This work does not claim zero misses, close media acceptance, install a beta,
register a camera, change shortcuts or restart the installed application.
