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
alongside corrected reruns. Installed checks are pending until recorded below.
