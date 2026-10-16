# Inline Sources patching review (#590)

Owner review of beta 26350c0 identified a regression: assignment moved behind
Inspect. Each retained source row now exposes the categorized source picker.
The inspector reuses the same control. Both use the existing canonical assignment
setter; no routing, scene, native media, subscription or preview-demand change.

Opening/hydrating the menu never assigns. Its entries stay stable while open;
clicks reject a replaced source, retired row, changed inspector input or removed
choice. Compact row labels retain the existing display name/offline identity.
Grade, MV, ISO and optional detailed inspection remain directly reachable.

Release build passed (0 errors; existing warnings). All 138 focused managed
Sources/ShowInput/preview-demand tests passed. The offscreen compiled operator
probe passed 31 checks, including real menu-item invocation with Inspect closed,
other-input/Program preservation, stale-choice rejection, roster-menu stability,
offline/reconnect labels and retained row objects across compact layouts.
The first compiled run caught a picker label-binding failure after patch checks
passed; the label now binds directly to the input in its dependency-property
callback, and the complete rerun passed. Failed receipt retained locally.

API/offscreen only. The installed beta, settings, camera and user session were
not changed. RenderTargetBitmap control captures exclude GPU pixels; they do not
prove first-frame presentation. The owner's inspector video-delay report is #916;
read-only triage did not establish its cause, and this patch does not claim a
preview latency fix. Installed visual acceptance remains open.
