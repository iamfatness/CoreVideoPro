# Graphics source and workflow audit — #1007

Owner scope: audit Zoom assumptions, support local-camera/screen lower thirds,
transparent PNGs, and verify the installed desktop-shortcut app. This does not
waive the independent full-workload/stuttering acceptance gate.

| Area reviewed | Finding and treatment |
| --- | --- |
| Native source catalog | Zoom-only roster projection omitted capture sources. Now also projects actual source-bus capture video incarnations, including with no Zoom session. |
| Binding/render admission | Unconditional `substr(5)` and camera-only draw checks rejected local/screens. Canonical frame selectors and exact-generation textured-draw checks now cover camera and screen layers. |
| Lifecycle/format change | Capture source-bus owner stamps a video incarnation, preserved through pixel-format changes and replaced after clear/removal. Graphics does not own another ingest lifecycle or auto-rebind old labels. |
| Studio lower-third handoff | Assigned look resolves actual Program attribution at each explicit LT in. Sources overrides, including intentionally blank secondary text, remain authoritative. Help now says source rather than guest and states when text is sampled. |
| Source/automation pickers | Local default display names now resolve through capture-device metadata, and label signatures refresh with source-name changes. |
| Automatic introductions | Draw facts were forcibly turned into Zoom IDs. Canonical local facts now qualify. Screen suppression defaults on; explicit disable permits a screen subject. Native manual/emergency holds, dwell and reconnect disarming remain intact. |
| Transparent PNG import | Existing verified asset loader and WIC premultiplied-alpha decode already support PNG alpha. Added standalone full-canvas image import, distinct from square logo import. Exact bytes survive package export/reimport. |
| Asynchronous import/editing | Import cannot discard a newer private selection or edit. Verification failures leave prior graphics intact. |
| Cue/Program, persistence, authority | Kept immutable accepted Cue/Program copies, explicit Show/Update, template pins, hidden restore, and no implicit activation. Existing tests retained. |
| Clocks/titles/bugs | Unbound native instances do not depend on Zoom; renderer/template/clock and resource-budget regressions remain in the native Graphics suite. |

Remaining scope boundaries: nested scenes/Tiles binding is not qualified here;
NDI/AJA/Blackmagic hardware-specific acceptance requires implemented adapters
and actual devices, even though canonical capture binding no longer assumes a
webcam or Zoom. Automation remains conservatively disarmed on a Zoom meeting
epoch change. A saved source-bound cue requires explicit current identity on
restore. These are disclosed boundaries, not evidence of those workflows passing.

Evidence is retained under `artifacts/graphics-1007*`. The initial shell test
resource-copy failure and native test-fixture output-size mismatch are retained
alongside corrected reruns. Installed functional checks are recorded below; workload acceptance stays separate.


Installed trial 1 (358d0baf): package installed and normal-shortcut app
started with native capture identities for webcam and screen while Zoom was
offline. The first direct launcher did not stay up; an independent normal-user
shortcut launcher succeeded. An attempted Zoom-engine toggle was refused while
offline (unneeded for local capture). The Studio handoff correctly rejected a
browsed template without a hidden cue. This trial is not accepted. Complete the
same Create hidden cue -> Use in Studio -> LT in workflow through its service
API, then repackage and repeat installed checks. Do not weaken assignment guards.


