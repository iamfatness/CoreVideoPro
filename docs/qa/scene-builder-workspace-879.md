# Scene Builder and CoreVideo Tiles acceptance

Owner-approved implementation of [#879](https://github.com/iamfatness/CoreVideoPro/issues/879), following the [reviewed workspace specification](../reference/scene-builder-workspace-spec.md). The owner's October 12 live meeting was preserved during development. The owner subsequently reported, "Bake was successful no new issues to report," and authorized further builds and testing. The restart/installation hold is resolved.

That owner observation applies to the previous installed runtime `63d1337`, not the new Scene Builder candidate. It is successful operator-observed bake evidence, without a new measured zero-miss claim or qualification of the redesigned workspace.

## Development evidence

The implementation was built on the installed isolation validation candidate `e5d29e1` (installed runtime `63d1337`). Development artifacts are under `artifacts/design-review/`; the self-contained review application is under `artifacts/scene-builder-879/review-self-contained-app/`. These are review artifacts, not a published beta or an installed update.

| Evidence | Observed result |
|---|---|
| Release WinUI compilation | Passed; existing compiler/analyzer warnings remain |
| Scene, Tiles, overlay, transport and startup shell tests | 313 passed with consistent self-contained `win-x64` settings |
| MediaCore managed tests | 2,395 passed |
| Release native compilation | Passed |
| Native `Tiles*` test selection | 81 passed, including new wall-instance/shared-source tests and the D3D rounded-mask/border/glow pixel test |
| Self-contained runtime probe | Passed, exit 0; 2,370,320-byte merged PRI; bundled runtime/theme modules; no window opened |
| Offscreen compiled-XAML/model probe | Passed: zero-route wall, protected Program, exact edit/Undo/Redo, saved-scene recall, source identity/order/visibility/lock/duplicate/removal, desktop/compact sizing; core startup disabled |
| Local review archive | `beta-2026-10-12-6438b19`; package manifest and all 856 file hashes passed; SHA256 `fcbd623eb7b2d7ce6fdbe8ed7305d83ce68ea224fae6d844ff2231f57978851a` |
| Local review installer | NSIS build completed; not installed or published during the bake |
| Active installed process preservation | Existing GUI PID 59312 and native PID 66072 remained running; no restart, installation, camera registration or live control command was performed |

The native tests verify wall bounds, draw opacity, transparent background, authored-layer order and suppression of unrelated-source fallback. The existing D3D test samples real rendered pixels for tile decoration. These do not establish a sustained zero-miss bake or installed workspace usability.

Failed development evidence is retained. The first offscreen probe read the old Program display projection after directly assigning a synthetic active scene; its fixture now performs the transport's normal local reconciliation before asserting protection. The second probe detected that lazy command sync could start a separate test core despite disabling launch-time startup. Probe mode now blocks the central startup/sync paths as well. Both reports remain beside the final passing report; the original installed GUI/core processes were not stopped or reconfigured.

The first local publish had only the app PRI. A clean self-contained publish produced the merged theme PRI and passed the runtime probe before packaging. Switching the test build back to framework-dependent settings then hit missing cached theme asset copy items; the final test run used the same self-contained `win-x64` settings and passed. Failed build logs remain in the evidence directory.

## Installed review protocol

The October 12 follow-up compiled the main-based PR with a duplication correction: copied layers retain spotlight targeting, border styling, source-framing state and visibility. The compiled-XAML/model probe passed on this corrected source (`workspace-main-operator-probe.json`), including a customized hidden-source duplicate assertion. The earlier `6438b19` review installer predates this correction; it is not evidence for the corrected duplicate behavior.

Use a staged candidate that also retains the installed Program/status isolation fixes. Record its exact source revision and executable paths. Preserve the saved show before installation. Verify the normal desktop shortcut and webcam registration through the established installer workflow after the meeting ends.

| Review action | Required observation |
|---|---|
| Select an existing zero-route Tiles scene | Moving composed Preview appears; wall is selected as one named source; no empty-scene placeholder |
| Add a camera, screen share or imported logo above Tiles | Authored source stays in the list and in native Preview; no regenerated duplicate participant routes |
| Drag a source row, use Move forward/back or Alt+arrows | Picture and list ordering agree; current selection and input focus stay intact |
| Hide, show, lock and unlock | Output visibility is distinct from opacity; locked transforms resist dragging/nudging; source remains selectable |
| Edit wall transform, opacity and styles | Native Preview changes; all enabled outputs use the same composition after Take |
| Change aspect, spacing, margin, capacity and animation | Tiles reflow with their requested look; unrelated scene layers retain position/order |
| Select a member, exclude/allow, assign/clear a manual slot | The wall's membership follows the chosen policy; meeting identity safeguards and stale-entry controls remain available |
| Crop a selected member | Apply crop retains automatic placement or the existing pin; it does not accidentally create a pin |
| Pin and return a member to automatic | Pin uses wall-local normalized coordinates; other tiles use the native remaining-space layout |
| Undo/Redo or cancel a canvas gesture with Escape | Draft geometry/style is restored; cancelled gesture does not remain as a history action |
| Save while editing the on-air scene | Saved definition changes, Program remains unchanged; next Take publishes the captured draft |
| Switch away and return to a saved scene | Saved layers, wall style, visibility/lock and authored names are restored |
| Resize at effective desktop/DPI widths | Lists and inspector scroll vertically; compact Properties drawer closes; no routine horizontal page scroll |
| Exercise controls while roster changes | No lost source/member selection, closing pickers, source substitution or focus jumps |
| Repeat the matched meeting workload | Separate render, Program, webcam and enabled sender delivery evidence; retain any misses and their initiating-cause attribution |

One Tiles wall per scene is supported. The wall's opacity multiplies its native draw layers; it is not an offscreen alpha-isolated group. Add-source entries reuse existing show feeds and media ownership. Arbitrary new text/color source adapters, nested groups and multiple independent walls are not added by this change. Keep #879 open until installed visual acceptance and persistence/output checks are recorded.