A separate output workflow defect was found: local recording controls still
require `Settings.IsInMeeting`, although actual local Program and registered OS
camera pixels work offline. Tracked in [#1009](https://github.com/iamfatness/CoreVideoPro/issues/1009)
for owner ranking; no offline recording acceptance is claimed by this Graphics
repair. Engine On is intentionally the Zoom media-subscription toggle and is
not required to render local capture. Do not join a meeting merely to hide the
local recording control dependency during qualification.

The first candidate independently submitted actual source-bound webcam and
screen graphics; cutting to screen stopped composing the webcam label. An
ordinary registered OS camera consumer delivered 180 frames and its decoded
image showed the screen picture and lower-third caption. The final candidate
must still repeat installed Studio Create/Assign/Show and PNG checks.


## Installed functional evidence and reconnect follow-up

Candidate b78be7b8 was installed from its immutable package and launched through
the regular desktop shortcut in the normal user session. All 864 manifest-listed
files matched. Create hidden cue -> Use in Studio -> LT in succeeded for the
physical webcam and local screen capture, retaining source names/secondary text.
Independent registered Windows-camera consumers captured 180 frames for each
source and 180 for the imported full-canvas PNG; decoded actual images were
inspected. Transparent and half-alpha PNG patches matched expected blended pixel
values within 8 RGB levels. The first analysis decode used BT.709 incorrectly;
that failed evidence is retained. Matching the documented BT.601 studio-swing
camera encoding passed without widening the tolerance.

The same audit identified #1010: automatic local introductions could use a raw
capture ID after reconnect when old incarnation-specific text was correctly
rejected. The repair now projects the core's existing accepted source-policy
labels into the capture catalog without device enumeration or UI polling. New
regressions retain exact incarnation fences, reject stale person overrides and
prove metadata-only changes do not replace source identity or republish unchanged
rows. Final installed reconnect acceptance remains pending for this follow-up.

No full-workload/per-frame/physical-display pass is established. #1009 remains a
separate recording-control dependency. Media-file bindings, nested scenes/Tiles
and real NDI/AJA/Blackmagic hardware workflows are not qualified by these checks.


The actual local screen route uses the generic `participant-video` layer, unlike
Zoom's `screen-share` layer. Native draw-fact classification now recognizes the
actual drawn `capture:screen:` / `capture:window:` selectors as presentation
content, so default screen suppression also applies to local capture. Explicit
manual Show remains available; an automatic rule can opt out of suppression.
A D3D pixel/draw-fact regression exercises the real capture-input layer kind.


Candidate 2aad1f93 verified automatic local-screen suppression, explicit opt-out,
and a real capture offline/online incarnation change through the installed app.
The new incarnation used the accepted friendly label while rejecting an old
incarnation-specific text override. Operator Unassign/reassign of the camera
also succeeded without restart. Evidence: graphics-1007-03/automatic-reconnect-result.json
and camera-reuse-result.json.

That candidate failed an immediate Take -> LT in comparison (#1011). Actual native
fade attribution contained both outgoing webcam and incoming screen, and the
shell's sticky subject preference selected the outgoing webcam. The native exact
source fence prevented a wrong caption being drawn after the fade, but the Show
workflow was still broken. The resolver now limits a Take's subjects to actual
native take-in layers before applying its sticky preference. Tests cover an
incoming screen, incoming Zoom picture and the outgoing-only boundary. Original
failed snapshots and the short timing study remain in graphics-1007-03; installed
acceptance of this repair is pending.


## Final installed functional acceptance

Installed `beta-2026-10-21-g10074a795bd`, source
`4a795bd0375b7ac580ffa538697b7bb886aed6da`, through the established local NSIS
installer (exit 0). All 864 manifest-listed files matched. The regular desktop
shortcut opened that exact installation with no arguments, normal user SID,
non-elevated session 1, and no QA video/renderer overrides. Registration Inspect
matched the desktop user's camera registration to the installed candidate.

Actual installed Studio services completed Create hidden cue -> Use in Studio ->
LT in for webcam and screen while Zoom was offline. Four independent ordinary OS
camera consumers each delivered 180 frames (720 total); decoded webcam, screen,
automatic reconnect and PNG images were inspected. Eight alternating camera/screen
Take -> LT in trials all started during active fades (progress 0.57-0.91), used no
fixed delay before Show, bound the incoming subject and remained correctly
composed after the Take completed. This resolves the retained #1011 failed trial.

The final candidate repeated the actual local-screen automatic suppression /
explicit opt-out and offline/online reconnect checks: generation 1 -> 3 in the
same core, friendly native label retained, old text binding rejected, unchanged
rule configuration. Operator Unassign/reassign reused camera input 10 without
restart through its real command/API; this is not a claim of physical mouse-click
retesting. Compiled controls separately cover empty-row picker reachability.

Standalone image import -> hidden cue -> Cue -> Show reached actual Program
composition. Decoded opaque-red, half-green-over-blue, transparent hidden-red and
transparent corner samples were [253,0,0], [0,126,126], [0,0,253], [0,0,253], within
the original tolerance of 8. Exact PNG bytes/package roundtrip and the historical
square-logo manifest remain covered by regressions.

Validation: 1,829 full production native tests; MediaCore 2,596, WinUI 1,722 and
Control 102 managed tests; 54 compiled Studio/Graphics/settings checks and bundled
runtime probe. The final native binary's SHA256 matches the full native test
candidate (a164ad12698c8a8f7b720a69ae6f2e7ac61e62434f833a730930d79e2c41a693);
only the managed Take resolver changed afterward. Hosted checks must still be
read from the PR; local results do not imply all hosted jobs have completed.

Evidence: `artifacts/graphics-1007-04/installed-functional-acceptance.json`,
`rapid-take-result.json`, `automatic-reconnect-result.json`,
`camera-reuse-result.json`, `png-alpha-acceptance.json`, four `*-os` captures,
`installed-payload-verification.json`, `installed-camera-registration.json` and
`final-camera-progress.json`. Earlier failed candidate evidence is retained.
The initial final-candidate API request preceded server readiness and was refused;
the successful ready-state rerun is saved separately.

Owned test graphics/outputs were cleared, the five original profile files were
restored with matching hashes, and the regular shortcut reopened the final app.
Program was then routed to the webcam for review: eight successive samples show
camera ingest and actual Program advancing. Recording/streaming are off; the
original saved virtual-camera enabled state is restored. No public beta was
published. Full-workload/per-frame/display acceptance remains false and #1009
is still open. This completes the installed Graphics functional checks, not the
independent stuttering/recording qualification or future hardware acceptance.


Owner follow-up #1012 exposed an unqualified embedded-logo workflow: standalone
PNG import acceptance above did not prove local-logo replacement in declarative
logo templates. See [logo upload evidence](graphics-logo-upload-2026-10-21.md).
